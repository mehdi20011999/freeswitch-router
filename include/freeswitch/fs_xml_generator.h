// fs_xml_generator.h - FreeSWITCH configuration written from the database.
// See src/freeswitch/fs_xml_generator.c for the files produced and the
// environment variables that control paths.
#ifndef FS_XML_GENERATOR_H
#define FS_XML_GENERATOR_H

// All functions return 0 on success, -1 if anything could not be written.
int fs_init_all_directories(void);
int fs_generate_main_config(void);          // no-op, kept for compatibility
int fs_generate_sip_profiles(void);         // sofia profile "router"
int fs_generate_dialplan_contexts(void);    // context "router"
int fs_generate_autoload_configs(void);     // router_vars.xml (router_db_dsn)

int fs_generate_provider_config(const char *uuid, const char *name, const char *host, int port,
                                const char *role, int auth_type, const char *auth_data);
int fs_remove_provider_config(const char *uuid, const char *role);
int fs_regenerate_all_providers(void);      // also removes gateways of deleted providers

int fs_generate_route_dialplan(void);       // all routes + installs the Lua handler
int fs_remove_route_dialplan(const char *route_id);
int fs_generate_route_handler_lua(void);    // copies freeswitch/scripts/route_handler.lua

int fs_reload_config(void);                 // fs_cli reloadxml + profile rescan
int fs_generate_complete_config(void);      // everything above except reload

#endif
