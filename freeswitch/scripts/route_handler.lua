--[[
route_handler.lua: the call path of the router, run by FreeSWITCH.

The generated dialplan calls it twice per call:

  lua route_handler.lua origin              <route_id> <intermediate_gw> <final_gw>
      S1 -> S2: take a DID, then bridge to S3 with ANI := DNIS and DNIS := DID.

  lua route_handler.lua intermediate_return <route_id> <origin_gw> <final_gw>
      S3 -> S2: the call comes back on the DID. Restore the original ANI/DNIS,
      free the DID, then bridge to the S4 recorded for that call.

All state changes happen in PostgreSQL functions (sql/002_functions.sql), one
round trip per leg, so concurrent calls cannot share a DID.

Configuration (FreeSWITCH vars.xml):
  <X-PRE-PROCESS cmd="set" data="router_db_dsn=pgsql://hostaddr=127.0.0.1 dbname=router_db user=router password=..."/>
]]

local M = {}

-- Inputs come from SIP headers, so they are untrusted. FreeSWITCH's Dbh has no
-- bound parameters: every value is checked against a strict format first and
-- quoted as a second line of defence.
local function is_number(s)
    return type(s) == "string" and #s <= 33 and s:match("^%+?%d+$") ~= nil
end

local function is_token(s)
    return type(s) == "string" and #s >= 1 and #s <= 128 and s:match("^[%w%-%._]+$") ~= nil
end

local function quote(s)
    return "'" .. tostring(s):gsub("'", "''") .. "'"
end

M.is_number, M.is_token, M.quote = is_number, is_token, quote

local function query_row(dbh, sql)
    local row
    dbh:query(sql, function(r) row = row or r end)
    return row
end

local BRIDGE_OK = { SUCCESS = true, NORMAL_CLEARING = true }

-- Runs one leg. Returns a short outcome string, used by the tests and the logs.
function M.run(session, argv, fs)
    local function log(level, fmt, ...)
        fs.consoleLog(level, string.format("[router] " .. fmt .. "\n", ...))
    end
    local function reject(cause, outcome, fmt, ...)
        log("WARNING", fmt, ...)
        session:hangup(cause)
        return outcome
    end

    local stage = argv[1]
    local route_id = tonumber(argv[2])
    local next_gw, final_gw = argv[3], argv[4]
    local call_uuid = session:getVariable("uuid")
    local ani = session:getVariable("caller_id_number")
    local dnis = session:getVariable("destination_number")
    local source_ip = session:getVariable("network_addr") or ""

    if not route_id or route_id ~= math.floor(route_id) or not is_token(final_gw) then
        return reject("UNSPECIFIED", "bad_args", "invalid dialplan arguments for stage %s", tostring(stage))
    end
    if not is_token(call_uuid) then
        return reject("UNSPECIFIED", "bad_uuid", "invalid call uuid")
    end
    if not is_number(ani) or not is_number(dnis) then
        return reject("INVALID_NUMBER_FORMAT", "bad_number",
                      "%s rejected: ANI or DNIS is not a phone number", call_uuid)
    end

    local dsn = fs.getGlobalVariable("router_db_dsn")
    if not dsn or dsn == "" then
        return reject("TEMPORARY_FAILURE", "no_dsn", "router_db_dsn is not set in vars.xml")
    end
    local dbh = fs.Dbh(dsn)
    if not dbh:connected() then
        return reject("TEMPORARY_FAILURE", "db_down", "database connection failed")
    end

    if stage == "origin" then
        if not is_token(next_gw) then
            dbh:release()
            return reject("UNSPECIFIED", "bad_args", "invalid intermediate gateway")
        end
        local row = query_row(dbh, string.format(
            "SELECT router_allocate_did(%d, %s, %s, %s, %s) AS did",
            route_id, quote(call_uuid), quote(ani), quote(dnis), quote(source_ip)))
        -- Give the pooled handle back before bridging: the bridge lasts as long
        -- as the call, and the database is not needed during it.
        dbh:release()

        local did = row and row.did
        if not did or did == "" then
            return reject("NORMAL_CIRCUIT_CONGESTION", "no_did", "%s: no free DID on route %d", call_uuid, route_id)
        end

        session:setVariable("router_did", did)
        session:setVariable("sip_h_X-Original-ANI", ani)
        session:setVariable("sip_h_X-Original-DNIS", dnis)
        session:setVariable("sip_h_X-Route-ID", tostring(route_id))
        -- Transformation towards S3: ANI-2 := DNIS-1, DNIS-2 := DID.
        session:setVariable("effective_caller_id_number", dnis)
        session:setVariable("effective_caller_id_name", dnis)

        log("INFO", "%s S1->S3 route=%d ani=%s dnis=%s did=%s", call_uuid, route_id, ani, dnis, did)
        session:execute("bridge", "sofia/gateway/" .. next_gw .. "/" .. did)

        local disposition = session:getVariable("originate_disposition")
        if not BRIDGE_OK[disposition or ""] then
            -- S3 never answered: free the DID now rather than after the stale sweep.
            local again = fs.Dbh(dsn)
            if again:connected() then
                again:query(string.format("SELECT router_cancel_did(%s, %s, %s)",
                    quote(did), quote(call_uuid), quote("bridge: " .. tostring(disposition))))
                again:release()
            end
            log("WARNING", "%s bridge to intermediate failed (%s), DID %s released",
                call_uuid, tostring(disposition), did)
            return "bridge_failed"
        end
        return "forwarded_to_intermediate"

    elseif stage == "intermediate_return" then
        local row = query_row(dbh, string.format(
            "SELECT original_ani, original_dnis, final_gateway FROM router_complete_return(%s, %s)",
            quote(dnis), quote(source_ip)))
        dbh:release()

        if not row or not is_number(row.original_dnis) then
            return reject("CALL_REJECTED", "unknown_did",
                          "%s: DID %s is not allocated, possible fraud from %s", call_uuid, dnis, source_ip)
        end

        -- The final provider recorded with the call wins over the dialplan
        -- argument: two routes may share S3 but not S4.
        local gateway = row.final_gateway
        if not is_token(gateway) then gateway = final_gw end

        -- Transformation towards S4: restore ANI-1 and DNIS-1.
        session:setVariable("effective_caller_id_number", row.original_ani)
        session:setVariable("effective_caller_id_name", row.original_ani)
        session:setVariable("sip_h_X-Route-ID", tostring(route_id))

        log("INFO", "%s S3->S4 did=%s restored ani=%s dnis=%s", call_uuid, dnis, row.original_ani, row.original_dnis)
        session:execute("bridge", "sofia/gateway/" .. gateway .. "/" .. row.original_dnis)
        return "forwarded_to_final"
    end

    dbh:release()
    return reject("UNSPECIFIED", "bad_stage", "unknown stage %s", tostring(stage))
end

-- Entry point when FreeSWITCH runs the script; when a test `require`s it, the
-- module is returned without running anything.
if session and freeswitch and argv then
    M.run(session, argv, freeswitch)
end

return M
