-- FreeSWITCH Router: call-path functions.
--
-- The Lua handler inside FreeSWITCH calls these, one round trip per call leg.
-- Each function is a single transaction, so two simultaneous calls can never
-- receive the same DID and a DID can never be released twice.

SET client_min_messages = warning;
BEGIN;

-- A phone number as the router accepts it: optional "+", then 1 to 32 digits.
CREATE OR REPLACE FUNCTION router_is_number(p TEXT)
RETURNS BOOLEAN LANGUAGE sql IMMUTABLE AS $$
    SELECT p IS NOT NULL AND p ~ '^\+?[0-9]{1,32}$'
$$;

-- ---------------------------------------------------------------------------
-- Leg 1 (S1 -> S2 -> S3): take a free DID for this call and remember the
-- original ANI/DNIS on it. Returns the DID, or NULL when the route is unknown
-- or inactive, the numbers are malformed, or the pool is exhausted.
--
-- DIDs owned by the route's intermediate provider are preferred; unassigned
-- DIDs (provider_id IS NULL) are the shared fallback pool.
-- FOR UPDATE SKIP LOCKED lets concurrent calls take different rows without
-- waiting on each other.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION router_allocate_did(
    p_route_id  INTEGER,
    p_call_id   TEXT,
    p_ani       TEXT,
    p_dnis      TEXT,
    p_source_ip TEXT DEFAULT NULL
) RETURNS TEXT
LANGUAGE plpgsql AS $$
DECLARE
    v_route routes%ROWTYPE;
    v_did   dids%ROWTYPE;
BEGIN
    IF NOT router_is_number(p_ani) OR NOT router_is_number(p_dnis)
       OR p_call_id IS NULL OR length(p_call_id) NOT BETWEEN 1 AND 256 THEN
        RETURN NULL;
    END IF;

    SELECT * INTO v_route FROM routes WHERE id = p_route_id AND active;
    IF NOT FOUND THEN
        RETURN NULL;
    END IF;

    -- Two indexed lookups rather than one "provider OR shared" query: an OR
    -- cannot be served in order by the index and forces a sort of the pool.
    SELECT * INTO v_did
    FROM dids
    WHERE in_use = false AND active = true
      AND provider_id = v_route.intermediate_provider_id
    ORDER BY released_at NULLS FIRST, id
    LIMIT 1
    FOR UPDATE SKIP LOCKED;

    IF NOT FOUND THEN
        SELECT * INTO v_did
        FROM dids
        WHERE in_use = false AND active = true AND provider_id IS NULL
        ORDER BY released_at NULLS FIRST, id
        LIMIT 1
        FOR UPDATE SKIP LOCKED;
    END IF;

    IF NOT FOUND THEN
        INSERT INTO security_events (event_type, severity, call_id, source_ip, details)
        VALUES ('DID_EXHAUSTED', 'HIGH', p_call_id, p_source_ip,
                format('No free DID for route %s', p_route_id));
        RETURN NULL;
    END IF;

    UPDATE dids
    SET in_use = true, destination = p_dnis, original_ani = p_ani,
        call_id = p_call_id, allocated_at = CURRENT_TIMESTAMP
    WHERE id = v_did.id;

    INSERT INTO call_records (call_id, route_id, original_ani, original_dnis, assigned_did,
                              origin_provider_id, intermediate_provider_id, final_provider_id,
                              source_server, status, stage)
    VALUES (p_call_id, v_route.id, p_ani, p_dnis, v_did.did,
            v_route.origin_provider_id, v_route.intermediate_provider_id, v_route.final_provider_id,
            p_source_ip, 'routing', 1);

    RETURN v_did.did;
END;
$$;

-- ---------------------------------------------------------------------------
-- Leg 2 (S3 -> S2 -> S4): the intermediate server calls back on the DID.
-- Atomically read the original ANI/DNIS and free the DID. Returns no row when
-- the DID is not currently allocated: the Lua handler then rejects the call.
-- final_gateway is the uuid of the final provider recorded for THIS call, so
-- routes that share an intermediate server still reach the right S4.
-- ---------------------------------------------------------------------------
DROP FUNCTION IF EXISTS router_complete_return(TEXT, TEXT);
CREATE FUNCTION router_complete_return(p_did TEXT, p_source_ip TEXT DEFAULT NULL)
RETURNS TABLE (original_ani TEXT, original_dnis TEXT, call_id TEXT, final_gateway TEXT)
LANGUAGE plpgsql AS $$
DECLARE
    v_did dids%ROWTYPE;
    v_gateway TEXT;
BEGIN
    IF NOT router_is_number(p_did) THEN
        RETURN;
    END IF;

    SELECT * INTO v_did FROM dids d
    WHERE d.did = p_did AND d.in_use
    FOR UPDATE;

    IF NOT FOUND THEN
        INSERT INTO security_events (event_type, severity, source_ip, details)
        VALUES ('UNKNOWN_RETURN', 'CRITICAL', p_source_ip,
                format('Return call on DID %s which is not allocated', p_did));
        RETURN;
    END IF;

    UPDATE dids
    SET in_use = false, destination = NULL, original_ani = NULL,
        call_id = NULL, allocated_at = NULL, released_at = CURRENT_TIMESTAMP
    WHERE id = v_did.id;

    UPDATE call_records cr
    SET stage = 2, status = 'forwarding', updated_at = CURRENT_TIMESTAMP
    WHERE cr.call_id = v_did.call_id AND cr.status = 'routing';

    SELECT p.uuid INTO v_gateway
    FROM call_records cr JOIN providers p ON p.id = cr.final_provider_id
    WHERE cr.call_id = v_did.call_id
    ORDER BY cr.id DESC LIMIT 1;

    original_ani  := v_did.original_ani;
    original_dnis := v_did.destination;
    call_id       := v_did.call_id;
    final_gateway := v_gateway;
    RETURN NEXT;
END;
$$;

-- ---------------------------------------------------------------------------
-- Leg 1 failed (S3 unreachable, busy, ...): give the DID back at once instead
-- of waiting for the stale sweep. Only the call that owns the DID can free it.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION router_cancel_did(p_did TEXT, p_call_id TEXT, p_reason TEXT DEFAULT NULL)
RETURNS BOOLEAN
LANGUAGE plpgsql AS $$
DECLARE
    v_id INTEGER;
BEGIN
    UPDATE dids d
    SET in_use = false, destination = NULL, original_ani = NULL,
        call_id = NULL, allocated_at = NULL, released_at = CURRENT_TIMESTAMP
    WHERE d.did = p_did AND d.in_use AND d.call_id = p_call_id
    RETURNING d.id INTO v_id;

    IF v_id IS NULL THEN
        RETURN false;
    END IF;

    UPDATE call_records cr
    SET status = 'failed', failure_reason = left(coalesce(p_reason, 'Bridge to intermediate failed'), 256),
        ended_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP
    WHERE cr.call_id = p_call_id AND cr.status = 'routing';
    RETURN true;
END;
$$;

-- ---------------------------------------------------------------------------
-- Housekeeping: free DIDs whose call never came back from S3, and mark those
-- calls as timed out. Returns how many DIDs were freed.
-- ---------------------------------------------------------------------------
CREATE OR REPLACE FUNCTION router_release_stale_dids(p_max_age INTERVAL DEFAULT INTERVAL '5 minutes')
RETURNS INTEGER
LANGUAGE plpgsql AS $$
DECLARE
    v_count INTEGER;
BEGIN
    WITH stale AS (
        SELECT id, call_id FROM dids
        WHERE in_use AND allocated_at < CURRENT_TIMESTAMP - p_max_age
        FOR UPDATE SKIP LOCKED
    ), freed AS (
        UPDATE dids d
        SET in_use = false, destination = NULL, original_ani = NULL,
            call_id = NULL, allocated_at = NULL, released_at = CURRENT_TIMESTAMP
        FROM stale
        WHERE d.id = stale.id
        RETURNING stale.call_id
    ), timed_out AS (
        UPDATE call_records cr
        SET status = 'timeout', failure_reason = 'No return from intermediate',
            ended_at = CURRENT_TIMESTAMP, updated_at = CURRENT_TIMESTAMP
        FROM freed
        WHERE cr.call_id = freed.call_id AND cr.status = 'routing'
        RETURNING 1
    )
    SELECT COUNT(*) INTO v_count FROM freed;
    RETURN v_count;
END;
$$;

COMMIT;
