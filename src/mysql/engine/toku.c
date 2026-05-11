/***********************************************************************************************************************************
TokuDB Engine Module

Offline path is real: walk the datadir top level for *.tokudb, log*.tokulog*, and __tokudb_lock_dont_delete_me_* files. Copy the
fixed-name tokudb.{environment,directory,rollback} metadata unconditionally if present. Streamed via storageCopyP — TokuDB
data files can be very large.

Online path requires libHotBackup.so + the SET GLOBAL tokudb_checkpoint_lock toggle dance — needs Phase B's MysqlClient.
Reference clones: xelabs tokudb-xtrabackup backup_copy.cc:848 / :870 / :1576 / :1589 / :1600.
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/engine/toku.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static void
tokuCopyIfPresent(
    const Storage *const srcStorage, const String *const path, const Storage *const dstStorage)
{
    storageCopyP(storageNewReadP(srcStorage, path, .ignoreMissing = true), storageNewWriteP(dstStorage, path));
}

static void
engineTokuCopyOnline(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        unsigned int dataCopied = 0;
        unsigned int logCopied = 0;
        unsigned int lockMarkerCopied = 0;

        tokuCopyIfPresent(srcStorage, STRDEF("tokudb.environment"), dstStorage);
        tokuCopyIfPresent(srcStorage, STRDEF("tokudb.directory"),   dstStorage);
        tokuCopyIfPresent(srcStorage, STRDEF("tokudb.rollback"),    dstStorage);

        StorageIterator *const itr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(itr))
        {
            const StorageInfo info = storageItrNext(itr);

            if (!info.exists || info.type != storageTypeFile)
                continue;

            if (strEndsWithZ(info.name, ".tokudb"))
            {
                storageCopyP(storageNewReadP(srcStorage, info.name), storageNewWriteP(dstStorage, info.name));
                dataCopied++;
            }
            else if (strBeginsWithZ(info.name, "log") && strstr(strZ(info.name), ".tokulog") != NULL)
            {
                storageCopyP(storageNewReadP(srcStorage, info.name), storageNewWriteP(dstStorage, info.name));
                logCopied++;
            }
            else if (strBeginsWithZ(info.name, "__tokudb_lock_dont_delete_me_"))
            {
                storageCopyP(storageNewReadP(srcStorage, info.name), storageNewWriteP(dstStorage, info.name));
                lockMarkerCopied++;
            }
        }

        LOG_INFO_FMT(
            "TokuDB: copied %u data file(s), %u log file(s), %u lock marker(s)",
            dataCopied, logCopied, lockMarkerCopied);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler tokuHandler =
{
    .kind = mysqlEngineTokudb,
    .name = "tokudb",
    .copyOnline = engineTokuCopyOnline,
};

FN_EXTERN const EngineHandler *
engineTokuHandler(void)
{
    return &tokuHandler;
}
