-- FreeSWITCH Router: canonical PostgreSQL schema (PostgreSQL 12+).
--
-- This file is the single source of truth for every table the router, its CLI
-- and the FreeSWITCH Lua handler use. It is idempotent: running it twice is safe
-- and it never drops data. Demo data lives in sql/seed_demo.sql.

SET client_min_messages = warning;
BEGIN;

-- ---------------------------------------------------------------------------
-- Providers: the SIP servers around the router (S1 origin, S3 intermediate,
-- S4 final). Each provider becomes a FreeSWITCH gateway named after its uuid.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS providers (
    id            SERIAL PRIMARY KEY,
    uuid          VARCHAR(64)  UNIQUE,
    name          VARCHAR(256) UNIQUE NOT NULL,
    host          VARCHAR(256) NOT NULL,
    port          INTEGER      NOT NULL DEFAULT 5060 CHECK (port BETWEEN 1 AND 65535),
    auth_type     VARCHAR(32)  NOT NULL DEFAULT 'ip',
    auth_data     TEXT,
    transport     VARCHAR(16)  NOT NULL DEFAULT 'udp',
    capacity      INTEGER      NOT NULL DEFAULT 100 CHECK (capacity >= 0),
    role          VARCHAR(32)  NOT NULL DEFAULT 'general'
                  CHECK (role IN ('origin', 'intermediate', 'final', 'general')),
    current_calls INTEGER      NOT NULL DEFAULT 0,
    active        BOOLEAN      NOT NULL DEFAULT true,
    priority      INTEGER      NOT NULL DEFAULT 100,
    created_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at    TIMESTAMP    NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- ---------------------------------------------------------------------------
-- Routes: a destination pattern and the three providers of the call flow.
-- The legacy *_provider text columns are kept because the CLI still fills them.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS routes (
    id                       SERIAL PRIMARY KEY,
    uuid                     VARCHAR(64) UNIQUE,
    name                     VARCHAR(256),
    pattern                  VARCHAR(256) NOT NULL,
    provider_id              INTEGER REFERENCES providers(id) ON DELETE CASCADE,
    origin_provider_id       INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    intermediate_provider_id INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    final_provider_id        INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    inbound_provider         VARCHAR(256),
    intermediate_provider    VARCHAR(256),
    final_provider           VARCHAR(256),
    priority                 INTEGER   NOT NULL DEFAULT 100,
    active                   BOOLEAN   NOT NULL DEFAULT true,
    created_at               TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- ---------------------------------------------------------------------------
-- DID pool. While a call is on the S3 leg, one DID carries it: the DID row
-- remembers the original ANI/DNIS so they can be restored on the way back.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS dids (
    id           SERIAL PRIMARY KEY,
    did          VARCHAR(64) UNIQUE NOT NULL,
    country      VARCHAR(32) DEFAULT 'US',
    provider_id  INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    in_use       BOOLEAN   NOT NULL DEFAULT false,
    destination  VARCHAR(64),
    original_ani VARCHAR(64),
    call_id      VARCHAR(256),
    allocated_at TIMESTAMP,
    released_at  TIMESTAMP,   -- least recently released DIDs are reused first
    active       BOOLEAN   NOT NULL DEFAULT true,
    created_at   TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
);

-- ---------------------------------------------------------------------------
-- Call tracking.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS calls (
    id          SERIAL PRIMARY KEY,
    uuid        VARCHAR(256),
    ani         VARCHAR(64),
    dnis        VARCHAR(64),
    provider    VARCHAR(256),
    route_id    INTEGER REFERENCES routes(id) ON DELETE SET NULL,
    status      VARCHAR(50) DEFAULT 'active',
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    answered_at TIMESTAMP,
    ended_at    TIMESTAMP,
    duration    INTEGER
);

CREATE TABLE IF NOT EXISTS call_records (
    id                       SERIAL PRIMARY KEY,
    call_id                  VARCHAR(256),
    route_id                 INTEGER REFERENCES routes(id) ON DELETE SET NULL,
    original_ani             VARCHAR(64),
    original_dnis            VARCHAR(64),
    assigned_did             VARCHAR(64),
    origin_provider_id       INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    intermediate_provider_id INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    final_provider_id        INTEGER REFERENCES providers(id) ON DELETE SET NULL,
    source_server            VARCHAR(64),
    status                   VARCHAR(32) NOT NULL DEFAULT 'routing',
    stage                    INTEGER     NOT NULL DEFAULT 1,  -- 1 = S1->S3 leg, 2 = S3->S4 leg
    failure_reason           VARCHAR(256),
    created_at               TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    updated_at               TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    ended_at                 TIMESTAMP
);
-- Columns added after the first release: make old databases match.
ALTER TABLE call_records ADD COLUMN IF NOT EXISTS route_id INTEGER REFERENCES routes(id) ON DELETE SET NULL;
ALTER TABLE call_records ADD COLUMN IF NOT EXISTS failure_reason VARCHAR(256);
ALTER TABLE dids ADD COLUMN IF NOT EXISTS released_at TIMESTAMP;

CREATE TABLE IF NOT EXISTS active_calls (
    id          SERIAL PRIMARY KEY,
    call_uuid   VARCHAR(256) UNIQUE NOT NULL,
    ani         VARCHAR(64),
    dnis        VARCHAR(64),
    provider_id INTEGER REFERENCES providers(id),
    status      VARCHAR(50),
    created_at  TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- ---------------------------------------------------------------------------
-- Call validation (used by the C validator and the `validation` command).
-- These were created at run time by the binary on every start; they now live here.
-- ---------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS call_validations (
    id                SERIAL PRIMARY KEY,
    call_id           VARCHAR(256) UNIQUE NOT NULL,
    call_uuid         VARCHAR(64),
    origin_ani        VARCHAR(64),
    origin_dnis       VARCHAR(64),
    assigned_did      VARCHAR(64),
    current_state     INTEGER,
    validation_status VARCHAR(32),
    created_at        TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    updated_at        TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS validation_checkpoints (
    id              SERIAL PRIMARY KEY,
    checkpoint_name VARCHAR(128),
    route_id        INTEGER,
    call_id         VARCHAR(256),
    passed          BOOLEAN,
    failure_reason  VARCHAR(256),
    checked_at      TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS security_events (
    id         SERIAL PRIMARY KEY,
    event_type VARCHAR(64),
    severity   VARCHAR(32),
    call_id    VARCHAR(256),
    source_ip  VARCHAR(64),
    details    TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS call_validation_rules (
    id             SERIAL PRIMARY KEY,
    rule_name      VARCHAR(128) UNIQUE NOT NULL,
    rule_type      VARCHAR(64),
    rule_condition TEXT,
    action         VARCHAR(64),
    priority       INTEGER DEFAULT 100,
    enabled        BOOLEAN DEFAULT true,
    created_at     TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS blocked_numbers (
    number     VARCHAR(64) PRIMARY KEY,
    reason     TEXT,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

-- Default rules. The original binary inserted these on every start with
-- ON CONFLICT DO NOTHING but no unique key, so the table grew by 5 rows per run.
INSERT INTO call_validation_rules (rule_name, rule_type, rule_condition, action, priority) VALUES
    ('Check Origin Server',   'origin_validation', 'source_ip IN (SELECT host FROM providers WHERE role = ''origin'')', 'allow', 100),
    ('Verify DID Ownership',  'did_validation',    'did IN (SELECT did FROM dids WHERE active = true)',              'allow',  90),
    ('Rate Limit Check',      'rate_limit',        'calls_per_minute < 100',                                          'allow',  80),
    ('Concurrent Call Limit', 'concurrent_limit',  'concurrent_calls < 500',                                          'allow',  70),
    ('Blocked Number Check',  'blacklist',         'ani NOT IN (SELECT number FROM blocked_numbers)',                 'allow',  60)
ON CONFLICT (rule_name) DO NOTHING;

-- ---------------------------------------------------------------------------
-- Indexes. The dids_free_* ones are the hot path: they hold only the DIDs a new call
-- can take, already in the order they should be taken (least recently released
-- first), so allocation reads one index entry however large the pool grows.
-- LRU order also avoids handing a DID straight back out while a late or
-- retransmitted return call for its previous use could still arrive.
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS providers_role_active_idx ON providers (role) WHERE active;
CREATE INDEX IF NOT EXISTS routes_active_priority_idx ON routes (priority DESC) WHERE active;
DROP INDEX IF EXISTS dids_free_idx;
DROP INDEX IF EXISTS dids_free_lru_idx;
-- Two partial indexes: "provider_id IS NULL" cannot fix the leading column the
-- way "=" does, so a single (provider_id, released_at, id) index would still
-- need a sort for the shared pool.
CREATE INDEX IF NOT EXISTS dids_free_shared_idx   ON dids (released_at NULLS FIRST, id)
    WHERE in_use = false AND active = true AND provider_id IS NULL;
CREATE INDEX IF NOT EXISTS dids_free_provider_idx ON dids (provider_id, released_at NULLS FIRST, id)
    WHERE in_use = false AND active = true AND provider_id IS NOT NULL;
CREATE INDEX IF NOT EXISTS dids_in_use_alloc_idx    ON dids (allocated_at) WHERE in_use;
CREATE INDEX IF NOT EXISTS calls_status_idx         ON calls (status);
CREATE INDEX IF NOT EXISTS calls_created_idx        ON calls (created_at);
CREATE INDEX IF NOT EXISTS call_records_call_id_idx ON call_records (call_id);
CREATE INDEX IF NOT EXISTS call_records_status_idx  ON call_records (status);
CREATE INDEX IF NOT EXISTS security_events_created_idx ON security_events (created_at);

-- ---------------------------------------------------------------------------
-- Monitoring view (used by `stats` and by tools/monitor.sh).
-- ---------------------------------------------------------------------------
CREATE OR REPLACE VIEW system_status AS
SELECT
    (SELECT COUNT(*) FROM providers WHERE active)                            AS active_providers,
    (SELECT COUNT(*) FROM routes    WHERE active)                            AS active_routes,
    (SELECT COUNT(*) FROM dids      WHERE active)                            AS total_dids,
    (SELECT COUNT(*) FROM dids      WHERE in_use)                            AS dids_in_use,
    (SELECT COUNT(*) FROM call_records WHERE status IN ('routing', 'forwarding')) AS active_calls,
    (SELECT COUNT(*) FROM providers WHERE role = 'origin'       AND active)  AS origin_providers,
    (SELECT COUNT(*) FROM providers WHERE role = 'intermediate' AND active)  AS intermediate_providers,
    (SELECT COUNT(*) FROM providers WHERE role = 'final'        AND active)  AS final_providers;

COMMIT;
