// include/commands/provider_cmd.h - Fixed with reload function
#ifndef PROVIDER_CMD_H
#define PROVIDER_CMD_H

int cmd_provider(int argc, char *argv[]);
int cmd_provider_add(int argc, char *argv[]);
int cmd_provider_delete(int argc, char *argv[]);
int cmd_provider_list(int argc, char *argv[]);
int cmd_provider_show(int argc, char *argv[]);
int cmd_provider_test(int argc, char *argv[]);
int cmd_provider_reload(int argc, char *argv[]);  // Add missing declaration

#endif
