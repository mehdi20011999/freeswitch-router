#!/usr/bin/env bash
# Create the router's PostgreSQL role and database, then load the schema.
#
#   ROUTER_DB_PASSWORD='a-long-random-secret' tools/setup-db.sh [--seed]
#
# Runs the admin part as the local `postgres` superuser (sudo). Loading the
# schema twice is safe. --seed adds the demo providers, DIDs and route.
set -euo pipefail

DB=${ROUTER_DB_NAME:-router_db}
ROLE=${ROUTER_DB_USER:-router}
HOST=${ROUTER_DB_HOST:-localhost}
ROOT=$(cd "$(dirname "$0")/.." && pwd)

if [[ -z ${ROUTER_DB_PASSWORD:-} || ${ROUTER_DB_PASSWORD} == router123 ]]; then
    echo "Set ROUTER_DB_PASSWORD to a strong password (the old default router123 is refused)." >&2
    exit 1
fi
[[ $DB =~ ^[a-z_][a-z0-9_]*$ && $ROLE =~ ^[a-z_][a-z0-9_]*$ ]] || { echo "invalid database or role name" >&2; exit 1; }

sudo -u postgres psql -X -q -v ON_ERROR_STOP=1 -v role="$ROLE" -v pass="$ROUTER_DB_PASSWORD" -v db="$DB" <<'SQL'
SELECT format('CREATE ROLE %I LOGIN PASSWORD %L', :'role', :'pass')
WHERE NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = :'role') \gexec
SELECT format('ALTER ROLE %I PASSWORD %L', :'role', :'pass') \gexec
SELECT format('CREATE DATABASE %I OWNER %I', :'db', :'role')
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = :'db') \gexec
SQL

export PGHOST=$HOST PGDATABASE=$DB PGUSER=$ROLE PGPASSWORD=$ROUTER_DB_PASSWORD
psql -X -q -v ON_ERROR_STOP=1 -f "$ROOT/sql/001_schema.sql" -f "$ROOT/sql/002_functions.sql"
if [[ ${1:-} == --seed ]]; then
    psql -X -q -v ON_ERROR_STOP=1 -f "$ROOT/sql/seed_demo.sql"
fi
psql -X -c "SELECT * FROM system_status"
echo "Done. Router connection: ROUTER_DB_CONNECTION='host=$HOST dbname=$DB user=$ROLE password=...'"
