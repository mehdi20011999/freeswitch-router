// cli_commands.c - command dispatcher shared by one-shot and interactive modes.
#include <stdio.h>
#include <string.h>
#include "cli/cli.h"
#include "cli/cli_commands.h"
#include "freeswitch/fs_xml_generator.h"

extern cli_t *g_cli;
extern int cmd_provider(int argc, char *argv[]);
extern int cmd_route(int argc, char *argv[]);
extern int cmd_did(int argc, char *argv[]);
extern int cmd_monitor(int argc, char *argv[]);
extern int cmd_stats(int argc, char *argv[]);
extern int cmd_calls(int argc, char *argv[]);
extern int cmd_sip(int argc, char *argv[]);
extern int cmd_validation(int argc, char *argv[]);
extern int cmd_module2(int argc, char *argv[]);

// `freeswitch generate [--no-reload]`: write every router-owned FreeSWITCH file.
static int cmd_freeswitch(int argc, char *argv[]) {
    if (argc < 2 || strcmp(argv[1], "generate") != 0) {
        printf("Usage: freeswitch generate [--no-reload]\n");
        return -1;
    }
    int rc = fs_generate_complete_config();
    printf(rc == 0 ? "FreeSWITCH configuration written\n" : "Some files could not be written, see the log\n");
    if (!(argc > 2 && strcmp(argv[2], "--no-reload") == 0) && fs_reload_config() != 0) {
        printf("Reload failed: is FreeSWITCH running?\n");
        rc = -1;
    }
    return rc;
}

typedef struct {
    const char *name;
    int (*run)(int, char **);
    const char *help;
} command_t;

static const command_t COMMANDS[] = {
    {"provider",   cmd_provider,   "add | delete | list | show | reload   SIP servers S1, S3, S4"},
    {"route",      cmd_route,      "add | delete | list | show | reload | test <number>"},
    {"did",        cmd_did,        "add | delete | list | import | release   the DID pool"},
    {"calls",      cmd_calls,      "list | add | delete"},
    {"module2",    cmd_module2,    "status | test <ani> <dnis> | monitor | validate <did>"},
    {"validation", cmd_validation, "status | stats | rules | events | test"},
    {"stats",      cmd_stats,      "system statistics"},
    {"monitor",    cmd_monitor,    "live view"},
    {"sip",        cmd_sip,        "status   call-path counters"},
    {"freeswitch", cmd_freeswitch, "generate [--no-reload]   write the FreeSWITCH files"},
};

int cli_execute_command(int argc, char *argv[]) {
    if (argc == 0) return 0;
    if (strcmp(argv[0], "help") == 0) {
        printf("\nCommands (S1 -> S2 -> S3 -> S2 -> S4 router):\n\n");
        for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
            printf("  %-11s %s\n", COMMANDS[i].name, COMMANDS[i].help);
        }
        printf("  %-11s %s\n\n", "exit", "leave the interactive CLI");
        printf("Examples:\n");
        printf("  provider add s1 10.0.0.1:5060 origin 100 ip:10.0.0.1\n");
        printf("  route add us_route s1 s3 s4 '1[2-9][0-9]{9}' 100\n");
        printf("  did add 12125551001 US\n");
        printf("  module2 test 15550001111 12125550000\n\n");
        return 0;
    }
    if (strcmp(argv[0], "exit") == 0 || strcmp(argv[0], "quit") == 0) {
        if (g_cli) cli_stop(g_cli);
        return 0;
    }

    // The original dispatcher nested `module2` inside the `validation` branch,
    // so `module2` was unreachable.
    for (size_t i = 0; i < sizeof(COMMANDS) / sizeof(COMMANDS[0]); i++) {
        if (strcmp(argv[0], COMMANDS[i].name) == 0) return COMMANDS[i].run(argc, argv);
    }
    printf("Unknown command: %s (type 'help')\n", argv[0]);
    return -1;
}
