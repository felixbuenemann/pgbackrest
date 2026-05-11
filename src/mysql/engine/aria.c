/***********************************************************************************************************************************
Aria Engine Module (MariaDB only)

Per-table files (.MAD/.MAI/.frm) use the shared engineFlatCopyByExtension helper. Top-level aria_log_control + aria_log.* files
are handled inline since they don't fit the per-schema walk shape.

Online mode coordinates with BACKUP STAGE: stable .MAD/.MAI during STAGE START, log tail during BLOCK_DDL. Offline mode copies
everything in one pass when the server is shut down — the lock ladder is a no-op.

Reference: mariabackup aria_backup_client.cc:BackupImpl::start() line 597, copy_log_tail() line 710.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/engine/aria.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static const char *const ariaCompanions[] = {".MAI", ".frm", NULL};

/***********************************************************************************************************************************
Copy the top-level aria_log_control + every aria_log.<N> file. Streamed via storageCopyP so log size doesn't bloat memory.
***********************************************************************************************************************************/
static unsigned int
ariaCopyTopLevelLogs(EngineBackupCtx *const ctx)
{
    unsigned int copied = 0;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        storageCopyP(
            storageNewReadP(srcStorage, STRDEF("aria_log_control"), .ignoreMissing = true),
            storageNewWriteP(dstStorage, STRDEF("aria_log_control")));

        StorageIterator *const itr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(itr))
        {
            const StorageInfo info = storageItrNext(itr);

            if (info.exists && info.type == storageTypeFile && strBeginsWithZ(info.name, "aria_log."))
            {
                storageCopyP(storageNewReadP(srcStorage, info.name), storageNewWriteP(dstStorage, info.name));
                copied++;
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    return copied;
}

static void
engineAriaCopyOnline(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);

    const unsigned int logsCopied = ariaCopyTopLevelLogs(ctx);
    engineFlatCopyByExtension(ctx, ".MAD", ariaCompanions, "Aria");

    LOG_INFO_FMT("Aria: %u log file(s) copied at top level", logsCopied);

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler ariaHandler =
{
    .kind = mysqlEngineAria,
    .name = "aria",
    .copyOnline = engineAriaCopyOnline,
};

FN_EXTERN const EngineHandler *
engineAriaHandler(void)
{
    return &ariaHandler;
}
