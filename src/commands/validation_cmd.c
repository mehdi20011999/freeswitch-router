// src/commands/validation_cmd.c
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "commands/validation_cmd.h"
#include "validation/call_validator.h"
#include "db/database.h"
#include "core/logging.h"

extern call_validator_t *g_validator;  // Defined in main.c

int cmd_validation(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: validation <status|stats|rules|events|test>\n");
        printf("\nCommands:\n");
        printf("  status              Show validation system status\n");
        printf("  stats               Show validation statistics\n");
        printf("  rules               Manage validation rules\n");
        printf("  events              Show recent security events\n");
        printf("  test                Test validation with live call data\n");
        return -1;
    }
    
    if (strcmp(argv[1], "status") == 0) {
        return cmd_validation_status(argc, argv);
    } else if (strcmp(argv[1], "stats") == 0) {
        return cmd_validation_stats(argc, argv);
    } else if (strcmp(argv[1], "rules") == 0) {
        return cmd_validation_rules(argc, argv);
    } else if (strcmp(argv[1], "events") == 0) {
        return cmd_validation_events(argc, argv);
    } else if (strcmp(argv[1], "test") == 0) {
        return cmd_validation_test(argc, argv);
    }
    
    printf("Unknown validation command: %s\n", argv[1]);
    return -1;
}

int cmd_validation_status(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    if (!g_validator) {
        printf("Validation system not initialized\n");
        return -1;
    }
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║          Call Validation System Status             ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    // Get validation statistics
    uint64_t total, successful, failed, diverted;
    get_validation_stats(g_validator, &total, &successful, &failed, &diverted);
    
    printf("Overall Statistics:\n");
    printf("  Total Validations:     %lu\n", total);
    printf("  Successful:            %lu (%.1f%%)\n", 
           successful, total > 0 ? (successful * 100.0 / total) : 0);
    printf("  Failed:                %lu (%.1f%%)\n", 
           failed, total > 0 ? (failed * 100.0 / total) : 0);
    printf("  Diverted Calls:        %lu\n", diverted);
    printf("\n");
    
    // Show active validations from database
    const char *active_query = 
        "SELECT call_id, origin_ani, origin_dnis, current_state, "
        "created_at, validation_status "
        "FROM call_validations "
        "WHERE validation_status = 'active' "
        "ORDER BY created_at DESC LIMIT 10";
    
    db_result_t *result = db_query(db, active_query);
    if (result && result->num_rows > 0) {
        printf("Active Validations:\n");
        printf("%-30s %-15s %-15s %-10s %-20s\n",
               "Call ID", "ANI", "DNIS", "State", "Started");
        printf("%-30s %-15s %-15s %-10s %-20s\n",
               "------------------------------", "---------------", 
               "---------------", "----------", "--------------------");
        
        for (int i = 0; i < result->num_rows; i++) {
            const char *state_str[] = {"", "ORIG", "RING", "ANS", "HUNG", "FAIL", "DIV"};
            int state = atoi(db_get_value(result, i, 3));
            
            printf("%-30s %-15s %-15s %-10s %-20s\n",
                   db_get_value(result, i, 0),  // call_id
                   db_get_value(result, i, 1),  // ani
                   db_get_value(result, i, 2),  // dnis
                   state_str[state],             // state
                   db_get_value(result, i, 4));  // created_at
        }
        db_free_result(result);
    } else {
        printf("No active validations\n");
    }
    
    printf("\n");
    
    // Show validation rules status
    const char *rules_query = 
        "SELECT COUNT(*) as total, "
        "SUM(CASE WHEN enabled THEN 1 ELSE 0 END) as enabled "
        "FROM call_validation_rules";
    
    result = db_query(db, rules_query);
    if (result && result->num_rows > 0) {
        printf("Validation Rules:\n");
        printf("  Total Rules:           %s\n", db_get_value(result, 0, 0));
        printf("  Enabled Rules:         %s\n", db_get_value(result, 0, 1));
        db_free_result(result);
    }
    
    return 0;
}

int cmd_validation_stats(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║         Call Validation Statistics                ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    // Hourly statistics
    const char *hourly_query = 
        "SELECT DATE_TRUNC('hour', checked_at) as hour, "
        "COUNT(*) as total, "
        "SUM(CASE WHEN passed THEN 1 ELSE 0 END) as passed, "
        "AVG(CASE WHEN passed THEN 100 ELSE 0 END) as success_rate "
        "FROM validation_checkpoints "
        "WHERE checked_at > NOW() - INTERVAL '24 hours' "
        "GROUP BY DATE_TRUNC('hour', checked_at) "
        "ORDER BY hour DESC LIMIT 24";
    
    db_result_t *result = db_query(db, hourly_query);
    if (result && result->num_rows > 0) {
        printf("Hourly Validation Performance (Last 24 Hours):\n");
        printf("%-20s %-10s %-10s %-15s\n",
               "Hour", "Total", "Passed", "Success Rate");
        printf("%-20s %-10s %-10s %-15s\n",
               "--------------------", "----------", "----------", "---------------");
        
        for (int i = 0; i < result->num_rows && i < 10; i++) {
            printf("%-20s %-10s %-10s %6.1f%%\n",
                   db_get_value(result, i, 0),
                   db_get_value(result, i, 1),
                   db_get_value(result, i, 2),
                   atof(db_get_value(result, i, 3)));
        }
        db_free_result(result);
    }
    
    printf("\n");
    
    // Checkpoint statistics
    const char *checkpoint_query = 
        "SELECT checkpoint_name, COUNT(*) as total, "
        "SUM(CASE WHEN passed THEN 1 ELSE 0 END) as passed, "
        "AVG(CASE WHEN passed THEN 100 ELSE 0 END) as success_rate "
        "FROM validation_checkpoints "
        "WHERE checked_at > NOW() - INTERVAL '1 hour' "
        "GROUP BY checkpoint_name "
        "ORDER BY total DESC";
    
    result = db_query(db, checkpoint_query);
    if (result && result->num_rows > 0) {
        printf("Checkpoint Performance (Last Hour):\n");
        printf("%-30s %-10s %-10s %-15s\n",
               "Checkpoint", "Total", "Passed", "Success Rate");
        printf("%-30s %-10s %-10s %-15s\n",
               "------------------------------", "----------", "----------", "---------------");
        
        for (int i = 0; i < result->num_rows; i++) {
            printf("%-30s %-10s %-10s %6.1f%%\n",
                   db_get_value(result, i, 0),
                   db_get_value(result, i, 1),
                   db_get_value(result, i, 2),
                   atof(db_get_value(result, i, 3)));
        }
        db_free_result(result);
    }
    
    // Failed validation reasons
    const char *failures_query = 
        "SELECT failure_reason, COUNT(*) as count "
        "FROM validation_checkpoints "
        "WHERE passed = false AND failure_reason IS NOT NULL "
        "AND checked_at > NOW() - INTERVAL '1 hour' "
        "GROUP BY failure_reason "
        "ORDER BY count DESC LIMIT 10";
    
    result = db_query(db, failures_query);
    if (result && result->num_rows > 0) {
        printf("\nTop Failure Reasons (Last Hour):\n");
        printf("%-50s %-10s\n", "Reason", "Count");
        printf("%-50s %-10s\n", 
               "--------------------------------------------------", "----------");
        
        for (int i = 0; i < result->num_rows; i++) {
            printf("%-50s %-10s\n",
                   db_get_value(result, i, 0),
                   db_get_value(result, i, 1));
        }
        db_free_result(result);
    }
    
    return 0;
}

int cmd_validation_rules(int argc, char *argv[]) {
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    if (argc < 3) {
        // List all rules
        const char *query = 
            "SELECT id, rule_name, rule_type, priority, enabled "
            "FROM call_validation_rules "
            "ORDER BY priority DESC, rule_name";
        
        db_result_t *result = db_query(db, query);
        if (result && result->num_rows > 0) {
            printf("\nValidation Rules:\n");
            printf("%-5s %-40s %-20s %-10s %-10s\n",
                   "ID", "Rule Name", "Type", "Priority", "Enabled");
            printf("%-5s %-40s %-20s %-10s %-10s\n",
                   "-----", "----------------------------------------",
                   "--------------------", "----------", "----------");
            
            for (int i = 0; i < result->num_rows; i++) {
                printf("%-5s %-40s %-20s %-10s %-10s\n",
                       db_get_value(result, i, 0),
                       db_get_value(result, i, 1),
                       db_get_value(result, i, 2),
                       db_get_value(result, i, 3),
                       strcmp(db_get_value(result, i, 4), "t") == 0 ? "Yes" : "No");
            }
            db_free_result(result);
        } else {
            printf("No validation rules configured\n");
        }
        
        printf("\nUsage: validation rules <enable|disable> <rule_id>\n");
        return 0;
    }
    
    if (strcmp(argv[2], "enable") == 0 && argc > 3) {
        int rule_id = atoi(argv[3]);
        char id_text[16];
        snprintf(id_text, sizeof(id_text), "%d", rule_id);
        const char *params[] = {id_text};
        // RETURNING makes "no such rule" distinguishable from success.
        db_result_t *res = db_query_params(db,
            "UPDATE call_validation_rules SET enabled = true WHERE id = $1::int RETURNING id", 1, params);
        int updated = res != NULL;
        db_free_result(res);
        if (updated) {
            printf("Rule %d enabled\n", rule_id);
        } else {
            printf("Failed to enable rule %d\n", rule_id);
        }
    } else if (strcmp(argv[2], "disable") == 0 && argc > 3) {
        int rule_id = atoi(argv[3]);
        char id_text[16];
        snprintf(id_text, sizeof(id_text), "%d", rule_id);
        const char *params[] = {id_text};
        // RETURNING makes "no such rule" distinguishable from success.
        db_result_t *res = db_query_params(db,
            "UPDATE call_validation_rules SET enabled = false WHERE id = $1::int RETURNING id", 1, params);
        int updated = res != NULL;
        db_free_result(res);
        if (updated) {
            printf("Rule %d disabled\n", rule_id);
        } else {
            printf("Failed to disable rule %d\n", rule_id);
        }
    }
    
    return 0;
}

int cmd_validation_events(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║            Security Events Log                    ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    const char *query = 
        "SELECT event_type, severity, call_id, source_ip, details, created_at "
        "FROM security_events "
        "ORDER BY created_at DESC LIMIT 20";
    
    db_result_t *result = db_query(db, query);
    if (result && result->num_rows > 0) {
        for (int i = 0; i < result->num_rows; i++) {
            const char *severity = db_get_value(result, i, 1);
            
            // Color code by severity
            if (strcmp(severity, "CRITICAL") == 0) {
                printf("\033[1;31m");  // Red
            } else if (strcmp(severity, "HIGH") == 0) {
                printf("\033[1;33m");  // Yellow
            } else {
                printf("\033[0m");     // Normal
            }
            
            printf("[%s] %s - %s\n",
                   db_get_value(result, i, 5),  // created_at
                   db_get_value(result, i, 0),  // event_type
                   severity);
            
            printf("  Call ID: %s\n", db_get_value(result, i, 2));
            
            const char *source_ip = db_get_value(result, i, 3);
            if (source_ip && strlen(source_ip) > 0) {
                printf("  Source: %s\n", source_ip);
            }
            
            printf("  Details: %s\n", db_get_value(result, i, 4));
            printf("\033[0m");  // Reset color
            printf("\n");
        }
        db_free_result(result);
    } else {
        printf("No security events recorded\n");
    }
    
    // Summary statistics
    const char *summary_query = 
        "SELECT severity, COUNT(*) as count "
        "FROM security_events "
        "WHERE created_at > NOW() - INTERVAL '24 hours' "
        "GROUP BY severity";
    
    result = db_query(db, summary_query);
    if (result && result->num_rows > 0) {
        printf("Event Summary (Last 24 Hours):\n");
        for (int i = 0; i < result->num_rows; i++) {
            printf("  %s: %s events\n",
                   db_get_value(result, i, 0),
                   db_get_value(result, i, 1));
        }
        db_free_result(result);
    }
    
    return 0;
}

int cmd_validation_test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    if (!g_validator) {
        printf("Validation system not initialized\n");
        return -1;
    }
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    printf("Testing validation with live data from database...\n\n");
    
    // Get a recent call from the database to test with
    const char *call_query = 
        "SELECT c.call_id, c.ani, c.dnis, p.host as source_ip, "
        "d.did, c.provider "
        "FROM calls c "
        "LEFT JOIN providers p ON p.name = c.provider "
        "LEFT JOIN dids d ON d.active = true AND d.in_use = false "
        "WHERE c.status = 'active' OR c.created_at > NOW() - INTERVAL '1 hour' "
        "ORDER BY c.created_at DESC LIMIT 1";
    
    db_result_t *result = db_query(db, call_query);
    
    if (!result || result->num_rows == 0) {
        printf("No recent calls found in database to test with.\n");
        printf("Generating test call with data from providers and DIDs tables...\n\n");
        
        // Get data from providers and DIDs
        const char *test_query = 
            "SELECT p.host, p.name, d.did "
            "FROM providers p, dids d "
            "WHERE p.active = true AND p.role = 'origin' "
            "AND d.active = true AND d.in_use = false "
            "LIMIT 1";
        
        result = db_query(db, test_query);
        if (!result || result->num_rows == 0) {
            printf("No test data available. Please add providers and DIDs first.\n");
            return -1;
        }
        
        // Generate test call ID
        char test_call_id[256];
        snprintf(test_call_id, sizeof(test_call_id), "test_%ld", time(NULL));
        
        const char *source_ip = db_get_value(result, 0, 0);
        const char *did = db_get_value(result, 0, 2);
        
        // Get sample ANI/DNIS from routes table
        const char *route_query = 
            "SELECT pattern FROM routes WHERE active = true LIMIT 1";
        db_result_t *route_result = db_query(db, route_query);
        
        char test_ani[64] = "15551234567";  // Default
        char test_dnis[64] = "18005551234"; // Default
        
        if (route_result && route_result->num_rows > 0) {
            const char *pattern = db_get_value(route_result, 0, 0);
            // Use pattern to generate test numbers
            if (strstr(pattern, "1[2-9]")) {
                strcpy(test_dnis, "12125551234");
            }
            db_free_result(route_result);
        }
        
        printf("Test Call ID: %s\n", test_call_id);
        printf("Test ANI: %s\n", test_ani);
        printf("Test DNIS: %s\n", test_dnis);
        printf("Source IP: %s\n", source_ip);
        printf("Test DID: %s\n\n", did);
        
        // Test validation stages
        printf("1. Testing call initiation validation...\n");
        validation_result_t val_result = validate_call_initiation(g_validator,
                                                                 test_call_id,
                                                                 test_ani,
                                                                 test_dnis,
                                                                 source_ip);
        
        printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
        
        if (val_result == VALIDATION_SUCCESS) {
            // Test state progression
            printf("2. Testing state progression to RINGING...\n");
            val_result = validate_call_progress(g_validator, test_call_id, 
                                               CALL_STATE_RINGING, source_ip);
            printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
            
            // Test DID assignment
            printf("3. Testing DID assignment...\n");
            val_result = validate_did_assignment(g_validator, test_call_id,
                                                did, source_ip);
            printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
            
            // Get destination from providers for routing test
            const char *dest_query = 
                "SELECT host FROM providers WHERE role = 'final' AND active = true LIMIT 1";
            db_result_t *dest_result = db_query(db, dest_query);
            
            if (dest_result && dest_result->num_rows > 0) {
                const char *dest_ip = db_get_value(dest_result, 0, 0);
                
                printf("4. Testing route validation...\n");
                val_result = validate_call_routing(g_validator, test_call_id,
                                                  dest_ip, dest_ip);
                printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
                
                db_free_result(dest_result);
            }
            
            // Test state progression to ANSWERED
            printf("5. Testing state progression to ANSWERED...\n");
            val_result = validate_call_progress(g_validator, test_call_id,
                                               CALL_STATE_ANSWERED, source_ip);
            printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
            
            // Test final state
            printf("6. Testing state progression to HUNG_UP...\n");
            val_result = validate_call_progress(g_validator, test_call_id,
                                               CALL_STATE_HUNG_UP, source_ip);
            printf("   Result: %s\n", val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
            
            // Clean up test data
            char cleanup_query[512];
            snprintf(cleanup_query, sizeof(cleanup_query),
                    "DELETE FROM call_validations WHERE call_id = '%s'",
                    test_call_id);
            db_query(db, cleanup_query);
            
            snprintf(cleanup_query, sizeof(cleanup_query),
                    "DELETE FROM validation_checkpoints WHERE call_id = '%s'",
                    test_call_id);
            db_query(db, cleanup_query);
        }
        
        db_free_result(result);
    } else {
        // Use actual call data from database
        const char *call_id = db_get_value(result, 0, 0);
        const char *ani = db_get_value(result, 0, 1);
        const char *dnis = db_get_value(result, 0, 2);
        const char *source_ip = db_get_value(result, 0, 3);
        const char *did = db_get_value(result, 0, 4);
        
        printf("Using live call data:\n");
        printf("  Call ID: %s\n", call_id);
        printf("  ANI: %s\n", ani);
        printf("  DNIS: %s\n", dnis);
        printf("  Source: %s\n\n", source_ip);
        
        // Run validation tests with live data
        printf("Running validation tests...\n");
        validation_result_t val_result = validate_call_initiation(g_validator,
                                                                 call_id,
                                                                 ani,
                                                                 dnis,
                                                                 source_ip);
        printf("Call initiation validation: %s\n", 
               val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
        
        if (did && strlen(did) > 0) {
            val_result = validate_did_assignment(g_validator, call_id, did, source_ip);
            printf("DID assignment validation: %s\n", 
                   val_result == VALIDATION_SUCCESS ? "PASSED" : "FAILED");
        }
        
        db_free_result(result);
    }
    
    printf("\nValidation test completed\n");
    
    // Show recent checkpoints for tested calls
    printf("\nRecent validation checkpoints:\n");
    const char *checkpoint_query = 
        "SELECT checkpoint_name, passed, failure_reason "
        "FROM validation_checkpoints "
        "WHERE checked_at > NOW() - INTERVAL '5 minutes' "
        "ORDER BY checked_at DESC LIMIT 10";
    
    result = db_query(db, checkpoint_query);
    if (result && result->num_rows > 0) {
        for (int i = 0; i < result->num_rows; i++) {
            printf("  %s: %s %s\n",
                   db_get_value(result, i, 0),
                   strcmp(db_get_value(result, i, 1), "t") == 0 ? "PASSED" : "FAILED",
                   db_get_value(result, i, 2) ? db_get_value(result, i, 2) : "");
        }
        db_free_result(result);
    }
    
    return 0;
}
