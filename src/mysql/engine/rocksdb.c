/***********************************************************************************************************************************
MyRocks / RocksDB Engine Module

Offline: walk both .rocksdb/ (Percona/MySQL) and #rocksdb/ (mariabackup convention), flat-copy every file into <backup>/#rocksdb/.
SSTs are immutable once written so a cold-mode copy is consistent by construction.

Online mode would issue `SET SESSION rocksdb_create_checkpoint = '<tmp>'` (Percona) or use mariabackup's `myrocks_checkpoint.create()`
API; that path needs Phase B's MysqlClient orchestration not yet wired into EngineBackupCtx.

Reference: mariabackup backup_copy.cc:has_rocksdb_plugin() line 2123, rocksdb_create_checkpoint() line 2269.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
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

        const String *const srcPath = strNewFmt("%s/%s", strZ(srcSubdir), strZ(entry.name));
        const String *const dstPath = strNewFmt("%s/%s", ROCKSDB_BACKUP_SUBDIR, strZ(entry.name));

        storageCopyP(storageNewReadP(srcStorage, srcPath), storageNewWriteP(dstStorage, dstPath));
        copied++;
    }

    return copied;
}

static void
engineRocksdbCopyOnline(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

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

static const EngineHandler rocksdbHandler =
{
    .kind = mysqlEngineMyrocks,
    .name = "rocksdb",
    .copyOnline = engineRocksdbCopyOnline,
};

FN_EXTERN const EngineHandler *
engineRocksdbHandler(void)
{
    return &rocksdbHandler;
}
