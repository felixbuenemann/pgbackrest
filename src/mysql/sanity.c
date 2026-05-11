/***********************************************************************************************************************************
MySQL / MariaDB Server Sanity Check

Implements the SHOW VARIABLES checks listed in the header. Each check is independent so a multi-failure scenario surfaces all
problems at once instead of forcing the operator to fix-and-retry one at a time.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/pack.h"
#include "mysql/sanity.h"

/***********************************************************************************************************************************
Run a "SHOW VARIABLES LIKE 'name'" and return the value column as a String, or NULL if the variable doesn't exist.

Result layout: rows of (Variable_name, Value). For a single LIKE pattern we expect 0 or 1 rows.
***********************************************************************************************************************************/
static String *
mysqlSanityReadVariable(MysqlClient *const client, const char *const name)
{
    String *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const query = strNewFmt("SHOW VARIABLES LIKE '%s'", name);
        Pack *const pack = mysqlClientQuery(client, query, mysqlClientQueryResultAny);

        if (pack != NULL)
        {
            PackRead *const read = pckReadNew(pack);

            // mysqlClientQuery wraps each row of a QueryResultAny in an array; iterate rows via pckReadNext at the top level.
            while (pckReadNext(read))
            {
                pckReadArrayBeginP(read);

                // Column 0 = Variable_name, column 1 = Value
                String *const varName = pckReadStrP(read);
                String *const value = pckReadStrP(read);

                pckReadArrayEndP(read);

                if (strEqZ(varName, name))
                {
                    MEM_CONTEXT_PRIOR_BEGIN()
                    {
                        result = strDup(value);
                    }
                    MEM_CONTEXT_PRIOR_END();

                    break;
                }
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    return result;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlSanityResult
mysqlServerSanityCheck(MysqlClient *const client)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);

    MysqlSanityResult result = {0};

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // log_bin: case-insensitive ON / 1 → enabled
        const String *const logBinValue = mysqlSanityReadVariable(client, "log_bin");

        if (logBinValue != NULL)
            result.logBin = strEqZ(logBinValue, "ON") || strEqZ(logBinValue, "1");

        if (!result.logBin)
            result.errorCount++;

        // binlog_format: must be ROW (MIXED is unsafe; STATEMENT is unsafe)
        const String *const formatValue = mysqlSanityReadVariable(client, "binlog_format");

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result.binlogFormat = formatValue != NULL ? strDup(formatValue) : NULL;
        }
        MEM_CONTEXT_PRIOR_END();

        result.binlogFormatRow = formatValue != NULL && strEqZ(formatValue, "ROW");

        if (!result.binlogFormatRow)
            result.errorCount++;

        // GTID detection — name differs by vendor
        const String *const gtidMode = mysqlSanityReadVariable(client, "gtid_mode");                        // MySQL
        const String *const gtidStrict = mysqlSanityReadVariable(client, "gtid_strict_mode");               // MariaDB

        result.gtidEnabled =
            (gtidMode != NULL && strEqZ(gtidMode, "ON")) ||
            (gtidStrict != NULL && (strEqZ(gtidStrict, "ON") || strEqZ(gtidStrict, "1")));

        if (!result.gtidEnabled)
            result.errorCount++;

        // server_id must be non-zero (every binlog event tags itself with this; 0 means "no server identity")
        const String *const serverIdValue = mysqlSanityReadVariable(client, "server_id");

        result.serverIdSet = serverIdValue != NULL && !strEqZ(serverIdValue, "0");

        if (!result.serverIdSet)
            result.errorCount++;

        // Capture identifying info for log messages. @@server_uuid is MySQL-only — MariaDB has no equivalent (it gets its UUID
        // from auto.cnf on disk, which the datadir inspector reads separately). Only query it on MySQL/Percona; for MariaDB
        // leave result.serverUuid NULL here and rely on mysqlDataDirInspect to capture it later.
        if (mysqlClientVendor(client) != mysqlVendorMariadb)
        {
            Pack *const uuidPack = mysqlClientQuery(client, STRDEF("SELECT @@server_uuid"), mysqlClientQueryResultColumn);

            if (uuidPack != NULL)
            {
                PackRead *const uuidRead = pckReadNew(uuidPack);
                String *const uuid = pckReadStrP(uuidRead);

                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    result.serverUuid = uuid != NULL ? strDup(uuid) : NULL;
                }
                MEM_CONTEXT_PRIOR_END();
            }
        }

        // Capture binlog basename (path prefix) so the orchestrator knows where to look
        Pack *const basePack = mysqlClientQuery(client, STRDEF("SELECT @@log_bin_basename"), mysqlClientQueryResultColumn);

        if (basePack != NULL)
        {
            PackRead *const baseRead = pckReadNew(basePack);
            String *const base = pckReadStrP(baseRead);

            MEM_CONTEXT_PRIOR_BEGIN()
            {
                result.binlogBasename = base != NULL ? strDup(base) : NULL;
            }
            MEM_CONTEXT_PRIOR_END();
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_SANITY, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlSanityResultToLog(const MysqlSanityResult *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog,
        "{logBin: %s, binlogFormat: %s, gtidEnabled: %s, serverIdSet: %s, errors: %u}",
        this->logBin ? "true" : "false",
        this->binlogFormat != NULL ? strZ(this->binlogFormat) : "(unknown)",
        this->gtidEnabled ? "true" : "false",
        this->serverIdSet ? "true" : "false",
        this->errorCount);
}
