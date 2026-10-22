// module2_cmd.c - inspect and dry-run the ANI/DNIS/DID transformation ("Module 2").
//
// The original command queried tables and functions that no schema created
// (call_transformations, s1_origin_calls, apply_call_transformation, ...). It
// now works on the real tables and calls the same SQL functions as the
// FreeSWITCH Lua handler.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "commands/module2_cmd.h"
#include "core/safety.h"
#include "db/database.h"

static void usage(void) {
    printf("Usage: module2 <command>\n");
    printf("  status                       DID pool and call outcomes (last hour)\n");
    printf("  test <ani> <dnis> [route_id] dry run of both legs (rolled back)\n");
    printf("  monitor                      DIDs currently carrying a call\n");
    printf("  validate <did>               who holds this DID\n");
}

int cmd_module2(int argc, char *argv[]) {
    if (argc < 2) {
        usage();
        return -1;
    }
    if (strcmp(argv[1], "status") == 0) return cmd_module2_status(argc, argv);
    if (strcmp(argv[1], "test") == 0) return cmd_module2_test(argc, argv);
    if (strcmp(argv[1], "monitor") == 0) return cmd_module2_monitor(argc, argv);
    if (strcmp(argv[1], "validate") == 0) return cmd_module2_validate(argc, argv);
    usage();
    return -1;
}

int cmd_module2_status(int argc, char *argv[]) {
    database_t *db = get_database();
    db_result_t *r = db_query(db,
        "SELECT (SELECT COUNT(*) FROM dids WHERE active),"
        "       (SELECT COUNT(*) FROM dids WHERE active AND in_use),"
        "       COUNT(*) FILTER (WHERE status = 'forwarding'),"
        "       COUNT(*) FILTER (WHERE status = 'routing'),"
        "       COUNT(*) FILTER (WHERE status = 'failed'),"
        "       COUNT(*) FILTER (WHERE status = 'timeout'),"
        "       COALESCE(round(avg(extract(epoch FROM updated_at - created_at)) "
        "                FILTER (WHERE stage = 2)::numeric, 2), 0) "
        "FROM call_records WHERE created_at > now() - interval '1 hour'");
    if (!r) {
        printf("Database unavailable\n");
        return -1;
    }
    printf("\nDID pool:   %s in use / %s active\n", db_get_value(r, 0, 1), db_get_value(r, 0, 0));
    printf("Last hour:  %s forwarded to S4, %s on the S3 leg, %s failed, %s timed out\n",
           db_get_value(r, 0, 2), db_get_value(r, 0, 3), db_get_value(r, 0, 4), db_get_value(r, 0, 5));
    printf("Avg S1->S3->S2 turnaround: %s s\n", db_get_value(r, 0, 6));
    db_free_result(r);
    return 0;
}

int cmd_module2_test(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage: module2 test <ani> <dnis> [route_id]\n");
        return -1;
    }
    const char *ani = argv[2], *dnis = argv[3];
    if (!safe_is_number(ani) || !safe_is_number(dnis)) {
        printf("ANI and DNIS must be phone numbers\n");
        return -1;
    }
    database_t *db = get_database();

    // Pick the route the dialplan would pick, unless one is given.
    char route_id[16] = "";
    if (argc > 4) {
        snprintf(route_id, sizeof(route_id), "%s", argv[4]);
    } else {
        const char *p[] = {dnis};
        db_result_t *r = db_query_params(db,
            "SELECT id FROM routes WHERE active AND $1 ~ ('^(' || pattern || ')$') "
            "ORDER BY priority DESC, id LIMIT 1", 1, p);
        if (r) snprintf(route_id, sizeof(route_id), "%s", db_get_value(r, 0, 0));
        db_free_result(r);
    }
    if (!route_id[0]) {
        printf("No route matches %s\n", dnis);
        return -1;
    }

    char call_id[64];
    snprintf(call_id, sizeof(call_id), "dry-run-%ld-%d", (long)time(NULL), (int)getpid());

    // Everything happens inside a transaction that is rolled back.
    db_free_result(db_query(db, "BEGIN"));
    const char *p1[] = {route_id, call_id, ani, dnis};
    db_result_t *a = db_query_params(db, "SELECT router_allocate_did($1::int, $2, $3, $4, 'cli-dry-run')", 4, p1);
    char did[64] = "";
    if (a) snprintf(did, sizeof(did), "%s", db_get_value(a, 0, 0));
    db_free_result(a);

    int rc = 0;
    if (!did[0]) {
        printf("Route %s: no DID available, the call would get 503/congestion\n", route_id);
        rc = -1;
    } else {
        const char *p2[] = {did};
        db_result_t *b = db_query_params(db,
            "SELECT original_ani, original_dnis, final_gateway FROM router_complete_return($1, 'cli-dry-run')", 1, p2);
        printf("\nRoute %s, dry run (nothing is kept):\n", route_id);
        printf("  S1 -> S2   ANI-1 %-16s DNIS-1 %s\n", ani, dnis);
        printf("  S2 -> S3   ANI-2 %-16s DNIS-2 %s   (DID taken from the pool)\n", dnis, did);
        printf("  S3 -> S2   calls back on DID %s\n", did);
        if (b) {
            printf("  S2 -> S4   ANI   %-16s DNIS   %s   (restored, DID freed, gateway %s)\n",
                   db_get_value(b, 0, 0), db_get_value(b, 0, 1), db_get_value(b, 0, 2));
        } else {
            printf("  S2 -> S4   FAILED: the DID mapping was not found\n");
            rc = -1;
        }
        db_free_result(b);
    }
    db_free_result(db_query(db, "ROLLBACK"));
    return rc;
}

int cmd_module2_monitor(int argc, char *argv[]) {
    db_result_t *r = db_query(get_database(),
        "SELECT did, original_ani, destination, call_id, "
        "       extract(epoch FROM now() - allocated_at)::int "
        "FROM dids WHERE in_use ORDER BY allocated_at LIMIT 50");
    if (!r) {
        printf("No DID is carrying a call\n");
        return 0;
    }
    printf("\n%-16s %-16s %-16s %-8s %s\n", "DID", "ANI-1", "DNIS-1", "held(s)", "call");
    for (int i = 0; i < r->num_rows; i++) {
        printf("%-16s %-16s %-16s %-8s %s\n", db_get_value(r, i, 0), db_get_value(r, i, 1),
               db_get_value(r, i, 2), db_get_value(r, i, 4), db_get_value(r, i, 3));
    }
    db_free_result(r);
    return 0;
}

int cmd_module2_validate(int argc, char *argv[]) {
    if (argc < 3 || !safe_is_number(argv[2])) {
        printf("Usage: module2 validate <did>\n");
        return -1;
    }
    const char *p[] = {argv[2]};
    db_result_t *r = db_query_params(get_database(),
        "SELECT d.in_use, COALESCE(d.original_ani, ''), COALESCE(d.destination, ''), "
        "       COALESCE(d.call_id, ''), COALESCE(cr.source_server, ''), COALESCE(cr.status, '') "
        "FROM dids d LEFT JOIN call_records cr ON cr.call_id = d.call_id "
        "WHERE d.did = $1 ORDER BY cr.id DESC LIMIT 1", 1, p);
    if (!r) {
        printf("DID %s is not in the pool\n", argv[2]);
        return -1;
    }
    if (strcmp(db_get_value(r, 0, 0), "t") != 0) {
        printf("DID %s is free: a return call on it would be rejected\n", argv[2]);
    } else {
        printf("DID %s carries call %s\n", argv[2], db_get_value(r, 0, 3));
        printf("  original ANI %s, DNIS %s, from %s, status %s\n", db_get_value(r, 0, 1),
               db_get_value(r, 0, 2), db_get_value(r, 0, 4), db_get_value(r, 0, 5));
    }
    db_free_result(r);
    return 0;
}
