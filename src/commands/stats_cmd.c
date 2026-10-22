#include <stdio.h>
#include <string.h>
#include "commands/stats_cmd.h"
#include "db/database.h"
#include "core/logging.h"

int cmd_stats(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    database_t *db = get_database();
    
    printf("\nSystem Statistics\n");
    printf("=================\n");
    
    // Provider stats
    db_result_t *result = db_query(db, "SELECT COUNT(*) FROM providers WHERE active");
    if (result) {
        printf("Active Providers: %s\n", db_get_value(result, 0, 0));
        db_free_result(result);
    }
    
    // Route stats
    result = db_query(db, "SELECT COUNT(*) FROM routes");
    if (result) {
        printf("Configured Routes: %s\n", db_get_value(result, 0, 0));
        db_free_result(result);
    }
    
    // DID stats
    result = db_query(db, "SELECT COUNT(*) FROM dids WHERE active");
    if (result) {
        printf("Active DIDs: %s\n", db_get_value(result, 0, 0));
        db_free_result(result);
    }
    
    // Call stats
    result = db_query(db, "SELECT COUNT(*) FROM calls WHERE status = 'active'");
    if (result) {
        printf("Active Calls: %s\n", db_get_value(result, 0, 0));
        db_free_result(result);
    }
    
    return 0;
}

// Stub functions for header compatibility
int cmd_stats_add(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
int cmd_stats_delete(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
int cmd_stats_list(int argc, char *argv[]) { return cmd_stats(argc, argv); }
int cmd_stats_show(int argc, char *argv[]) { return cmd_stats(argc, argv); }
int cmd_stats_test(int argc, char *argv[]) { (void)argc; (void)argv; return 0; }
