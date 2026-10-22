// sip_server_dynamic.c - stateless SIP redirect server for the S1->S2->S3->S2->S4 flow.
//
// This is the router's own SIP endpoint (daemon mode). It answers INVITEs with
// 302 redirects and keeps all call state in PostgreSQL through the same SQL
// functions the FreeSWITCH Lua handler uses (sql/002_functions.sql):
//
//   INVITE from an origin provider (S1)        -> router_allocate_did
//       302 Contact: sip:<DID>@<intermediate>
//   INVITE from an intermediate provider (S3)  -> router_complete_return
//       302 Contact: sip:<original DNIS>@<final provider of that call>
//
// A redirect cannot rewrite the caller ID; the full ANI/DNIS swap needs the
// FreeSWITCH (B2BUA) mode. See docs/ARCHITECTURE.md.
//
// Concurrency model: one receive thread handles messages; a monitor thread
// reloads providers/routes into fresh arrays and swaps them under `lock`.
// Handlers copy what they need while holding the lock, so a reload can never
// free memory a handler is still reading.
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "core/logging.h"
#include "core/safety.h"
#include "sip/sip_parse.h"
#include "sip/sip_server.h"

#define BUFFER_SIZE 65536
#define RESPONSE_SIZE 4096

typedef struct {
    int id;
    char uuid[64];
    char name[128];
    char host[256];
    int port;
    char auth_type[16];
    char auth_data[512];
    char role[16];
    bool active;
} provider_t;

typedef struct {
    int id;
    char pattern[256];
    int origin_id;
    int intermediate_id;
    int final_id;
} route_t;

struct sip_server {
    int socket_fd;
    int port;
    database_t *db;
    router_t *router;

    pthread_mutex_t lock;  // guards the two arrays below
    provider_t *providers;
    int nproviders;
    route_t *routes;  // active routes, highest priority first
    int nroutes;

    atomic_uint_fast64_t total_invites;
    atomic_uint_fast64_t forwarded_calls;
    atomic_uint_fast64_t failed_calls;
    atomic_bool running;
    int reload_seconds;
    pthread_t listen_thread;
    pthread_t monitor_thread;
    bool threads_started;
};

// ------------------------------------------------------------------ loading --

static void copy_str(char *dst, size_t len, const char *src) {
    snprintf(dst, len, "%s", src ? src : "");
}

static int load_snapshot(sip_server_t *server) {
    db_result_t *pr = db_query(server->db,
        "SELECT id, COALESCE(uuid, ''), name, host, port, auth_type, COALESCE(auth_data, ''), role, active "
        "FROM providers WHERE active ORDER BY id");
    db_result_t *rr = db_query(server->db,
        "SELECT id, pattern, COALESCE(origin_provider_id, 0), COALESCE(intermediate_provider_id, 0), "
        "COALESCE(final_provider_id, 0) FROM routes WHERE active ORDER BY priority DESC, id");

    int np = pr ? pr->num_rows : 0;
    int nr = rr ? rr->num_rows : 0;
    provider_t *providers = np ? calloc((size_t)np, sizeof(provider_t)) : NULL;
    route_t *routes = nr ? calloc((size_t)nr, sizeof(route_t)) : NULL;
    if ((np && !providers) || (nr && !routes)) {
        free(providers);
        free(routes);
        db_free_result(pr);
        db_free_result(rr);
        return -1;
    }

    for (int i = 0; i < np; i++) {
        provider_t *p = &providers[i];
        p->id = atoi(db_get_value(pr, i, 0));
        copy_str(p->uuid, sizeof(p->uuid), db_get_value(pr, i, 1));
        copy_str(p->name, sizeof(p->name), db_get_value(pr, i, 2));
        copy_str(p->host, sizeof(p->host), db_get_value(pr, i, 3));
        p->port = atoi(db_get_value(pr, i, 4));
        copy_str(p->auth_type, sizeof(p->auth_type), db_get_value(pr, i, 5));
        copy_str(p->auth_data, sizeof(p->auth_data), db_get_value(pr, i, 6));
        copy_str(p->role, sizeof(p->role), db_get_value(pr, i, 7));
        p->active = strcmp(db_get_value(pr, i, 8), "t") == 0;
    }
    for (int i = 0; i < nr; i++) {
        route_t *r = &routes[i];
        r->id = atoi(db_get_value(rr, i, 0));
        copy_str(r->pattern, sizeof(r->pattern), db_get_value(rr, i, 1));
        r->origin_id = atoi(db_get_value(rr, i, 2));
        r->intermediate_id = atoi(db_get_value(rr, i, 3));
        r->final_id = atoi(db_get_value(rr, i, 4));
    }
    db_free_result(pr);
    db_free_result(rr);

    pthread_mutex_lock(&server->lock);
    provider_t *old_p = server->providers;
    route_t *old_r = server->routes;
    server->providers = providers;
    server->nproviders = np;
    server->routes = routes;
    server->nroutes = nr;
    pthread_mutex_unlock(&server->lock);

    free(old_p);
    free(old_r);
    LOG_DEBUG("SIP snapshot loaded: %d providers, %d routes", np, nr);
    return 0;
}

// Copy of the provider sending from `ip`, matched exactly on host or on the
// IP list of an ip-authenticated provider.
static bool provider_by_ip(sip_server_t *server, const char *ip, provider_t *out) {
    bool found = false;
    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < server->nproviders && !found; i++) {
        const provider_t *p = &server->providers[i];
        if (strcmp(p->host, ip) == 0 ||
            (strcmp(p->auth_type, "ip") == 0 && safe_ip_in_list(p->auth_data, ip))) {
            *out = *p;
            found = true;
        }
    }
    pthread_mutex_unlock(&server->lock);
    return found;
}

static bool provider_by(sip_server_t *server, int id, const char *uuid, provider_t *out) {
    bool found = false;
    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < server->nproviders && !found; i++) {
        const provider_t *p = &server->providers[i];
        if ((uuid && strcmp(p->uuid, uuid) == 0) || (!uuid && p->id == id)) {
            *out = *p;
            found = true;
        }
    }
    pthread_mutex_unlock(&server->lock);
    return found;
}

// First route (by priority) for this origin whose pattern matches the DNIS.
static bool route_for(sip_server_t *server, int origin_id, const char *dnis, route_t *out) {
    bool found = false;
    pthread_mutex_lock(&server->lock);
    for (int i = 0; i < server->nroutes && !found; i++) {
        const route_t *r = &server->routes[i];
        if ((r->origin_id == 0 || r->origin_id == origin_id) && safe_pattern_matches(r->pattern, dnis)) {
            *out = *r;
            found = true;
        }
    }
    pthread_mutex_unlock(&server->lock);
    return found;
}

// ---------------------------------------------------------------- responses --

static void reply(sip_server_t *server, const struct sockaddr_in *to, const char *request,
                  int code, const char *reason, const char *contact) {
    char buf[RESPONSE_SIZE];
    int len = sip_build_response(buf, sizeof(buf), request, code, reason, contact);
    if (len < 0) {
        LOG_WARN("Cannot answer %d: request lacks mandatory headers", code);
        return;
    }
    if (sendto(server->socket_fd, buf, (size_t)len, 0, (const struct sockaddr *)to, sizeof(*to)) < 0) {
        LOG_WARN("sendto failed: %s", strerror(errno));
    }
}

static void fail(sip_server_t *server, const struct sockaddr_in *to, const char *request,
                 int code, const char *reason) {
    atomic_fetch_add(&server->failed_calls, 1);
    reply(server, to, request, code, reason, NULL);
}

// ------------------------------------------------------------------ INVITE --

static void handle_origin(sip_server_t *server, const char *msg, const struct sockaddr_in *from,
                          const provider_t *src, const char *ip, const char *call_id,
                          const char *ani, const char *dnis) {
    route_t route;
    if (!route_for(server, src->id, dnis, &route)) {
        LOG_WARN("No route for %s from %s", dnis, src->name);
        fail(server, from, msg, 404, "Not Found");
        return;
    }
    provider_t inter;
    if (!provider_by(server, route.intermediate_id, NULL, &inter)) {
        LOG_ERROR("Route %d has no active intermediate provider", route.id);
        fail(server, from, msg, 503, "Service Unavailable");
        return;
    }

    char route_id[16];
    snprintf(route_id, sizeof(route_id), "%d", route.id);
    const char *params[] = {route_id, call_id, ani, dnis, ip};
    db_result_t *res = db_query_params(server->db,
        "SELECT router_allocate_did($1::int, $2, $3, $4, $5) AS did", 5, params);
    char did[64] = "";
    if (res && res->num_rows == 1) copy_str(did, sizeof(did), db_get_value(res, 0, 0));
    db_free_result(res);

    if (!did[0]) {
        LOG_WARN("No DID available for call %s on route %d", call_id, route.id);
        fail(server, from, msg, 503, "Service Unavailable");
        return;
    }

    char contact[512];
    snprintf(contact, sizeof(contact), "sip:%s@%s:%d", did, inter.host, inter.port);
    reply(server, from, msg, 100, "Trying", NULL);
    reply(server, from, msg, 302, "Moved Temporarily", contact);
    atomic_fetch_add(&server->forwarded_calls, 1);
    LOG_INFO("S1->S3 call %s: ANI %s DNIS %s -> DID %s via %s", call_id, ani, dnis, did, inter.name);
}

static void handle_return(sip_server_t *server, const char *msg, const struct sockaddr_in *from,
                          const provider_t *src, const char *ip, const char *did) {
    const char *params[] = {did, ip};
    db_result_t *res = db_query_params(server->db,
        "SELECT original_ani, original_dnis, COALESCE(final_gateway, '') "
        "FROM router_complete_return($1, $2)", 2, params);
    if (!res || res->num_rows != 1) {
        db_free_result(res);
        LOG_WARN("Return from %s on DID %s which is not allocated", src->name, did);
        fail(server, from, msg, 403, "Forbidden");
        return;
    }
    char ani[64], dnis[64], gateway[64];
    copy_str(ani, sizeof(ani), db_get_value(res, 0, 0));
    copy_str(dnis, sizeof(dnis), db_get_value(res, 0, 1));
    copy_str(gateway, sizeof(gateway), db_get_value(res, 0, 2));
    db_free_result(res);

    provider_t final;
    if (!gateway[0] || !provider_by(server, 0, gateway, &final)) {
        LOG_ERROR("Final provider %s for DID %s is not active", gateway, did);
        fail(server, from, msg, 503, "Service Unavailable");
        return;
    }
    char contact[512];
    snprintf(contact, sizeof(contact), "sip:%s@%s:%d", dnis, final.host, final.port);
    reply(server, from, msg, 302, "Moved Temporarily", contact);
    atomic_fetch_add(&server->forwarded_calls, 1);
    LOG_INFO("S3->S4: DID %s restored to ANI %s DNIS %s via %s", did, ani, dnis, final.name);
}

static void handle_invite(sip_server_t *server, const char *msg, const struct sockaddr_in *from) {
    atomic_fetch_add(&server->total_invites, 1);

    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &from->sin_addr, ip, sizeof(ip));

    char call_id[256], from_hdr[512], to_hdr[512], ani[64], dnis[64];
    if (!sip_header(msg, "Call-ID", call_id, sizeof(call_id)) ||
        !sip_header(msg, "From", from_hdr, sizeof(from_hdr)) ||
        !sip_header(msg, "To", to_hdr, sizeof(to_hdr))) {
        fail(server, from, msg, 400, "Bad Request");
        return;
    }
    if (!sip_uri_user(from_hdr, ani, sizeof(ani)) || !sip_uri_user(to_hdr, dnis, sizeof(dnis)) ||
        !safe_is_number(ani) || !safe_is_number(dnis)) {
        fail(server, from, msg, 484, "Address Incomplete");
        return;
    }

    provider_t src;
    if (!provider_by_ip(server, ip, &src)) {
        LOG_WARN("INVITE from unknown address %s", ip);
        fail(server, from, msg, 403, "Forbidden");
        return;
    }

    if (strcmp(src.role, "origin") == 0) {
        handle_origin(server, msg, from, &src, ip, call_id, ani, dnis);
    } else if (strcmp(src.role, "intermediate") == 0) {
        handle_return(server, msg, from, &src, ip, dnis);
    } else {
        LOG_WARN("Provider %s (role %s) may not send calls", src.name, src.role);
        fail(server, from, msg, 403, "Forbidden");
    }
}

static void process_message(sip_server_t *server, const char *msg, const struct sockaddr_in *from) {
    char method[32];
    if (!sip_method(msg, method, sizeof(method))) return;  // responses and garbage are ignored

    if (strcmp(method, "INVITE") == 0) {
        handle_invite(server, msg, from);
    } else if (strcmp(method, "OPTIONS") == 0) {
        reply(server, from, msg, 200, "OK", NULL);
    } else if (strcmp(method, "ACK") == 0) {
        // ACK for our final response: nothing to do.
    } else {
        reply(server, from, msg, 405, "Method Not Allowed", NULL);
    }
}

// ------------------------------------------------------------------ threads --

static void *listen_thread(void *arg) {
    sip_server_t *server = arg;
    char *buffer = malloc(BUFFER_SIZE);
    if (!buffer) return NULL;
    LOG_INFO("SIP server listening on UDP port %d", server->port);

    while (atomic_load(&server->running)) {
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        ssize_t len = recvfrom(server->socket_fd, buffer, BUFFER_SIZE - 1, 0,
                               (struct sockaddr *)&from, &from_len);
        if (len > 0) {
            buffer[len] = '\0';
            process_message(server, buffer, &from);
        } else if (len < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            LOG_ERROR("recvfrom: %s", strerror(errno));
        }
    }
    free(buffer);
    return NULL;
}

static void *monitor_thread(void *arg) {
    sip_server_t *server = arg;
    int elapsed = 0;
    while (atomic_load(&server->running)) {
        sleep(1);  // short naps so stop() returns within a second
        if (++elapsed < server->reload_seconds) continue;
        elapsed = 0;
        load_snapshot(server);
        db_result_t *res = db_query(server->db, "SELECT router_release_stale_dids(interval '5 minutes')");
        if (res && res->num_rows == 1 && atoi(db_get_value(res, 0, 0)) > 0) {
            LOG_WARN("Released %s stale DID(s)", db_get_value(res, 0, 0));
        }
        db_free_result(res);
    }
    return NULL;
}

// --------------------------------------------------------------- lifecycle --

sip_server_t *sip_server_create(router_t *router, database_t *db) {
    sip_server_t *server = calloc(1, sizeof(*server));
    if (!server) return NULL;
    server->db = db;
    server->router = router;
    server->socket_fd = -1;
    server->port = atoi(safe_env("ROUTER_SIP_PORT", "5060"));
    server->reload_seconds = atoi(safe_env("ROUTER_RELOAD_SECONDS", "30"));
    if (server->reload_seconds < 1) server->reload_seconds = 30;
    pthread_mutex_init(&server->lock, NULL);

    server->socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (server->socket_fd < 0) {
        LOG_ERROR("socket: %s", strerror(errno));
        sip_server_destroy(server);
        return NULL;
    }
    int reuse = 1;
    setsockopt(server->socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(server->socket_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)server->port);
    if (inet_pton(AF_INET, safe_env("ROUTER_SIP_BIND", "0.0.0.0"), &addr.sin_addr) != 1 ||
        bind(server->socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        LOG_ERROR("Cannot bind SIP port %d: %s", server->port, strerror(errno));
        sip_server_destroy(server);
        return NULL;
    }

    if (load_snapshot(server) != 0) {
        LOG_ERROR("Cannot load providers and routes");
        sip_server_destroy(server);
        return NULL;
    }
    return server;
}

int sip_server_start(sip_server_t *server) {
    if (!server) return -1;
    atomic_store(&server->running, true);
    if (pthread_create(&server->listen_thread, NULL, listen_thread, server) != 0) {
        atomic_store(&server->running, false);
        return -1;
    }
    if (pthread_create(&server->monitor_thread, NULL, monitor_thread, server) != 0) {
        atomic_store(&server->running, false);
        pthread_join(server->listen_thread, NULL);
        return -1;
    }
    server->threads_started = true;
    return 0;
}

void sip_server_stop(sip_server_t *server) {
    if (!server || !server->threads_started) return;
    atomic_store(&server->running, false);
    pthread_join(server->listen_thread, NULL);
    pthread_join(server->monitor_thread, NULL);
    server->threads_started = false;
}

void sip_server_destroy(sip_server_t *server) {
    if (!server) return;
    sip_server_stop(server);
    if (server->socket_fd >= 0) close(server->socket_fd);
    free(server->providers);
    free(server->routes);
    pthread_mutex_destroy(&server->lock);
    free(server);
}

void sip_server_get_stats(sip_server_t *server, sip_stats_t *stats) {
    if (!server || !stats) return;
    stats->total_invites = atomic_load(&server->total_invites);
    stats->forwarded_calls = atomic_load(&server->forwarded_calls);
    stats->failed_calls = atomic_load(&server->failed_calls);
    stats->active = atomic_load(&server->running);
}
