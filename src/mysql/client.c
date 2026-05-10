/***********************************************************************************************************************************
MySQL / MariaDB Client (Phase B scaffolding)

Constructor and destructor are real and link against MariaDB Connector/C. Open, query, and toLog are stubs that throw AssertError
with a clear "TODO(myBackRest-B)" marker so dependent rewrites can compile and link without producing a misleadingly-functional
binary. Reference algorithm: Percona xtrabackup backup_mysql.cc check_server_version() line 368, plus the lock-ladder branch logic
that follows it.
***********************************************************************************************************************************/
#include <build.h>

#include <mysql.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/client.h"

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct MysqlClient
{
    MysqlClientPub pub;                                                 // Publicly accessible fields
    MYSQL *connection;                                                  // libmariadb connection handle (NULL until mysqlClientOpen)
};

// mysqlClientFreeResource (mem-context callback that calls mysql_close on this->connection) will be added together with
// mysqlClientOpen — keeping it out for now avoids an unused-static-function warning on the scaffolding build.

/**********************************************************************************************************************************/
FN_EXTERN MysqlClient *
mysqlClientNew(
    const String *const host, const unsigned int port, const String *const socket, const String *const database,
    const String *const user, const String *const password, const TimeMSec timeout)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, host);
        FUNCTION_LOG_PARAM(UINT, port);
        FUNCTION_LOG_PARAM(STRING, socket);
        FUNCTION_LOG_PARAM(STRING, database);
        FUNCTION_LOG_PARAM(STRING, user);
        FUNCTION_LOG_PARAM(STRING, password);
        FUNCTION_LOG_PARAM(TIME_MSEC, timeout);
    FUNCTION_LOG_END();

    // Either a TCP port or a unix socket must be supplied
    ASSERT(socket != NULL || (port >= 1 && port <= 65535));

    OBJ_NEW_BEGIN(MysqlClient, .childQty = MEM_CONTEXT_QTY_MAX, .callbackQty = 1)
    {
        *this = (MysqlClient)
        {
            .pub =
            {
                .host = strDup(host),
                .port = port,
                .socket = strDup(socket),
                .database = strDup(database),
                .user = strDup(user),
                .password = strDup(password),
                .timeout = timeout,
                .vendor = mysqlVendorUnknown,
                .serverVersionNum = 0,
            },
            .connection = NULL,
        };
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(MY_CLIENT, this);
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlClient *
mysqlClientOpen(MysqlClient *const this)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    // TODO(myBackRest-B): mysql_init() + mysql_options(MYSQL_OPT_CONNECT_TIMEOUT/READ_TIMEOUT/WRITE_TIMEOUT) +
    // mysql_real_connect(socket-or-host, port) + memContextCallbackSet(mysqlClientFreeResource) + parse mysql_get_server_info()
    // into vendor + serverVersionNum (mirrors percona-xtrabackup backup_mysql.cc:check_server_version()).
    THROW(AssertError, "TODO(myBackRest-B): mysqlClientOpen not implemented");
}

/**********************************************************************************************************************************/
FN_EXTERN Pack *
mysqlClientQuery(MysqlClient *const this, const String *const query, const MysqlClientQueryResult resultType)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, this);
        FUNCTION_LOG_PARAM(STRING, query);
        FUNCTION_LOG_PARAM(STRING_ID, resultType);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(query != NULL);
    ASSERT(resultType != 0);

    // TODO(myBackRest-B): mysql_real_query() + mysql_store_result() + iterate rows. Map MYSQL_TYPE_TINY/SHORT/LONG/LONGLONG to
    // pckWriteI32/I64; MYSQL_TYPE_STRING/VAR_STRING/BLOB to pckWriteStr; NULL via mysql_fetch_lengths(). Apply the resultType
    // expectation checks from PgClient as-is — the call sites already understand them.
    THROW(AssertError, "TODO(myBackRest-B): mysqlClientQuery not implemented");
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlClientToLog(const MysqlClient *const this, StringStatic *const debugLog)
{
    strStcCat(debugLog, "{host: ");
    strStcResultSizeInc(
        debugLog,
        FUNCTION_LOG_OBJECT_FORMAT(mysqlClientHost(this), strToLog, strStcRemains(debugLog), strStcRemainsSize(debugLog)));

    strStcCat(debugLog, ", database: ");
    strToLog(mysqlClientDatabase(this), debugLog);

    strStcCat(debugLog, ", user: ");
    strStcResultSizeInc(
        debugLog,
        FUNCTION_LOG_OBJECT_FORMAT(mysqlClientUser(this), strToLog, strStcRemains(debugLog), strStcRemainsSize(debugLog)));

    strStcFmt(
        debugLog, ", port: %u, queryTimeout: %" PRIu64 "}", mysqlClientPort(this), mysqlClientTimeout(this));
}
