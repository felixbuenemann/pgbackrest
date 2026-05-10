/***********************************************************************************************************************************
MyRocks / RocksDB Engine Module

Online (with live server): RocksDB's checkpoint API (SET SESSION rocksdb_create_checkpoint='<path>') would snapshot the SSTs to
a tmp directory; we'd then copy from there. That path is left as a stub because it requires the DB connection that Phase B's
db.c rewrite hasn't delivered yet.

Offline (no live server, ctx->client == NULL): walk <datadir>/.rocksdb/ (Percona/MySQL convention) or <datadir>/#rocksdb/
(MariaDB convention) and flat-copy every file. Output always goes under <backup>/#rocksdb/ so the restore path is identical
across vendors.

Files copied as-is: *.sst, *.log, LOG, LOG.old.*, MANIFEST-*, CURRENT, OPTIONS-*, IDENTITY. RocksDB's SST files are immutable
once written, so the offline copy is consistent by construction.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "mysql/engine/rocksdb.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

#define ROCKSDB_BACKUP_SUBDIR                                       "#rocksdb"

static unsigned int
rocksdbCopyDir(const Storage *const srcStorage, const String *const srcSubdir, const Storage *const dstStorage)
{
    StorageIterator *const itr = storageNewItrP(
        srcStorage, srcSubdir, .level = storageInfoLevelType, .nullOnMissing = true);

    if (itr == NULL)
        return 0;

    unsigned int copied = 0;

    while (storageItrMore(itr))
    {
        const StorageInfo entry = storageItrNext(itr);

        if (!entry.exists || entry.type != storageTypeFile)
            continue;

        // Source path: <srcSubdir>/<filename>; dest path always under <backup>/#rocksdb/
        const String *const srcPath = strNewFmt("%s/%s", strZ(srcSubdir), strZ(entry.name));
        const String *const dstPath = strNewFmt("%s/%s", ROCKSDB_BACKUP_SUBDIR, strZ(entry.name));

        Buffer *const content = storageGetP(storageNewReadP(srcStorage, srcPath));
        storagePutP(storageNewWriteP(dstStorage, dstPath), content);

        copied++;
    }

    return copied;
}

/**********************************************************************************************************************************/
static void
engineRocksdbPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // For online mode this would issue: SET SESSION rocksdb_create_checkpoint = '<tmp_path>';
    // Skipped in offline mode — the SST files are already immutable.
}

static void
engineRocksdbCopyOnline(EngineBackupCtx *const ctx)
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

        // Try both possible RocksDB data subdirs. A single datadir won't have both, but handling both keeps us vendor-agnostic.
        const unsigned int copiedDot = rocksdbCopyDir(srcStorage, STRDEF(".rocksdb"), dstStorage);
        const unsigned int copiedHash = rocksdbCopyDir(srcStorage, STRDEF("#rocksdb"), dstStorage);

        if (copiedDot + copiedHash == 0)
        {
            LOG_INFO_FMT("RocksDB: no .rocksdb or #rocksdb subdir under %s — nothing to back up", strZ(ctx->dataPath));
        }
        else
        {
            LOG_INFO_FMT(
                "RocksDB: copied %u file(s) (%u from .rocksdb/, %u from #rocksdb/)",
                copiedDot + copiedHash, copiedDot, copiedHash);
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static void
engineRocksdbFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // For online mode this would drop the transient checkpoint dir created by SET SESSION rocksdb_create_checkpoint.
    // Offline mode: nothing to clean up.
}

static const EngineHandler rocksdbHandler =
{
    .kind = mysqlEngineMyrocks,
    .name = "rocksdb",
    .prepare = engineRocksdbPrepare,
    .copyOnline = engineRocksdbCopyOnline,
    .copyUnderLock = NULL,
    .finalize = engineRocksdbFinalize,
};

FN_EXTERN const EngineHandler *
engineRocksdbHandler(void)
{
    return &rocksdbHandler;
}
