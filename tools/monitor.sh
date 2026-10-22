#!/usr/bin/env bash
# Live view of the router state from PostgreSQL (uses PGHOST/PGUSER/PGDATABASE/PGPASSWORD).
#   tools/monitor.sh [interval_seconds]
set -euo pipefail
INTERVAL=${1:-2}
while true; do
    clear
    date '+%F %T'
    psql -X -q -P pager=off -c "SELECT * FROM system_status" \
         -c "SELECT status, count(*) AS calls_last_hour FROM call_records
             WHERE created_at > now() - interval '1 hour' GROUP BY status ORDER BY status" \
         -c "SELECT did, original_ani, destination AS original_dnis,
                    extract(epoch FROM now() - allocated_at)::int AS held_s
             FROM dids WHERE in_use ORDER BY allocated_at LIMIT 15" \
         -c "SELECT created_at::time(0), event_type, severity, source_ip, details
             FROM security_events ORDER BY id DESC LIMIT 5"
    sleep "$INTERVAL"
done
