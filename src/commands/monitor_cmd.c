#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "commands/monitor_cmd.h"
#include "db/database.h"
#include "core/logging.h"

int cmd_monitor(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: monitor <add|delete|list|show|test>\n");
        return -1;
    }
    
    if (strcmp(argv[1], "show") == 0) {
        return cmd_monitor_show(argc, argv);
    }
    
    printf("Monitor command '%s' not implemented\n", argv[1]);
    return 0;
}

int cmd_monitor_show(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("\nSystem Monitor\n");
    printf("==============\n");
    
    // Show database statistics
    database_t *db = get_database();
    if (db) {
        // Count active calls
        db_result_t *result = db_query(db, "SELECT COUNT(*) FROM calls WHERE status = 'active'");
        int active_calls = 0;
        if (result && result->num_rows > 0) {
            active_calls = atoi(db_get_value(result, 0, 0));
            db_free_result(result);
        }
        
        // Count providers
        result = db_query(db, "SELECT COUNT(*) FROM providers WHERE active");
        int active_providers = 0;
        if (result && result->num_rows > 0) {
            active_providers = atoi(db_get_value(result, 0, 0));
            db_free_result(result);
        }
        
        // Count routes
        result = db_query(db, "SELECT COUNT(*) FROM routes WHERE active");
        int active_routes = 0;
        if (result && result->num_rows > 0) {
            active_routes = atoi(db_get_value(result, 0, 0));
            db_free_result(result);
        }
        
        printf("Router Statistics:\n");
        printf("  Active Providers: %d\n", active_providers);
        printf("  Active Routes:    %d\n", active_routes);
        printf("  Active Calls:     %d\n", active_calls);
    }
    
    // Check if SIP port is listening
    FILE *fp = popen("netstat -tuln 2>/dev/null | grep -q ':5060 ' && echo 'Active' || echo 'Inactive'", "r");
    if (fp) {
        char status[32];
        if (fgets(status, sizeof(status), fp)) {
            status[strcspn(status, "\n")] = 0;
            printf("\nSIP Server Status:\n");
            printf("  Port 5060:       %s\n", status);
        }
        pclose(fp);
    }
    
    // Show recent call activity from database
    if (db) {
        printf("\nRecent Call Activity:\n");
        db_result_t *result = db_query(db, 
            "SELECT datetime(created_at, 'localtime') as time, ani, dnis, provider "
            "FROM calls ORDER BY created_at DESC LIMIT 5");
        
        if (result && result->num_rows > 0) {
            for (int i = 0; i < result->num_rows; i++) {
                printf("  %s: %s -> %s via %s\n",
                    db_get_value(result, i, 0),
                    db_get_value(result, i, 1),
                    db_get_value(result, i, 2),
                    db_get_value(result, i, 3));
            }
            db_free_result(result);
        } else {
            printf("  No recent calls\n");
        }
    }
    
    return 0;
}

// Stub functions
int cmd_monitor_add(int argc, char *argv[]) { 
    (void)argc; (void)argv; 
    printf("Monitor add not implemented\n");
    return 0; 
}

int cmd_monitor_delete(int argc, char *argv[]) { 
    (void)argc; (void)argv; 
    printf("Monitor delete not implemented\n");
    return 0; 
}

int cmd_monitor_list(int argc, char *argv[]) { 
    return cmd_monitor_show(argc, argv); 
}

int cmd_monitor_test(int argc, char *argv[]) { 
    (void)argc; (void)argv; 
    printf("Monitor test not implemented\n");
    return 0; 
}
