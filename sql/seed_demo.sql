-- Demo data for a lab or CI: three providers, ten DIDs and one route.
-- Addresses follow the lab plan in docs/ARCHITECTURE.md (S1 10.0.0.1, S2 10.0.0.2,
-- S3 10.0.0.3, S4 10.0.0.4). Safe to run more than once.

SET client_min_messages = warning;
BEGIN;

INSERT INTO providers (uuid, name, host, port, auth_type, auth_data, role, capacity) VALUES
    ('a1b2c3d4-0001-4000-8000-000000000001', 's1', '10.0.0.1', 5060, 'ip', '10.0.0.1', 'origin',       1000),
    ('a1b2c3d4-0003-4000-8000-000000000003', 's3', '10.0.0.3', 5060, 'ip', '10.0.0.3', 'intermediate', 1000),
    ('a1b2c3d4-0004-4000-8000-000000000004', 's4', '10.0.0.4', 5060, 'ip', '10.0.0.4', 'final',        1000)
ON CONFLICT (name) DO NOTHING;

INSERT INTO dids (did, country)
SELECT '1212555' || lpad(n::text, 4, '0'), 'US'
FROM generate_series(1001, 1010) AS n
ON CONFLICT (did) DO NOTHING;

INSERT INTO routes (uuid, name, pattern, origin_provider_id, intermediate_provider_id, final_provider_id, priority)
SELECT 'b1b2c3d4-0001-4000-8000-000000000001', 'default_route', '1[2-9][0-9]{9}', s1.id, s3.id, s4.id, 100
FROM providers s1, providers s3, providers s4
WHERE s1.name = 's1' AND s3.name = 's3' AND s4.name = 's4'
ON CONFLICT (uuid) DO NOTHING;

COMMIT;
