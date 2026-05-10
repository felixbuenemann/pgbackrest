/***********************************************************************************************************************************
InnoDB Redo Log Copier (Phase D scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/redoLog.h"
#include "common/debug.h"
#include "common/log.h"

struct RedoLogCopier
{
    MemContext *memContext;
};

/**********************************************************************************************************************************/
FN_EXTERN RedoLogCopier *
redoLogCopierNew(
    const Storage *const dataStorage, const String *const dataPath, const Storage *const repoStorage,
    const String *const repoPath, const uint64_t startLsn)
{
    (void)dataStorage; (void)dataPath; (void)repoStorage; (void)repoPath; (void)startLsn;
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierNew — open ib_logfile* or #innodb_redo/, prep mem context");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierStart(RedoLogCopier *const this)
{
    (void)this;
    // Spawn pthread (via common/fork.c or src/protocol/parallel.c). Reader loop = read 512-byte log block, validate LOG_BLOCK_CHECKSUM,
    // write through the IO pipeline; advance scanned-lsn atomic. On wrap-around (5.7 fixed), follow LSN monotonicity.
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierStart — pthread spawn + block-by-block copy");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierStop(RedoLogCopier *const this, const uint64_t stopLsn)
{
    (void)this; (void)stopLsn;
    // Atomic stop_at(stopLsn). Reader thread polls and exits when scanned >= stopLsn.
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierStop — signal stop LSN");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierJoin(RedoLogCopier *const this)
{
    (void)this;
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierJoin — pthread_join + propagate thread error");
}
