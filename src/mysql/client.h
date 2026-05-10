/***********************************************************************************************************************************
MySQL / MariaDB Client

Connect to a MySQL, MariaDB, or Percona Server instance and run queries via MariaDB Connector/C (libmariadb), which speaks both
wire protocols. This is not a general-purpose client — it is the minimum subset myBackRest needs: connection management, simple
text/integer result sets returned as Pack objects (mirrors PgClient), and the lock-ladder primitives that vary by vendor.

Phase B scaffolding: only constructors and the public API are implemented. The query and lock methods throw AssertError until the
full Phase B implementation lands. The header is the contract that src/db/db.c will eventually be rewritten against.
***********************************************************************************************************************************/
#ifndef MYSQL_CLIENT_H
#define MYSQL_CLIENT_H

#include "common/time.h"
#include "common/type/object.h"
#include "common/type/pack.h"
#include "common/type/string.h"

/***********************************************************************************************************************************
Query result expectation (mirrors PgClientQueryResult so the call sites read the same)
***********************************************************************************************************************************/
typedef enum
{
    mysqlClientQueryResultAny = STRID5("any", 0x65c10),                 // Any number of rows/columns expected
    mysqlClientQueryResultRow = STRID5("row", 0x5df20),                 // Exactly one row expected
    mysqlClientQueryResultColumn = STRID5("column", 0x1cdab1e30),       // Exactly one row, one column expected
    mysqlClientQueryResultNone = STRID5("none", 0x2b9ee0),              // No rows expected (DDL/DML)
} MysqlClientQueryResult;

/***********************************************************************************************************************************
Vendor flavor reported by check_server_version() during dbOpen()
***********************************************************************************************************************************/
typedef enum
{
    mysqlVendorUnknown = 0,
    mysqlVendorMysql,                                                   // Oracle MySQL
    mysqlVendorMariadb,                                                 // MariaDB Server
    mysqlVendorPercona,                                                 // Percona Server (MySQL-compatible + RocksDB/audit)
} MysqlVendor;

/***********************************************************************************************************************************
Object type
***********************************************************************************************************************************/
typedef struct MysqlClient MysqlClient;

/***********************************************************************************************************************************
Constructors
***********************************************************************************************************************************/
FN_EXTERN MysqlClient *mysqlClientNew(
    const String *host, unsigned int port, const String *socket, const String *database, const String *user,
    const String *password, const TimeMSec timeout);

/***********************************************************************************************************************************
Getters/Setters
***********************************************************************************************************************************/
typedef struct MysqlClientPub
{
    const String *host;                                                 // Db host (NULL means socket-only / localhost)
    unsigned int port;                                                  // Db port (0 if using socket)
    const String *socket;                                               // Unix socket path (NULL if TCP)
    const String *database;                                             // Default schema
    const String *user;                                                 // Connection user
    const String *password;                                             // Connection password (NULL if no auth)
    TimeMSec timeout;                                                   // Statement/query timeout
    MysqlVendor vendor;                                                 // Detected at open()
    unsigned int serverVersionNum;                                      // Numeric version, e.g. 80400 for MySQL 8.4.0
} MysqlClientPub;

FN_INLINE_ALWAYS const String *
mysqlClientHost(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->host;
}

FN_INLINE_ALWAYS unsigned int
mysqlClientPort(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->port;
}

FN_INLINE_ALWAYS const String *
mysqlClientSocket(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->socket;
}

FN_INLINE_ALWAYS const String *
mysqlClientDatabase(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->database;
}

FN_INLINE_ALWAYS const String *
mysqlClientUser(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->user;
}

FN_INLINE_ALWAYS TimeMSec
mysqlClientTimeout(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->timeout;
}

FN_INLINE_ALWAYS MysqlVendor
mysqlClientVendor(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->vendor;
}

FN_INLINE_ALWAYS unsigned int
mysqlClientServerVersionNum(const MysqlClient *const this)
{
    return THIS_PUB(MysqlClient)->serverVersionNum;
}

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Open the connection, run handshake, detect vendor + version
FN_EXTERN MysqlClient *mysqlClientOpen(MysqlClient *this);

// Move to a new parent mem context
FN_INLINE_ALWAYS MysqlClient *
mysqlClientMove(MysqlClient *const this, MemContext *const parentNew)
{
    return objMove(this, parentNew);
}

// Execute a query and return the result as a Pack (NULL if resultType == None)
FN_EXTERN Pack *mysqlClientQuery(MysqlClient *this, const String *query, MysqlClientQueryResult resultType);

/***********************************************************************************************************************************
Destructor
***********************************************************************************************************************************/
FN_INLINE_ALWAYS void
mysqlClientFree(MysqlClient *const this)
{
    objFree(this);
}

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlClientToLog(const MysqlClient *this, StringStatic *debugLog);

// Note: log macro suffix is MY_CLIENT (not MYSQL_CLIENT) because <mysql.h> from libmariadb already #defines MYSQL_CLIENT
#define FUNCTION_LOG_MY_CLIENT_TYPE                                                                                                \
    MysqlClient *
#define FUNCTION_LOG_MY_CLIENT_FORMAT(value, buffer, bufferSize)                                                                   \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlClientToLog, buffer, bufferSize)

#endif
