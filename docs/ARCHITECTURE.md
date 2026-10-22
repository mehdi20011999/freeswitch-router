# Architecture

This page explains how a call crosses the router, where its state lives, and why the design holds under load and under attack. Each diagram below is also available as an image in [`img/`](img), and its Mermaid source is in [`diagrams/`](diagrams).

## 1. The four call legs

```mermaid
flowchart LR
    S1["S1<br/>origin carrier<br/>10.0.0.1"]
    S2["S2 router<br/>FreeSWITCH + router + PostgreSQL<br/>10.0.0.2"]
    S3["S3<br/>intermediate<br/>10.0.0.3"]
    S4["S4<br/>final carrier<br/>10.0.0.4"]
    S1 -->|"1. ANI-1 / DNIS-1"| S2
    S2 -->|"2. ANI-2 = DNIS-1 / DID"| S3
    S3 -->|"3. ANI-2 / DID"| S2
    S2 -->|"4. ANI-1 / DNIS-1 restored"| S4
```

A call from S1 must reach S4 after passing through S3. S3 must never see the original destination, and S4 must see the original caller. S2 therefore sends the call to S3 on a temporary number (a DID) and keeps the mapping DID → (ANI-1, DNIS-1) until S3 calls back on that DID.

| Leg | Caller ID sent | Number dialled | Who decides |
|---|---|---|---|
| S1 → S2 | ANI-1 | DNIS-1 | S1 |
| S2 → S3 | **DNIS-1** (ANI-2) | **DID** | `route_handler.lua origin` + `router_allocate_did` |
| S3 → S2 | ANI-2 | DID | S3 |
| S2 → S4 | **ANI-1** | **DNIS-1** | `route_handler.lua intermediate_return` + `router_complete_return` |

## 2. Components on S2

```mermaid
flowchart LR
    SIPIN(["SIP from S1 and S3"]) --> PROF
    subgraph FS["FreeSWITCH: B2BUA and media"]
        PROF["sofia profile router<br/>one gateway per provider"] --> DP["dialplan context router<br/>source IP AND number pattern"]
        DP --> LUA["route_handler.lua<br/>input checks, one SQL call per leg"]
    end
    LUA -->|"router_allocate_did<br/>router_complete_return"| DB[("PostgreSQL<br/>tables and call-path functions")]
    LUA --> SIPOUT(["SIP to S3 and S4"])

    subgraph RT["router daemon, C"]
        CLI["CLI<br/>provider, route, did, module2"]
        GEN["config generator<br/>atomic writes, XML escaping"]
        SIP["SIP redirect server<br/>UDP 5060, optional mode"]
        API["HTTP API<br/>127.0.0.1:8083"]
    end
    CLI --> GEN
    GEN -.->|"XML files + fs_cli reload"| PROF
    CLI --> DB
    SIP --> DB
    API --> DB
```

- **FreeSWITCH** carries the media and acts as a B2BUA, which is the only place the caller ID can be rewritten. The router owns one sofia profile (`router`) and one dialplan context (`router`). It never edits FreeSWITCH's own files.
- **route_handler.lua** is deliberately thin:
  - it rejects anything that is not a phone number before any SQL is built;
  - it makes one SQL call per leg;
  - it releases its pooled DB handle **before** bridging. A bridge lasts as long as the call, and the original handler held a connection for the whole call.
- **PostgreSQL** holds every piece of state. The four call-path functions are the only code that changes DID ownership, so the Lua handler, the C redirect server and the CLI all behave the same way.
- **The router daemon** is the control plane (CLI, configuration generator, HTTP API). It can also serve as a stateless SIP redirect server for labs or setups without media.

## 3. Call sequence, including failures

```mermaid
sequenceDiagram
    autonumber
    participant S1 as S1 origin
    participant FS as S2 FreeSWITCH (Lua)
    participant DB as PostgreSQL
    participant S3 as S3 intermediate
    participant S4 as S4 final

    S1->>FS: INVITE From ANI-1, To DNIS-1
    Note over S1,FS: dialplan: source IP is S1<br/>AND DNIS-1 matches the route
    FS->>FS: ANI/DNIS must be digits (else INVITE rejected, no SQL sent)
    FS->>DB: SELECT router_allocate_did(route, uuid, ANI-1, DNIS-1, ip)
    Note over FS,DB: FOR UPDATE SKIP LOCKED on the free-DID index<br/>the DID row keeps ANI-1 and DNIS-1, call_records = routing
    alt pool empty
        DB-->>FS: NULL (security_event DID_EXHAUSTED)
        FS-->>S1: NORMAL_CIRCUIT_CONGESTION
    else DID available
        DB-->>FS: DID
        FS->>S3: INVITE From ANI-2 = DNIS-1, To DID
        alt bridge to S3 fails
            FS->>DB: router_cancel_did(DID, uuid, cause)
            Note over FS,DB: DID free again at once, call_records = failed
        end
    end

    S3->>FS: INVITE From ANI-2, To DID
    Note over FS,S3: dialplan: source IP is S3 AND To is a number
    FS->>DB: SELECT * FROM router_complete_return(DID, ip)
    alt DID not allocated (replay, fraud, late call)
        DB-->>FS: no row (security_event UNKNOWN_RETURN)
        FS-->>S3: CALL_REJECTED
    else DID allocated
        DB-->>FS: ANI-1, DNIS-1, S4 gateway of this call
        Note over FS,DB: DID freed (LRU), call_records = forwarding, stage 2
        FS->>S4: INVITE From ANI-1, To DNIS-1
    end

    Note over FS,S4: every 30 s router_release_stale_dids frees DIDs<br/>whose call never came back within 5 minutes (call_records = timeout)
```

Every failure is answered and recorded:

| Situation | Answer to the caller | Database effect |
|---|---|---|
| ANI or DNIS not a phone number | `INVITE` rejected (`INVALID_NUMBER_FORMAT` / SIP 484) | nothing. No SQL is sent |
| Source IP not a provider | not matched by the dialplan / SIP 403 | nothing |
| No route for DNIS-1 | 404 | nothing |
| DID pool empty | congestion / SIP 503 | `security_events: DID_EXHAUSTED` |
| S3 busy or unreachable | bridge failure | `router_cancel_did`: DID freed at once, call `failed` |
| Return on a DID that is not allocated (replay, late call, fraud) | `CALL_REJECTED` / SIP 403 | `security_events: UNKNOWN_RETURN` |
| S3 never calls back | none | `router_release_stale_dids` frees the DID after 5 min, call `timeout` |

## 4. Data model

```mermaid
erDiagram
    providers ||--o{ routes : "origin / intermediate / final"
    providers ||--o{ dids : "owns (NULL = shared pool)"
    routes ||--o{ call_records : "routed by"
    providers ||--o{ call_records : "S1 / S3 / S4 of the call"
    dids ||--o| call_records : "carries (by call_id)"

    providers {
        serial id PK
        varchar uuid UK "FreeSWITCH gateway name"
        varchar name UK
        varchar host "matched against network_addr"
        int port
        varchar role "origin | intermediate | final | general"
        varchar auth_type "ip | userpass"
        text auth_data
        int capacity
        bool active
    }
    routes {
        serial id PK
        varchar uuid UK
        varchar name
        varchar pattern "regex on DNIS, validated"
        int origin_provider_id FK
        int intermediate_provider_id FK
        int final_provider_id FK
        int priority "highest wins"
        bool active
    }
    dids {
        serial id PK
        varchar did UK
        int provider_id FK
        bool in_use
        varchar original_ani "ANI-1 while in use"
        varchar destination "DNIS-1 while in use"
        varchar call_id
        timestamp allocated_at
        timestamp released_at "LRU order"
        bool active
    }
    call_records {
        serial id PK
        varchar call_id
        int route_id FK
        varchar original_ani
        varchar original_dnis
        varchar assigned_did
        varchar source_server
        varchar status "routing | forwarding | failed | timeout"
        int stage "1 = S3 leg, 2 = S4 leg"
        varchar failure_reason
    }
    security_events {
        serial id PK
        varchar event_type "DID_EXHAUSTED | UNKNOWN_RETURN | ..."
        varchar severity
        varchar call_id
        varchar source_ip
        text details
    }
```

`sql/001_schema.sql` is the only schema file. It is idempotent (`IF NOT EXISTS`, `ADD COLUMN IF NOT EXISTS`, `ON CONFLICT`), so upgrades are just a rerun. The original repository had nine schema files that disagreed with each other, and some tables were created by the binary at every start.

### Why allocation is fast and safe

```sql
SELECT * FROM dids
WHERE in_use = false AND active = true AND provider_id IS NULL
ORDER BY released_at NULLS FIRST, id
LIMIT 1
FOR UPDATE SKIP LOCKED;
```

- **Partial indexes** (`dids_free_shared_idx`, `dids_free_provider_idx`) contain only free DIDs, already in the order they should be handed out. The planner reads one index entry, whatever the pool size. The original `ORDER BY RANDOM()` sorted every free DID on every call.
- **Two indexes instead of one.** `provider_id = X` fixes the leading index column, so the rows come out already ordered. `provider_id IS NULL` does not, and would force a sort. The provider's own DIDs are tried first, then the shared pool.
- **`FOR UPDATE SKIP LOCKED`.** Concurrent calls lock different rows without waiting for each other. The original code read a DID, then updated it in a second statement. Between the two, another call could take the same DID: 16 of 400 calls in the benchmark.
- **Least recently released first.** A DID is not handed out again immediately, which leaves time for late or retransmitted return calls from its previous use to arrive and be rejected.

The benchmark is `tests/bench/did_alloc_bench.sh`: 50,000 DIDs, 8 clients, allocate + free.

| | calls/s | ms per call | DIDs held by two calls |
|---|---|---|---|
| original logic | 213 | 37.6 | 16 / 400 |
| `router_allocate_did` | **2,293** | **3.5** | **0 / 400** |

## 5. Life cycles

DID:

```mermaid
flowchart TB
    NEW(["did add / did import"]) --> FREE
    FREE["<b>Free</b><br/>in partial index,<br/>least recently released first"]
    INUSE["<b>In use</b><br/>holds ANI-1, DNIS-1, call id"]
    INACTIVE["<b>Inactive</b><br/>active = false"]
    FREE -->|"router_allocate_did()<br/>FOR UPDATE SKIP LOCKED"| INUSE
    INUSE -->|"router_complete_return(): S3 called back<br/>router_cancel_did(): bridge to S3 failed<br/>router_release_stale_dids(): no return in 5 min<br/>did release: operator"| FREE
    FREE <-->|"active true / false"| INACTIVE
```

Call record (`call_records.status`):

```mermaid
flowchart LR
    START(["INVITE from S1<br/>router_allocate_did()"]) --> ROUTING["<b>routing</b><br/>stage 1, DID held,<br/>call on the S3 leg"]
    ROUTING -->|"router_complete_return()<br/>S3 called back"| FWD["<b>forwarding</b><br/>stage 2, sent to S4"]
    ROUTING -->|"router_cancel_did()<br/>S3 busy or unreachable,<br/>or did release"| FAILED["<b>failed</b><br/>failure_reason set"]
    ROUTING -->|"router_release_stale_dids()<br/>no return within 5 min"| TIMEOUT["<b>timeout</b>"]
```

## 6. Configuration generation

```mermaid
flowchart LR
    OP(["operator"]) -->|"router provider add / route add"| CLI["router CLI"]
    CLI -->|"validated, bound parameters"| DB[("PostgreSQL")]
    CLI --> GEN["fs_xml_generator"]
    GEN -->|"read active providers and routes"| DB
    GEN --> F1["sip_profiles/router.xml"]
    GEN --> F2["sip_profiles/router/&lt;uuid&gt;.xml<br/>one gateway per provider"]
    GEN --> F3["dialplan/router.xml + router/route_N.xml"]
    GEN --> F4["scripts/route_handler.lua"]
    GEN --> F5["router_vars.xml (router_db_dsn, 0640)"]
    GEN -->|"stale files pruned"| F2
    F1 & F2 & F3 & F4 & F5 --> RL["fs_cli -x reloadxml<br/>sofia profile router rescan"]
    RL --> FS["FreeSWITCH"]
```

- Every file is written to `name.tmp` and then renamed, so FreeSWITCH never reads a half-written file.
- Every value that comes from the database is either validated (uuid, host, number pattern) or XML-escaped. A route pattern such as `1"/><x` is refused at `route add` and again at generation time.
- The two dialplan conditions of an extension (source IP and number) are ANDed. The original used `break="never"` on the IP condition, which let any source through.
- Gateways of deleted or deactivated providers are pruned on reload.
- Files that hold secrets (`router_vars.xml`, and gateways with a SIP password) are written with mode 0640.

[`examples/freeswitch/`](examples/freeswitch) contains exactly what the generator produces from `sql/seed_demo.sql`.

## 7. Concurrency inside the C daemon

| Shared data | Protection |
|---|---|
| Providers and routes in the SIP server | Arrays rebuilt from the database, then swapped under a mutex. Request handlers copy the entries they need while holding the lock, so a reload never frees memory a request is reading. The original freed linked lists that handlers were still using. |
| SIP / router counters | C11 atomics |
| Validation records | The caller holds the mutex for the whole time it uses a record. The hang-up helper takes no lock, so the validation thread can call it. The original re-locked a non-recursive mutex, which deadlocks. |
| Stop flags | `atomic_bool`. The threads sleep in short slices, so SIGTERM returns within about a second. |
| PostgreSQL connection | One connection behind a mutex. This is a known limit, see the README. |

## 8. Trust boundaries

| Input | From | Checks |
|---|---|---|
| SIP headers (Call-ID, From, To) | network, spoofable over UDP | Parsed into separate bounded buffers. The URI user part must be a number. Only bound SQL parameters are used |
| Caller ID / destination in Lua | network via FreeSWITCH | `^%+?%d+$`, at most 33 characters, quoted as well, and checked again by `router_is_number()` inside SQL |
| Call UUID passed to `fs_cli` | network | Token format checked, then passed as an argument to `posix_spawnp`, never through a shell |
| CLI arguments | operator | Names, hosts and patterns validated. SQL uses bound parameters |
| Database values written into XML | operator via the database | Validated or XML-escaped |
| HTTP API | local host | GET only, read-only, bound to 127.0.0.1 by default |

## 9. CI

```mermaid
flowchart LR
    PUSH(["push / pull request"]) --> B["build: gcc and clang<br/>-Werror, hardening check"]
    B --> U["unit tests<br/>ASan + UBSan, 20k fuzz rounds"]
    PUSH --> L["lint<br/>cppcheck, shellcheck, luac"]
    B --> I["integration (PostgreSQL 16)"]
    I --> I1["CLI + FreeSWITCH XML (xmllint)"]
    I --> I2["SQL concurrency: 40 callers, 10 DIDs"]
    I --> I3["Lua handler on real SQL"]
    I --> I4["SIP over UDP end-to-end<br/>daemon under ASan"]
    I --> I5["benchmark old vs new<br/>job summary"]
    PUSH --> C["CodeQL security-extended<br/>+ weekly"]
    TAG(["tag v*"]) --> R["release: build, test,<br/>tarball + sha256"]
```
