/***********************************************************************************************************************************
MyRocks / RocksDB Engine Module (Phase D scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/rocksdb.h"

static void
engineRocksdbPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // mkdir <backup>/#rocksdb_checkpoint  (transient)
    // SET SESSION rocksdb_create_checkpoint = '<that path>'
    THROW(AssertError, "TODO(myBackRest-D): engineRocksdbPrepare — issue SET SESSION rocksdb_create_checkpoint");
}

static void
engineRocksdbCopyOnline(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Walk checkpoint dir; copy *.sst, MANIFEST-*, CURRENT, OPTIONS-*, *.log into <backup>/#rocksdb/
    THROW(AssertError, "TODO(myBackRest-D): engineRocksdbCopyOnline — copy SST/MANIFEST/CURRENT/OPTIONS files");
}

static void
engineRocksdbFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // rmdir transient checkpoint dir
    THROW(AssertError, "TODO(myBackRest-D): engineRocksdbFinalize — drop transient checkpoint directory");
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
