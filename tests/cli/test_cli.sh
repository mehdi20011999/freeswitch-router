#!/usr/bin/env bash
# CLI and FreeSWITCH-generation tests. Runs the real binary against PostgreSQL.
#
# Usage: tests/cli/test_cli.sh ./router
# DESTRUCTIVE: rebuilds the schema of PGDATABASE. Requires ROUTER_TEST_DB=1.
set -euo pipefail

BIN=$(realpath "${1:-./router}")
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
[[ ${ROUTER_TEST_DB:-} == 1 ]] || { echo "refusing to run: set ROUTER_TEST_DB=1 (throwaway database)"; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
export ROUTER_LOG_FILE=$WORK/router.log
export ROUTER_FS_CONF_DIR=$WORK/fs ROUTER_FS_SCRIPTS_DIR=$WORK/scripts ROUTER_SHARE_DIR=$ROOT/freeswitch/scripts
export ROUTER_FS_CLI=true   # pretend fs_cli succeeded
export ROUTER_FS_DB_DSN='pgsql://hostaddr=127.0.0.1 dbname=router_db user=router password=CHANGE_ME'

PSQL=(psql -X -q -At -v ON_ERROR_STOP=1)
"${PSQL[@]}" -c "DROP SCHEMA public CASCADE; CREATE SCHEMA public;" 2>/dev/null
"${PSQL[@]}" -f "$ROOT/sql/001_schema.sql" -f "$ROOT/sql/002_functions.sql" -f "$ROOT/sql/seed_demo.sql"

pass=0
fail() { echo "FAIL $*"; exit 1; }
ok() { echo "ok   $*"; pass=$((pass + 1)); }
run() { (cd "$WORK" && "$BIN" "$@") 2>&1; }
expect() {  # expect <name> <regex> <command...>
    local name=$1 re=$2; shift 2
    local out
    out=$(run "$@") || true
    grep -Eq -- "$re" <<<"$out" || fail "$name: /$re/ not in output of '$*':"$'\n'"$out"
    ok "$name"
}

expect "help lists every command"         "freeswitch +generate"                 help
expect "provider list shows the seed"     "s3 +intermediate +10\.0\.0\.3"        provider list
expect "provider add"                     "added successfully"                   provider add s1b 10.0.0.11:5060 origin 50 ip:10.0.0.11
expect "provider add rejects bad names"   "Invalid name"                         provider add 'x"y' 10.0.0.12 origin 1 ip:x
expect "did add to the shared pool"       "added successfully"                   did add 12125559999 US
expect "did add for a provider"           "added successfully"                   did add 13135550000 US s3
expect "route add"                        "added successfully"                   route add uk_route s1b s3 s4 '44[0-9]{10}' 90
expect "route add rejects XML/regex junk" "not a valid number pattern"           route add bad s1b s3 s4 '1"/><x' 1
expect "route test uses the real regex"   "Route: uk_route"                      route test 441234567890
expect "route test: no false match"       "No matching route"                    route test 99
expect "route delete is not injectable"   "not found"                            route delete "x' OR '1'='1"
[[ $("${PSQL[@]}" -c "SELECT count(*) FROM routes") == 2 ]] || fail "routes were deleted by the injection"
ok "routes table intact"
expect "module2 is reachable (was dead)"  "S2 -> S4 +ANI +15550001111"           module2 test 15550001111 12125550000
[[ $("${PSQL[@]}" -c "SELECT count(*) FROM call_records") == 0 ]] || fail "module2 test left data behind"
ok "module2 test rolls back"
expect "validation rule enable"           "Rule 1 enabled"                       validation rules enable 1
expect "validation rule unknown id"       "Failed to enable rule 999"            validation rules enable 999
[[ $("${PSQL[@]}" -c "SELECT count(*) FROM call_validation_rules") == 5 ]] || fail "default rules duplicated"
ok "default rules not duplicated by repeated runs"
expect "stats"                            "Configured Routes: 2"                 stats
expect "sip status reads the database"    "DIDs in use: +0 / 12"                 sip status

# Allocate a DID through SQL, then release it from the CLI.
did=$("${PSQL[@]}" -c "SELECT router_allocate_did(1, 'cli-test', '15550001111', '12125550000')")
expect "module2 validate shows the holder" "carries call cli-test"               module2 validate "$did"
expect "did release"                      "released"                             did release "$did"
[[ $("${PSQL[@]}" -c "SELECT status FROM call_records WHERE call_id = 'cli-test'") == failed ]] || fail "call not closed"
ok "released DID closes its call record"

# FreeSWITCH files: all well-formed, nothing outside the router namespace.
expect "freeswitch generate"              "configuration written"                freeswitch generate
find "$WORK/fs" -name '*.xml' -exec xmllint --noout {} + || fail "invalid XML"
ok "every generated XML file is well-formed"
[[ -f $WORK/fs/sip_profiles/router.xml && -f $WORK/fs/dialplan/router.xml && -f $WORK/scripts/route_handler.lua ]] \
    || fail "missing generated files"
[[ $(find "$WORK/fs/sip_profiles/router" -name '*.xml' | wc -l) == 4 ]] || fail "expected 4 gateways"
ok "profile, context, 4 gateways and the Lua handler written"
! grep -rq 'break="never"' "$WORK/fs" || fail "source-address check can be bypassed"
grep -q 'expression="^10\\.0\\.0\\.11\$"' "$WORK/fs/dialplan/router/route_2.xml" || fail "origin address not escaped/anchored"
ok "dialplan ANDs the source address with the number pattern"
[[ $(stat -c %a "$WORK/fs/router_vars.xml") == 640 ]] || fail "router_vars.xml readable by everyone"
ok "files holding secrets are not world-readable"
cmp -s "$ROOT/freeswitch/scripts/route_handler.lua" "$WORK/scripts/route_handler.lua" || fail "Lua handler differs"
ok "installed Lua handler is the tested file"

# Deactivated providers lose their gateway on the next reload.
"${PSQL[@]}" -c "UPDATE providers SET active = false WHERE name = 's1b'"
run provider reload >/dev/null
[[ $(find "$WORK/fs/sip_profiles/router" -name '*.xml' | wc -l) == 3 ]] || fail "stale gateway kept"
ok "stale gateway removed on reload"

! grep -E "Query failed|Execute failed" "$WORK/router.log" || fail "SQL errors in the log"
ok "no SQL error in the log"
echo "all $pass CLI checks passed"
