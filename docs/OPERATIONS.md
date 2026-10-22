# Operations

This guide covers installing, configuring, running, monitoring and troubleshooting the router on S2. The commands assume Debian or Ubuntu with FreeSWITCH 1.10.

## 1. Install

```bash
sudo apt install build-essential pkg-config libpq-dev libjson-c-dev libmicrohttpd-dev \
                 uuid-dev libreadline-dev postgresql
make && make test
sudo make install          # /usr/local/bin/freeswitch-router
                           # /usr/local/share/freeswitch-router/{route_handler.lua,sql/}
```

## 2. Database

```bash
ROUTER_DB_PASSWORD='a-long-random-secret' tools/setup-db.sh          # role, database, schema
ROUTER_DB_PASSWORD='a-long-random-secret' tools/setup-db.sh --seed   # plus demo data
```

The script is idempotent. To upgrade the schema after pulling a new version, run it again or load `sql/001_schema.sql` and `sql/002_functions.sql`. Neither file ever drops data.

The router connects with `ROUTER_DB_CONNECTION` (a libpq conninfo string). If that is not set, it uses the standard `PGHOST`, `PGDATABASE`, `PGUSER`, `PGPASSWORD` or `~/.pgpass`. No password is compiled in.

## 3. FreeSWITCH

1. **Load the modules.** `mod_lua` runs the handler. `mod_pgsql` provides `pgsql://` handles to Lua: since FreeSWITCH 1.10, PostgreSQL support is a module. In `autoload_configs/modules.conf.xml`:
   ```xml
   <load module="mod_pgsql"/>
   <load module="mod_lua"/>
   ```
2. **Free the SIP port.** The router's sofia profile listens on `ROUTER_FS_SIP_PORT` (default 5060). Either disable FreeSWITCH's default `internal` profile, or pick another port.
3. **Give Lua the DSN.** Set `ROUTER_FS_DB_DSN` when generating. The router then writes `router_vars.xml` with mode 0640. Include it once from `vars.xml`:
   ```xml
   <X-PRE-PROCESS cmd="include" data="router_vars.xml"/>
   ```
   You can also define `router_db_dsn` yourself, for example `pgsql://hostaddr=127.0.0.1 dbname=router_db user=router password=...`.
4. **Generate and load.**
   ```bash
   sudo -u freeswitch env ROUTER_DB_CONNECTION='host=localhost dbname=router_db user=router password=...' \
        ROUTER_FS_DB_DSN='pgsql://hostaddr=127.0.0.1 dbname=router_db user=router password=...' \
        freeswitch-router freeswitch generate
   ```
   This writes the `router` profile, one gateway per provider, the `router` dialplan context, and the Lua handler, then runs `fs_cli -x reloadxml` and `sofia profile router rescan`. After that, `provider add|delete|reload` and `route add|delete|reload` keep the files in sync automatically.

`docs/examples/freeswitch/` shows the exact output for the demo data.

## 4. Run as a service

```bash
sudo install -m644 scripts/freeswitch-router.service /etc/systemd/system/
sudo install -m600 -o freeswitch /dev/null /etc/freeswitch-router.env
sudoedit /etc/freeswitch-router.env      # ROUTER_DB_CONNECTION=..., ROUTER_FS_DB_DSN=..., ...
sudo systemctl enable --now freeswitch-router
journalctl -u freeswitch-router -f
```

The unit runs as the `freeswitch` user, so FreeSWITCH can read the generated files. It applies `ProtectSystem=strict` and only grants write access to the FreeSWITCH configuration and script directories.

## 5. Configuration reference

| Variable | Default | Used by |
|---|---|---|
| `ROUTER_DB_CONNECTION` | standard `PG*` variables | all modes: libpq conninfo |
| `ROUTER_CONFIG` | `configs/config.json` | HTTP port and log settings |
| `ROUTER_LOG_FILE` | `logs/router.log` | log file. `-` logs to stderr (systemd) |
| `ROUTER_HTTP_PORT` / `ROUTER_HTTP_BIND` | 8083 / `127.0.0.1` | HTTP API |
| `ROUTER_SIP_REDIRECT` | `0` | `1` also starts the SIP redirect server when FreeSWITCH is managed |
| `ROUTER_SIP_PORT` / `ROUTER_SIP_BIND` | 5060 / `0.0.0.0` | SIP redirect server |
| `ROUTER_RELOAD_SECONDS` | 30 | how often the redirect server reloads providers and routes and runs the stale-DID sweep |
| `ROUTER_FS_CONF_DIR` | `/etc/freeswitch` | where the generated XML goes |
| `ROUTER_FS_SCRIPTS_DIR` | `/usr/share/freeswitch/scripts` | where `route_handler.lua` is installed |
| `ROUTER_SHARE_DIR` | `/usr/local/share/freeswitch-router` | where the shipped `route_handler.lua` is read from |
| `ROUTER_FS_SIP_IP` / `ROUTER_FS_SIP_PORT` | `$${local_ip_v4}` / 5060 | the `router` sofia profile |
| `ROUTER_FS_DB_DSN` | unset | written to `router_vars.xml` as `router_db_dsn` |
| `ROUTER_FS_CLI` | `fs_cli` | command used to reload FreeSWITCH (`true` in tests) |

## 6. Modes

| Command | What runs |
|---|---|
| `freeswitch-router` | interactive CLI |
| `freeswitch-router <command> …` | one CLI command, then exit |
| `freeswitch-router --daemon` | FreeSWITCH sync and the HTTP API. B2BUA mode: FreeSWITCH carries the calls |
| `freeswitch-router --daemon --no-freeswitch` | SIP redirect server and the HTTP API. FreeSWITCH is not touched |

In both daemon modes, a sweep releases DIDs whose call never came back. The SIP server runs it every `ROUTER_RELOAD_SECONDS`. In pure FreeSWITCH mode, schedule it yourself, for example with cron:

```cron
* * * * *  psql -X -q -c "SELECT router_release_stale_dids(interval '5 minutes')"
```

## 7. Monitoring

```bash
curl -s localhost:8083/health              # 200 {"status":"ok","database":"ok"} / 503
curl -s localhost:8083/api/v1/stats        # providers, routes, DID pool, SIP counters
tools/monitor.sh                           # live view: pool, outcomes, DIDs held, security events
freeswitch-router module2 status           # DID pool and outcomes over the last hour
```

Useful queries:

```sql
-- Pool pressure: alert when free DIDs run low
SELECT count(*) FILTER (WHERE NOT in_use) AS free, count(*) AS total FROM dids WHERE active;

-- Outcomes in the last hour (routing = still on the S3 leg)
SELECT status, count(*) FROM call_records WHERE created_at > now() - interval '1 hour' GROUP BY 1;

-- Possible fraud or misrouting: returns on DIDs that were not issued
SELECT created_at, source_ip, details FROM security_events
WHERE event_type = 'UNKNOWN_RETURN' ORDER BY id DESC LIMIT 20;
```

## 8. Troubleshooting

| Symptom | Likely cause | Check |
|---|---|---|
| Calls from S1 get `404 Not Found` | no route pattern matches, or the source IP is not the origin provider's `host` | `freeswitch-router route test <number>`. Compare `network_addr` in the FreeSWITCH log with `provider list` |
| Calls get congestion or 503 | DID pool exhausted | `module2 status`, `module2 monitor`, `security_events` with `DID_EXHAUSTED` |
| Return calls from S3 rejected | the DID was already freed (stale sweep, replay) or S3 changed the number | `module2 validate <did>`, `security_events` with `UNKNOWN_RETURN` |
| Lua logs `router_db_dsn is not set` | `router_vars.xml` is not included from `vars.xml` | section 3 |
| `bridge` fails with "gateway not found" | profile not rescanned, or the provider is inactive | `fs_cli -x 'sofia status gateway <uuid>'`, then `freeswitch-router provider reload` |
| `Cannot read …/route_handler.lua` | the router was not installed, or `ROUTER_SHARE_DIR` is wrong | `make install`, or point `ROUTER_SHARE_DIR` at `freeswitch/scripts` |

## 9. Development

```bash
make asan                                  # ./router-asan
createdb router_test                       # throwaway: the suites rebuild it
PGDATABASE=router_test make test-db test-e2e
PGDATABASE=router_bench make bench
make lint
```
