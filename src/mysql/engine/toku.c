/***********************************************************************************************************************************
TokuDB Engine Module

Offline mode is now real — same filesystem-walk pattern as MyISAM/Aria/RocksDB. Online mode (live server) still requires the
libHotBackup.so plugin handshake plus the SET GLOBAL lock toggle ordering, which depend on Phase B's MysqlClient orchestration
not being a stub. For cold backups (server shutdown) the offline path captures everything needed.

File taxonomy (drawn from /home/user/tokudb-xtrabackup):
  Top-level (per-instance):
    tokudb.environment              env metadata
    tokudb.directory                table → file map
    tokudb.rollback                 undo
    __tokudb_lock_dont_delete_me_*  lock-state markers; copied as-is so the next start sees them

  Top-level (recovery log):
    log000000000000.tokulog<N>      recovery log files, one per generation

  Per-table (any directory):
    <table>_main_NNNNNNNN.tokudb    primary data
    <table>_status_NNNNNNNN.tokudb  per-index catalog
    <table>_key_*.tokudb            secondary index data

The pattern matcher trusts the .tokudb file extension for per-table files and copies them flat from wherever they appear under
the datadir (TokuDB stores them all in one directory by default but can be relocated via tokudb_data_dir).

Reference: tokudb-xtrabackup backup_copy.cc:848 tokudb_data_file_copy_backup, :870 tokudb_redolog_file_copy_backup, :1600
backup_tokudb_env_files. Online mode reference: backup_copy.cc:1576 tokudb_lock_checkpoint (note the SET GLOBAL toggle order).
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/engine/toku.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Optional flat copy: if file exists copy it, else silently skip
***********************************************************************************************************************************/
static bool
tokuCopyIfPresent(
    const Storage *const srcStorage, const String *const srcPath, const Storage *const dstStorage, const String *const dstPath)
{
    Buffer *const content = storageGetP(storageNewReadP(srcStorage, srcPath, .ignoreMissing = true));

    if (content == NULL)
        return false;

    storagePutP(storageNewWriteP(dstStorage, dstPath), content);
    return true;
}

/**********************************************************************************************************************************/
static void
engineTokuPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Offline mode: nothing to prepare. Online mode would issue:
    //   SET GLOBAL tokudb_checkpoint_on_flush_logs = OFF       (must come first)
    //   SET GLOBAL tokudb_checkpoint_lock = ON
    // Reference: tokudb-xtrabackup backup_copy.cc:1576 tokudb_lock_checkpoint
}

/**********************************************************************************************************************************/
static void
engineTokuCopyOnline(EngineBackupCtx *const ctx)
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

        unsigned int envCopied = 0;
        unsigned int dataCopied = 0;
        unsigned int logCopied = 0;
        unsigned int lockMarkerCopied = 0;

        // Top-level metadata files. These live at the datadir root regardless of where per-table .tokudb files sit.
        if (tokuCopyIfPresent(srcStorage, STRDEF("tokudb.environment"), dstStorage, STRDEF("tokudb.environment"))) envCopied++;
        if (tokuCopyIfPresent(srcStorage, STRDEF("tokudb.directory"),   dstStorage, STRDEF("tokudb.directory")))   envCopied++;
        if (tokuCopyIfPresent(srcStorage, STRDEF("tokudb.rollback"),    dstStorage, STRDEF("tokudb.rollback")))    envCopied++;

        // Walk the top level for *.tokudb files (per-table data) + log*.tokulog* (recovery log) +
        // __tokudb_lock_dont_delete_me_* (lock-state markers).
        StorageIterator *const itr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(itr))
        {
            const StorageInfo info = storageItrNext(itr);

            if (!info.exists || info.type != storageTypeFile)
                continue;

            if (strEndsWithZ(info.name, ".tokudb"))
            {
                tokuCopyIfPresent(srcStorage, info.name, dstStorage, info.name);
                dataCopied++;
            }
            else if (strBeginsWithZ(info.name, "log") && strstr(strZ(info.name), ".tokulog") != NULL)
            {
                tokuCopyIfPresent(srcStorage, info.name, dstStorage, info.name);
                logCopied++;
            }
            else if (strBeginsWithZ(info.name, "__tokudb_lock_dont_delete_me_"))
            {
                tokuCopyIfPresent(srcStorage, info.name, dstStorage, info.name);
                lockMarkerCopied++;
            }
        }

        LOG_INFO_FMT(
            "TokuDB: copied %u env file(s), %u data file(s), %u log file(s), %u lock marker(s)",
            envCopied, dataCopied, logCopied, lockMarkerCopied);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/**********************************************************************************************************************************/
static void
engineTokuFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Online mode would: SET GLOBAL tokudb_checkpoint_lock = OFF
    // Reference: tokudb-xtrabackup backup_copy.cc:1589 tokudb_unlock_checkpoint
}

static const EngineHandler tokuHandler =
{
    .kind = mysqlEngineTokudb,
    .name = "tokudb",
    .prepare = engineTokuPrepare,
    .copyOnline = engineTokuCopyOnline,
    .copyUnderLock = NULL,
    .finalize = engineTokuFinalize,
};

FN_EXTERN const EngineHandler *
engineTokuHandler(void)
{
    return &tokuHandler;
}
