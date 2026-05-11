/***********************************************************************************************************************************
InnoDB / XtraDB Engine Module

Backs up ibdata*, mysql.ibd, per-table .ibd files, undo_*.ibu. Doublewrite (#innodb_dblwr/) excluded — mysqld recreates.
Redo log handled by RedoLogCopier, not this module.

Online mode coordinates with the redo copier + lock ladder; offline mode (server shutdown) is a clean flat copy because torn
pages can't exist when no one is writing.

Reference: percona-xtrabackup xtrabackup.cc:xtrabackup_backup_func() line 4238 + redo_log.cc:Redo_Log_Data_Manager.
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/engine/innodb.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static void
engineInnodbCopyOnline(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);
    ASSERT(ctx->dataPath != NULL);
    ASSERT(ctx->backupPath != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        TableSpaceIter *const iter = tableSpaceIterNew(srcStorage, ctx->dataPath);

        LOG_INFO_FMT("InnoDB: copying %u tablespace file(s)", tableSpaceIterFileTotal(iter));

        unsigned int copied = 0;
        String *path;

        while ((path = tableSpaceIterNext(iter)) != NULL)
        {
            // Streaming copy — InnoDB files commonly run 1-100 GiB so we can't slurp them. Online-mode page-by-page checksum
            // validation will go through a filter pipeline in Phase D's real orchestrator; offline mode skips validation
            // because the server is shut down and torn pages are impossible.
            storageCopyP(storageNewReadP(srcStorage, path), storageNewWriteP(dstStorage, path));
            copied++;
        }

        tableSpaceIterFree(iter);

        LOG_INFO_FMT("InnoDB: copied %u tablespace file(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler innodbHandler =
{
    .kind = mysqlEngineInnodb,
    .name = "innodb",
    .copyOnline = engineInnodbCopyOnline,
};

FN_EXTERN const EngineHandler *
engineInnodbHandler(void)
{
    return &innodbHandler;
}
