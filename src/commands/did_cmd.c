// src/commands/did_cmd.c
// Complete DID Management with Module 2 Support

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "commands/did_cmd.h"
#include "db/database.h"
#include "core/logging.h"

// Forward declarations for internal functions
static int cmd_did_import_csv(int argc, char *argv[]);
static int cmd_did_list_enhanced(int argc, char *argv[]);

// ============================================================================
// Basic DID Management Functions
// ============================================================================

int cmd_did_add(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: did add <number> <country> [provider]\n");
        printf("Example: did add 18005551234 US provider1\n");
        return -1;
    }
    
    const char *number = argv[2];
    const char *country = argv[3];
    const char *provider = (argc > 4) ? argv[4] : NULL;
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    int provider_id = 0;
    if (provider) {
        const char *sql = "SELECT id FROM providers WHERE name = ?";
        db_stmt_t *stmt = db_prepare(db, sql);
        db_bind_string(stmt, 1, provider);
        db_result_t *result = db_execute_query(stmt);
        
        if (!result || result->num_rows == 0) {
            printf("Provider '%s' not found\n", provider);
            if (result) db_free_result(result);
            db_finalize(stmt);
            return -1;
        }
        
        provider_id = atoi(db_get_value(result, 0, 0));
        db_free_result(result);
        db_finalize(stmt);
    }
    
    // Add DID to database
    const char *sql = "INSERT INTO dids (did, country, provider_id, in_use, active) "
                     "VALUES (?, ?, ?, false, true)";
    db_stmt_t *stmt = db_prepare(db, sql);
    db_bind_string(stmt, 1, number);
    db_bind_string(stmt, 2, country);
    if (provider_id > 0) {
        db_bind_int(stmt, 3, provider_id);
    } else {
        db_bind_string(stmt, 3, NULL);  // shared pool: NULL, not 0 (0 violates the foreign key)
    }
    
    if (db_execute(stmt) < 0) {
        printf("Failed to add DID (may already exist)\n");
        db_finalize(stmt);
        return -1;
    }
    
    db_finalize(stmt);
    printf("DID '%s' added successfully (Country: %s)\n", number, country);
    return 0;
}

int cmd_did_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: did delete <number>\n");
        return -1;
    }
    
    const char *number = argv[2];
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    const char *sql = "DELETE FROM dids WHERE did = ?";
    db_stmt_t *stmt = db_prepare(db, sql);
    db_bind_string(stmt, 1, number);
    
    if (db_execute(stmt) < 0) {
        printf("Failed to delete DID\n");
        db_finalize(stmt);
        return -1;
    }
    
    db_finalize(stmt);
    printf("DID '%s' deleted successfully\n", number);
    return 0;
}

int cmd_did_list(int argc, char *argv[]) {
    // Use enhanced listing
    return cmd_did_list_enhanced(argc, argv);
}

int cmd_did_show(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: did show <number>\n");
        return -1;
    }
    
    const char *number = argv[2];
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    const char *sql = "SELECT d.*, p.name as provider_name "
                     "FROM dids d "
                     "LEFT JOIN providers p ON d.provider_id = p.id "
                     "WHERE d.did = ?";
    
    db_stmt_t *stmt = db_prepare(db, sql);
    db_bind_string(stmt, 1, number);
    db_result_t *result = db_execute_query(stmt);
    
    if (!result || result->num_rows == 0) {
        printf("DID '%s' not found\n", number);
        if (result) db_free_result(result);
        db_finalize(stmt);
        return -1;
    }
    
    printf("\nDID Details\n");
    printf("===========\n");
    for (int i = 0; i < result->num_cols; i++) {
        printf("%-20s: %s\n", result->columns[i], db_get_value(result, 0, i));
    }
    
    db_free_result(result);
    db_finalize(stmt);
    return 0;
}

int cmd_did_test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("DID test function - not implemented\n");
    return 0;
}

// ============================================================================
// Module 2 Enhanced Functions
// ============================================================================

static int cmd_did_import_csv(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: did import <csv_file> <provider_name> [country]\n");
        printf("CSV Format: One DID number per line\n");
        printf("Example: did import dids.csv provider1 US\n");
        return -1;
    }
    
    const char *csv_file = argv[2];
    const char *provider_name = argv[3];
    const char *country = (argc > 4) ? argv[4] : "US";
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    // Get provider ID
    const char *provider_query = "SELECT id FROM providers WHERE name = ?";
    db_stmt_t *stmt = db_prepare(db, provider_query);
    db_bind_string(stmt, 1, provider_name);
    db_result_t *result = db_execute_query(stmt);
    
    if (!result || result->num_rows == 0) {
        printf("Provider '%s' not found\n", provider_name);
        printf("Please add the provider first:\n");
        printf("  provider add %s <host:port> <role> <capacity> <auth>\n", provider_name);
        if (result) db_free_result(result);
        db_finalize(stmt);
        return -1;
    }
    
    int provider_id = atoi(db_get_value(result, 0, 0));
    db_free_result(result);
    db_finalize(stmt);
    
    // Open CSV file
    FILE *fp = fopen(csv_file, "r");
    if (!fp) {
        printf("Failed to open CSV file: %s\n", csv_file);
        return -1;
    }
    
    char line[256];
    int imported = 0;
    int failed = 0;
    int skipped = 0;
    
    printf("Importing DIDs from %s...\n", csv_file);
    
    // Begin transaction for better performance
    db_query(db, "BEGIN TRANSACTION");
    
    while (fgets(line, sizeof(line), fp)) {
        // Remove newline and whitespace
        char *did = line;
        while (*did && (*did == ' ' || *did == '\t')) did++;
        char *end = did + strlen(did) - 1;
        while (end > did && (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
            *end = '\0';
            end--;
        }
        
        // Skip empty lines
        if (strlen(did) == 0) {
            continue;
        }
        
        // Skip comment lines
        if (did[0] == '#' || did[0] == '/') {
            skipped++;
            continue;
        }
        
        // Validate DID format (basic validation - digits only, 10-15 chars)
        int valid = 1;
        for (char *p = did; *p; p++) {
            if (!isdigit(*p)) {
                valid = 0;
                break;
            }
        }
        
        if (!valid || strlen(did) < 10 || strlen(did) > 15) {
            printf("  ✗ Invalid DID format: %s\n", did);
            failed++;
            continue;
        }
        
        // Insert DID into database
        const char *insert_query = 
            "INSERT INTO dids (did, country, provider_id, in_use, active) "
            "VALUES (?, ?, ?, false, true) "
            "ON CONFLICT (did) DO NOTHING";
        
        stmt = db_prepare(db, insert_query);
        db_bind_string(stmt, 1, did);
        db_bind_string(stmt, 2, country);
        if (provider_id > 0) {
            db_bind_int(stmt, 3, provider_id);
        } else {
            db_bind_string(stmt, 3, NULL);
        }
        
        if (db_execute(stmt) == 0) {
            imported++;
            printf("  ✓ Imported: %s\n", did);
        } else {
            failed++;
            printf("  ✗ Failed or duplicate: %s\n", did);
        }
        
        db_finalize(stmt);
    }
    
    // Commit transaction
    db_query(db, "COMMIT");
    
    fclose(fp);
    
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║                 Import Summary                     ║\n");
    printf("╚════════════════════════════════════════════════════╝\n");
    printf("  Successfully imported: %d DIDs\n", imported);
    printf("  Failed/Duplicates: %d\n", failed);
    printf("  Skipped (comments/empty): %d\n", skipped);
    printf("  Provider: %s (ID: %d)\n", provider_name, provider_id);
    printf("  Country: %s\n", country);
    
    return 0;
}

static int cmd_did_list_enhanced(int argc, char *argv[]) {
    database_t *db = get_database();
    if (!db) {
        printf("Database not available\n");
        return -1;
    }
    
    // Parse filter options
    const char *filter_provider = NULL;
    const char *filter_status = NULL;
    
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc) {
            filter_provider = argv[++i];
        } else if (strcmp(argv[i], "--status") == 0 && i + 1 < argc) {
            filter_status = argv[++i];
        }
    }
    
    // Build query with filters
    char query[1024];
    snprintf(query, sizeof(query),
        "SELECT d.did, d.country, d.in_use, d.destination, "
        "p.name as provider_name, d.call_id, d.original_ani, "
        "d.allocated_at "
        "FROM dids d "
        "LEFT JOIN providers p ON d.provider_id = p.id "
        "WHERE d.active = true");
    
    if (filter_provider) {
        strcat(query, " AND p.name = $1");  // bound below, never pasted into the SQL
    }
    
    if (filter_status) {
        if (strcmp(filter_status, "in_use") == 0) {
            strcat(query, " AND d.in_use = true");
        } else if (strcmp(filter_status, "available") == 0) {
            strcat(query, " AND d.in_use = false");
        }
    }
    
    strcat(query, " ORDER BY p.name, d.country, d.did");
    
    const char *params[] = {filter_provider};
    db_result_t *result = filter_provider ? db_query_params(db, query, 1, params) : db_query(db, query);
    if (!result || result->num_rows == 0) {
        printf("No DIDs found\n");
        if (result) db_free_result(result);
        return 0;
    }
    
    printf("\n╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║                          DID Pool Management                               ║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n\n");
    
    printf("%-20s %-10s %-15s %-10s %-20s %-30s\n",
           "DID", "Country", "Provider", "Status", "Destination", "Call Info");
    printf("%-20s %-10s %-15s %-10s %-20s %-30s\n",
           "--------------------", "----------", "---------------", "----------",
           "--------------------", "------------------------------");
    
    int total = 0, in_use = 0, available = 0;
    char current_provider[256] = "";
    
    for (int i = 0; i < result->num_rows; i++) {
        const char *did = db_get_value(result, i, 0);
        const char *country = db_get_value(result, i, 1);
        int is_in_use = strcmp(db_get_value(result, i, 2), "t") == 0;
        const char *destination = db_get_value(result, i, 3);
        const char *provider = db_get_value(result, i, 4);
        const char *call_id = db_get_value(result, i, 5);
        const char *original_ani = db_get_value(result, i, 6);
        
        // Group by provider
        if (provider && strcmp(current_provider, provider) != 0) {
            if (strlen(current_provider) > 0) {
                printf("\n");
            }
            strcpy(current_provider, provider);
        }
        
        char call_info[64] = "-";
        if (is_in_use && call_id && strlen(call_id) > 0) {
            snprintf(call_info, sizeof(call_info), "%.15s... ANI:%.10s",
                    call_id, original_ani ? original_ani : "N/A");
        }
        
        printf("%-20s %-10s %-15s %-10s %-20s %-30s\n",
               did,
               country ? country : "-",
               provider ? provider : "Unassigned",
               is_in_use ? "IN USE" : "Available",
               (destination && strlen(destination) > 0) ? destination : "-",
               call_info);
        
        total++;
        if (is_in_use) in_use++; else available++;
    }
    
    printf("\n");
    printf("╔════════════════════════════════════════════════════════════════════════════╗\n");
    printf("║ Summary: Total: %d | In Use: %d | Available: %d", total, in_use, available);
    // Pad to fill the box
    int padding = 77 - 30 - snprintf(NULL, 0, "%d | In Use: %d | Available: %d", total, in_use, available);
    for (int i = 0; i < padding; i++) printf(" ");
    printf("║\n");
    printf("╚════════════════════════════════════════════════════════════════════════════╝\n");
    
    // Show DIDs by provider summary
    printf("\nDIDs by Provider:\n");
    snprintf(query, sizeof(query),
        "SELECT p.name, COUNT(d.id) as total, "
        "SUM(CASE WHEN d.in_use THEN 1 ELSE 0 END) as in_use "
        "FROM providers p "
        "LEFT JOIN dids d ON p.id = d.provider_id AND d.active = true "
        "WHERE p.role = 'intermediate' "
        "GROUP BY p.name "
        "ORDER BY p.name");
    
    db_result_t *summary = db_query(db, query);
    if (summary && summary->num_rows > 0) {
        for (int i = 0; i < summary->num_rows; i++) {
            printf("  %s: %s DIDs (%s in use)\n",
                   db_get_value(summary, i, 0),
                   db_get_value(summary, i, 1),
                   db_get_value(summary, i, 2));
        }
        db_free_result(summary);
    }
    
    db_free_result(result);
    return 0;
}

// ============================================================================
// Main Command Handler - EXPORTED (NOT STATIC!)
// ============================================================================

int cmd_did(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: did <command> [options]\n");
        printf("\nCommands:\n");
        printf("  add <number> <country> [provider]     - Add single DID\n");
        printf("  import <csv_file> <provider> [country] - Import DIDs from CSV\n");
        printf("  list [--provider NAME] [--status in_use|available]\n");
        printf("  show <number>                          - Show DID details\n");
        printf("  delete <number>                        - Delete DID\n");
        printf("  release <number>                       - Release DID from active call\n");
        printf("\nModule 2 DID Management:\n");
        printf("  DIDs are used for dynamic call routing in S1→S2→S3→S2→S4 flow\n");
        printf("  DIDs must be assigned to intermediate providers (S3)\n");
        printf("\nCSV Format for import:\n");
        printf("  One DID number per line (10-15 digits)\n");
        printf("  Lines starting with # are treated as comments\n");
        printf("\nExamples:\n");
        printf("  did add 18005551234 US s3\n");
        printf("  did import dids.csv s3 US\n");
        printf("  did list --provider s3 --status available\n");
        return -1;
    }
    
    const char *command = argv[1];
    
    if (strcmp(command, "add") == 0) {
        return cmd_did_add(argc, argv);
    } else if (strcmp(command, "delete") == 0) {
        return cmd_did_delete(argc, argv);
    } else if (strcmp(command, "list") == 0) {
        return cmd_did_list(argc, argv);
    } else if (strcmp(command, "show") == 0) {
        return cmd_did_show(argc, argv);
    } else if (strcmp(command, "test") == 0) {
        return cmd_did_test(argc, argv);
    } else if (strcmp(command, "import") == 0) {
        return cmd_did_import_csv(argc, argv);
    } else if (strcmp(command, "release") == 0) {
        if (argc < 3) {
            printf("Usage: did release <number>\n");
            return -1;
        }
        
        const char *did_number = argv[2];
        database_t *db = get_database();
        if (!db) {
            printf("Database not available\n");
            return -1;
        }
        
        // Free the DID and close the call that held it, in one statement.
        const char *params[] = {did_number};
        db_result_t *result = db_query_params(db,
            "WITH freed AS ("
            "  UPDATE dids d SET in_use = false, destination = NULL, original_ani = NULL, "
            "         allocated_at = NULL, released_at = now(), call_id = NULL "
            "  FROM (SELECT id, call_id FROM dids WHERE did = $1 AND in_use FOR UPDATE) old "
            "  WHERE d.id = old.id RETURNING old.call_id), "
            "closed AS ("
            "  UPDATE call_records cr SET status = 'failed', failure_reason = 'Released by operator', "
            "         ended_at = now(), updated_at = now() "
            "  FROM freed WHERE cr.call_id = freed.call_id AND cr.status IN ('routing', 'forwarding') "
            "  RETURNING 1) "
            "SELECT call_id FROM freed", 1, params);
        if (result) {
            printf("DID %s released\n", did_number);
            db_free_result(result);
        } else {
            printf("DID %s is not in use (or does not exist)\n", did_number);
        }
        return 0;
    }
    
    printf("Unknown did command: %s\n", command);
    printf("Type 'did' for usage information\n");
    return -1;
}

