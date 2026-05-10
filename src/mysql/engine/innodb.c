/***********************************************************************************************************************************
InnoDB / XtraDB Engine Module

Backs up the InnoDB data file family:

  - System tablespace:        ibdata1, ibdata2, ...     (always at datadir top level)
  - Data dictionary (8.0+):   mysql.ibd
  - Per-table tablespaces:    <schema>/<table>.ibd
  - Undo tablespaces (8.0+):  undo_NNN.ibu

Doublewrite buffer files (#innodb_dblwr/) are intentionally skipped — mysqld recreates them on startup.
Redo log files (#innodb_redo/ or ib_logfile0/1) are handled by RedoLogCopier, not this module.

Online mode coordinates with the redo copier and the lock ladder; offline mode (server shutdown) is a clean flat copy because
torn pages can't exist when no one is writing.

Reference algorithm: percona-xtrabackup xtrabackup.cc:xtrabackup_backup_func() line 4238 + redo_log.cc:Redo_Log_Data_Manager.
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "mysql/engine/innodb.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/**********************************************************************************************************************************/
static void
engineInnodbPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Online-mode: load tablespace catalog from INFORMATION_SCHEMA.INNODB_TABLESPACES so we know what to expect on disk.
    // Offline-mode: nothing to prepare — the tableSpaceIter walk discovers everything from the filesystem.
}

/**********************************************************************************************************************************/
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

        // TODO(myBackRest-D): online mode would call redoLogCopierStart() here. The plan calls for the redo copier to spawn
        // BEFORE any data file copy begins, so we have continuous LSN coverage from the earliest LSN we'll need at recovery
        // time. That requires the worker-process plumbing in src/protocol/parallel.c — deferred.

        // Enumerate every InnoDB tablespace under the datadir
        TableSpaceIter *const iter = tableSpaceIterNew(srcStorage, ctx->dataPath);

        LOG_INFO_FMT("InnoDB: copying %u tablespace file(s)", tableSpaceIterFileTotal(iter));

        unsigned int copied = 0;
        String *path;

        while ((path = tableSpaceIterNext(iter)) != NULL)
        {
            // TODO(myBackRest-D): online mode should validate each page through mysqlPageChecksumValidate as we copy. Offline
            // mode skips validation because torn pages are impossible when the server is shut down.
            Buffer *const content = storageGetP(storageNewReadP(srcStorage, path));
            storagePutP(storageNewWriteP(dstStorage, path), content);

            copied++;
        }

        tableSpaceIterFree(iter);

        LOG_INFO_FMT("InnoDB: copied %u tablespace file(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static void
engineInnodbCopyUnderLock(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Online-mode would copy any tablespaces created since copyOnline started (newly-allocated FIL space IDs visible in
    // INFORMATION_SCHEMA.INNODB_TABLESPACES that we hadn't enumerated yet) and signal redoLogCopier to flush+stop at the
    // locked LSN. Offline mode: no new files can appear, so this is a no-op.
}

/**********************************************************************************************************************************/
static void
engineInnodbFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Online mode: write mybackrest_checkpoints with the start/stop LSN, the redo log file count, and verify redo coverage
    // is complete. Offline mode: no LSN tracking — the copied datadir is a point-in-time snapshot from when the server was
    // shut down.
}

static const EngineHandler innodbHandler =
{
    .kind = mysqlEngineInnodb,
    .name = "innodb",
    .prepare = engineInnodbPrepare,
    .copyOnline = engineInnodbCopyOnline,
    .copyUnderLock = engineInnodbCopyUnderLock,
    .finalize = engineInnodbFinalize,
};

FN_EXTERN const EngineHandler *
engineInnodbHandler(void)
{
    return &innodbHandler;
}
