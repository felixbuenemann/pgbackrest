/***********************************************************************************************************************************
MySQL / MariaDB Binlog Event-Header Parser (Phase F scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/binlog.h"

/**********************************************************************************************************************************/
FN_EXTERN MysqlBinlogInfo *
mysqlBinlogScan(const Storage *const storage, const String *const binlogPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, binlogPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(binlogPath != NULL);

    // TODO(myBackRest-F): magic-check + iterate event headers (19 bytes each), capture FORMAT_DESCRIPTION (offset 0..18+payload),
    // GTID_LOG_EVENT (extract GTID = uuid + ':' + txn_id), and final event log_pos for fileSize. Stop at end-of-file or ROTATE.
    THROW(AssertError, "TODO(myBackRest-F): mysqlBinlogScan not implemented");
}
