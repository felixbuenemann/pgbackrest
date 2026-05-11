/***********************************************************************************************************************************
Cold-Mode (Offline) Restore Orchestrator
***********************************************************************************************************************************/
#include <build.h>

#include "command/restore/prepare.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/coldRestore.h"
#include "mysql/interface.h"
#include "mysql/manifest.h"
#include "storage/iterator.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Skip-list for backup-meta artefacts that don't belong in the target datadir. Currently just the manifest itself — the operator
can still inspect it via the backup directory, and a future restore would re-read it from there anyway. mybackrest_recovery.*
files don't appear here because the backup never writes them.
***********************************************************************************************************************************/
static bool
restoreShouldSkip(const String *const relPath)
{
    return strEqZ(relPath, MYSQL_FILE_BACKUP_INFO);
}

/***********************************************************************************************************************************
Recursively walk srcRel under srcStorage and copy every file to dstStorage at the same relative path. Mirrors the structure of
the cold backup output: top-level files (auto.cnf, ibdata1, ...), the #innodb_redo/ subdirectory, and per-schema subdirectories
with engine files (.MYD/.MYI, .MAD/.MAI, .ibd, ...).
***********************************************************************************************************************************/
static unsigned int
restoreWalkAndCopy(
    const Storage *const srcStorage, const String *const srcRel,
    const Storage *const dstStorage, const String *const dstRel)
{
    unsigned int copied = 0;

    StorageIterator *const itr = storageNewItrP(
        srcStorage, srcRel, .level = storageInfoLevelType, .nullOnMissing = true);

    if (itr == NULL)
        return 0;

    while (storageItrMore(itr))
    {
        const StorageInfo entry = storageItrNext(itr);

        if (!entry.exists)
            continue;

        const String *const childSrcRel = strNewFmt("%s/%s", strZ(srcRel), strZ(entry.name));
        const String *const childDstRel = strNewFmt("%s/%s", strZ(dstRel), strZ(entry.name));

        if (entry.type == storageTypeFile)
        {
            // Skip manifest + any future meta artefacts (relative-to-backup-root path)
            if (restoreShouldSkip(entry.name))
                continue;

            storageCopyP(storageNewReadP(srcStorage, childSrcRel), storageNewWriteP(dstStorage, childDstRel));
            copied++;
        }
        else if (entry.type == storageTypePath)
        {
            copied += restoreWalkAndCopy(srcStorage, childSrcRel, dstStorage, childDstRel);
        }
    }

    return copied;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlColdRestoreResult *
mysqlColdRestore(
    const Storage *const srcStorage, const String *const backupPath,
    const Storage *const dstStorage, const String *const restorePath,
    const String *const mysqldPath)
{
    FUNCTION_LOG_BEGIN(logLevelInfo);
        FUNCTION_LOG_PARAM(STORAGE, srcStorage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
        FUNCTION_LOG_PARAM(STORAGE, dstStorage);
        FUNCTION_LOG_PARAM(STRING, restorePath);
        FUNCTION_LOG_PARAM(STRING, mysqldPath);
    FUNCTION_LOG_END();

    ASSERT(srcStorage != NULL);
    ASSERT(backupPath != NULL);
    ASSERT(dstStorage != NULL);
    ASSERT(restorePath != NULL);

    MysqlColdRestoreResult *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlColdRestoreResult));
            *result = (MysqlColdRestoreResult){0};
        }
        MEM_CONTEXT_PRIOR_END();

        // Step 1: read the manifest. Failure here is fatal — we won't restore a directory we can't identify.
        LOG_INFO_FMT("cold restore: reading manifest from %s", strZ(backupPath));
        MysqlBackupManifestParsed *const manifest = mysqlBackupManifestRead(srcStorage, backupPath);

        if (manifest == NULL)
        {
            THROW_FMT(
                FileMissingError,
                "no %s under %s — backup directory is incomplete or corrupt",
                MYSQL_FILE_BACKUP_INFO, strZ(backupPath));
        }

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result->manifest = manifest;
        }
        MEM_CONTEXT_PRIOR_END();

        LOG_INFO_FMT(
            "cold restore: manifest indicates vendor=%u, version=%u, taken=%s",
            (unsigned int)manifest->info->vendor, manifest->info->versionNum,
            manifest->backupTime != NULL ? strZ(manifest->backupTime) : "(unknown)");

        // Step 2: walk the backup tree and flat-copy every file (skipping meta). Recursion is fine here because the backup tree
        // depth is bounded: 1 (top level) + 1 (#innodb_redo/ or schema dir) + 1 (engine files) = 3 levels max in practice.
        result->filesCopied = restoreWalkAndCopy(srcStorage, backupPath, dstStorage, restorePath);

        LOG_INFO_FMT("cold restore: %u file(s) copied into %s", result->filesCopied, strZ(restorePath));

        // Step 3: optional recovery-files generation. Without this step the restored datadir is byte-equivalent to a crashed
        // server's — it won't start cleanly until mysqld replays the redo log. prepareWriteRecoveryFiles makes that hand-off
        // explicit; caller can chain prepareInvokeMysqld to actually run it.
        if (mysqldPath != NULL)
        {
            prepareWriteRecoveryFiles(dstStorage, restorePath, mysqldPath);
            result->recoveryFilesWritten = true;
        }
        else
        {
            LOG_INFO(
                "cold restore: mysqldPath unset — recovery files NOT written. The restored datadir is in a crash-equivalent state;"
                " run mybackrest's prepare step or invoke mysqld with --defaults-file pointing at a recovery cnf before"
                " using the datadir.");
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_P(VOID, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlColdRestoreResultFree(MysqlColdRestoreResult *const this)
{
    // Symmetric API with mysqlColdBackupResultFree — both result and manifest live in the caller's mem context.
    (void)this;
}
