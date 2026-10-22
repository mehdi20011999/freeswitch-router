// src/commands/route_cmd.c
// Complete implementation with enhanced functions properly integrated

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "commands/route_cmd.h"
#include "db/database.h"
#include "core/logging.h"
#include "freeswitch/fs_xml_generator.h"
#include "core/safety.h"

// External function declarations for Module 2

// Forward declarations for enhanced functions
static int cmd_route_add_enhanced(int argc, char *argv[]);
static int cmd_route_delete_enhanced(int argc, char *argv[]);
static int cmd_route_reload_enhanced(int argc, char *argv[]);
static int cmd_route_test_enhanced(int argc, char *argv[]);

static char* generate_uuid(void) {
    uuid_t uuid;
    char *uuid_str = malloc(37);
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);
    return uuid_str;
}

// Main route command dispatcher - USES ENHANCED VERSIONS
int cmd_route(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: route <add|delete|list|show|reload|test> [options]\n");
        printf("\nCommands:\n");
        printf("  add <name> <origin> <intermediate> <final> <pattern> <priority>\n");
        printf("  delete <id|name>  - Delete route and its dialplan\n");
        printf("  list              - List all routes\n");
        printf("  show <id|name>    - Show route details\n");
        printf("  reload            - Regenerate all dialplans\n");
        printf("  test <number>     - Test number against routes\n");
        return -1;
    }
    
    // USE ENHANCED VERSIONS FOR ALL COMMANDS
    if (strcmp(argv[1], "add") == 0) {
        return cmd_route_add_enhanced(argc, argv);  // Enhanced version
    } else if (strcmp(argv[1], "delete") == 0) {
        return cmd_route_delete_enhanced(argc, argv);  // Enhanced version
    } else if (strcmp(argv[1], "list") == 0) {
        return cmd_route_list(argc, argv);
    } else if (strcmp(argv[1], "show") == 0) {
        return cmd_route_show(argc, argv);
    } else if (strcmp(argv[1], "reload") == 0) {
        return cmd_route_reload_enhanced(argc, argv);  // Enhanced version
    } else if (strcmp(argv[1], "test") == 0) {
        return cmd_route_test_enhanced(argc, argv);  // Enhanced version
    }
    
    printf("Unknown route command: %s\n", argv[1]);
    return -1;
}

// Enhanced add command with full Module 2 support
static int cmd_route_add_enhanced(int argc, char *argv[]) {
    if (argc < 8) {
        printf("Usage: route add <name> <origin> <intermediate> <final> <pattern> <priority>\n");
        printf("\nModule 2 Call Flow:\n");
        printf("  S1 (origin) -> S2 (our server) -> S3 (intermediate) -> S2 -> S4 (final)\n");
        printf("\nTransformations:\n");
        printf("  S1->S2: Receive ANI-1/DNIS-1\n");
        printf("  S2->S3: Send DNIS-1(as ANI-2)/DID\n");
        printf("  S3->S2: Return ANI-2/DID\n");
        printf("  S2->S4: Send ANI-1/DNIS-1 (restored)\n");
        printf("\nExample:\n");
        printf("  route add us_route s1 s3 s4 '1[2-9][0-9]{9}' 100\n");
        return -1;
    }
    
    const char *name = argv[2];
    const char *origin_name = argv[3];
    const char *inter_name = argv[4];
    const char *final_name = argv[5];
    const char *pattern = argv[6];
    int priority = atoi(argv[7]);

    if (!safe_is_token(name)) {
        printf("Error: route name may only contain letters, digits, '.', '_' and '-'\n");
        return -1;
    }
    if (!safe_is_route_pattern(pattern)) {
        printf("Error: '%s' is not a valid number pattern\n", pattern);
        printf("Use digits and regex operators only, e.g. '1[2-9][0-9]{9}' or '212[5-7]\\d{8}'\n");
        return -1;
    }
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not initialized\n");
        return -1;
    }
    
    // Validate providers and check they have DIDs
    const char *provider_names[] = {origin_name, inter_name, final_name};
    const char *expected_roles[] = {"origin", "intermediate", "final"};
    int provider_ids[3] = {0, 0, 0};
    int did_counts[3] = {0, 0, 0};
    
    for (int i = 0; i < 3; i++) {
        const char *sql = "SELECT p.id, p.role, COUNT(d.id) as did_count "
                         "FROM providers p "
                         "LEFT JOIN dids d ON (d.provider_id = p.id OR d.provider_id IS NULL) AND d.active = true "
                         "WHERE p.name = ? "
                         "GROUP BY p.id, p.role";
        
        db_stmt_t *stmt = db_prepare(db, sql);
        if (!stmt) continue;
        
        db_bind_string(stmt, 1, provider_names[i]);
        db_result_t *result = db_execute_query(stmt);
        
        if (!result || result->num_rows == 0) {
            printf("Error: Provider '%s' not found\n", provider_names[i]);
            if (result) db_free_result(result);
            db_finalize(stmt);
            return -1;
        }
        
        provider_ids[i] = atoi(db_get_value(result, 0, 0));
        const char *actual_role = db_get_value(result, 0, 1);
        did_counts[i] = atoi(db_get_value(result, 0, 2));
        
        // Check role
        if (strcmp(actual_role, expected_roles[i]) != 0 && strcmp(actual_role, "general") != 0) {
            printf("Warning: Provider '%s' has role '%s', expected '%s'\n",
                   provider_names[i], actual_role, expected_roles[i]);
        }
        
        // Check DIDs for intermediate provider (required for Module 2)
        if (i == 1 && did_counts[i] == 0) {
            printf("Error: no DIDs available for intermediate provider '%s' (own or shared pool)\n", inter_name);
            printf("Please add DIDs first using: did add <number> <country> %s\n", inter_name);
            printf("Or import multiple: did import <csv_file> %s\n", inter_name);
            db_free_result(result);
            db_finalize(stmt);
            return -1;
        }
        
        db_free_result(result);
        db_finalize(stmt);
    }
    
    // Check for duplicate route name
    const char *check_sql = "SELECT id FROM routes WHERE name = ?";
    db_stmt_t *check_stmt = db_prepare(db, check_sql);
    if (check_stmt) {
        db_bind_string(check_stmt, 1, name);
        db_result_t *check_result = db_execute_query(check_stmt);
        if (check_result && check_result->num_rows > 0) {
            printf("Error: Route '%s' already exists\n", name);
            db_free_result(check_result);
            db_finalize(check_stmt);
            return -1;
        }
        if (check_result) db_free_result(check_result);
        db_finalize(check_stmt);
    }
    
    // Generate UUID for the route
    char *uuid = generate_uuid();
    
    // Insert route into database
    const char *sql = "INSERT INTO routes (uuid, name, pattern, origin_provider_id, "
                     "intermediate_provider_id, final_provider_id, priority, active) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, true)";
    
    db_stmt_t *stmt = db_prepare(db, sql);
    if (!stmt) {
        printf("Failed to prepare statement\n");
        free(uuid);
        return -1;
    }
    
    db_bind_string(stmt, 1, uuid);
    db_bind_string(stmt, 2, name);
    db_bind_string(stmt, 3, pattern);
    db_bind_int(stmt, 4, provider_ids[0]);
    db_bind_int(stmt, 5, provider_ids[1]);
    db_bind_int(stmt, 6, provider_ids[2]);
    db_bind_int(stmt, 7, priority);
    
    if (db_execute(stmt) < 0) {
        printf("Failed to add route to database\n");
        db_finalize(stmt);
        free(uuid);
        return -1;
    }
    
    db_finalize(stmt);
    
    printf("Route '%s' added successfully\n", name);
    printf("  UUID: %s\n", uuid);
    printf("  Module 2 Call Flow:\n");
    printf("    S1 (%s) -> S2 (transforms ANI/DNIS)\n", origin_name);
    printf("    S2 -> S3 (%s) with DNIS-1/DID\n", inter_name);
    printf("    S3 -> S2 (returns with ANI-2/DID)\n");
    printf("    S2 -> S4 (%s) with restored ANI-1/DNIS-1\n", final_name);
    printf("  Pattern: %s\n", pattern);
    printf("  Priority: %d\n", priority);
    printf("  Available DIDs in %s: %d\n", inter_name, did_counts[1]);
    
    printf("\nGenerating FreeSWITCH dialplan...\n");
    
    // CALL THE DIALPLAN GENERATION FUNCTION
    if (fs_generate_route_dialplan() == 0) {
        printf("  ✓ Generated route dialplan and installed the Lua handler\n");
        printf("  ✓ Reloading FreeSWITCH...\n");
        fs_reload_config();
        printf("\nRoute is now active and ready to handle Module 2 call flow\n");
    } else {
        printf("  ✗ Failed to generate dialplan\n");
    }
    
    free(uuid);
    return 0;
}

// Enhanced delete command
static int cmd_route_delete_enhanced(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: route delete <id|name>\n");
        return -1;
    }
    
    const char *identifier = argv[2];
    database_t *db = get_database();
    
    // Get route info (by id or by name, as a bound parameter)
    const char *params[] = {identifier};
    db_result_t *result = db_query_params(db,
        "SELECT id, name, uuid FROM routes WHERE id::text = $1 OR name = $1 ORDER BY id LIMIT 1",
        1, params);
    
    if (!result || result->num_rows == 0) {
        printf("Route '%s' not found\n", identifier);
        if (result) db_free_result(result);
        return -1;
    }
    
    const char *route_id = db_get_value(result, 0, 0);
    const char *route_name = db_get_value(result, 0, 1);
    
    printf("Deleting route '%s' (ID: %s)...\n", route_name, route_id);
    
    fs_remove_route_dialplan(route_id);
    printf("  ✓ Removed dialplan file\n");
    
    const char *del_params[] = {route_id};
    db_free_result(db_query_params(db, "DELETE FROM routes WHERE id = $1::int", 1, del_params));
    
    db_free_result(result);
    
    printf("Route '%s' deleted successfully\n", route_name);
    
    // Regenerate dialplans
    printf("Regenerating remaining dialplans...\n");
    fs_generate_route_dialplan();
    fs_reload_config();
    
    return 0;
}

// Enhanced reload command
static int cmd_route_reload_enhanced(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("Reloading all route configurations...\n");
    
    if (fs_generate_route_dialplan() == 0) {
        printf("  ✓ Regenerated route dialplans and the Lua handler\n");
        fs_reload_config();
        printf("  ✓ Reloaded FreeSWITCH\n");
        printf("\nModule 2 route configurations reloaded successfully\n");
    } else {
        printf("  ✗ Failed to reload routes\n");
        return -1;
    }
    
    return 0;
}

// Enhanced test command
static int cmd_route_test_enhanced(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: route test <number>\n");
        printf("Tests which route would handle the given number\n");
        return -1;
    }
    
    const char *test_number = argv[2];
    database_t *db = get_database();
    
    printf("\nTesting number: %s\n", test_number);
    printf("=====================================\n");
    
    if (!safe_is_number(test_number)) {
        printf("'%s' is not a phone number\n", test_number);
        return -1;
    }

    // Highest-priority active route whose pattern matches the whole number,
    // evaluated by PostgreSQL's regex engine with the number bound as a parameter.
    const char *params[] = {test_number};
    db_result_t *result = db_query_params(db,
        "SELECT r.name, r.pattern, r.priority, "
        "p1.name, p2.name, p3.name, "
        "(SELECT COUNT(*) FROM dids d WHERE (d.provider_id = p2.id OR d.provider_id IS NULL) "
        "   AND d.active AND NOT d.in_use) "
        "FROM routes r "
        "JOIN providers p1 ON r.origin_provider_id = p1.id "
        "JOIN providers p2 ON r.intermediate_provider_id = p2.id "
        "JOIN providers p3 ON r.final_provider_id = p3.id "
        "WHERE r.active AND $1 ~ ('^(' || r.pattern || ')$') "
        "ORDER BY r.priority DESC, r.id LIMIT 1",
        1, params);

    int found = result && result->num_rows > 0;
    if (found) {
        printf("Matching route found:\n");
        printf("\nRoute: %s (Priority: %s)\n", db_get_value(result, 0, 0), db_get_value(result, 0, 2));
        printf("  Pattern: %s\n", db_get_value(result, 0, 1));
        printf("  Call flow:\n");
        printf("    1. S1 (%s) sends ANI-1/DNIS-1 to S2\n", db_get_value(result, 0, 3));
        printf("    2. S2 allocates a DID: ANI-2 = DNIS-1, DNIS-2 = DID\n");
        printf("    3. S2 sends to S3 (%s)\n", db_get_value(result, 0, 4));
        printf("    4. S3 calls back S2 on the DID\n");
        printf("    5. S2 releases the DID and restores ANI-1/DNIS-1\n");
        printf("    6. S2 sends to S4 (%s)\n", db_get_value(result, 0, 5));
        printf("  Available DIDs: %s\n", db_get_value(result, 0, 6));
    }
    
    if (!found) {
        printf("No matching route found for %s\n", test_number);
    }
    
    db_free_result(result);
    return 0;
}

// List command
int cmd_route_list(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not initialized\n");
        return -1;
    }
    
    const char *sql = 
        "SELECT r.id, r.name, "
        "p1.name as origin, p2.name as intermediate, p3.name as final, "
        "r.pattern, r.priority, r.active "
        "FROM routes r "
        "LEFT JOIN providers p1 ON r.origin_provider_id = p1.id "
        "LEFT JOIN providers p2 ON r.intermediate_provider_id = p2.id "
        "LEFT JOIN providers p3 ON r.final_provider_id = p3.id "
        "ORDER BY r.priority DESC, r.name";
    
    db_result_t *result = db_query(db, sql);
    if (!result || result->num_rows == 0) {
        printf("No routes configured\n");
        if (result) db_free_result(result);
        return 0;
    }
    
    printf("\nRoutes (S1 → S2 → S3 → S2 → S4 flow)\n");
    printf("=====================================\n");
    printf("%-5s %-20s %-12s %-12s %-12s %-25s %-8s %-8s\n", 
           "ID", "Name", "Origin", "Intermediate", "Final", "Pattern", "Priority", "Status");
    printf("%-5s %-20s %-12s %-12s %-12s %-25s %-8s %-8s\n", 
           "-----", "--------------------", "------------", "------------", 
           "------------", "-------------------------", "--------", "--------");
    
    for (int i = 0; i < result->num_rows; i++) {
        const char *id = db_get_value(result, i, 0);
        const char *name = db_get_value(result, i, 1);
        const char *origin = db_get_value(result, i, 2);
        const char *inter = db_get_value(result, i, 3);
        const char *final = db_get_value(result, i, 4);
        const char *pattern = db_get_value(result, i, 5);
        const char *priority = db_get_value(result, i, 6);
        int active = strcmp(db_get_value(result, i, 7), "t") == 0;
        
        char pattern_display[26];
        if (strlen(pattern) > 25) {
            strncpy(pattern_display, pattern, 22);
            strcpy(pattern_display + 22, "...");
        } else {
            strcpy(pattern_display, pattern);
        }
        
        printf("%-5s %-20s %-12s %-12s %-12s %-25s %-8s %-8s\n",
               id, name, 
               origin ? origin : "-",
               inter ? inter : "-",
               final ? final : "-",
               pattern_display, priority,
               active ? "Active" : "Inactive");
    }
    
    printf("\nTotal routes: %d\n", result->num_rows);
    db_free_result(result);
    
    return 0;
}

// Show command
int cmd_route_show(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: route show <id|name>\n");
        return -1;
    }
    
    const char *identifier = argv[2];
    database_t *db = get_database();
    
    const char *params[] = {identifier};
    db_result_t *result = db_query_params(db,
        "SELECT * FROM routes WHERE id::text = $1 OR name = $1 ORDER BY id LIMIT 1", 1, params);
    
    if (!result || result->num_rows == 0) {
        printf("Route '%s' not found\n", identifier);
        if (result) db_free_result(result);
        return -1;
    }
    
    printf("\nRoute Details\n");
    printf("=============\n");
    
    // Display all fields
    for (int i = 0; i < result->num_cols; i++) {
        printf("%-20s: %s\n", result->columns[i], db_get_value(result, 0, i));
    }
    
    db_free_result(result);
    return 0;
}

// Stub for backward compatibility
int cmd_route_add(int argc, char *argv[]) {
    return cmd_route_add_enhanced(argc, argv);
}

int cmd_route_delete(int argc, char *argv[]) {
    return cmd_route_delete_enhanced(argc, argv);
}

int cmd_route_test(int argc, char *argv[]) {
    return cmd_route_test_enhanced(argc, argv);
}

int cmd_route_reload(int argc, char *argv[]) {
    return cmd_route_reload_enhanced(argc, argv);
}
