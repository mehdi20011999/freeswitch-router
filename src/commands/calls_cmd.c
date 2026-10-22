#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "commands/calls_cmd.h"
#include "db/database.h"
#include "core/logging.h"

int cmd_calls(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: calls <add|delete|list>\n");
        return -1;
    }
    
    if (strcmp(argv[1], "add") == 0) {
        return cmd_calls_add(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        return cmd_calls_delete(argc, argv);
    } else if (strcmp(argv[1], "list") == 0) {
        return cmd_calls_list(argc, argv);
    }
    
    printf("Unknown calls command: %s\n", argv[1]);
    return -1;
}

int cmd_calls_add(int argc, char *argv[]) {
    if (argc < 5) {
        printf("Usage: calls add <from> <to> <provider>\n");
        return -1;
    }
    
    const char *from = argv[2];
    const char *to = argv[3];
    const char *provider = argv[4];
    
    printf("Simulating call from %s to %s via %s\n", from, to, provider);
    return 0;
}

int cmd_calls_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: calls delete <call_id>\n");
        return -1;
    }
    
    printf("Deleting call %s\n", argv[2]);
    return 0;
}

int cmd_calls_list(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    database_t *db = get_database();
    const char *sql = "SELECT id, ani, dnis, provider, status, created_at "
                     "FROM calls WHERE status = 'active' "
                     "ORDER BY created_at DESC LIMIT 20";
    
    db_result_t *result = db_query(db, sql);
    if (!result) {
        printf("No active calls\n");
        return 0;
    }
    
    printf("\nActive Calls\n");
    printf("============\n");
    printf("%-10s %-15s %-15s %-20s %-10s\n", 
           "ID", "From", "To", "Provider", "Status");
    printf("%-10s %-15s %-15s %-20s %-10s\n", 
           "----------", "---------------", "---------------", 
           "--------------------", "----------");
    
    for (int i = 0; i < result->num_rows; i++) {
        printf("%-10s %-15s %-15s %-20s %-10s\n",
               db_get_value(result, i, 0),
               db_get_value(result, i, 1),
               db_get_value(result, i, 2),
               db_get_value(result, i, 3),
               db_get_value(result, i, 4));
    }
    
    db_free_result(result);
    return 0;
}

// Stub functions
int cmd_calls_show(int argc, char *argv[]) { return cmd_calls_list(argc, argv); }
int cmd_calls_test(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
