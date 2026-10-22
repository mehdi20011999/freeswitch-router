// sip_cmd.c - `sip status`: call-path state as recorded in the database.
// (The original printed a hard-coded "Status: Active, Port: 5060".)
#include <stdio.h>
#include <string.h>
#include "core/safety.h"
#include "db/database.h"

int cmd_sip(int argc, char *argv[]) {
    if (argc < 2 || (strcmp(argv[1], "status") != 0 && strcmp(argv[1], "stats") != 0)) {
        printf("Usage: sip <status|stats>\n");
        return -1;
    }
    database_t *db = get_database();
    db_result_t *r = db ? db_query(db,
        "SELECT "
        " (SELECT COUNT(*) FROM call_records WHERE status = 'routing'),"
        " (SELECT COUNT(*) FROM call_records WHERE status = 'forwarding'),"
        " (SELECT COUNT(*) FROM call_records WHERE status IN ('failed', 'timeout') "
        "    AND created_at > now() - interval '1 hour'),"
        " (SELECT COUNT(*) FROM dids WHERE in_use),"
        " (SELECT COUNT(*) FROM dids WHERE active)") : NULL;
    if (!r) {
        printf("Database unavailable\n");
        return -1;
    }
    printf("\nCall path (from the database)\n");
    printf("=============================\n");
    printf("On S3 leg (DID held):      %s\n", db_get_value(r, 0, 0));
    printf("Forwarded to S4:           %s\n", db_get_value(r, 0, 1));
    printf("Failed/timed out (1 hour): %s\n", db_get_value(r, 0, 2));
    printf("DIDs in use:               %s / %s\n", db_get_value(r, 0, 3), db_get_value(r, 0, 4));
    printf("\nLive SIP counters of a running daemon: curl http://127.0.0.1:%s/api/v1/stats\n",
           safe_env("ROUTER_HTTP_PORT", "8083"));
    db_free_result(r);
    return 0;
}
