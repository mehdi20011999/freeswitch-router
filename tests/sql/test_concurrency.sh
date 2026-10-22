#!/usr/bin/env bash
# Concurrency and housekeeping tests for the call-path SQL functions.
# Uses the usual libpq variables (PGHOST, PGUSER, PGPASSWORD, PGDATABASE)
# on a database where sql/*.sql and sql/seed_demo.sql have been loaded.
set -euo pipefail

PSQL=(psql -X -q -At -v ON_ERROR_STOP=1)
fail() { echo "FAIL $*"; exit 1; }
ok() { echo "ok   $*"; }

reset() {
    "${PSQL[@]}" -c "TRUNCATE call_records, security_events RESTART IDENTITY;
        UPDATE dids SET in_use = false, destination = NULL, original_ani = NULL,
                        call_id = NULL, allocated_at = NULL;"
}

route_id=$("${PSQL[@]}" -c "SELECT id FROM routes WHERE name = 'default_route'")
pool=$("${PSQL[@]}" -c "SELECT count(*) FROM dids WHERE active")
[[ $pool -ge 1 ]] || fail "seed data missing"

# 1. 4x more simultaneous calls than DIDs: every DID is handed out exactly once.
reset
callers=$((pool * 4))
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for i in $(seq 1 "$callers"); do
    "${PSQL[@]}" -c "SELECT router_allocate_did($route_id, 'race-$i', '1555000$i', '12125550000')" >"$tmp/$i" &
done
wait
got=$(cat "$tmp"/* | grep -c . || true)
distinct=$(cat "$tmp"/* | grep . | sort -u | wc -l)
in_use=$("${PSQL[@]}" -c "SELECT count(*) FROM dids WHERE in_use")
records=$("${PSQL[@]}" -c "SELECT count(*) FROM call_records")
[[ $got -eq $pool && $distinct -eq $pool && $in_use -eq $pool && $records -eq $pool ]] \
    || fail "race: $callers callers, pool $pool, got $got, distinct $distinct, in_use $in_use, records $records"
ok "$callers concurrent calls on $pool DIDs: $got allocations, all distinct"

# 2. Simultaneous returns on one DID: exactly one wins.
reset
did=$("${PSQL[@]}" -c "SELECT router_allocate_did($route_id, 'dup', '15550001111', '12125550000')")
for i in $(seq 1 20); do
    "${PSQL[@]}" -c "SELECT original_dnis FROM router_complete_return('$did')" >"$tmp/r$i" &
done
wait
winners=$(cat "$tmp"/r* | grep -c . || true)
[[ $winners -eq 1 ]] || fail "double return: $winners winners"
ok "20 concurrent returns on one DID: exactly 1 accepted"

# 3. Stale sweep frees DIDs whose call never came back, and only those.
reset
"${PSQL[@]}" -c "SELECT router_allocate_did($route_id, 'old', '15550001111', '12125550000');
                 SELECT router_allocate_did($route_id, 'new', '15550002222', '12125550000');
                 UPDATE dids SET allocated_at = now() - interval '10 minutes' WHERE call_id = 'old';" >/dev/null
freed=$("${PSQL[@]}" -c "SELECT router_release_stale_dids(interval '5 minutes')")
status=$("${PSQL[@]}" -c "SELECT string_agg(call_id || '=' || status, ',' ORDER BY call_id) FROM call_records")
[[ $freed -eq 1 && $status == "new=routing,old=timeout" ]] || fail "stale sweep: freed=$freed status=$status"
ok "stale sweep frees only expired DIDs and marks their calls timed out"

# 4. Cancel only works for the call that owns the DID.
reset
did=$("${PSQL[@]}" -c "SELECT router_allocate_did($route_id, 'owner', '15550001111', '12125550000')")
other=$("${PSQL[@]}" -c "SELECT router_cancel_did('$did', 'someone-else')")
mine=$("${PSQL[@]}" -c "SELECT router_cancel_did('$did', 'owner', 'busy')")
[[ $other == f && $mine == t ]] || fail "cancel ownership: other=$other mine=$mine"
ok "router_cancel_did only frees the owner's DID"

# 5. Malformed input is refused inside the database too (defence in depth).
reset
bad=$("${PSQL[@]}" -c "SELECT router_allocate_did($route_id, 'x', '1''; DROP TABLE dids;--', '1') IS NULL")
[[ $bad == t ]] || fail "malformed ANI accepted"
ok "malformed numbers are refused by the SQL functions"

echo "all SQL tests passed"
