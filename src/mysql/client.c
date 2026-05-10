/***********************************************************************************************************************************
MySQL / MariaDB Client

Wraps MariaDB Connector/C (libmariadb) for myBackRest's needs: connect, run a single statement, store the result, return rows
encoded as a Pack. Vendor and numeric server version are captured at open time so callers can branch on them without parsing
strings later.

What this client does NOT implement (intentionally, see plan Phase B):
  - Async cancellation on per-query timeout. We rely on MYSQL_OPT_READ_TIMEOUT / MYSQL_OPT_WRITE_TIMEOUT so a hung server
    can't pin a worker indefinitely; the precise PgClient-style PQcancel-on-deadline pattern is left for a follow-up.
  - Prepared statements. Every backup-time query in pgBackRest uses literal SQL and that's fine here too.
  - SSL/TLS configuration. To be added when Phase B wires in --db-host-cert/key/ca/etc.
***********************************************************************************************************************************/
#include <build.h>

#include <errmsg.h>
#include <mysql.h>
#include <mysqld_error.h>
#include <string.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/convert.h"
#include "mysql/client.h"
#include "version.h"

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
struct MysqlClient
{
    MysqlClientPub pub;                                                 // Publicly accessible fields
    MYSQL *connection;                                                  // libmariadb handle (NULL until mysqlClientOpen)
};

/***********************************************************************************************************************************
Tear down the libmariadb handle on mem-context free
***********************************************************************************************************************************/
static void
mysqlClientFreeResource(THIS_VOID)
{
    THIS(MysqlClient);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(MY_CLIENT, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    if (this->connection != NULL)
    {
        mysql_close(this->connection);
        this->connection = NULL;
    }

    FUNCTION_LOG_RETURN_VOID();
}

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

/***********************************************************************************************************************************
Detect server vendor from the version string returned by mysql_get_server_info()

Heuristic, in order:
  - "MariaDB" anywhere in the string  → MariaDB
  - mysql_get_server_info contains "-rel" / "-Percona" or @@version_comment starts with "Percona" → Percona
  - else                              → upstream Oracle MySQL

The @@version_comment lookup runs in mysqlClientOpen after the handshake completes, so this helper sees the raw server-info
string only.
***********************************************************************************************************************************/
static MysqlVendor
mysqlClientDetectVendor(const char *const serverInfo, const char *const versionComment)
{
    if (strstr(serverInfo, "MariaDB") != NULL || (versionComment != NULL && strstr(versionComment, "MariaDB") != NULL))
        return mysqlVendorMariadb;

    if (versionComment != NULL && strstr(versionComment, "Percona") != NULL)
        return mysqlVendorPercona;

    if (strstr(serverInfo, "-Percona") != NULL || strstr(serverInfo, "-rel") != NULL)
        return mysqlVendorPercona;

    return mysqlVendorMysql;
}

/***********************************************************************************************************************************
Initialize a fresh MYSQL handle with our standard options. If allowOldPassword is true, additionally disable MYSQL_SECURE_AUTH
and ask libmariadb to use mysql_old_password as the default auth plugin.

Returns the handle (must be mysql_close'd by the caller) or NULL on out-of-memory.
***********************************************************************************************************************************/
static MYSQL *
mysqlClientInitHandle(const TimeMSec timeoutMs, const bool allowOldPassword)
{
    MYSQL *const conn = mysql_init(NULL);
    if (conn == NULL)
        return NULL;

    const unsigned int timeoutSec = (unsigned int)((timeoutMs + 999) / 1000);
    const unsigned int timeoutSecOrOne = timeoutSec == 0 ? 1 : timeoutSec;

    mysql_options(conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeoutSecOrOne);
    mysql_options(conn, MYSQL_OPT_READ_TIMEOUT, &timeoutSecOrOne);
    mysql_options(conn, MYSQL_OPT_WRITE_TIMEOUT, &timeoutSecOrOne);

    mysql_optionsv(conn, MYSQL_OPT_CONNECT_ATTR_ADD, (const void *)"program_name", (const void *)PROJECT_BIN);

    if (allowOldPassword)
    {
        // MYSQL_SECURE_AUTH defaults to true and refuses pre-4.1 hashes; disable it so legacy 5.5 servers (or any server with
        // a user account that still has a pre-4.1 password hash) can be reached. Also nudge the default auth plugin so the
        // old protocol is the FIRST one we offer, which avoids a round-trip through the modern plugins.
        my_bool no = 0;
        mysql_options(conn, MYSQL_SECURE_AUTH, &no);
        mysql_options(conn, MYSQL_DEFAULT_AUTH, "mysql_old_password");
    }

    return conn;
}

/***********************************************************************************************************************************
Try mysql_real_connect on the supplied handle with our connection parameters. Returns true on success, false on failure (caller
inspects mysql_errno on the handle to decide what to do next).
***********************************************************************************************************************************/
static bool
mysqlClientTryConnect(MYSQL *const conn, const MysqlClient *const this)
{
    return mysql_real_connect(
        conn,
        mysqlClientHost(this) != NULL ? strZ(mysqlClientHost(this)) : NULL,
        mysqlClientUser(this) != NULL ? strZ(mysqlClientUser(this)) : NULL,
        this->pub.password != NULL ? strZ(this->pub.password) : NULL,
        mysqlClientDatabase(this) != NULL ? strZ(mysqlClientDatabase(this)) : NULL,
        mysqlClientPort(this),
        mysqlClientSocket(this) != NULL ? strZ(mysqlClientSocket(this)) : NULL,
        /* client_flag */ 0) != NULL;
}

/***********************************************************************************************************************************
Decide whether a connection error is the "server requires the old (pre-4.1.1) auth protocol" case, which is the only failure mode
we'll auto-retry with old-password enabled. Two error codes can surface this depending on which side detected the mismatch:

  CR_SECURE_AUTH (2049)              libmariadb client refuses the server's old-hash response
  ER_NOT_SUPPORTED_AUTH_MODE (1251)  server explicitly says the client doesn't support the requested protocol

Anything else (wrong password, host unreachable, refused connection, timeout) is a real failure that retrying with old-password
won't help and shouldn't try to.
***********************************************************************************************************************************/
static bool
mysqlClientErrorWantsOldPassword(const unsigned int errnoVal)
{
    return errnoVal == CR_SECURE_AUTH || errnoVal == ER_NOT_SUPPORTED_AUTH_MODE;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlClient *
mysqlClientOpen(MysqlClient *const this)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_CLIENT, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    CHECK(AssertError, this->connection == NULL, "invalid connection");

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // First attempt: default auth (mysql_native_password / caching_sha2_password)
        this->connection = mysqlClientInitHandle(mysqlClientTimeout(this), /*allowOldPassword*/ false);

        if (this->connection == NULL)
            THROW(DbConnectError, "mysql_init returned NULL (out of memory)");

        // Register cleanup BEFORE the connect attempt so a partially-initialized handle still gets freed
        memContextCallbackSet(objMemContext(this), mysqlClientFreeResource, this);

        if (!mysqlClientTryConnect(this->connection, this))
        {
            // Capture error info BEFORE we close + retry, since mysql_close clears them
            const unsigned int errnoVal = mysql_errno(this->connection);
            const char *const errMsg = mysql_error(this->connection);

            if (mysqlClientErrorWantsOldPassword(errnoVal))
            {
                LOG_WARN_FMT(
                    "first connection attempt rejected (errno %u: %s); retrying with mysql_old_password enabled",
                    errnoVal, errMsg);

                // Fresh handle — libmariadb's auth state isn't safely reusable after a failed connect
                mysql_close(this->connection);
                this->connection = mysqlClientInitHandle(mysqlClientTimeout(this), /*allowOldPassword*/ true);

                if (this->connection == NULL)
                    THROW(DbConnectError, "mysql_init returned NULL on retry");

                if (!mysqlClientTryConnect(this->connection, this))
                {
                    THROW_FMT(
                        DbConnectError, "unable to connect to MySQL server at %s:%u (after old-password retry): %s",
                        mysqlClientHost(this) != NULL ? strZ(mysqlClientHost(this)) :
                            (mysqlClientSocket(this) != NULL ? strZ(mysqlClientSocket(this)) : "(unspecified)"),
                        mysqlClientPort(this),
                        mysql_error(this->connection));
                }
            }
            else
            {
                // Non-auth failure: don't waste a round-trip on a retry that won't help
                THROW_FMT(
                    DbConnectError, "unable to connect to MySQL server at %s:%u: %s",
                    mysqlClientHost(this) != NULL ? strZ(mysqlClientHost(this)) :
                        (mysqlClientSocket(this) != NULL ? strZ(mysqlClientSocket(this)) : "(unspecified)"),
                    mysqlClientPort(this),
                    errMsg);
            }
        }

        // Capture vendor + numeric version. mysql_get_server_version returns NN_NN_NN packed as MAJOR*10000 + MINOR*100 + PATCH.
        this->pub.serverVersionNum = (unsigned int)mysql_get_server_version(this->connection);

        const char *const serverInfo = mysql_get_server_info(this->connection);
        const char *versionComment = NULL;

        if (mysql_query(this->connection, "SELECT @@version_comment") == 0)
        {
            MYSQL_RES *const res = mysql_store_result(this->connection);

            if (res != NULL)
            {
                MYSQL_ROW row = mysql_fetch_row(res);

                if (row != NULL && row[0] != NULL)
                    versionComment = row[0];

                this->pub.vendor = mysqlClientDetectVendor(serverInfo, versionComment);
                mysql_free_result(res);
            }
            else
            {
                this->pub.vendor = mysqlClientDetectVendor(serverInfo, NULL);
            }
        }
        else
        {
            this->pub.vendor = mysqlClientDetectVendor(serverInfo, NULL);
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_CLIENT, this);
}

/***********************************************************************************************************************************
Encode one MYSQL_ROW value into the running pack
***********************************************************************************************************************************/
static void
mysqlClientPackValue(
    PackWrite *const pack, const enum enum_field_types fieldType, const char *const value, const unsigned long valueLen,
    const unsigned int columnIdx, const String *const query)
{
    if (value == NULL)
    {
        pckWriteNullP(pack);
        return;
    }

    switch (fieldType)
    {
        // Boolean is reported as TINY(1) by MySQL. Treat MYSQL_TYPE_TINY as int and let callers cast if they want bool.
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_YEAR:
            pckWriteI32P(pack, cvtZToInt(value), .defaultWrite = true);
            break;

        case MYSQL_TYPE_LONGLONG:
            pckWriteI64P(pack, cvtZToInt64(value), .defaultWrite = true);
            break;

        case MYSQL_TYPE_NULL:
            pckWriteNullP(pack);
            break;

        case MYSQL_TYPE_DECIMAL:
        case MYSQL_TYPE_NEWDECIMAL:
        case MYSQL_TYPE_FLOAT:
        case MYSQL_TYPE_DOUBLE:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_TIME:
        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_NEWDATE:
        case MYSQL_TYPE_VARCHAR:
        case MYSQL_TYPE_VAR_STRING:
        case MYSQL_TYPE_STRING:
        case MYSQL_TYPE_ENUM:
        case MYSQL_TYPE_SET:
        case MYSQL_TYPE_JSON:
        case MYSQL_TYPE_BIT:
            // Strings come back zero-terminated and length-tagged; STR(value) creates a String referencing the libmariadb buffer.
            // The pack copies the bytes immediately, so the temporary lifetime is fine.
            (void)valueLen;
            pckWriteStrP(pack, STR(value), .defaultWrite = true);
            break;

        // Binary types we currently surface as text. If callers ever need raw bytes we can extend the API.
        case MYSQL_TYPE_TINY_BLOB:
        case MYSQL_TYPE_MEDIUM_BLOB:
        case MYSQL_TYPE_LONG_BLOB:
        case MYSQL_TYPE_BLOB:
        case MYSQL_TYPE_GEOMETRY:
            pckWriteStrP(pack, STR(value), .defaultWrite = true);
            break;

        default:
            THROW_FMT(
                FormatError, "unable to parse mysql type %u in column %u for query '%s'", (unsigned int)fieldType, columnIdx,
                strZ(query));
    }
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
    CHECK(AssertError, this->connection != NULL, "invalid connection");
    ASSERT(query != NULL);
    ASSERT(resultType != 0);

    Pack *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        if (mysql_real_query(this->connection, strZ(query), (unsigned long)strSize(query)) != 0)
        {
            THROW_FMT(
                DbQueryError, "unable to execute query '%s': %s", strZ(query), mysql_error(this->connection));
        }

        MYSQL_RES *const sqlResult = mysql_store_result(this->connection);

        TRY_BEGIN()
        {
            if (sqlResult == NULL)
            {
                // Either a non-result statement (DDL/DML) or an error. mysql_field_count distinguishes the two.
                if (mysql_field_count(this->connection) != 0)
                {
                    THROW_FMT(
                        DbQueryError, "unable to retrieve result for '%s': %s", strZ(query), mysql_error(this->connection));
                }

                if (resultType != mysqlClientQueryResultNone && resultType != mysqlClientQueryResultAny)
                    THROW_FMT(DbQueryError, "result expected from '%s'", strZ(query));
            }
            else
            {
                if (resultType == mysqlClientQueryResultNone)
                    THROW_FMT(DbQueryError, "no result expected from '%s'", strZ(query));

                const my_ulonglong rowTotal = mysql_num_rows(sqlResult);
                const unsigned int columnTotal = mysql_num_fields(sqlResult);

                if (resultType != mysqlClientQueryResultAny)
                {
                    if (rowTotal != 1)
                        THROW_FMT(DbQueryError, "expected one row from '%s'", strZ(query));

                    if (resultType == mysqlClientQueryResultColumn && columnTotal != 1)
                        THROW_FMT(DbQueryError, "expected one column from '%s'", strZ(query));
                }

                // Snapshot field types up front so we don't refetch on every row
                const MYSQL_FIELD *const fields = mysql_fetch_fields(sqlResult);

                PackWrite *const pack = pckWriteNewP();
                MYSQL_ROW row;

                while ((row = mysql_fetch_row(sqlResult)) != NULL)
                {
                    const unsigned long *const lengths = mysql_fetch_lengths(sqlResult);

                    if (resultType == mysqlClientQueryResultAny)
                        pckWriteArrayBeginP(pack);

                    for (unsigned int columnIdx = 0; columnIdx < columnTotal; columnIdx++)
                    {
                        mysqlClientPackValue(
                            pack, fields[columnIdx].type, row[columnIdx], lengths != NULL ? lengths[columnIdx] : 0, columnIdx,
                            query);
                    }

                    if (resultType == mysqlClientQueryResultAny)
                        pckWriteArrayEndP(pack);
                }

                pckWriteEndP(pack);
                result = pckMove(pckWriteResult(pack), memContextPrior());
            }
        }
        FINALLY()
        {
            if (sqlResult != NULL)
                mysql_free_result(sqlResult);
        }
        TRY_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(PACK, result);
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
