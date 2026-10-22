# Audit of the original code

This audit covers `freeswitch-router-main` as received: 192 files, about 8,100 lines of C, built with `Makefile`.

Each finding below was confirmed by reading the code. Most were also reproduced, either against the original binary or in tests. The **Test** column names the test that now fails if the defect comes back.

Severity scale:
- **Critical**: remote and unauthenticated, or loses calls.
- **High**: corrupts data or crashes the process.
- **Medium**: wrong behaviour.
- **Low**: hygiene.

## Reproduced against the original binary

The original daemon received these SIP requests from an origin address:

```text
INVITE with Call-ID: orig-call-1, From: <sip:15550001111@…>, To: <sip:19995550000@…>
-> SIP/2.0 302 Moved Temporarily
   Via: SIP/2.0/UDP 10.0.0.2:5060              (hard-coded, not the caller's Via)
   Call-ID: <sip:19995550000@127.0.0.1>        (the To header, not the Call-ID)
   (no Contact header: a 302 that redirects nowhere)

INVITE without a From header
-> no answer; the process exits with status 139 (segmentation fault)
```

The rows it wrote to `call_records` had `call_id = original_ani = original_dnis = '<sip:19995550000@127.0.0.1>'`.

## Findings

| # | Severity | Finding | Where (original) | Fix | Test |
|---|---|---|---|---|---|
| 1 | Critical | `extract_sip_header` returns one `static` buffer. Call-ID, From and To all point to it, so all three hold the last header read. Every call record, DID mapping and response is wrong | `sip_server_dynamic.c` | `sip_parse.c`: caller-provided buffers, compact forms, header block only | unit `test_headers_do_not_alias`, e2e |
| 2 | Critical | An INVITE without From or To dereferences NULL, so one UDP packet kills the router | `handle_sip_invite` | Mandatory headers checked; 400 / 484 answers | e2e `INVITE without From -> 400`, unit fuzz |
| 3 | Critical | SQL injection from SIP headers: ANI/DNIS/Call-ID are pasted into SQL. This is in the C SIP server, in the validator, and in the Lua handler, which runs for every call | `allocate_did`, `create_call_record`, `route_handler_*.lua` | Bound parameters (`db_query_params`). The Lua handler accepts digits only, quotes values anyway, and SQL re-checks them with `router_is_number` | Lua `SQL injection…`, e2e `SQL in the caller ID -> 484`, SQL test 5 |
| 4 | Critical | Shell injection: the call id from the network goes into `system("fs_cli -x 'uuid_kill %s …'")` | `force_hangup_invalid_call` | Token check, then `posix_spawnp` with an argument vector. No shell anywhere | unit `test_run` |
| 5 | Critical | Two concurrent calls can receive the same DID: `SELECT … ORDER BY RANDOM()` then a separate `UPDATE`. Measured: 16 of 400 calls under 8 clients | Lua handlers, C `allocate_did` | `router_allocate_did`: one transaction, `FOR UPDATE SKIP LOCKED` | SQL test 1, e2e 35 parallel calls, benchmark |
| 6 | Critical | The dialplan's source-address condition uses `break="never"`, so the destination condition runs whatever the source IP. Any host can enter the origin flow | `fs_generate_*_dialplan` | Conditions ANDed (default `break="on-false"`); return extension restricted to numbers | CLI `dialplan ANDs the source address…` |
| 7 | High | Deadlock: the monitor thread holds `did_mutex` and calls `release_did`, which locks it again. This triggers as soon as a DID is older than 5 minutes | `monitor_thread` | DID state moved to SQL; `router_release_stale_dids` | e2e (daemon runs with reloads), SQL test 3 |
| 8 | High | Deadlock: the validation thread holds `validation_mutex` and calls `force_hangup_invalid_call` → `find_validation_record`, which locks it again | `call_validator.c` | Lock-free internal helper; callers hold the lock while they use a record | code review, e2e shutdown |
| 9 | High | Use-after-free: providers, DIDs and routes are freed and reloaded every 30 s while request handlers keep pointers into them (and increment `current_calls` on freed memory) | `load_*_from_db`, `handle_sip_invite` | Arrays swapped under a lock; handlers copy entries | e2e reload test under ASan |
| 10 | High | Use-after-free: `result->num_rows` read after `db_free_result(result)` | `load_routes_from_db`, module2 generator | Count kept before freeing | ASan builds |
| 11 | Medium | The generated profiles look for gateways in `/etc/freeswitch/conf/sip_profiles/<profile>/`, but gateways are written to `/etc/freeswitch/sip_profiles/<profile>/`. Whether a gateway loads (and so whether `bridge` works) depends on which tree FreeSWITCH actually reads | `fs_xml_generator.c` | One `router` profile, with its gateways in `sip_profiles/router/` next to it, under one configurable root | CLI generation checks |
| 12 | High | Every daemon start rewrites a complete FreeSWITCH base configuration (`freeswitch.xml`, `vars.xml`, `modules.conf.xml`, `event_socket.conf.xml`, `db.conf.xml`), with hard-coded IPs (`10.0.0.2`), `sip-trace=yes` and the DB password. Manual changes are silently lost | `fs_generate_main_config` and others | Only router-owned, namespaced files are written | CLI generation checks |
| 13 | High | Two generators write conflicting dialplans: daemon start uses one directory and a Lua handler without DID logic, while `route add` uses another directory and a different handler | `fs_xml_generator.c`, `fs_generate_module2_dialplan.c` | One generator, one tested Lua file installed as is | CLI `installed Lua handler is the tested file` |
| 14 | High | The routing query uses SQLite `GLOB` against PostgreSQL (always an error) and pastes the DNIS into the SQL | `router.c` | PostgreSQL regex with a bound parameter | CLI `route test…` |
| 15 | High | `did add` without a provider binds `provider_id = 0`, which violates the foreign key, so shared-pool DIDs cannot be added | `did_cmd.c` | NULL bound for the shared pool | CLI `did add to the shared pool` |
| 16 | Medium | IP authentication with `strstr`: `10.0.0.1` is accepted for a provider at `10.0.0.10` | `find_provider_by_ip` | Exact list membership | unit `test_ip_list` |
| 17 | Medium | `module2` is unreachable: the dispatcher nests it inside the `validation` branch. It also queries tables and functions that no schema creates | `cli_commands.c`, `module2_cmd.c` | Table-driven dispatcher; module2 rewritten on the real functions | CLI `module2 is reachable` |
| 18 | Medium | The default validation rules are inserted on every start (`ON CONFLICT` without a unique key), adding 5 rows per CLI command | `call_validator_create` | Rules in the schema with `UNIQUE(rule_name)` | CLI `default rules not duplicated` |
| 19 | Medium | `route test` matches with `strstr(pattern, number)` instead of the regex | `route_cmd.c` | PostgreSQL regex | CLI `route test: no false match` |
| 20 | Medium | `route show/delete`, `did list/release` and the `validation` queries build SQL from arguments | `*_cmd.c` | Bound parameters | CLI `route delete is not injectable` |
| 21 | Medium | `validation rules enable` and `did release` report failure on success: `db_query` returns NULL for commands | `validation_cmd.c`, `did_cmd.c` | `RETURNING` | CLI rule and release checks |
| 22 | Medium | SIP responses lack From/To/CSeq and copy a hard-coded Via, so no SIP client can match them. The 302 has no Contact | `create_sip_response` | `sip_build_response` per RFC 3261 §8.2.6 | unit `test_response`, e2e |
| 23 | Medium | The HTTP API answers `{"status":"ok"}` to every URL and method, and listens on 0.0.0.0. The README documents endpoints that do not exist | `handlers.c`, README | Real read-only endpoints, loopback by default | e2e API checks |
| 24 | Medium | On shutdown, the monitor thread can sleep for 300 s and the validator for 5 s before `join` | `main.c`, validator | Short sleep slices | e2e `clean shutdown on SIGTERM` |
| 25 | Medium | `config_load(NULL)` calls `fopen(NULL)` (reported by cppcheck) | `main.c` / `config.c` | NULL handled | cppcheck in CI |
| 26 | Medium | Prepared-statement parameter arrays grow with `realloc` without zeroing, then free garbage pointers (latent: triggers above 10 parameters) | `db_bind_string` | Zeroed growth, errors checked | ASan builds |
| 27 | Low | Password `router123` compiled into the binary, the Lua handlers, the generated `vars.xml`/`db.conf.xml` and 6 scripts | many | Environment variables, 0640 files, `setup-db.sh` refuses the old default | CLI permission check |
| 28 | Low | The original Lua handler holds a DB handle for the whole call (bridge), exhausting the pool under load | Lua handlers | Handle released before bridging | Lua tests (one round trip per leg) |
| 29 | Low | Repository hygiene: committed ELF binaries, `.o` files, `.bak`/`.orig`/`.rej`, logs, 9 schemas, 6 Makefiles, a CMakeLists naming non-existent files, and 8 dead source files | repository | Removed, `.gitignore`, one Makefile | CI |
| 30 | Low | README and DEPLOYMENT.md contradict each other (PostgreSQL vs MySQL/SQLite, port 8080 vs 8083, CMake vs Makefile) | docs | Rewritten from the code | — |

## Kept on purpose

- **The `validation` subsystem.** It is now safe (findings 4, 8, 18, 20, 21), but it relies on an `active_calls` feed that nothing populates. It is listed in the README's known limits rather than removed, because it is part of the original feature set.
- **CLI output formats** are unchanged, except where they printed false information (`sip status`).
