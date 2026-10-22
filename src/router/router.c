// router.c - route lookup: which intermediate gateway serves a destination number.
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "core/logging.h"
#include "core/safety.h"
#include "router/router.h"

struct router {
    database_t *db;
    atomic_uint_fast64_t total_calls;
    atomic_uint_fast64_t successful_calls;
    atomic_uint_fast64_t failed_calls;
};

router_t *router_create(database_t *db, void *cache, void *fs_handler) {
    (void)cache;
    (void)fs_handler;
    router_t *router = calloc(1, sizeof(router_t));
    if (!router) return NULL;
    router->db = db;
    return router;
}

void router_destroy(router_t *router) { free(router); }

// The original query used SQLite's GLOB against PostgreSQL (always an error)
// and pasted the DNIS into the SQL text. The pattern match now runs in
// PostgreSQL with the number as a bound parameter.
int router_route_call(router_t *router, route_request_t *request) {
    if (!router || !request || !safe_is_number(request->dnis)) return -1;
    atomic_fetch_add(&router->total_calls, 1);

    const char *params[] = {request->dnis};
    db_result_t *result = db_query_params(router->db,
        "SELECT p.uuid, r.name FROM routes r "
        "JOIN providers p ON p.id = r.intermediate_provider_id AND p.active "
        "WHERE r.active AND $1 ~ ('^(' || r.pattern || ')$') "
        "ORDER BY r.priority DESC, r.id LIMIT 1",
        1, params);

    if (!result || result->num_rows == 0) {
        db_free_result(result);
        atomic_fetch_add(&router->failed_calls, 1);
        LOG_WARN("No route found for %s", request->dnis);
        return -1;
    }
    request->gateway = strdup(db_get_value(result, 0, 0));
    request->route_type = strdup(db_get_value(result, 0, 1));
    db_free_result(result);
    atomic_fetch_add(&router->successful_calls, 1);
    return (request->gateway && request->route_type) ? 0 : -1;
}

int router_get_stats(router_t *router, void *stats) {
    if (!router || !stats) return -1;
    uint64_t *out = stats;
    out[0] = atomic_load(&router->total_calls);
    out[1] = atomic_load(&router->successful_calls);
    out[2] = atomic_load(&router->failed_calls);
    return 0;
}
