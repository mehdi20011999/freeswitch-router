#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <json-c/json.h>
#include "core/config.h"
#include "core/logging.h"

static char* json_get_string(struct json_object *obj, const char *key, const char *default_val) {
    struct json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return strdup(json_object_get_string(val));
    }
    return strdup(default_val);
}

static int json_get_int(struct json_object *obj, const char *key, int default_val) {
    struct json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return json_object_get_int(val);
    }
    return default_val;
}

static bool json_get_bool(struct json_object *obj, const char *key, bool default_val) {
    struct json_object *val;
    if (json_object_object_get_ex(obj, key, &val)) {
        return json_object_get_boolean(val);
    }
    return default_val;
}

app_config_t* config_load(const char *filename) {
    FILE *fp = filename ? fopen(filename, "r") : NULL;
    if (!fp) {
        if (filename) LOG_WARN("Config file %s not found, using defaults", filename);
        
        // Return default configuration
        app_config_t *config = calloc(1, sizeof(app_config_t));
        if (!config) return NULL;
        
        // FreeSWITCH defaults
        config->freeswitch.host = strdup("localhost");
        config->freeswitch.port = 8021;
        config->freeswitch.password = strdup("");  // ESL is not used; never ship a default secret
        config->freeswitch.max_connections = 10;
        config->freeswitch.event_threads = 4;
        
        // Database defaults - optimized for high load
        config->database.path = strdup("router.db");
        config->database.pool_size = 50;
        config->database.max_connections = 100;
        config->database.wal_mode = true;
        
        // Cache defaults
        config->cache.host = strdup("localhost");
        config->cache.port = 6379;
        config->cache.pool_size = 20;
        config->cache.db_index = 0;
        config->cache.password = strdup("");
        config->cache.timeout_ms = 1000;
        
        // Server defaults
        config->server.listen_address = strdup("127.0.0.1");
        config->server.port = 8083;
        config->server.worker_threads = 8;
        config->server.max_connections = 1000;
        
        // Router defaults - optimized for thousands of routes
        config->router.max_routes = 10000;
        config->router.max_providers = 1000;
        config->router.route_cache_ttl = 300;
        config->router.stats_interval = 60;
        config->router.enable_failover = true;
        config->router.failover_timeout = 5;
        
        // Logging defaults
        config->log_file = strdup("logs/router.log");
        config->log_level = strdup("INFO");
        
        return config;
    }
    
    // Read file contents
    fseek(fp, 0, SEEK_END);
    long length = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *data = length >= 0 ? malloc((size_t)length + 1) : NULL;
    size_t got = data ? fread(data, 1, (size_t)length, fp) : 0;
    fclose(fp);
    if (!data || got != (size_t)length) {
        free(data);
        return config_load(NULL);  // unreadable file: built-in defaults
    }
    data[length] = '\0';
    
    // Parse JSON
    struct json_object *root = json_tokener_parse(data);
    free(data);
    
    if (!root) {
        LOG_ERROR("Failed to parse config file");
        return NULL;
    }
    
    app_config_t *config = calloc(1, sizeof(app_config_t));
    if (!config) {
        json_object_put(root);
        return NULL;
    }
    
    // Parse FreeSWITCH section
    struct json_object *fs_obj;
    if (json_object_object_get_ex(root, "freeswitch", &fs_obj)) {
        config->freeswitch.host = json_get_string(fs_obj, "host", "localhost");
        config->freeswitch.port = json_get_int(fs_obj, "port", 8021);
        config->freeswitch.password = json_get_string(fs_obj, "password", "");
        config->freeswitch.max_connections = json_get_int(fs_obj, "max_connections", 10);
        config->freeswitch.event_threads = json_get_int(fs_obj, "event_threads", 4);
    }
    
    // Parse Database section
    struct json_object *db_obj;
    if (json_object_object_get_ex(root, "database", &db_obj)) {
        config->database.path = json_get_string(db_obj, "path", "router.db");
        config->database.pool_size = json_get_int(db_obj, "pool_size", 50);
        config->database.max_connections = json_get_int(db_obj, "max_connections", 100);
        config->database.wal_mode = json_get_bool(db_obj, "wal_mode", true);
    }
    
    // Parse Cache section
    struct json_object *cache_obj;
    if (json_object_object_get_ex(root, "cache", &cache_obj)) {
        config->cache.host = json_get_string(cache_obj, "host", "localhost");
        config->cache.port = json_get_int(cache_obj, "port", 6379);
        config->cache.pool_size = json_get_int(cache_obj, "pool_size", 20);
        config->cache.timeout_ms = json_get_int(cache_obj, "timeout_ms", 1000);
    }
    
    // Parse Server section
    struct json_object *server_obj;
    if (json_object_object_get_ex(root, "server", &server_obj)) {
        config->server.listen_address = json_get_string(server_obj, "listen_address", "0.0.0.0");
        config->server.port = json_get_int(server_obj, "port", 8083);
        config->server.worker_threads = json_get_int(server_obj, "worker_threads", 8);
        config->server.max_connections = json_get_int(server_obj, "max_connections", 1000);
    }
    
    // Parse Router section
    struct json_object *router_obj;
    if (json_object_object_get_ex(root, "router", &router_obj)) {
        config->router.max_routes = json_get_int(router_obj, "max_routes", 10000);
        config->router.max_providers = json_get_int(router_obj, "max_providers", 1000);
        config->router.route_cache_ttl = json_get_int(router_obj, "route_cache_ttl", 300);
        config->router.enable_failover = json_get_bool(router_obj, "enable_failover", true);
    }
    
    // Logging
    config->log_file = json_get_string(root, "log_file", "logs/router.log");
    config->log_level = json_get_string(root, "log_level", "INFO");
    
    json_object_put(root);
    
    LOG_INFO("Configuration loaded successfully");
    return config;
}

void config_free(app_config_t *config) {
    if (!config) return;
    
    free(config->freeswitch.host);
    free(config->freeswitch.password);
    free(config->database.path);
    free(config->cache.host);
    free(config->cache.password);
    free(config->server.listen_address);
    free(config->log_file);
    free(config->log_level);
    free(config);
}

void config_print(app_config_t *config) {
    if (!config) return;
    
    printf("Configuration:\n");
    printf("  FreeSWITCH:\n");
    printf("    Host: %s:%d\n", config->freeswitch.host, config->freeswitch.port);
    printf("    Max Connections: %d\n", config->freeswitch.max_connections);
    printf("  Database:\n");
    printf("    Path: %s\n", config->database.path);
    printf("    Pool Size: %d\n", config->database.pool_size);
    printf("  Server:\n");
    printf("    Listen: %s:%d\n", config->server.listen_address, config->server.port);
    printf("  Router:\n");
    printf("    Max Routes: %d\n", config->router.max_routes);
    printf("    Max Providers: %d\n", config->router.max_providers);
}
