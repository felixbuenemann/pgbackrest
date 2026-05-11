/***********************************************************************************************************************************
libmariadb / libmysqlclient Test Harness — implementation

Provides scripted implementations of mysql_init, mysql_real_connect, mysql_real_query, mysql_store_result, and friends. When
linked into a test executable alongside libmariadb.so, the object-file symbols here win over the shared-library ones, so
src/mysql/client.c's libmariadb calls flow through this shim transparently.
***********************************************************************************************************************************/
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harnessMysql.h"

/***********************************************************************************************************************************
Module-local state. One script with up to HRN_MYSQL_MAX_SCRIPT entries; current index advances as each call consumes an entry
that has a matching expectedSql. Connection bookkeeping is independent — a single MYSQL handle is "open" between mysql_init and
mysql_close, regardless of script.
***********************************************************************************************************************************/
typedef struct HrnMysqlState
{
    HrnMysqlScript script[HRN_MYSQL_MAX_SCRIPT];
    unsigned int scriptSize;
    unsigned int scriptIdx;

    // Fake connection + result handles. We hand the same pointers back to client.c — libmariadb treats them as opaque.
    MYSQL fakeHandle;                                                   // Address returned by mysql_init / mysql_real_connect
    MYSQL_RES fakeResult;                                               // Address returned by mysql_store_result
    bool connectionOpen;                                                // Set on mysql_real_connect, cleared on mysql_close

    // Set when mysql_store_result + mysql_field_count have been called and we're done with the current entry. The next
    // mysql_real_query call advances past it. Lets mysql_field_count read the CURRENT entry's count (0 for noResult) instead
    // of accidentally peeking at the next entry.
    bool currentEntryConsumed;

    // Per-query iteration state
    unsigned int currentRow;                                            // Next row mysql_fetch_row will return
    const char *currentRowCells[HRN_MYSQL_MAX_COLS];                    // Pointers we hand to mysql_fetch_row's caller
    unsigned long currentRowLengths[HRN_MYSQL_MAX_COLS];                // Returned by mysql_fetch_lengths
    MYSQL_FIELD currentFields[HRN_MYSQL_MAX_COLS];                      // Returned by mysql_fetch_fields
    char fieldName[HRN_MYSQL_MAX_COLS][32];                             // Backing storage for synthesized field names

    // Server version info supplied via HRN_MYSQL_VERSION on the first script entry
    char serverVersion[64];
    unsigned int serverVersionNum;

    // Surfaced via mysql_errno / mysql_error
    unsigned int lastErrno;
    char lastErrMsg[256];

    // For unconsumed-script asserts
    char failMsg[512];
} HrnMysqlState;

static HrnMysqlState hrnMysql = {0};

/***********************************************************************************************************************************
Helper: fail loudly if the script ended or doesn't match
***********************************************************************************************************************************/
static void
hrnMysqlAssertExpect(const char *const sql)
{
    if (hrnMysql.scriptIdx >= hrnMysql.scriptSize)
    {
        snprintf(hrnMysql.failMsg, sizeof(hrnMysql.failMsg), "MYSQL SHIM: script exhausted but got query: %s", sql);
        fprintf(stderr, "%s\n", hrnMysql.failMsg);
        abort();
    }

    const HrnMysqlScript *const entry = &hrnMysql.script[hrnMysql.scriptIdx];

    if (entry->expectedSql == NULL || strcmp(entry->expectedSql, sql) != 0)
    {
        snprintf(
            hrnMysql.failMsg, sizeof(hrnMysql.failMsg),
            "MYSQL SHIM [%u]: expected SQL '%s' but got '%s'",
            hrnMysql.scriptIdx, entry->expectedSql != NULL ? entry->expectedSql : "(no-query entry)", sql);
        fprintf(stderr, "%s\n", hrnMysql.failMsg);
        abort();
    }
}

/**********************************************************************************************************************************/
void
hrnMysqlScriptSet(const HrnMysqlScript *const script, const unsigned int scriptSize)
{
    if (hrnMysql.scriptIdx != hrnMysql.scriptSize)
    {
        fprintf(
            stderr, "MYSQL SHIM: previous script not fully consumed (%u of %u entries used)\n",
            hrnMysql.scriptIdx, hrnMysql.scriptSize);
        abort();
    }

    if (scriptSize > HRN_MYSQL_MAX_SCRIPT)
    {
        fprintf(stderr, "MYSQL SHIM: script size %u exceeds HRN_MYSQL_MAX_SCRIPT (%u)\n", scriptSize, HRN_MYSQL_MAX_SCRIPT);
        abort();
    }

    memcpy(hrnMysql.script, script, scriptSize * sizeof(HrnMysqlScript));
    hrnMysql.scriptSize = scriptSize;
    hrnMysql.scriptIdx = 0;

    // Apply version info from the first entry if it has any
    const char *const ver = script[0].serverVersionStr != NULL ? script[0].serverVersionStr : "8.0.36";
    snprintf(hrnMysql.serverVersion, sizeof(hrnMysql.serverVersion), "%s", ver);
    hrnMysql.serverVersionNum = script[0].serverVersionNum != 0 ? script[0].serverVersionNum : 80036;
}

/**********************************************************************************************************************************/
void
hrnMysqlScriptVerifyComplete(void)
{
    // The final entry is normally consumed but not advanced (advance happens at next real_query). Account for that.
    const unsigned int effectiveIdx = hrnMysql.currentEntryConsumed ? hrnMysql.scriptIdx + 1 : hrnMysql.scriptIdx;

    if (effectiveIdx != hrnMysql.scriptSize)
    {
        // Find the next entry with an expectedSql (skip version-only entries)
        const char *nextSql = "(version-only entry)";
        for (unsigned int i = hrnMysql.scriptIdx; i < hrnMysql.scriptSize; i++)
        {
            if (hrnMysql.script[i].expectedSql != NULL)
            {
                nextSql = hrnMysql.script[i].expectedSql;
                break;
            }
        }
        fprintf(
            stderr, "MYSQL SHIM: %u script entries unconsumed (next expected: '%s')\n",
            hrnMysql.scriptSize - hrnMysql.scriptIdx, nextSql);
        abort();
    }
}

/**********************************************************************************************************************************/
void
hrnMysqlScriptReset(void)
{
    memset(&hrnMysql, 0, sizeof(hrnMysql));
}

/***********************************************************************************************************************************
libmariadb function shims
***********************************************************************************************************************************/
MYSQL *
mysql_init(MYSQL *const handle)
{
    (void)handle;
    hrnMysql.connectionOpen = false;
    return &hrnMysql.fakeHandle;
}

int
mysql_options(MYSQL *const handle, enum mysql_option option, const void *arg)
{
    (void)handle;
    (void)option;
    (void)arg;
    return 0;
}

int
mysql_optionsv(MYSQL *const handle, enum mysql_option option, ...)
{
    (void)handle;
    (void)option;
    return 0;
}

MYSQL *
mysql_real_connect(
    MYSQL *const handle, const char *host, const char *user, const char *passwd, const char *db, unsigned int port,
    const char *unix_socket, unsigned long client_flag)
{
    (void)handle;
    (void)host;
    (void)user;
    (void)passwd;
    (void)db;
    (void)port;
    (void)unix_socket;
    (void)client_flag;

    // Honor connectFail on the first script entry if present
    if (hrnMysql.scriptSize > 0 && hrnMysql.script[0].connectFail)
    {
        hrnMysql.lastErrno = 2002;                                      // CR_CONNECTION_ERROR
        snprintf(hrnMysql.lastErrMsg, sizeof(hrnMysql.lastErrMsg), "scripted connection failure");
        return NULL;
    }

    hrnMysql.connectionOpen = true;
    return &hrnMysql.fakeHandle;
}

void
mysql_close(MYSQL *const handle)
{
    (void)handle;
    hrnMysql.connectionOpen = false;
}

unsigned int
mysql_errno(MYSQL *const handle)
{
    (void)handle;
    return hrnMysql.lastErrno;
}

const char *
mysql_error(MYSQL *const handle)
{
    (void)handle;
    return hrnMysql.lastErrMsg;
}

char *
mysql_get_server_info(MYSQL *const handle)
{
    (void)handle;
    return hrnMysql.serverVersion;
}

unsigned long
mysql_get_server_version(MYSQL *const handle)
{
    (void)handle;
    return hrnMysql.serverVersionNum;
}

/***********************************************************************************************************************************
Query path: real_query → store_result → fetch_fields / num_rows / num_fields → fetch_row* → fetch_lengths → free_result
***********************************************************************************************************************************/
// Advance past any leading version-only entries so the next real call lands on an SQL entry
static void
hrnMysqlAdvancePastVersionEntries(void)
{
    while (hrnMysql.scriptIdx < hrnMysql.scriptSize && hrnMysql.script[hrnMysql.scriptIdx].expectedSql == NULL)
        hrnMysql.scriptIdx++;
}

int
mysql_real_query(MYSQL *const handle, const char *const stmt, unsigned long length)
{
    (void)handle;
    (void)length;

    // Advance past the prior entry if it's fully consumed (mysql_store_result + mysql_field_count flow already completed)
    if (hrnMysql.currentEntryConsumed)
    {
        hrnMysql.scriptIdx++;
        hrnMysql.currentEntryConsumed = false;
    }

    hrnMysqlAdvancePastVersionEntries();
    hrnMysqlAssertExpect(stmt);

    const HrnMysqlScript *const entry = &hrnMysql.script[hrnMysql.scriptIdx];

    if (entry->sqlFail)
    {
        hrnMysql.lastErrno = entry->sqlErrno != 0 ? entry->sqlErrno : 1064;
        snprintf(
            hrnMysql.lastErrMsg, sizeof(hrnMysql.lastErrMsg), "%s",
            entry->sqlErrMsg != NULL ? entry->sqlErrMsg : "scripted query failure");
        return 1;
    }

    hrnMysql.lastErrno = 0;
    hrnMysql.lastErrMsg[0] = '\0';
    hrnMysql.currentRow = 0;
    return 0;
}

int
mysql_query(MYSQL *const handle, const char *const stmt)
{
    return mysql_real_query(handle, stmt, (unsigned long)strlen(stmt));
}

MYSQL_RES *
mysql_store_result(MYSQL *const handle)
{
    (void)handle;

    if (hrnMysql.scriptIdx >= hrnMysql.scriptSize)
        return NULL;

    const HrnMysqlScript *const entry = &hrnMysql.script[hrnMysql.scriptIdx];

    if (entry->noResult)
    {
        // Don't advance the index yet — client.c calls mysql_field_count next to confirm "no result set was expected"
        // (returns 0). The next mysql_real_query advances past us.
        hrnMysql.currentEntryConsumed = true;
        return NULL;
    }

    // Synthesize field metadata once per query
    for (unsigned int i = 0; i < entry->fieldCount && i < HRN_MYSQL_MAX_COLS; i++)
    {
        snprintf(hrnMysql.fieldName[i], sizeof(hrnMysql.fieldName[i]), "c%u", i);
        hrnMysql.currentFields[i] = (MYSQL_FIELD){0};
        hrnMysql.currentFields[i].name = hrnMysql.fieldName[i];
        hrnMysql.currentFields[i].type =
            entry->fieldTypes[i] != 0 ? (enum enum_field_types)entry->fieldTypes[i] : MYSQL_TYPE_VAR_STRING;
    }

    hrnMysql.currentRow = 0;
    return &hrnMysql.fakeResult;
}

unsigned int
mysql_field_count(MYSQL *const handle)
{
    (void)handle;

    if (hrnMysql.scriptIdx >= hrnMysql.scriptSize)
        return 0;

    return hrnMysql.script[hrnMysql.scriptIdx].fieldCount;
}

my_ulonglong
mysql_num_rows(MYSQL_RES *const result)
{
    (void)result;
    return (my_ulonglong)hrnMysql.script[hrnMysql.scriptIdx].rowCount;
}

unsigned int
mysql_num_fields(MYSQL_RES *const result)
{
    (void)result;
    return hrnMysql.script[hrnMysql.scriptIdx].fieldCount;
}

MYSQL_ROW
mysql_fetch_row(MYSQL_RES *const result)
{
    (void)result;

    const HrnMysqlScript *const entry = &hrnMysql.script[hrnMysql.scriptIdx];

    if (hrnMysql.currentRow >= entry->rowCount)
        return NULL;

    for (unsigned int i = 0; i < entry->fieldCount && i < HRN_MYSQL_MAX_COLS; i++)
    {
        hrnMysql.currentRowCells[i] = entry->rows[hrnMysql.currentRow][i];
        hrnMysql.currentRowLengths[i] = hrnMysql.currentRowCells[i] != NULL ? (unsigned long)strlen(hrnMysql.currentRowCells[i]) : 0;
    }

    hrnMysql.currentRow++;
    return (MYSQL_ROW)hrnMysql.currentRowCells;
}

unsigned long *
mysql_fetch_lengths(MYSQL_RES *const result)
{
    (void)result;
    return hrnMysql.currentRowLengths;
}

MYSQL_FIELD *
mysql_fetch_fields(MYSQL_RES *const result)
{
    (void)result;
    return hrnMysql.currentFields;
}

void
mysql_free_result(MYSQL_RES *const result)
{
    (void)result;
    // Mark the entry as fully consumed; the next mysql_real_query advances past it
    hrnMysql.currentEntryConsumed = true;
    hrnMysql.currentRow = 0;
}
