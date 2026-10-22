#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include "commands/provider_cmd.h"
#include "db/database.h"
#include "core/logging.h"
#include "freeswitch/fs_xml_generator.h"
#include "core/safety.h"

// Generate UUID for provider
static char* generate_provider_uuid() {
    uuid_t uuid;
    char *uuid_str = malloc(37);
    
    uuid_generate(uuid);
    uuid_unparse_lower(uuid, uuid_str);
    
    return uuid_str;
}

int cmd_provider(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: provider <add|delete|list|show|reload>\n");
        return -1;
    }
    
    if (strcmp(argv[1], "add") == 0) {
        return cmd_provider_add(argc, argv);
    } else if (strcmp(argv[1], "delete") == 0) {
        return cmd_provider_delete(argc, argv);
    } else if (strcmp(argv[1], "list") == 0) {
        return cmd_provider_list(argc, argv);
    } else if (strcmp(argv[1], "show") == 0) {
        return cmd_provider_show(argc, argv);
    } else if (strcmp(argv[1], "reload") == 0) {
        return cmd_provider_reload(argc, argv);
    }
    
    printf("Unknown provider command: %s\n", argv[1]);
    return -1;
}

int cmd_provider_add(int argc, char *argv[]) {
    if (argc < 6) {
        printf("Usage: provider add <name> <host:port> <role> <capacity> <auth>\n");
        printf("  role: origin|intermediate|final\n");
        printf("  auth: ip:<allowed_ip> or user:<username>:<password>\n");
        return -1;
    }
    
    const char *name = argv[2];
    char host[256];
    int port = 5060;
    
    // Parse host:port
    char *colon = strchr(argv[3], ':');
    if (colon) {
        size_t host_len = colon - argv[3];
        if (host_len >= sizeof(host)) host_len = sizeof(host) - 1;
        strncpy(host, argv[3], host_len);
        host[host_len] = '\0';
        port = atoi(colon + 1);
    } else {
        strncpy(host, argv[3], sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }
    
    const char *role = argv[4];
    int capacity = atoi(argv[5]);
    
    // Parse auth
    int auth_type = 0;  // 0=IP, 1=User/Pass
    char auth_data[512] = "";
    if (argc > 6) {
        if (strncmp(argv[6], "ip:", 3) == 0) {
            auth_type = 0;
            strncpy(auth_data, argv[6] + 3, sizeof(auth_data) - 1);
        } else if (strncmp(argv[6], "user:", 5) == 0) {
            auth_type = 1;
            strncpy(auth_data, argv[6] + 5, sizeof(auth_data) - 1);
        }
    }
    
    if (!safe_is_token(name) || !safe_is_host(host) || port < 1 || port > 65535) {
        printf("Invalid name, host or port (name: letters, digits, . _ -; host: IP or hostname)\n");
        return -1;
    }

    // Validate role
    if (strcmp(role, "origin") != 0 && 
        strcmp(role, "intermediate") != 0 && 
        strcmp(role, "final") != 0 &&
        strcmp(role, "general") != 0) {
        printf("Invalid role. Must be: origin, intermediate, final, or general\n");
        return -1;
    }
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not initialized\n");
        return -1;
    }
    
    // Generate UUID
    char *uuid = generate_provider_uuid();
    
    // Insert into database
    const char *sql = "INSERT INTO providers (uuid, name, host, port, role, capacity, "
                     "auth_type, auth_data, transport, active) "
                     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, 'udp', true)";
    
    db_stmt_t *stmt = db_prepare(db, sql);
    if (!stmt) {
        free(uuid);
        printf("Failed to prepare statement\n");
        return -1;
    }
    
    db_bind_string(stmt, 1, uuid);
    db_bind_string(stmt, 2, name);
    db_bind_string(stmt, 3, host);
    db_bind_int(stmt, 4, port);
    db_bind_string(stmt, 5, role);
    db_bind_int(stmt, 6, capacity);
    db_bind_string(stmt, 7, auth_type == 0 ? "ip" : "userpass");
    db_bind_string(stmt, 8, auth_data);
    
    if (db_execute(stmt) < 0) {
        printf("Failed to add provider (may already exist)\n");
        db_finalize(stmt);
        free(uuid);
        return -1;
    }
    
    db_finalize(stmt);
    
    // Generate FreeSWITCH configuration
    fs_generate_provider_config(uuid, name, host, port, role, auth_type, auth_data);
    
    printf("Provider '%s' added successfully\n", name);
    printf("  UUID: %s\n", uuid);
    printf("  Role: %s\n", role);
    printf("  Host: %s:%d\n", host, port);
    printf("  Capacity: %d\n", capacity);
    
    free(uuid);
    return 0;
}

int cmd_provider_delete(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: provider delete <name>\n");
        return -1;
    }
    
    const char *name = argv[2];
    database_t *db = get_database();
    
    // Get provider info first
    const char *select_sql = "SELECT uuid, role FROM providers WHERE name = ?";
    db_stmt_t *select_stmt = db_prepare(db, select_sql);
    db_bind_string(select_stmt, 1, name);
    db_result_t *result = db_execute_query(select_stmt);
    
    if (!result || result->num_rows == 0) {
        printf("Provider '%s' not found\n", name);
        if (result) db_free_result(result);
        db_finalize(select_stmt);
        return -1;
    }
    
    const char *uuid = db_get_value(result, 0, 0);
    const char *role = db_get_value(result, 0, 1);
    
    // Remove FreeSWITCH configuration
    fs_remove_provider_config(uuid, role);
    
    db_free_result(result);
    db_finalize(select_stmt);
    
    // Delete from database
    const char *sql = "DELETE FROM providers WHERE name = ?";
    db_stmt_t *stmt = db_prepare(db, sql);
    db_bind_string(stmt, 1, name);
    
    if (db_execute(stmt) < 0) {
        printf("Failed to delete provider\n");
        db_finalize(stmt);
        return -1;
    }
    
    db_finalize(stmt);
    printf("Provider '%s' deleted successfully\n", name);
    
    // Reload FreeSWITCH
    fs_reload_config();
    
    return 0;
}

int cmd_provider_list(int argc, char *argv[]) {
    int verbose = 0;
    if (argc > 2 && strcmp(argv[2], "-v") == 0) {
        verbose = 1;
    }
    
    database_t *db = get_database();
    if (!db) {
        printf("Database not initialized\n");
        return -1;
    }
    
    const char *sql = 
        "SELECT name, uuid, role, host, port, auth_type, capacity, "
        "current_calls, active "
        "FROM providers ORDER BY role, name";
    
    db_result_t *result = db_query(db, sql);
    if (!result || result->num_rows == 0) {
        printf("No providers found\n");
        if (result) db_free_result(result);
        return 0;
    }
    
    printf("\n╔════════════════════════════════════════════════════╗\n");
    printf("║     FreeSWITCH Dynamic Call Router v3.0           ║\n");
    printf("║     S1 → S2 → S3 → S2 → S4 Call Flow              ║\n");
    printf("╚════════════════════════════════════════════════════╝\n\n");
    
    if (!verbose) {
        printf("%-20s %-12s %-20s %-6s %-10s %-8s %-8s %-8s\n", 
               "Name", "Role", "Host", "Port", "Auth", "Capacity", "Calls", "Status");
        printf("%-20s %-12s %-20s %-6s %-10s %-8s %-8s %-8s\n", 
               "--------------------", "------------", "--------------------", 
               "------", "----------", "--------", "--------", "--------");
        
        for (int i = 0; i < result->num_rows; i++) {
            printf("%-20s %-12s %-20s %-6s %-10s %-8s %-8s %-8s\n",
                   db_get_value(result, i, 0),  // name
                   db_get_value(result, i, 2),  // role
                   db_get_value(result, i, 3),  // host
                   db_get_value(result, i, 4),  // port
                   db_get_value(result, i, 5),  // auth_type
                   db_get_value(result, i, 6),  // capacity
                   db_get_value(result, i, 7),  // current_calls
                   strcmp(db_get_value(result, i, 8), "t") == 0 ? "Active" : "Inactive");
        }
    } else {
        printf("Providers (Verbose Mode)\n");
        printf("========================\n\n");
        
        for (int i = 0; i < result->num_rows; i++) {
            printf("Provider: %s\n", db_get_value(result, i, 0));
            printf("  UUID: %s\n", db_get_value(result, i, 1));
            printf("  Role: %s\n", db_get_value(result, i, 2));
            printf("  Host:Port: %s:%s\n", db_get_value(result, i, 3), db_get_value(result, i, 4));
            printf("  Auth Type: %s\n", db_get_value(result, i, 5));
            printf("  Capacity/Current: %s/%s\n", db_get_value(result, i, 6), db_get_value(result, i, 7));
            printf("  Status: %s\n", strcmp(db_get_value(result, i, 8), "t") == 0 ? "Active" : "Inactive");
            printf("\n");
        }
    }
    
    if (!verbose) {
        printf("\nUse './router provider list -v' for detailed view\n");
    }
    
    db_free_result(result);
    return 0;
}

int cmd_provider_show(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: provider show <name>\n");
        return -1;
    }
    
    const char *name = argv[2];
    database_t *db = get_database();
    
    const char *sql = "SELECT * FROM providers WHERE name = ?";
    db_stmt_t *stmt = db_prepare(db, sql);
    db_bind_string(stmt, 1, name);
    db_result_t *result = db_execute_query(stmt);
    
    if (!result || result->num_rows == 0) {
        printf("Provider '%s' not found\n", name);
        if (result) db_free_result(result);
        db_finalize(stmt);
        return -1;
    }
    
    printf("\nProvider Details\n");
    printf("================\n");
    
    for (int i = 0; i < result->num_cols; i++) {
        printf("%-20s: %s\n", result->columns[i], db_get_value(result, 0, i));
    }
    
    db_free_result(result);
    db_finalize(stmt);
    
    return 0;
}

int cmd_provider_reload(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    printf("Reloading all provider configurations...\n");
    
    if (fs_regenerate_all_providers() == 0) {
        printf("  ✓ Regenerated provider configs\n");
        fs_reload_config();
        printf("  ✓ Reloaded FreeSWITCH\n");
        printf("\nProvider configurations reloaded successfully\n");
    } else {
        printf("  ✗ Failed to reload providers\n");
        return -1;
    }
    
    return 0;
}

int cmd_provider_test(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    printf("Provider test not implemented\n");
    return 0;
}
