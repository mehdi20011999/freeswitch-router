// fs_xml_generator.c - writes the router's FreeSWITCH configuration from the database.
//
// The router owns a few namespaced files and never overwrites FreeSWITCH's own
// configuration (freeswitch.xml, vars.xml, modules.conf.xml, ...):
//
//   <conf>/sip_profiles/router.xml          sofia profile "router", context "router"
//   <conf>/sip_profiles/router/<uuid>.xml   one gateway per active provider
//   <conf>/dialplan/router.xml              context "router": routes + final 404
//   <conf>/dialplan/router/route_<id>.xml   two extensions per route
//   <conf>/router_vars.xml                  router_db_dsn (only if ROUTER_FS_DB_DSN is set)
//   <scripts>/route_handler.lua             copied from <share>
//
// Paths and settings come from the environment (see docs/OPERATIONS.md):
//   ROUTER_FS_CONF_DIR     default /etc/freeswitch
//   ROUTER_FS_SCRIPTS_DIR  default /usr/share/freeswitch/scripts
//   ROUTER_SHARE_DIR       default /usr/local/share/freeswitch-router
//   ROUTER_FS_SIP_IP       default $${local_ip_v4}
//   ROUTER_FS_SIP_PORT     default 5060
//   ROUTER_FS_DB_DSN       DSN the Lua handler uses (written to router_vars.xml)
//   ROUTER_FS_CLI          default fs_cli
//
// Every value taken from the database is validated or XML-escaped, and every
// file is written to a temporary name and renamed, so FreeSWITCH never reads a
// half-written file.
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "core/logging.h"
#include "core/safety.h"
#include "db/database.h"
#include "freeswitch/fs_xml_generator.h"

#define PATH_LEN 1024

static const char *conf_dir(void) { return safe_env("ROUTER_FS_CONF_DIR", "/etc/freeswitch"); }
static const char *scripts_dir(void) { return safe_env("ROUTER_FS_SCRIPTS_DIR", "/usr/share/freeswitch/scripts"); }
static const char *share_dir(void) { return safe_env("ROUTER_SHARE_DIR", "/usr/local/share/freeswitch-router"); }

// ------------------------------------------------------------------ files --

static int ensure_dir(const char *path) {
    char tmp[PATH_LEN];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        LOG_ERROR("Cannot create %s: %s", tmp, strerror(errno));
        return -1;
    }
    return 0;
}

typedef struct {
    FILE *fp;
    char path[PATH_LEN];
    char tmp[PATH_LEN + 8];
} out_file_t;

static int out_open(out_file_t *o, const char *path) {
    snprintf(o->path, sizeof(o->path), "%s", path);
    snprintf(o->tmp, sizeof(o->tmp), "%s.tmp", path);
    o->fp = fopen(o->tmp, "w");
    if (!o->fp) LOG_ERROR("Cannot write %s: %s", o->tmp, strerror(errno));
    return o->fp ? 0 : -1;
}

static int out_commit(out_file_t *o, mode_t mode) {
    int bad = ferror(o->fp);
    if (fclose(o->fp) != 0) bad = 1;
    o->fp = NULL;
    if (bad || chmod(o->tmp, mode) != 0 || rename(o->tmp, o->path) != 0) {
        LOG_ERROR("Cannot finish %s", o->path);
        unlink(o->tmp);
        return -1;
    }
    return 0;
}

// Remove files in `dir` named <prefix>*.xml, except the ones listed in `keep`.
static void prune_dir(const char *dir, const char *prefix, char keep[][96], int nkeep) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, strlen(prefix)) != 0 || len < 5 ||
            strcmp(e->d_name + len - 4, ".xml") != 0) {
            continue;
        }
        bool kept = false;
        for (int i = 0; i < nkeep && !kept; i++) kept = strcmp(keep[i], e->d_name) == 0;
        if (!kept) {
            char path[PATH_LEN];
            snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            unlink(path);
            LOG_INFO("Removed stale %s", path);
        }
    }
    closedir(d);
}

// IPv4/IPv6/hostname -> regex matching exactly that string.
static void host_regex(const char *host, char *out, size_t len) {
    size_t o = 0;
    for (const char *p = host; *p && o + 3 < len; p++) {
        if (*p == '.') out[o++] = '\\';
        out[o++] = *p;
    }
    out[o] = '\0';
}

// ------------------------------------------------------- static config ----

int fs_init_all_directories(void) {
    char path[PATH_LEN];
    const char *subdirs[] = {"sip_profiles/router", "dialplan/router", NULL};
    for (int i = 0; subdirs[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", conf_dir(), subdirs[i]);
        if (ensure_dir(path) != 0) return -1;
    }
    return ensure_dir(scripts_dir());
}

int fs_generate_sip_profiles(void) {
    char path[PATH_LEN], sip_ip[256], port[16];
    if (!safe_xml_escape(safe_env("ROUTER_FS_SIP_IP", "$${local_ip_v4}"), sip_ip, sizeof(sip_ip))) return -1;
    snprintf(port, sizeof(port), "%d", atoi(safe_env("ROUTER_FS_SIP_PORT", "5060")));

    snprintf(path, sizeof(path), "%s/sip_profiles/router.xml", conf_dir());
    out_file_t o;
    if (out_open(&o, path) != 0) return -1;
    fprintf(o.fp,
        "<!-- Generated by freeswitch-router: do not edit, run `router provider reload`. -->\n"
        "<profile name=\"router\">\n"
        "  <gateways>\n"
        "    <X-PRE-PROCESS cmd=\"include\" data=\"router/*.xml\"/>\n"
        "  </gateways>\n"
        "  <settings>\n"
        "    <param name=\"context\" value=\"router\"/>\n"
        "    <param name=\"dialplan\" value=\"XML\"/>\n"
        "    <param name=\"sip-ip\" value=\"%s\"/>\n"
        "    <param name=\"rtp-ip\" value=\"%s\"/>\n"
        "    <param name=\"sip-port\" value=\"%s\"/>\n"
        "    <param name=\"auth-calls\" value=\"false\"/>\n"
        "    <param name=\"accept-blind-reg\" value=\"false\"/>\n"
        "    <param name=\"accept-blind-auth\" value=\"false\"/>\n"
        "    <param name=\"manage-presence\" value=\"false\"/>\n"
        "    <param name=\"inbound-codec-negotiation\" value=\"generous\"/>\n"
        "    <param name=\"inbound-late-negotiation\" value=\"true\"/>\n"
        "  </settings>\n"
        "</profile>\n",
        sip_ip, sip_ip, port);
    return out_commit(&o, 0644);
}

int fs_generate_dialplan_contexts(void) {
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/dialplan/router.xml", conf_dir());
    out_file_t o;
    if (out_open(&o, path) != 0) return -1;
    fprintf(o.fp,
        "<!-- Generated by freeswitch-router: do not edit, run `router route reload`. -->\n"
        "<include>\n"
        "  <context name=\"router\">\n"
        "    <X-PRE-PROCESS cmd=\"include\" data=\"router/*.xml\"/>\n"
        "    <extension name=\"router_no_route\">\n"
        "      <condition field=\"destination_number\" expression=\"^.*$\">\n"
        "        <action application=\"log\" data=\"WARNING router: no route for ${destination_number} from ${network_addr}\"/>\n"
        "        <action application=\"respond\" data=\"404 Not Found\"/>\n"
        "      </condition>\n"
        "    </extension>\n"
        "  </context>\n"
        "</include>\n");
    return out_commit(&o, 0644);
}

// router_db_dsn for the Lua handler. Written only when ROUTER_FS_DB_DSN is set,
// readable by FreeSWITCH only: it contains a password.
int fs_generate_autoload_configs(void) {
    const char *dsn = getenv("ROUTER_FS_DB_DSN");
    if (!dsn || !*dsn) {
        LOG_WARN("ROUTER_FS_DB_DSN not set: define router_db_dsn in FreeSWITCH vars.xml yourself");
        return 0;
    }
    char escaped[1024];
    if (!safe_xml_escape(dsn, escaped, sizeof(escaped)) || strchr(dsn, '"')) {
        LOG_ERROR("ROUTER_FS_DB_DSN is too long or contains a double quote");
        return -1;
    }
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/router_vars.xml", conf_dir());
    out_file_t o;
    if (out_open(&o, path) != 0) return -1;
    fprintf(o.fp,
        "<!-- Generated by freeswitch-router. Include it from vars.xml:\n"
        "     <X-PRE-PROCESS cmd=\"include\" data=\"router_vars.xml\"/> -->\n"
        "<include>\n"
        "  <X-PRE-PROCESS cmd=\"set\" data=\"router_db_dsn=%s\"/>\n"
        "</include>\n",
        escaped);
    return out_commit(&o, 0640);
}

// Kept for API compatibility: FreeSWITCH's own freeswitch.xml is no longer
// overwritten (the original replaced it, with hard-coded IPs and password).
int fs_generate_main_config(void) { return 0; }

// --------------------------------------------------------------- providers --

int fs_generate_provider_config(const char *uuid, const char *name, const char *host, int port,
                                const char *role, int auth_type, const char *auth_data) {
    (void)role;
    if (!safe_is_token(uuid) || !safe_is_host(host) || port < 1 || port > 65535) {
        LOG_ERROR("Provider %s: invalid uuid, host or port, gateway not written", name ? name : "?");
        return -1;
    }
    if (fs_init_all_directories() != 0) return -1;
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/sip_profiles/router/%s.xml", conf_dir(), uuid);
    out_file_t o;
    if (out_open(&o, path) != 0) return -1;

    char label[96] = "";
    if (name && safe_is_token(name)) snprintf(label, sizeof(label), " (%s)", name);
    fprintf(o.fp,
        "<!-- Generated by freeswitch-router: provider %s%s -->\n"
        "<include>\n"
        "  <gateway name=\"%s\">\n"
        "    <param name=\"proxy\" value=\"%s:%d\"/>\n"
        "    <param name=\"realm\" value=\"%s\"/>\n"
        "    <param name=\"caller-id-in-from\" value=\"true\"/>\n"
        "    <param name=\"ping\" value=\"30\"/>\n",
        uuid, label, uuid, host, port, host);

    char user[128] = "", pass[128] = "";
    if (auth_type == 1 && auth_data && sscanf(auth_data, "%127[^:]:%127s", user, pass) == 2) {
        char u[400], p[400];
        if (!safe_xml_escape(user, u, sizeof(u)) || !safe_xml_escape(pass, p, sizeof(p))) {
            fclose(o.fp);
            unlink(o.tmp);
            return -1;
        }
        fprintf(o.fp,
            "    <param name=\"username\" value=\"%s\"/>\n"
            "    <param name=\"password\" value=\"%s\"/>\n"
            "    <param name=\"register\" value=\"true\"/>\n", u, p);
    } else {
        fprintf(o.fp, "    <param name=\"register\" value=\"false\"/>\n");
    }
    fprintf(o.fp, "  </gateway>\n</include>\n");
    // 0640: the file may hold a SIP password.
    return out_commit(&o, 0640);
}

int fs_remove_provider_config(const char *uuid, const char *role) {
    (void)role;
    if (!safe_is_token(uuid)) return -1;
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/sip_profiles/router/%s.xml", conf_dir(), uuid);
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

int fs_regenerate_all_providers(void) {
    database_t *db = get_database();
    if (!db) return -1;
    db_result_t *result = db_query(db,
        "SELECT uuid, name, host, port, role, auth_type, COALESCE(auth_data, '') "
        "FROM providers WHERE active AND uuid IS NOT NULL ORDER BY id");

    int n = result ? result->num_rows : 0;
    char (*keep)[96] = n ? calloc((size_t)n, sizeof(*keep)) : NULL;
    int written = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        const char *uuid = db_get_value(result, i, 0);
        int auth = strcmp(db_get_value(result, i, 5), "ip") == 0 ? 0 : 1;
        if (fs_generate_provider_config(uuid, db_get_value(result, i, 1), db_get_value(result, i, 2),
                                        atoi(db_get_value(result, i, 3)), db_get_value(result, i, 4),
                                        auth, db_get_value(result, i, 6)) == 0) {
            if (keep) snprintf(keep[written], sizeof(keep[0]), "%s.xml", uuid);
            written++;
        } else {
            failed++;
        }
    }
    db_free_result(result);

    // Gateways of deleted or deactivated providers must disappear too.
    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/sip_profiles/router", conf_dir());
    prune_dir(dir, "", keep, written);
    free(keep);

    LOG_INFO("Provider gateways: %d written, %d invalid", written, failed);
    return failed ? -1 : 0;
}

// ------------------------------------------------------------------ routes --

static int write_route(const char *id, const char *pattern, const char *origin_uuid, const char *origin_host,
                       const char *inter_uuid, const char *inter_host, const char *final_uuid) {
    if (!safe_is_route_pattern(pattern) || !safe_is_token(origin_uuid) || !safe_is_token(inter_uuid) ||
        !safe_is_token(final_uuid) || !safe_is_host(origin_host) || !safe_is_host(inter_host)) {
        LOG_ERROR("Route %s: invalid pattern, provider or host, dialplan not written", id);
        return -1;
    }
    char origin_re[600], inter_re[600];
    host_regex(origin_host, origin_re, sizeof(origin_re));
    host_regex(inter_host, inter_re, sizeof(inter_re));

    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/dialplan/router/route_%s.xml", conf_dir(), id);
    out_file_t o;
    if (out_open(&o, path) != 0) return -1;
    // Two conditions in one extension are ANDed (break="on-false" is the
    // default). The original used break="never" on the address check, which
    // let any source address through.
    fprintf(o.fp,
        "<!-- Generated by freeswitch-router: route %s -->\n"
        "<include>\n"
        "  <extension name=\"route_%s_from_origin\">\n"
        "    <condition field=\"network_addr\" expression=\"^%s$\"/>\n"
        "    <condition field=\"destination_number\" expression=\"^(%s)$\">\n"
        "      <action application=\"lua\" data=\"route_handler.lua origin %s %s %s\"/>\n"
        "    </condition>\n"
        "  </extension>\n"
        "  <extension name=\"route_%s_return_from_intermediate\">\n"
        "    <condition field=\"network_addr\" expression=\"^%s$\"/>\n"
        "    <condition field=\"destination_number\" expression=\"^\\+?\\d{1,32}$\">\n"
        "      <action application=\"lua\" data=\"route_handler.lua intermediate_return %s %s %s\"/>\n"
        "    </condition>\n"
        "  </extension>\n"
        "</include>\n",
        id, id, origin_re, pattern, id, inter_uuid, final_uuid,
        id, inter_re, id, origin_uuid, final_uuid);
    return out_commit(&o, 0644);
}

int fs_generate_route_dialplan(void) {
    database_t *db = get_database();
    if (!db) return -1;
    if (fs_init_all_directories() != 0 || fs_generate_dialplan_contexts() != 0) return -1;

    db_result_t *result = db_query(db,
        "SELECT r.id, r.pattern, p1.uuid, p1.host, p2.uuid, p2.host, p3.uuid "
        "FROM routes r "
        "JOIN providers p1 ON p1.id = r.origin_provider_id AND p1.active "
        "JOIN providers p2 ON p2.id = r.intermediate_provider_id AND p2.active "
        "JOIN providers p3 ON p3.id = r.final_provider_id AND p3.active "
        "WHERE r.active ORDER BY r.priority DESC, r.id");

    int n = result ? result->num_rows : 0;
    char (*keep)[96] = n ? calloc((size_t)n, sizeof(*keep)) : NULL;
    int written = 0, failed = 0;
    for (int i = 0; i < n; i++) {
        const char *id = db_get_value(result, i, 0);
        if (write_route(id, db_get_value(result, i, 1), db_get_value(result, i, 2), db_get_value(result, i, 3),
                        db_get_value(result, i, 4), db_get_value(result, i, 5), db_get_value(result, i, 6)) == 0) {
            if (keep) snprintf(keep[written], sizeof(keep[0]), "route_%s.xml", id);
            written++;
        } else {
            failed++;
        }
    }
    // `written` is read before the result is freed (the original logged
    // result->num_rows after db_free_result).
    db_free_result(result);

    char dir[PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/dialplan/router", conf_dir());
    prune_dir(dir, "route_", keep, written);
    free(keep);

    int lua = fs_generate_route_handler_lua();
    LOG_INFO("Route dialplans: %d written, %d invalid", written, failed);
    return (failed || lua) ? -1 : 0;
}

int fs_remove_route_dialplan(const char *route_id) {
    if (!route_id || strspn(route_id, "0123456789") != strlen(route_id) || !*route_id) return -1;
    char path[PATH_LEN];
    snprintf(path, sizeof(path), "%s/dialplan/router/route_%s.xml", conf_dir(), route_id);
    if (unlink(path) != 0 && errno != ENOENT) return -1;
    return 0;
}

// Install the call-path script shipped with the router (no code generation:
// the script is a normal, tested file in freeswitch/scripts/).
int fs_generate_route_handler_lua(void) {
    char src[PATH_LEN], dst[PATH_LEN];
    snprintf(src, sizeof(src), "%s/route_handler.lua", share_dir());
    snprintf(dst, sizeof(dst), "%s/route_handler.lua", scripts_dir());
    FILE *in = fopen(src, "r");
    if (!in) {
        LOG_ERROR("Cannot read %s (run `make install` or set ROUTER_SHARE_DIR)", src);
        return -1;
    }
    out_file_t o;
    if (out_open(&o, dst) != 0) {
        fclose(in);
        return -1;
    }
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) fwrite(buf, 1, n, o.fp);
    fclose(in);
    return out_commit(&o, 0644);
}

// --------------------------------------------------------------- lifecycle --

static int fs_cli(const char *command) {
    const char *argv[] = {safe_env("ROUTER_FS_CLI", "fs_cli"), "-x", command, NULL};
    return safe_run(argv);
}

int fs_reload_config(void) {
    int rc = 0;
    if (fs_cli("reloadxml") != 0) rc = -1;
    // rescan picks up new gateways; start is needed the first time.
    if (fs_cli("sofia profile router rescan") != 0 && fs_cli("sofia profile router start") != 0) rc = -1;
    if (rc != 0) LOG_WARN("FreeSWITCH reload failed (is FreeSWITCH running and fs_cli in PATH?)");
    return rc;
}

int fs_generate_complete_config(void) {
    if (fs_init_all_directories() != 0) return -1;
    int rc = 0;
    if (fs_generate_sip_profiles() != 0) rc = -1;
    if (fs_generate_autoload_configs() != 0) rc = -1;
    if (fs_regenerate_all_providers() != 0) rc = -1;
    if (fs_generate_route_dialplan() != 0) rc = -1;
    return rc;
}
