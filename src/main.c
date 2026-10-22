// main.c - FreeSWITCH call router (S1 -> S2 -> S3 -> S2 -> S4).
//
//   router                      interactive CLI
//   router <command> [args]     run one CLI command and exit (e.g. `router route list`)
//   router --daemon             FreeSWITCH sync + HTTP API (+ SIP redirect server
//                               if ROUTER_SIP_REDIRECT=1)
//   router --daemon --no-freeswitch
//                               SIP redirect server + HTTP API, FreeSWITCH untouched
//
// Configuration: configs/config.json (or ROUTER_CONFIG) plus environment
// variables documented in docs/OPERATIONS.md. No secret is compiled in.
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "cli/cli.h"
#include "core/config.h"
#include "core/logging.h"
#include "core/safety.h"
#include "core/server.h"
#include "db/database.h"
#include "freeswitch/fs_xml_generator.h"
#include "router/router.h"
#include "sip/sip_server.h"
#include "validation/call_validator.h"

// Globals shared with the CLI commands and the HTTP API.
cli_t *g_cli = NULL;
sip_server_t *g_sip_server = NULL;
call_validator_t *g_validator = NULL;

static atomic_bool running = true;

static void signal_handler(int sig) {
    (void)sig;
    running = false;  // async-signal-safe: no logging, no locks here
}

static bool freeswitch_running(void) {
    const char *argv[] = {"pgrep", "-x", "freeswitch", NULL};
    return safe_run(argv) == 0;
}

static int sync_freeswitch(void) {
    if (!freeswitch_running()) {
        LOG_ERROR("FreeSWITCH is not running (start it, or use --no-freeswitch)");
        return -1;
    }
    if (fs_generate_complete_config() != 0) {
        LOG_WARN("Some FreeSWITCH files could not be generated, see the log");
    }
    return fs_reload_config();
}

static void *validation_monitor_thread(void *arg) {
    (void)arg;
    int elapsed = 0;
    while (running) {
        sleep(1);  // short naps: shutdown must not wait 5 minutes
        if (++elapsed >= 300) {
            elapsed = 0;
            if (g_validator) cleanup_expired_validations(g_validator);
        }
    }
    return NULL;
}

static void usage(void) {
    printf("Usage: router [--daemon [--no-freeswitch]] | router <command> [args] | router help\n");
}

int main(int argc, char *argv[]) {
    bool daemon_mode = false, manage_freeswitch = true;
    int first_command = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = true;
        } else if (strcmp(argv[i], "--no-freeswitch") == 0) {
            manage_freeswitch = false;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage();
            return 0;
        } else if (!first_command) {
            first_command = i;
        }
    }

    struct sigaction sa = {0};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    const char *log_file = safe_env("ROUTER_LOG_FILE", "logs/router.log");
    if (strncmp(log_file, "logs/", 5) == 0) mkdir("logs", 0755);
    logger_init(strcmp(log_file, "-") == 0 ? NULL : log_file);  // "-" logs to stderr

    app_config_t *config = config_load(safe_env("ROUTER_CONFIG", "configs/config.json"));
    if (!config) config = config_load(NULL);  // built-in defaults

    database_t *db = db_init(NULL);
    if (!db) {
        fprintf(stderr, "Cannot connect to PostgreSQL. Set ROUTER_DB_CONNECTION or PGHOST/PGUSER/PGDATABASE.\n");
        config_free(config);
        return 1;
    }
    router_t *router = router_create(db, NULL, NULL);
    g_validator = call_validator_create(db, router);
    if (!router || !g_validator) {
        LOG_ERROR("Initialisation failed");
        return 1;
    }

    int rc = 0;
    if (first_command && !daemon_mode) {
        // One-shot command: no background threads to start or wait for.
        rc = cli_execute_command(argc - first_command, &argv[first_command]);
    } else if (daemon_mode) {
        printf("FreeSWITCH router: daemon mode%s\n", manage_freeswitch ? "" : " (FreeSWITCH not managed)");
        call_validator_start(g_validator);
        pthread_t monitor;
        pthread_create(&monitor, NULL, validation_monitor_thread, NULL);

        if (manage_freeswitch && sync_freeswitch() != 0) {
            LOG_ERROR("FreeSWITCH synchronisation failed");
            rc = 1;
            running = false;
        }

        server_t *http = NULL;
        if (running) {
            int port = atoi(safe_env("ROUTER_HTTP_PORT", "0"));
            http = server_create(port ? port : config->server.port, router);
            if (http && server_start(http) != 0) {
                server_destroy(http);
                http = NULL;
            }
            // FreeSWITCH owns the SIP port in B2BUA mode, so the redirect
            // server only starts without FreeSWITCH or when asked for.
            bool redirect = !manage_freeswitch || strcmp(safe_env("ROUTER_SIP_REDIRECT", "0"), "1") == 0;
            if (redirect) {
                g_sip_server = sip_server_create(router, db);
                if (!g_sip_server || sip_server_start(g_sip_server) != 0) {
                    LOG_ERROR("SIP redirect server could not start");
                    rc = 1;
                    running = false;
                }
            }
            if (running) LOG_INFO("Router ready (SIP redirect server %s)", redirect ? "on" : "off");
        }

        while (running) sleep(1);  // SIGINT/SIGTERM clear `running`
        LOG_INFO("Shutting down");

        if (g_sip_server) {
            sip_server_destroy(g_sip_server);  // stops its threads first
            g_sip_server = NULL;
        }
        if (http) server_destroy(http);
        pthread_join(monitor, NULL);
    } else {
        call_validator_start(g_validator);
        printf("FreeSWITCH router CLI. Type 'help' for commands, 'exit' to quit.\n");
        g_cli = cli_create();
        if (g_cli) {
            cli_run(g_cli);
            cli_destroy(g_cli);
        }
    }

    call_validator_destroy(g_validator);  // stops the validation thread if started
    router_destroy(router);
    db_close(db);
    config_free(config);
    logger_close();
    return rc;
}
