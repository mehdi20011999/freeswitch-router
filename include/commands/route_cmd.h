#ifndef ROUTE_CMD_H
#define ROUTE_CMD_H

int cmd_route(int argc, char *argv[]);
int cmd_route_add(int argc, char *argv[]);
int cmd_route_delete(int argc, char *argv[]);
int cmd_route_list(int argc, char *argv[]);
int cmd_route_show(int argc, char *argv[]);
int cmd_route_test(int argc, char *argv[]);
int cmd_route_reload(int argc, char *argv[]);  // Added this

#endif
