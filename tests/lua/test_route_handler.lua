-- Tests for freeswitch/scripts/route_handler.lua without FreeSWITCH.
--
-- `session` and `freeswitch` are replaced by fakes; the database is real: the
-- fake Dbh sends each query to PostgreSQL through psql (connection taken from
-- the usual PGHOST/PGUSER/PGPASSWORD/PGDATABASE variables).
--
-- Run: lua5.4 tests/lua/test_route_handler.lua   (from the repository root)

package.path = "./freeswitch/scripts/?.lua;" .. package.path
local handler = require("route_handler")

-- ---------------------------------------------------------------- helpers --
local function sh_quote(s) return "'" .. s:gsub("'", "'\\''") .. "'" end

local function psql(sql)
    local p = assert(io.popen("psql -X -q -v ON_ERROR_STOP=1 --csv -c " .. sh_quote(sql) .. " 2>&1"))
    local out = p:read("a")
    local ok = p:close()
    assert(ok, "psql failed: " .. out .. "\nSQL: " .. sql)
    local lines = {}
    for line in out:gmatch("[^\n]+") do lines[#lines + 1] = line end
    local rows, header = {}, {}
    for col in (lines[1] or ""):gmatch("[^,]+") do header[#header + 1] = col end
    for i = 2, #lines do
        local row, j = {}, 1
        for val in (lines[i] .. ","):gmatch("([^,]*),") do
            row[header[j]] = val; j = j + 1
        end
        rows[#rows + 1] = row
    end
    return rows
end

local queries = {}
local function fake_freeswitch(dsn)
    return {
        consoleLog = function() end,
        getGlobalVariable = function(name) return name == "router_db_dsn" and dsn or nil end,
        Dbh = function()
            return {
                connected = function() return true end,
                release = function() end,
                query = function(_, sql, cb)
                    queries[#queries + 1] = sql
                    for _, row in ipairs(psql(sql)) do if cb then cb(row) end end
                    return true
                end,
            }
        end,
    }
end

local function fake_session(vars)
    local s = { vars = vars, hangup_cause = nil, bridged_to = nil, disposition = "SUCCESS" }
    function s:getVariable(k)
        if k == "originate_disposition" then return self.bridged_to and self.disposition or nil end
        return self.vars[k]
    end
    function s:setVariable(k, v) self.vars[k] = v end
    function s:hangup(cause) self.hangup_cause = cause end
    function s:execute(app, data) if app == "bridge" then self.bridged_to = data end end
    return s
end

local FS = fake_freeswitch("pgsql://test")
local function call(uuid, ani, dnis, ip)
    return fake_session({ uuid = uuid, caller_id_number = ani, destination_number = dnis, network_addr = ip })
end

local function reset_db()
    psql("TRUNCATE call_records, security_events RESTART IDENTITY; " ..
         "UPDATE dids SET in_use = false, destination = NULL, original_ani = NULL, call_id = NULL, allocated_at = NULL;")
end

local route_id = psql("SELECT id FROM routes WHERE name = 'default_route'")[1].id
local POOL = tonumber(psql("SELECT count(*) AS n FROM dids WHERE active")[1].n)
local S3, S4 = "gw-s3", "gw-s4"

local passed, failed = 0, 0
local function test(name, fn)
    reset_db(); queries = {}
    local ok, err = pcall(fn)
    if ok then passed = passed + 1; print("ok   " .. name)
    else failed = failed + 1; print("FAIL " .. name .. "\n     " .. tostring(err)) end
end
local function eq(a, b, what) assert(a == b, string.format("%s: expected %s, got %s", what, tostring(b), tostring(a))) end

-- ------------------------------------------------------------------ tests --
test("origin leg takes a DID and swaps ANI/DNIS towards S3", function()
    local s = call("uuid-1", "15550001111", "12125550000", "10.0.0.1")
    eq(handler.run(s, { "origin", route_id, S3, S4 }, FS), "forwarded_to_intermediate", "outcome")
    local did = s.vars.router_did
    assert(did and did:match("^%d+$"), "DID allocated")
    eq(s.vars.effective_caller_id_number, "12125550000", "ANI-2 is DNIS-1")
    eq(s.bridged_to, "sofia/gateway/gw-s3/" .. did, "bridge string")
    local d = psql("SELECT in_use, destination, original_ani, call_id FROM dids WHERE did = '" .. did .. "'")[1]
    eq(d.in_use, "t", "DID in use"); eq(d.destination, "12125550000", "DNIS stored")
    eq(d.original_ani, "15550001111", "ANI stored"); eq(d.call_id, "uuid-1", "call id stored")
    eq(#queries, 1, "one database round trip")
end)

test("return leg restores the original numbers, frees the DID and reaches S4", function()
    local s1 = call("uuid-2", "15550002222", "13105550000", "10.0.0.1")
    handler.run(s1, { "origin", route_id, S3, S4 }, FS)
    local did = s1.vars.router_did
    local s3 = call("uuid-2b", "13105550000", did, "10.0.0.3")
    eq(handler.run(s3, { "intermediate_return", route_id, "gw-s1", S4 }, FS), "forwarded_to_final", "outcome")
    eq(s3.vars.effective_caller_id_number, "15550002222", "ANI-1 restored")
    local s4_uuid = psql("SELECT uuid FROM providers WHERE name = 's4'")[1].uuid
    eq(s3.bridged_to, "sofia/gateway/" .. s4_uuid .. "/13105550000", "bridged to the call's S4 with DNIS-1")
    eq(psql("SELECT in_use FROM dids WHERE did = '" .. did .. "'")[1].in_use, "f", "DID released")
    local cr = psql("SELECT status, stage FROM call_records WHERE call_id = 'uuid-2'")[1]
    eq(cr.status, "forwarding", "record status"); eq(cr.stage, "2", "record stage")
end)

test("a second return on the same DID is rejected", function()
    local s1 = call("uuid-3", "15550003333", "12125550001", "10.0.0.1")
    handler.run(s1, { "origin", route_id, S3, S4 }, FS)
    local did = s1.vars.router_did
    handler.run(call("uuid-3b", "1", did, "10.0.0.3"), { "intermediate_return", route_id, "gw-s1", S4 }, FS)
    local replay = call("uuid-3c", "1", did, "10.66.6.6")
    eq(handler.run(replay, { "intermediate_return", route_id, "gw-s1", S4 }, FS), "unknown_did", "outcome")
    eq(replay.hangup_cause, "CALL_REJECTED", "hangup cause")
    assert(replay.bridged_to == nil, "no bridge")
    eq(psql("SELECT count(*) AS n FROM security_events WHERE event_type = 'UNKNOWN_RETURN'")[1].n, "1", "event logged")
end)

test("SQL injection in the caller ID never reaches the database", function()
    local s = call("uuid-4", "1'; DROP TABLE dids; --", "12125550000", "10.0.0.1")
    eq(handler.run(s, { "origin", route_id, S3, S4 }, FS), "bad_number", "outcome")
    eq(s.hangup_cause, "INVALID_NUMBER_FORMAT", "hangup cause")
    eq(#queries, 0, "no query sent")
    eq(tonumber(psql("SELECT count(*) AS n FROM dids WHERE active")[1].n), POOL, "dids intact")
end)

test("quote() doubles single quotes", function()
    eq(handler.quote("a'b"), "'a''b'", "quoted")
    assert(not handler.is_token("gw;rm -rf /"), "shell characters refused")
    assert(handler.is_number("+212600000000") and not handler.is_number("12 34"), "number format")
end)

test("a failed bridge to S3 releases the DID immediately", function()
    local s = call("uuid-5", "15550005555", "12125550002", "10.0.0.1")
    s.disposition = "USER_BUSY"
    eq(handler.run(s, { "origin", route_id, S3, S4 }, FS), "bridge_failed", "outcome")
    eq(psql("SELECT count(*) AS n FROM dids WHERE in_use")[1].n, "0", "no DID left in use")
    local cr = psql("SELECT status, failure_reason FROM call_records WHERE call_id = 'uuid-5'")[1]
    eq(cr.status, "failed", "record failed"); eq(cr.failure_reason, "bridge: USER_BUSY", "reason")
end)

test("an exhausted pool answers congestion", function()
    for i = 1, POOL do handler.run(call("fill-" .. i, "1555000" .. i, "12125550000", "10.0.0.1"), { "origin", route_id, S3, S4 }, FS) end
    local s = call("uuid-6", "15550006666", "12125550000", "10.0.0.1")
    eq(handler.run(s, { "origin", route_id, S3, S4 }, FS), "no_did", "outcome")
    eq(s.hangup_cause, "NORMAL_CIRCUIT_CONGESTION", "hangup cause")
end)

test("an unknown route gets no DID", function()
    local s = call("uuid-7", "15550007777", "12125550000", "10.0.0.1")
    eq(handler.run(s, { "origin", "9999", S3, S4 }, FS), "no_did", "outcome")
end)

test("missing router_db_dsn fails closed", function()
    local s = call("uuid-8", "15550008888", "12125550000", "10.0.0.1")
    eq(handler.run(s, { "origin", route_id, S3, S4 }, fake_freeswitch(nil)), "no_dsn", "outcome")
    eq(s.hangup_cause, "TEMPORARY_FAILURE", "hangup cause")
end)

test("unknown stage and bad arguments are refused", function()
    eq(handler.run(call("uuid-9", "1", "2", "x"), { "sideways", route_id, S3, S4 }, FS), "bad_stage", "stage")
    eq(handler.run(call("uuid-9", "1", "2", "x"), { "origin", "1 OR 1=1", S3, S4 }, FS), "bad_args", "route id")
    eq(handler.run(call("uuid-9", "1", "2", "x"), { "origin", route_id, "gw/../x", S4 }, FS), "bad_args", "gateway")
end)

print(string.format("\n%d passed, %d failed", passed, failed))
os.exit(failed == 0 and 0 or 1)
