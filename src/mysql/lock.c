/***********************************************************************************************************************************
MySQL / MariaDB Backup Lock Ladder

Implements the four entry-point lifecycle (begin / blockDdl / blockCommit / release) on top of the three supported lock methods.
Each step is idempotent against an already-released lock — repeated mysqlLockRelease is safe.

Auto-detection rules in mysqlLockMethodSelect:

  vendor      version              method
  ----------  -------------------  --------
  MySQL       >= 80016             instance
  MySQL       <  80016             ftwrl
  Percona     >= 80016             instance
  Percona     <  80016             ftwrl
  MariaDB     >= 100400            stage
  MariaDB     <  100400            ftwrl
  Unknown     —                    ftwrl   (most conservative; works everywhere)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/lock.h"

// Version thresholds. Pre-LOCK_INSTANCE / pre-BACKUP_STAGE versions (every MySQL release prior to 8.0.16) fall back to
// FLUSH TABLES WITH READ LOCK in mysqlLockBlockCommit. The connector handles pre-4.1 password hashes via the auto-retry path
// in mysqlClientOpen.
//
// MINIMUM_SUPPORTED is set to MySQL 3.21 — the connector's wire-protocol floor (Protocol::HandshakeV10). With ISAM engine
// support added (engine/isam.c), we can now back up the .ISD/.ISM/.frm trio that pre-3.23 servers used. Pre-3.21 servers are
// rejected at the wire-protocol layer by the connector itself (HandshakeV9). Pre-3.23 servers also lack the binary log → no PITR.
//
// "Practical" support is much narrower: 5.5 is the lowest version we exercise in test fixtures, and the lowest version
// xtrabackup 2.4 ever supported. Anything older is "supportable in principle, rarely needed in practice".
#define MYSQL_VERSION_MINIMUM_SUPPORTED                             32100       // MySQL 3.21 — Protocol::HandshakeV10 + ISAM
#define MYSQL_VERSION_GTID_AVAILABLE                                50600       // GTID introduced in 5.6.0; pre-5.6 = file:pos PITR only
#define MYSQL_VERSION_LOCK_INSTANCE                                 80016
#define MARIADB_VERSION_BACKUP_STAGE                                100400

/**********************************************************************************************************************************/
FN_EXTERN const char *
mysqlLockMethodName(const MysqlLockMethod method)
{
    switch (method)
    {
        case mysqlLockMethodAuto:     return "auto";
        case mysqlLockMethodInstance: return "instance";
        case mysqlLockMethodStage:    return "stage";
        case mysqlLockMethodFtwrl:    return "ftwrl";
        default:                      return "unknown";
    }
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlLockMethod
mysqlLockMethodSelect(MysqlClient *const client, const MysqlLockMethod userPreference)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING_ID, userPreference);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    const MysqlVendor vendor = mysqlClientVendor(client);
    const unsigned int version = mysqlClientServerVersionNum(client);

    // Honor an explicit user choice but validate it against the server
    if (userPreference != mysqlLockMethodAuto)
    {
        if (userPreference == mysqlLockMethodInstance && vendor != mysqlVendorMysql && vendor != mysqlVendorPercona)
        {
            THROW_FMT(
                OptionInvalidError, "backup-lock-method=instance is only supported on MySQL/Percona, not vendor %u",
                (unsigned int)vendor);
        }

        if (userPreference == mysqlLockMethodInstance && version < MYSQL_VERSION_LOCK_INSTANCE)
        {
            THROW_FMT(
                OptionInvalidError,
                "backup-lock-method=instance requires MySQL/Percona >= 8.0.16; server reports version %u", version);
        }

        if (userPreference == mysqlLockMethodStage && vendor != mysqlVendorMariadb)
            THROW(OptionInvalidError, "backup-lock-method=stage is only supported on MariaDB");

        if (userPreference == mysqlLockMethodStage && version < MARIADB_VERSION_BACKUP_STAGE)
        {
            THROW_FMT(
                OptionInvalidError, "backup-lock-method=stage requires MariaDB >= 10.4; server reports version %u", version);
        }

        FUNCTION_LOG_RETURN(STRING_ID, userPreference);
    }

    // Auto: pick the highest-precedence supported method
    MysqlLockMethod result = mysqlLockMethodFtwrl;

    if (vendor == mysqlVendorMariadb && version >= MARIADB_VERSION_BACKUP_STAGE)
        result = mysqlLockMethodStage;
    else if ((vendor == mysqlVendorMysql || vendor == mysqlVendorPercona) && version >= MYSQL_VERSION_LOCK_INSTANCE)
        result = mysqlLockMethodInstance;

    LOG_DETAIL_FMT(
        "auto-selected backup-lock-method=%s for vendor=%u version=%u", mysqlLockMethodName(result), (unsigned int)vendor, version);

    FUNCTION_LOG_RETURN(STRING_ID, result);
}

/***********************************************************************************************************************************
Helper to issue a SQL statement that returns no rows
***********************************************************************************************************************************/
static void
mysqlLockExec(MysqlClient *const client, const char *const sql)
{
    LOG_DEBUG_FMT("lock SQL: %s", sql);
    (void)mysqlClientQuery(client, STR(sql), mysqlClientQueryResultNone);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlLockBegin(MysqlClient *const client, const MysqlLockMethod method)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING_ID, method);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    switch (method)
    {
        case mysqlLockMethodStage:
            // Two transitions to get into a state where Aria + RocksDB online tables can be safely copied
            mysqlLockExec(client, "BACKUP STAGE START");
            mysqlLockExec(client, "BACKUP STAGE FLUSH");
            break;

        case mysqlLockMethodInstance:
        case mysqlLockMethodFtwrl:
        case mysqlLockMethodAuto:
            // Both methods take their lock LATE (in mysqlLockBlockDdl / BlockCommit) — minimizes lock window
            break;

        default:
            THROW_FMT(AssertError, "unknown lock method %u", (unsigned int)method);
    }

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlLockBlockDdl(MysqlClient *const client, const MysqlLockMethod method)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING_ID, method);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    switch (method)
    {
        case mysqlLockMethodStage:
            mysqlLockExec(client, "BACKUP STAGE BLOCK_DDL");
            break;

        case mysqlLockMethodInstance:
            mysqlLockExec(client, "LOCK INSTANCE FOR BACKUP");
            break;

        case mysqlLockMethodFtwrl:
        case mysqlLockMethodAuto:
            // FTWRL covers both DDL and commit blocking in one shot — issued in BlockCommit
            break;

        default:
            THROW_FMT(AssertError, "unknown lock method %u", (unsigned int)method);
    }

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlLockBlockCommit(MysqlClient *const client, const MysqlLockMethod method)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING_ID, method);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    switch (method)
    {
        case mysqlLockMethodStage:
            mysqlLockExec(client, "BACKUP STAGE BLOCK_COMMIT");
            break;

        case mysqlLockMethodFtwrl:
            mysqlLockExec(client, "FLUSH TABLES WITH READ LOCK");
            break;

        case mysqlLockMethodInstance:
        case mysqlLockMethodAuto:
            // LOCK INSTANCE FOR BACKUP already blocks new commits
            break;

        default:
            THROW_FMT(AssertError, "unknown lock method %u", (unsigned int)method);
    }

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlLockRelease(MysqlClient *const client, const MysqlLockMethod method)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STRING_ID, method);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    // Errors are swallowed during release — partial-state cleanup must not mask the original failure that triggered the release
    TRY_BEGIN()
    {
        switch (method)
        {
            case mysqlLockMethodStage:
                mysqlLockExec(client, "BACKUP STAGE END");
                break;

            case mysqlLockMethodInstance:
                mysqlLockExec(client, "UNLOCK INSTANCE");
                break;

            case mysqlLockMethodFtwrl:
                mysqlLockExec(client, "UNLOCK TABLES");
                break;

            case mysqlLockMethodAuto:
            default:
                break;
        }
    }
    CATCH_ANY()
    {
        LOG_WARN_FMT("lock release failed (method=%s): %s", mysqlLockMethodName(method), errorMessage());
    }
    TRY_END();

    FUNCTION_LOG_RETURN_VOID();
}
