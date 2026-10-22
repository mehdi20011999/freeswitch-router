#ifndef ROUTER_H
#define ROUTER_H

#include <stdint.h>
#include "db/database.h"

typedef struct router router_t;

typedef struct {
    char *call_id;
    char *ani;
    char *dnis;
    char *provider;
    char *gateway;      // Output parameter - gateway to use
    char *route_type;   // Output parameter - type of route
} route_request_t;

router_t* router_create(database_t *db, void *cache, void *fs_handler);
void router_destroy(router_t *router);
int router_route_call(router_t *router, route_request_t *request);
int router_get_stats(router_t *router, void *stats);

#endif
