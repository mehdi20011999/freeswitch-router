// handlers.c - the router's read-only HTTP API. See include/api/handlers.h.
// (The original handler answered {"status":"ok"} to every URL and method.)
#include <json-c/json.h>
#include <stdlib.h>
#include <string.h>
#include "api/handlers.h"
#include "core/safety.h"
#include "db/database.h"
#include "sip/sip_server.h"

extern sip_server_t *g_sip_server;  // main.c; NULL when the SIP server is off

static enum MHD_Result send_json(struct MHD_Connection *c, unsigned int status, json_object *body) {
    const char *text = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
    struct MHD_Response *r = MHD_create_response_from_buffer(strlen(text), (void *)text, MHD_RESPMEM_MUST_COPY);
    json_object_put(body);
    if (!r) return MHD_NO;
    MHD_add_response_header(r, "Content-Type", "application/json");
    MHD_add_response_header(r, "Cache-Control", "no-store");
    enum MHD_Result ret = MHD_queue_response(c, status, r);
    MHD_destroy_response(r);
    return ret;
}

static enum MHD_Result error(struct MHD_Connection *c, unsigned int status, const char *message) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "error", json_object_new_string(message));
    return send_json(c, status, o);
}

static enum MHD_Result health(struct MHD_Connection *c) {
    db_result_t *r = db_query(get_database(), "SELECT 1");
    bool ok = r != NULL;
    db_free_result(r);
    json_object *o = json_object_new_object();
    json_object_object_add(o, "status", json_object_new_string(ok ? "ok" : "degraded"));
    json_object_object_add(o, "database", json_object_new_string(ok ? "ok" : "unreachable"));
    return send_json(c, ok ? MHD_HTTP_OK : MHD_HTTP_SERVICE_UNAVAILABLE, o);
}

static enum MHD_Result stats(struct MHD_Connection *c) {
    db_result_t *r = db_query(get_database(), "SELECT * FROM system_status");
    if (!r) return error(c, MHD_HTTP_SERVICE_UNAVAILABLE, "database unavailable");
    json_object *o = json_object_new_object();
    for (int i = 0; i < r->num_cols; i++) {
        json_object_object_add(o, r->columns[i], json_object_new_int64(atoll(db_get_value(r, 0, i))));
    }
    db_free_result(r);
    if (g_sip_server) {
        sip_stats_t s = {0};
        sip_server_get_stats(g_sip_server, &s);
        json_object *sip = json_object_new_object();
        json_object_object_add(sip, "invites", json_object_new_int64((int64_t)s.total_invites));
        json_object_object_add(sip, "forwarded", json_object_new_int64((int64_t)s.forwarded_calls));
        json_object_object_add(sip, "failed", json_object_new_int64((int64_t)s.failed_calls));
        json_object_object_add(o, "sip", sip);
    }
    return send_json(c, MHD_HTTP_OK, o);
}

static enum MHD_Result route(struct MHD_Connection *c) {
    const char *number = MHD_lookup_connection_value(c, MHD_GET_ARGUMENT_KIND, "number");
    if (!safe_is_number(number)) return error(c, MHD_HTTP_BAD_REQUEST, "number must be digits, optional leading +");
    const char *params[] = {number};
    db_result_t *r = db_query_params(get_database(),
        "SELECT r.id, r.name, r.pattern, p2.name, p2.uuid, p3.name "
        "FROM routes r "
        "JOIN providers p2 ON p2.id = r.intermediate_provider_id "
        "JOIN providers p3 ON p3.id = r.final_provider_id "
        "WHERE r.active AND $1 ~ ('^(' || r.pattern || ')$') "
        "ORDER BY r.priority DESC, r.id LIMIT 1", 1, params);
    if (!r) return error(c, MHD_HTTP_NOT_FOUND, "no route");
    json_object *o = json_object_new_object();
    json_object_object_add(o, "number", json_object_new_string(number));
    json_object_object_add(o, "route_id", json_object_new_int(atoi(db_get_value(r, 0, 0))));
    json_object_object_add(o, "route", json_object_new_string(db_get_value(r, 0, 1)));
    json_object_object_add(o, "pattern", json_object_new_string(db_get_value(r, 0, 2)));
    json_object_object_add(o, "intermediate", json_object_new_string(db_get_value(r, 0, 3)));
    json_object_object_add(o, "intermediate_gateway", json_object_new_string(db_get_value(r, 0, 4)));
    json_object_object_add(o, "final", json_object_new_string(db_get_value(r, 0, 5)));
    db_free_result(r);
    return send_json(c, MHD_HTTP_OK, o);
}

enum MHD_Result handle_api_request(struct MHD_Connection *connection, const char *url, const char *method) {
    if (strcmp(method, "GET") != 0) return error(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "read-only API: use GET");
    if (strcmp(url, "/health") == 0) return health(connection);
    if (strcmp(url, "/api/v1/stats") == 0) return stats(connection);
    if (strcmp(url, "/api/v1/route") == 0) return route(connection);
    return error(connection, MHD_HTTP_NOT_FOUND, "unknown endpoint");
}
