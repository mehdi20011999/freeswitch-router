#ifndef CLI_H
#define CLI_H

#include <stdbool.h>

#define MAX_ARGS 32

typedef struct cli {
    bool running;
} cli_t;

cli_t* cli_create(void);
void cli_destroy(cli_t *cli);
void cli_run(cli_t *cli);
void cli_stop(cli_t *cli);
int cli_execute_command(int argc, char *argv[]);

//extern cli_t *g_cli;

#endif
