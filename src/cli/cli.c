#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <readline/readline.h>
#include <readline/history.h>
#include "cli/cli.h"
#include "cli/cli_commands.h"
#include "core/logging.h"

cli_t* cli_create(void) {
    cli_t *cli = calloc(1, sizeof(cli_t));
    if (!cli) return NULL;
    
    cli->running = true;
    return cli;
}

void cli_destroy(cli_t *cli) {
    if (cli) {
        free(cli);
    }
}

void cli_run(cli_t *cli) {
    char *line;
    char *argv[MAX_ARGS];
    int argc;
    
    while (cli->running) {
        line = readline("fs-router> ");
        if (!line) break;
        
        if (strlen(line) > 0) {
            add_history(line);
            
            argc = 0;
            char *token = strtok(line, " \t");
            while (token && argc < MAX_ARGS) {
                // Accept shell-style quoting of one word: '1[2-9][0-9]{9}'
                size_t len = strlen(token);
                if (len >= 2 && (token[0] == '\'' || token[0] == '"') && token[len - 1] == token[0]) {
                    token[len - 1] = '\0';
                    token++;
                }
                argv[argc++] = token;
                token = strtok(NULL, " \t");
            }
            
            if (argc > 0) {
                cli_execute_command(argc, argv);
            }
        }
        
        free(line);
    }
}

void cli_stop(cli_t *cli) {
    if (cli) {
        cli->running = false;
    }
}
