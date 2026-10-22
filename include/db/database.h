#ifndef DATABASE_H
#define DATABASE_H

#include <stdbool.h>

// PostgreSQL access layer (src/db/database_pg.c). One connection, guarded by a
// mutex; reconnects automatically.
typedef struct database database_t;

typedef struct db_result {
    int num_rows;
    int num_cols;
    char **data;      // num_rows * num_cols strings; SQL NULL is returned as ""
    char **columns;
} db_result_t;

// Opaque prepared statement ("?" placeholders, bound as text).
typedef struct db_stmt db_stmt_t;

// conninfo may be NULL: ROUTER_DB_CONNECTION is used, else the standard PG* variables.
database_t* db_init(const char *conninfo);
void db_close(database_t *db);

// Returns NULL on error AND when the statement returns no rows.
db_result_t* db_query(database_t *db, const char *query);
db_result_t* db_query_params(database_t *db, const char *query, int nparams, const char *const *values);
void db_free_result(db_result_t *result);
const char* db_get_value(db_result_t *result, int row, int col);

db_stmt_t* db_prepare(database_t *db, const char *sql);
int db_bind_string(db_stmt_t *stmt, int index, const char *value);
int db_bind_int(db_stmt_t *stmt, int index, int value);
int db_execute(db_stmt_t *stmt);
db_result_t* db_execute_query(db_stmt_t *stmt);
void db_finalize(db_stmt_t *stmt);

database_t* get_database(void);

#define ERR_SUCCESS 0
#define ERR_FAILURE -1

#endif
