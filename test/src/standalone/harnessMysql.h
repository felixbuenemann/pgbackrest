/***********************************************************************************************************************************
libmariadb / libmysqlclient Test Harness

Lightweight scripted shim for the subset of libmariadb that src/mysql/client.c uses. Lets standalone tests exercise the hot
backup orchestrator (and any future MysqlClient-driven code) without needing a live mysqld.

Pattern is similar to test/src/common/harnessPq.c but simpler: one script entry describes one full query exchange (the SQL the
caller will issue, the result rows we return, the error state). Connection scripting is implicit — mysql_init/mysql_real_connect
always succeed unless the very first script entry has connectFail set.

Linker semantics: this file defines mysql_init, mysql_real_connect, etc. with the SAME names as libmariadb. When linked into a
test binary alongside libmariadb.so, the object-file symbol wins over the shared-library symbol, so the test's calls to client.c
go through the shim transparently.

Usage:

  HrnMysqlScript script[] = {
      HRN_MYSQL_VERSION("8.0.36", 80036),
      HRN_MYSQL_QUERY_ONE("SELECT @@server_uuid", "8c0fd6f0-bf8f-11ee-9821-0242ac120002"),
      HRN_MYSQL_QUERY_NONE("LOCK INSTANCE FOR BACKUP"),
      HRN_MYSQL_QUERY_NONE("UNLOCK INSTANCE"),
  };
  hrnMysqlScriptSet(script, LENGTH_OF(script));

  // ... run code under test ...

  hrnMysqlScriptVerifyComplete();
***********************************************************************************************************************************/
#ifndef TEST_STANDALONE_HARNESS_MYSQL_H
#define TEST_STANDALONE_HARNESS_MYSQL_H

#include <mysql.h>
#include <stdbool.h>
#include <stddef.h>

/***********************************************************************************************************************************
Maximum dimensions of canned results — bounded so the script can sit in static storage
***********************************************************************************************************************************/
#define HRN_MYSQL_MAX_COLS                                          8
#define HRN_MYSQL_MAX_ROWS                                          16
#define HRN_MYSQL_MAX_SCRIPT                                        128

/***********************************************************************************************************************************
One scripted exchange
***********************************************************************************************************************************/
typedef struct HrnMysqlScript
{
    // What the caller is expected to do
    const char *expectedSql;                                            // NULL means "no query for this entry" (e.g. version probe)

    // If true, mysql_real_query returns non-zero (caller sees a DbQueryError). Otherwise it returns 0 (success).
    bool sqlFail;
    unsigned int sqlErrno;                                              // Surfaced via mysql_errno when sqlFail is true
    const char *sqlErrMsg;                                              // Surfaced via mysql_error

    // Result shape — used by mysql_store_result + friends. If rowCount == 0 the result set has no rows (DDL/DML pattern, but
    // mysql_store_result still returns non-NULL for SELECTs returning zero rows).
    bool noResult;                                                      // true → mysql_store_result returns NULL + field_count == 0
    unsigned int fieldCount;
    int fieldTypes[HRN_MYSQL_MAX_COLS];                                 // MYSQL_TYPE_* per column; default = MYSQL_TYPE_VAR_STRING

    unsigned int rowCount;
    const char *rows[HRN_MYSQL_MAX_ROWS][HRN_MYSQL_MAX_COLS];           // Cell values as C strings (NULL = SQL NULL)

    // For the very first script entry only — controls handshake outcome
    const char *serverVersionStr;                                       // mysql_get_server_info — defaults to "8.0.36"
    unsigned int serverVersionNum;                                      // mysql_get_server_version — defaults to 80036
    bool connectFail;                                                   // mysql_real_connect returns NULL
} HrnMysqlScript;

/***********************************************************************************************************************************
Convenience macros for common script entries
***********************************************************************************************************************************/
// Connection handshake + version detection — first entry sets vendor/version reporting
#define HRN_MYSQL_VERSION(versionStr, versionNum)                                                                                  \
    {.serverVersionStr = versionStr, .serverVersionNum = versionNum}

// A query that returns one row of one string column
#define HRN_MYSQL_QUERY_ONE(sql, value)                                                                                            \
    {.expectedSql = sql, .fieldCount = 1, .rowCount = 1, .rows = {{value}}}

// A query that returns a Variables-like result (Variable_name, Value) — what SHOW VARIABLES LIKE returns
#define HRN_MYSQL_QUERY_VAR(sql, varName, value)                                                                                   \
    {.expectedSql = sql, .fieldCount = 2, .rowCount = 1, .rows = {{varName, value}}}

// A query expected to return no rows (DDL/DML — LOCK INSTANCE FOR BACKUP, BACKUP STAGE *, etc.)
#define HRN_MYSQL_QUERY_NONE(sql)                                                                                                  \
    {.expectedSql = sql, .noResult = true}

// SHOW MASTER STATUS — 5 columns: File, Position(BIGINT), Binlog_Do_DB, Binlog_Ignore_DB, Executed_Gtid_Set.
// Position's MYSQL_TYPE_LONGLONG is critical so client.c packs it as i64, matching hotBackup.c's pckReadI64P expectation.
#define HRN_MYSQL_QUERY_MASTER_STATUS(file, position, gtidSet)                                                                     \
    {.expectedSql = "SHOW MASTER STATUS", .fieldCount = 5,                                                                         \
     .fieldTypes = {MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_LONGLONG, MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_VAR_STRING, MYSQL_TYPE_VAR_STRING},\
     .rowCount = 1, .rows = {{file, position, "", "", gtidSet}}}

// SHOW MASTER STATUS returning no rows (log_bin OFF case)
#define HRN_MYSQL_QUERY_MASTER_STATUS_EMPTY()                                                                                      \
    {.expectedSql = "SHOW MASTER STATUS", .fieldCount = 5, .rowCount = 0}

/***********************************************************************************************************************************
Script management
***********************************************************************************************************************************/
// Set the script. Throws AssertError if a previous script hasn't been fully consumed.
void hrnMysqlScriptSet(const HrnMysqlScript *script, unsigned int scriptSize);

// Throw AssertError if the script still has unconsumed entries (asserts every expected call actually happened).
void hrnMysqlScriptVerifyComplete(void);

// Reset state — useful in test teardown if the test path threw before consuming the script.
void hrnMysqlScriptReset(void);

#endif
