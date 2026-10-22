#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>

// FreeSWITCH configuration
typedef struct {
    char *host;
    int port;
    char *password;
    int max_connections;
    int event_threads;
} fs_config_t;

// Database configuration
typedef struct {
    char *path;
    int pool_size;
    int max_connections;
    bool wal_mode;
} db_config_t;

// Cache configuration
typedef struct {
    char *host;
    int port;
    int pool_size;
    int db_index;
    char *password;
    int timeout_ms;
} cache_config_t;

// HTTP Server configuration
typedef struct {
    char *listen_address;
    int port;
    int worker_threads;
    int max_connections;
} server_config_t;

// Router configuration
typedef struct {
    int max_routes;
    int max_providers;
    int route_cache_ttl;
    int stats_interval;
    bool enable_failover;
    int failover_timeout;
} router_config_t;

// Main application configuration
typedef struct {
    fs_config_t freeswitch;
    db_config_t database;
    cache_config_t cache;
    server_config_t server;
    router_config_t router;
    char *log_file;
    char *log_level;
} app_config_t;

// Functions
app_config_t* config_load(const char *filename);
void config_free(app_config_t *config);
void config_print(app_config_t *config);

#endif
