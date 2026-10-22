#!/usr/bin/env bash
# Benchmark: original DID allocation (SELECT ... ORDER BY RANDOM() then UPDATE,
# two round trips, no lock) against router_allocate_did (one call, indexed,
# FOR UPDATE SKIP LOCKED). Each transaction allocates a DID then frees it again,
# so the pool never runs dry and both sides do the same work.
#
# Usage: tests/bench/did_alloc_bench.sh [pool_size] [clients] [seconds]
# Needs: pgbench, and a THROWAWAY database in PGDATABASE (the DID table is rebuilt).
set -euo pipefail

POOL=${1:-50000}
CLIENTS=${2:-8}
SECONDS_RUN=${3:-10}
PSQL=(psql -X -q -At -v ON_ERROR_STOP=1)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"${PSQL[@]}" <<SQL
TRUNCATE call_records, security_events RESTART IDENTITY;
DELETE FROM dids;
INSERT INTO dids (did, country) SELECT (10000000000 + n)::text, 'US' FROM generate_series(1, $POOL) n;
DROP TABLE IF EXISTS bench_alloc;
CREATE UNLOGGED TABLE bench_alloc (did TEXT, call_id TEXT);
VACUUM ANALYZE dids;
SQL
route_id=$("${PSQL[@]}" -c "SELECT id FROM routes WHERE name = 'default_route'")

# Original Lua logic (route_handler_corrected.lua). The holding period between
# allocation and release is where a second call can grab the same DID.
cat >"$tmp/old.sql" <<'SQL'
\set n random(1, 1000000000)
SELECT id AS did_id, did AS did_num FROM dids WHERE in_use = false AND active = true ORDER BY RANDOM() LIMIT 1 \gset
UPDATE dids SET in_use = true, destination = '12125550000', original_ani = '15550001111', call_id = 'c' || :n, allocated_at = now() WHERE id = :did_id;
INSERT INTO bench_alloc VALUES (:did_num::text, 'c' || :n);
UPDATE dids SET in_use = false, destination = NULL, original_ani = NULL, call_id = NULL, allocated_at = NULL WHERE id = :did_id;
SQL

cat >"$tmp/new.sql" <<SQL
\set n random(1, 1000000000)
SELECT COALESCE(router_allocate_did($route_id, 'c' || :n, '15550001111', '12125550000'), '0') AS did_num \gset
INSERT INTO bench_alloc VALUES (:did_num::text, 'c' || :n);
SELECT router_cancel_did(:did_num::text, 'c' || :n);
SQL

perf() {
    local name=$1
    "${PSQL[@]}" -c "TRUNCATE bench_alloc, call_records; UPDATE dids SET in_use = false WHERE in_use;"
    local out tps lat
    out=$(pgbench -n -c "$CLIENTS" -j "$CLIENTS" -T "$SECONDS_RUN" -f "$tmp/$name.sql" 2>&1)
    tps=$(grep -m1 -oP 'tps = \K[0-9.]+' <<<"$out")
    lat=$(grep -oP 'latency average = \K[0-9.]+' <<<"$out")
    printf '  %-4s %10.0f calls/s   %7.3f ms per call\n' "$name" "$tps" "$lat"
}

# Race: hold every DID (no release) until the pool is used up once, then count
# DIDs that were handed to two calls at the same time.
race() {
    local name=$1 small=400
    "${PSQL[@]}" -c "TRUNCATE bench_alloc, call_records;
        UPDATE dids SET in_use = false WHERE in_use;
        UPDATE dids SET active = (id IN (SELECT id FROM dids ORDER BY id LIMIT $small));"
    sed '/^UPDATE dids SET in_use = false/d; /^SELECT router_cancel_did/d' "$tmp/$name.sql" >"$tmp/$name.race.sql"
    pgbench -n -c "$CLIENTS" -j "$CLIENTS" -t $((small / CLIENTS)) -f "$tmp/$name.race.sql" >/dev/null 2>&1 || true
    local dup
    dup=$("${PSQL[@]}" -c "SELECT count(*) - count(DISTINCT did) FROM bench_alloc WHERE did <> '0'")
    printf '  %-4s %4s of %s calls got a DID already held by another call\n' "$name" "$dup" "$small"
    "${PSQL[@]}" -c "UPDATE dids SET active = true;"
}

echo "pool=$POOL DIDs, $CLIENTS clients, ${SECONDS_RUN}s per run"
echo "Throughput (allocate + free):"
perf old
perf new
echo "Correctness under concurrency:"
race old
race new
