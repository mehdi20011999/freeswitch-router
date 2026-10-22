#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include "router/router.h"

typedef struct server server_t;

server_t* server_create(int port, router_t *router);
void server_destroy(server_t *server);
int server_start(server_t *server);
void server_stop(server_t *server);
bool server_is_running(server_t *server);

#endif
