/***********************************************************************************************************************************
InnoDB Redo Log Copier (Phase D scaffolding)

Architectural note (resolves the implementation choice flagged by the plan):

pgBackRest is a multi-process design — every parallel worker is a separate child process talking to the orchestrator over the
src/protocol/ RPC machinery. There are no pthreads anywhere in the existing codebase. The xtrabackup reference uses a pthread
for redo capture because its data model is single-process; that pattern is NOT idiomatic for this codebase.

The pgBackRest-native shape for the redo copier is therefore a long-running WORKER PROCESS spawned via src/protocol/parallel.c,
not a thread. The worker:

  1. Opens the redo log files (ib_logfile{0,1} or every #ib_redoN_<lsn> in #innodb_redo/)
  2. Loops reading 512-byte log blocks, validates LOG_BLOCK_CHECKSUM (CRC32 of bytes 0..507),
     writes through the standard pgBackRest IO pipeline (compression/encryption/blockIncr filters apply transparently)
  3. Polls every db-redo-poll-interval ms for new bytes appended to the current file
  4. Listens for a "stop at LSN" message from the orchestrator over the protocol pipe
  5. Drains to that LSN, exits

This approach also gets us free progress reporting, retry-on-error, and the ability to run the copier on a remote backup-from-
standby host — all features that pgBackRest's parallel/protocol layer already provides.

Implementing requires extending src/protocol/parallel.c with a "service worker" notion (vs the current "one-shot job" model).
That's significant scope and is left for the dedicated Phase D commit. The stub functions below intentionally throw to flag the
architectural decision that needs to land before the protocol extension is written.
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/redoLog.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/interface.h"
#include "storage/iterator.h"
#include "storage/storage.h"

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
    THROW(
        AssertError,
        "TODO(myBackRest-D): redoLogCopierNew — needs the 'service worker' extension to src/protocol/parallel.c."
        " See file header for the design rationale (process not thread).");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierStart(RedoLogCopier *const this)
{
    (void)this;
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierStart — spawn service worker once protocol extension is in place");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierStop(RedoLogCopier *const this, const uint64_t stopLsn)
{
    (void)this; (void)stopLsn;
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierStop — send stop-at-LSN protocol message to worker");
}

/**********************************************************************************************************************************/
FN_EXTERN void
redoLogCopierJoin(RedoLogCopier *const this)
{
    (void)this;
    THROW(AssertError, "TODO(myBackRest-D): redoLogCopierJoin — waitpid worker + propagate any worker error");
}

/**********************************************************************************************************************************/
FN_EXTERN unsigned int
redoLogColdCopy(
    const Storage *const srcStorage, const String *const srcPath, const Storage *const dstStorage, const String *const dstPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, srcStorage);
        FUNCTION_LOG_PARAM(STRING, srcPath);
        FUNCTION_LOG_PARAM(STORAGE, dstStorage);
        FUNCTION_LOG_PARAM(STRING, dstPath);
    FUNCTION_LOG_END();

    ASSERT(srcStorage != NULL);
    ASSERT(srcPath != NULL);
    ASSERT(dstStorage != NULL);
    ASSERT(dstPath != NULL);

    unsigned int copied = 0;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // 8.0.30+ dynamic layout: every regular file under #innodb_redo/ gets copied, including the _tmp pre-allocated files —
        // the restore target should look bit-identical to the source.
        const String *const srcRedoDir = strNewFmt("%s/%s", strZ(srcPath), MYSQL_PATH_INNODB_REDO);
        const String *const dstRedoDir = strNewFmt("%s/%s", strZ(dstPath), MYSQL_PATH_INNODB_REDO);

        StorageIterator *const dirItr = storageNewItrP(
            srcStorage, srcRedoDir, .level = storageInfoLevelType, .nullOnMissing = true);

        if (dirItr != NULL)
        {
            while (storageItrMore(dirItr))
            {
                const StorageInfo entry = storageItrNext(dirItr);

                if (!entry.exists || entry.type != storageTypeFile)
                    continue;

                const String *const srcFile = strNewFmt("%s/%s", strZ(srcRedoDir), strZ(entry.name));
                const String *const dstFile = strNewFmt("%s/%s", strZ(dstRedoDir), strZ(entry.name));

                storageCopyP(storageNewReadP(srcStorage, srcFile), storageNewWriteP(dstStorage, dstFile));
                copied++;
            }
        }

        // Fixed layout: ib_logfile0..1 at the datadir root. Try both even on MariaDB 10.5+ (single-file) — the loop simply
        // skips the missing one. ib_logfile1 is only present on pre-8.0.30 servers with innodb_log_files_in_group >= 2.
        for (unsigned int n = 0; n < 2; n++)
        {
            const String *const fileName = strNewFmt("%s%u", MYSQL_FILE_IB_LOGFILE_PREFIX, n);
            const String *const srcFile = strNewFmt("%s/%s", strZ(srcPath), strZ(fileName));

            if (!storageExistsP(srcStorage, srcFile))
                continue;

            const String *const dstFile = strNewFmt("%s/%s", strZ(dstPath), strZ(fileName));
            storageCopyP(storageNewReadP(srcStorage, srcFile), storageNewWriteP(dstStorage, dstFile));
            copied++;
        }

        if (copied == 0)
            LOG_INFO("redo log: no files found (datadir may predate server bootstrap)");
        else
            LOG_INFO_FMT("redo log: copied %u file(s)", copied);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(UINT, copied);
}
