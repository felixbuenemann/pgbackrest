/***********************************************************************************************************************************
InnoDB / XtraDB Engine Module (Phase D scaffolding)

Real implementation lands in subsequent commits. The four lifecycle callbacks all throw with a TODO marker pointing back to the
xtrabackup reference algorithm so the next worker can pick up exactly where the trail leaves off.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/innodb.h"

/**********************************************************************************************************************************/
static void
engineInnodbPrepare(EngineBackupCtx *const ctx)
{
    (void)ctx;
    THROW(AssertError, "TODO(myBackRest-D): engineInnodbPrepare — load tablespace catalog from INFORMATION_SCHEMA.INNODB_TABLESPACES");
}

static void
engineInnodbCopyOnline(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Algorithm: spawn a redo-log copier thread (src/command/backup/redoLog.c) at the live LSN; in the foreground iterate the
    // tablespace list with --process-max parallel workers (src/protocol/parallel.c) copying .ibd / ibdata* / mysql.ibd /
    // undo_NNN.ibu files page-by-page through mysqlPageChecksumValidate. Reference: xtrabackup_backup_func() lines 4442 + 4500.
    THROW(AssertError, "TODO(myBackRest-D): engineInnodbCopyOnline — start redo copier + parallel file copy");
}

static void
engineInnodbCopyUnderLock(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Final pass: any tablespaces created since copyOnline started (newly created .ibd files visible via FIL space ID
    // monotonicity), plus signal the redo copier to flush-and-stop at the locked LSN.
    THROW(AssertError, "TODO(myBackRest-D): engineInnodbCopyUnderLock — copy newly created .ibd + stop redo at lock LSN");
}

static void
engineInnodbFinalize(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Write mybackrest_checkpoints (start/stop LSN, checkpoint LSN, redo file count). Verify redo coverage.
    THROW(AssertError, "TODO(myBackRest-D): engineInnodbFinalize — write checkpoints metadata");
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

/**********************************************************************************************************************************/
FN_EXTERN const EngineHandler *
engineInnodbHandler(void)
{
    return &innodbHandler;
}
