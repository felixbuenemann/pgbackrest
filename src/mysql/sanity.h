/***********************************************************************************************************************************
MySQL / MariaDB Server Sanity Check

Validates that a server is configured for archive-based backup before stanza-create commits a config to the repo. Specifically
verifies:

  - log_bin              = ON         (binary logging required for archive-push)
  - binlog_format        = ROW        (statement-based replication is unsafe across replicas)
  - gtid_mode            = ON         (MySQL: GTID_MODE; required for clean PITR target selection)
  - gtid_strict_mode     = 1          (MariaDB: enforce_gtid_consistency)
  - server_uuid          / server_id  not empty

Behavior is suppressible via --skip-binlog-archive for development environments where PITR isn't needed.

This function lives separately from db.c so it can be called from stanza-create / check directly without dragging in the entire
PgClient → MysqlClient rewrite.
***********************************************************************************************************************************/
#ifndef MYSQL_SANITY_H
#define MYSQL_SANITY_H

#include "common/type/string.h"
#include "mysql/client.h"

/***********************************************************************************************************************************
Result of a sanity check; populated by mysqlServerSanityCheck. errorCount > 0 means the server is not ready.
***********************************************************************************************************************************/
typedef struct MysqlSanityResult
{
    bool logBin;                                                        // SHOW VARIABLES LIKE 'log_bin'                    = 'ON'
    bool binlogFormatRow;                                               // SHOW VARIABLES LIKE 'binlog_format'              = 'ROW'
    bool gtidEnabled;                                                   // (MySQL gtid_mode) OR (MariaDB gtid_strict_mode)
    bool serverIdSet;                                                   // server_id != 0  (required for binlog uniqueness)
    String *serverUuid;                                                 // From auto.cnf via the live SELECT @@server_uuid
    String *binlogBasename;                                             // @@log_bin_basename — path prefix for binlogs
    String *binlogFormat;                                               // Raw value as reported (for error messages)
    unsigned int errorCount;                                            // Sum of failed checks
} MysqlSanityResult;

// Run all checks and return the result. Caller decides how to surface errors (errorCount > 0 means at least one failed).
FN_EXTERN MysqlSanityResult mysqlServerSanityCheck(MysqlClient *client);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlSanityResultToLog(const MysqlSanityResult *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_SANITY_TYPE                                                                                                \
    MysqlSanityResult
#define FUNCTION_LOG_MY_SANITY_FORMAT(value, buffer, bufferSize)                                                                   \
    FUNCTION_LOG_OBJECT_FORMAT(&value, mysqlSanityResultToLog, buffer, bufferSize)

#endif
