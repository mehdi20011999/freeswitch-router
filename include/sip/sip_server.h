#ifndef SIP_SERVER_H
#define SIP_SERVER_H
#include <stdint.h>
#include "router/router.h"
#include "db/database.h"

typedef struct sip_server sip_server_t;

typedef struct {
    uint64_t total_invites;
    uint64_t forwarded_calls;
    uint64_t failed_calls;
    int active;
} sip_stats_t;

// SIP server functions
sip_server_t* sip_server_create(router_t *router, database_t *db);
void sip_server_destroy(sip_server_t *server);
int sip_server_start(sip_server_t *server);
void sip_server_stop(sip_server_t *server);
void sip_server_get_stats(sip_server_t *server, sip_stats_t *stats);

#endif // SIP_SERVER_H
