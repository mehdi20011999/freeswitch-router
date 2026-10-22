#!/usr/bin/env python3
"""End-to-end test: real SIP over UDP against the router daemon and PostgreSQL.

Usage: tests/e2e/test_sip_flow.py ./router-asan

The daemon runs with --no-freeswitch. S1, S3 and S4 are played by UDP sockets
bound to 127.0.0.1, 127.0.0.3 and 127.0.0.4 (any 127/8 address works on Linux).

DESTRUCTIVE: the test empties the router tables. It refuses to run unless
ROUTER_TEST_DB=1 is set; point PGDATABASE at a throwaway database.
Standard library only.
"""
import concurrent.futures
import json
import os
import random
import re
import signal
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.request

ROUTER = sys.argv[1] if len(sys.argv) > 1 else "./router-asan"
SIP_PORT = int(os.environ.get("E2E_SIP_PORT", "15060"))
HTTP_PORT = int(os.environ.get("E2E_HTTP_PORT", "18083"))
S1, S3, S4, STRANGER, NEW_ORIGIN = "127.0.0.1", "127.0.0.3", "127.0.0.4", "127.0.0.9", "127.0.0.5"
DID_POOL = 20

passed = failed = 0


def check(name, cond, detail=""):
    global passed, failed
    if cond:
        passed += 1
        print(f"ok   {name}")
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def psql(sql):
    out = subprocess.run(["psql", "-X", "-q", "-At", "-v", "ON_ERROR_STOP=1", "-c", sql],
                         check=True, capture_output=True, text=True).stdout
    return [line.split("|") for line in out.splitlines() if line]


def reset_database():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    for f in ("001_schema.sql", "002_functions.sql"):
        subprocess.run(["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1", "-f", os.path.join(root, "sql", f)],
                       check=True, capture_output=True)
    psql("TRUNCATE providers, routes, dids, calls, call_records, active_calls, security_events "
         "RESTART IDENTITY CASCADE")
    psql(f"""
        INSERT INTO providers (uuid, name, host, port, role) VALUES
          ('e2e-s1', 's1', '{S1}', 5060, 'origin'),
          ('e2e-s3', 's3', '{S3}', 5060, 'intermediate'),
          ('e2e-s4', 's4', '{S4}', 5060, 'final');
        INSERT INTO routes (uuid, name, pattern, origin_provider_id, intermediate_provider_id, final_provider_id, priority)
          VALUES ('e2e-route', 'e2e', '1999[0-9]{{7}}', 1, 2, 3, 100);
        INSERT INTO dids (did, provider_id)
          SELECT '1888000' || lpad(n::text, 4, '0'), 2 FROM generate_series(1, {DID_POOL}) n;
    """)


def invite(src_ip, ani, dnis, call_id=None, extra=b"", drop=()):
    call_id = call_id or f"{random.getrandbits(64):x}@e2e"
    headers = {
        "Via": f"SIP/2.0/UDP {src_ip}:5060;branch=z9hG4bK{random.getrandbits(32):x}",
        "Max-Forwards": "70",
        "From": f"<sip:{ani}@{src_ip}>;tag={random.getrandbits(32):x}",
        "To": f"<sip:{dnis}@127.0.0.1>",
        "Call-ID": call_id,
        "CSeq": "1 INVITE",
        "Content-Length": "0",
    }
    lines = [f"INVITE sip:{dnis}@127.0.0.1:{SIP_PORT} SIP/2.0"]
    lines += [f"{k}: {v}" for k, v in headers.items() if k not in drop]
    return ("\r\n".join(lines) + "\r\n\r\n").encode() + extra, call_id


def exchange(src_ip, payload, expect=1, timeout=3.0):
    """Send one datagram from src_ip, return the decoded responses received."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind((src_ip, 0))
    s.settimeout(timeout)
    s.sendto(payload, ("127.0.0.1", SIP_PORT))
    replies = []
    try:
        while len(replies) < expect:
            data, _ = s.recvfrom(65535)
            replies.append(data.decode(errors="replace"))
    except socket.timeout:
        pass
    finally:
        s.close()
    return replies


def status(reply):
    m = re.match(r"SIP/2\.0 (\d{3})", reply or "")
    return int(m.group(1)) if m else None


def header(reply, name):
    m = re.search(rf"^{name}:\s*(.+?)\r$", reply or "", re.M | re.I)
    return m.group(1) if m else None


def http(path, method="GET"):
    req = urllib.request.Request(f"http://127.0.0.1:{HTTP_PORT}{path}", method=method)
    try:
        with urllib.request.urlopen(req, timeout=3) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read() or b"{}")


def wait_until(fn, timeout=10.0):
    end = time.time() + timeout
    while time.time() < end:
        try:
            if fn():
                return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def main():
    if os.environ.get("ROUTER_TEST_DB") != "1":
        sys.exit("refusing to run: set ROUTER_TEST_DB=1 and point PGDATABASE at a throwaway database")
    reset_database()

    env = dict(os.environ, ROUTER_SIP_PORT=str(SIP_PORT), ROUTER_SIP_BIND="127.0.0.1",
               ROUTER_HTTP_PORT=str(HTTP_PORT), ROUTER_RELOAD_SECONDS="1", ROUTER_LOG_FILE="-",
               ASAN_OPTIONS="detect_leaks=1:abort_on_error=0", UBSAN_OPTIONS="print_stacktrace=1")
    daemon = subprocess.Popen([ROUTER, "--daemon", "--no-freeswitch"], env=env,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        ready = wait_until(lambda: http("/health")[0] == 200)
        check("daemon starts and /health is 200", ready)
        if not ready:
            return

        # OPTIONS keepalive: a valid response echoing the request's CSeq.
        req = (f"OPTIONS sip:router@127.0.0.1 SIP/2.0\r\nVia: SIP/2.0/UDP {S1}:5060;branch=z9hG4bKo\r\n"
               f"From: <sip:probe@{S1}>;tag=1\r\nTo: <sip:router@127.0.0.1>\r\nCall-ID: opt-1\r\n"
               "CSeq: 7 OPTIONS\r\nContent-Length: 0\r\n\r\n").encode()
        r = exchange(S1, req)
        check("OPTIONS -> 200 with CSeq copied", r and status(r[0]) == 200 and header(r[0], "CSeq") == "7 OPTIONS", r)

        # Leg 1: S1 -> S2, redirected to S3 on a DID.
        payload, call_id = invite(S1, "+15550001111", "19995550000")
        r = exchange(S1, payload, expect=2)
        codes = [status(x) for x in r]
        check("origin INVITE -> 100 then 302", codes == [100, 302], codes)
        contact = header(r[-1], "Contact") if r else None
        m = re.match(r"<sip:(1888000\d{4})@127\.0\.0\.3:5060>", contact or "")
        check("302 Contact = DID at the intermediate", m is not None, contact)
        did = m.group(1) if m else "none"
        row = psql(f"SELECT in_use, original_ani, destination FROM dids WHERE did = '{did}'")
        check("DID holds the original ANI/DNIS", row == [["t", "+15550001111", "19995550000"]], row)
        rec = psql(f"SELECT status, stage, source_server FROM call_records WHERE call_id = '{call_id}'")
        check("call record created on leg 1", rec == [["routing", "1", S1]], rec)

        # Leg 2: S3 calls back on the DID, redirected to S4 with the original DNIS.
        payload, _ = invite(S3, "19995550000", did)
        r = exchange(S3, payload)
        check("return INVITE -> 302 to S4 with DNIS-1",
              r and status(r[0]) == 302 and header(r[0], "Contact") == f"<sip:19995550000@{S4}:5060>", r)
        check("DID released after the return", psql(f"SELECT in_use FROM dids WHERE did = '{did}'") == [["f"]])
        rec = psql(f"SELECT status, stage FROM call_records WHERE call_id = '{call_id}'")
        check("call record moved to leg 2", rec == [["forwarding", "2"]], rec)

        r = exchange(S3, invite(S3, "19995550000", did)[0])
        check("replayed return on a free DID -> 403", r and status(r[0]) == 403, r)

        r = exchange(STRANGER, invite(STRANGER, "15550001111", "19995550000")[0])
        check("unknown source address -> 403", r and status(r[0]) == 403, r)

        r = exchange(S1, invite(S1, "15550001111", "17775550000")[0])
        check("number with no route -> 404", r and status(r[0]) == 404, r)

        r = exchange(S1, invite(S1, "1'; DROP TABLE dids; --", "19995550000")[0])
        check("SQL in the caller ID -> 484, tables intact",
              r and status(r[0]) == 484 and psql("SELECT count(*) FROM dids") == [[str(DID_POOL)]], r)

        r = exchange(S1, invite(S1, "15550001111", "19995550000", drop=("From",))[0])
        check("INVITE without From -> 400", r and status(r[0]) == 400, r)

        # Garbage must not crash the server (it used to dereference NULL headers).
        rnd = random.Random(7)
        for size in (1, 17, 512, 4096, 60000):
            exchange(S1, bytes(rnd.getrandbits(8) for _ in range(size)), expect=0, timeout=0.05)
        exchange(S1, b"INVITE sip:x SIP/2.0\r\n\r\n", expect=0, timeout=0.05)
        r = exchange(S1, req)
        check("still answering after garbage datagrams", r and status(r[0]) == 200)

        # Providers added while running are picked up by the reload thread
        # (the reload used to free lists that requests were still reading).
        psql(f"""INSERT INTO providers (uuid, name, host, role) VALUES ('e2e-s5', 's5', '{NEW_ORIGIN}', 'origin');
                 INSERT INTO routes (uuid, name, pattern, origin_provider_id, intermediate_provider_id, final_provider_id)
                 SELECT 'e2e-route-5', 'e2e5', '1777[0-9]{{7}}', id, 2, 3 FROM providers WHERE name = 's5'""")
        ok = wait_until(lambda: status((exchange(NEW_ORIGIN, invite(NEW_ORIGIN, "15550005555", "17775550001")[0], expect=2) or [""])[-1]) == 302, 6)
        check("provider and route added at run time are used after reload", ok)
        psql("SELECT router_release_stale_dids(interval '0 seconds')")

        # Concurrency: more callers than DIDs, every DID handed out once.
        callers = DID_POOL + 15
        def one(i):
            rep = exchange(S1, invite(S1, f"1555{i:07d}", "19995550002")[0], expect=2, timeout=5)
            return rep[-1] if rep else ""
        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            replies = list(pool.map(one, range(callers)))
        moved = [header(x, "Contact") for x in replies if status(x) == 302]
        busy = sum(1 for x in replies if status(x) == 503)
        check(f"{callers} simultaneous calls: {DID_POOL} redirected, rest 503",
              len(moved) == DID_POOL and busy == callers - DID_POOL, (len(moved), busy))
        check("no DID given to two calls", len(set(moved)) == len(moved))

        code, body = http("/api/v1/stats")
        check("/api/v1/stats reports SIP counters and the DID pool",
              code == 200 and body.get("sip", {}).get("invites", 0) >= callers and body.get("dids_in_use") == DID_POOL,
              body)
        code, body = http("/api/v1/route?number=19995550000")
        check("/api/v1/route finds the route", code == 200 and body.get("route") == "e2e", body)
        check("/api/v1/route rejects non-numbers", http("/api/v1/route?number=1%27--")[0] == 400)
        check("unknown path -> 404, POST -> 405", http("/nope")[0] == 404 and http("/health", "POST")[0] == 405)
    finally:
        daemon.send_signal(signal.SIGTERM)
        try:
            out, err = daemon.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            daemon.kill()
            out, err = daemon.communicate()
        check("clean shutdown on SIGTERM", daemon.returncode == 0, daemon.returncode)
        sanitizer = [l for l in err.splitlines() if "Sanitizer" in l or "runtime error" in l]
        check("no AddressSanitizer/UBSan report", not sanitizer, "\n".join(sanitizer[:20]))
        if failed:
            print("---- daemon stderr (tail) ----")
            print("\n".join(err.splitlines()[-40:]))

    print(f"\n{passed} passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
