/***********************************************************************************************************************************
TokuDB Engine Module (Post-v1 scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/toku.h"

static void
engineTokuPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Reference: /home/user/tokudb-xtrabackup/  (xelabs fork)
    THROW(
        AssertError,
        "TODO(myBackRest-D-tokudb post-v1): engineTokuPrepare — dlopen libHotBackup.so + SET GLOBAL tokudb_checkpoint_lock=1");
}

static void
engineTokuCopyOnline(EngineBackupCtx *const ctx)
{
    (void)ctx;
    THROW(AssertError, "TODO(myBackRest-D-tokudb post-v1): engineTokuCopyOnline — invoke HotBackup::do_backup primitives");
}

static void
engineTokuFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    THROW(AssertError, "TODO(myBackRest-D-tokudb post-v1): engineTokuFinalize — release tokudb_checkpoint_lock");
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
