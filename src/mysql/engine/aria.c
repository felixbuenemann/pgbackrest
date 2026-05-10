/***********************************************************************************************************************************
Aria Engine Module (MariaDB only)

Aria is MariaDB's crash-safe replacement for MyISAM and is the default engine for system tables in modern MariaDB. Backup
artefacts:

  Top-level (per-instance):
    aria_log_control                   ← Aria recovery state
    aria_log.NNNNNNNN                  ← write-ahead log files (one or more)

  Per-table (under <datadir>/<schema>/):
    <table>.MAD                        ← data file
    <table>.MAI                        ← index file
    <table>.frm    (5.x compatibility) ← optional schema descriptor

Online mode (with live MariaDB server): coordinated with BACKUP STAGE: copy stable .MAD/.MAI files during STAGE START, copy the
aria_log tail during STAGE FLUSH/BLOCK_DDL. That coordination needs Phase D's orchestrator wiring; this module's offline path
copies everything as-is which is safe when the server is shut down.

Reference: mariabackup aria_backup_client.cc:BackupImpl::start() line 597 + copy_log_tail() line 710.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/engine/aria.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Copy one source file to the destination, ignoring missing-source. Used for both top-level and per-schema files.
***********************************************************************************************************************************/
static void
ariaCopyOne(
    const Storage *const srcStorage, const String *const srcPath, const Storage *const dstStorage, const String *const dstPath)
{
    Buffer *const content = storageGetP(storageNewReadP(srcStorage, srcPath, .ignoreMissing = true));

    if (content != NULL)
        storagePutP(storageNewWriteP(dstStorage, dstPath), content);
}

/**********************************************************************************************************************************/
static void
engineAriaCopyOnline(EngineBackupCtx *const ctx)
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

        // Top-level: aria_log_control plus every aria_log.* file
        ariaCopyOne(srcStorage, STRDEF("aria_log_control"), dstStorage, STRDEF("aria_log_control"));

        unsigned int logsCopied = 0;
        StorageIterator *const topItr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(topItr))
        {
            const StorageInfo info = storageItrNext(topItr);

            if (info.exists && info.type == storageTypeFile && strBeginsWithZ(info.name, "aria_log."))
            {
                ariaCopyOne(srcStorage, info.name, dstStorage, info.name);
                logsCopied++;
            }
        }

        // Per-schema: walk for *.MAD, copy .MAD/.MAI/.frm trio
        StringList *const schemas = strLstNew();
        StorageIterator *const schemaScan = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(schemaScan))
        {
            const StorageInfo info = storageItrNext(schemaScan);

            if (info.exists && info.type == storageTypePath && strSize(info.name) > 0 && strZ(info.name)[0] != '.' &&
                !strEqZ(info.name, "lost+found"))
            {
                strLstAdd(schemas, info.name);
            }
        }

        unsigned int tablesCopied = 0;

        for (unsigned int schemaIdx = 0; schemaIdx < strLstSize(schemas); schemaIdx++)
        {
            const String *const schema = strLstGet(schemas, schemaIdx);
            StorageIterator *const itr = storageNewItrP(
                srcStorage, schema, .level = storageInfoLevelType, .nullOnMissing = true);

            if (itr == NULL)
                continue;

            while (storageItrMore(itr))
            {
                const StorageInfo entry = storageItrNext(itr);

                if (!entry.exists || entry.type != storageTypeFile || !strEndsWithZ(entry.name, ".MAD"))
                    continue;

                const String *const tableName = strSubN(entry.name, 0, strSize(entry.name) - 4);
                const String *const mad = strNewFmt("%s/%s.MAD", strZ(schema), strZ(tableName));
                const String *const mai = strNewFmt("%s/%s.MAI", strZ(schema), strZ(tableName));
                const String *const frm = strNewFmt("%s/%s.frm", strZ(schema), strZ(tableName));

                ariaCopyOne(srcStorage, mad, dstStorage, mad);
                ariaCopyOne(srcStorage, mai, dstStorage, mai);
                ariaCopyOne(srcStorage, frm, dstStorage, frm);

                tablesCopied++;
            }
        }

        LOG_INFO_FMT("Aria: copied %u table(s) and %u log file(s)", tablesCopied, logsCopied);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static void
engineAriaCopyUnderLock(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Online mode would re-snapshot the aria_log tail here. Offline mode already captured the full set in copyOnline.
}

static const EngineHandler ariaHandler =
{
    .kind = mysqlEngineAria,
    .name = "aria",
    .prepare = NULL,
    .copyOnline = engineAriaCopyOnline,
    .copyUnderLock = engineAriaCopyUnderLock,
    .finalize = NULL,
};

FN_EXTERN const EngineHandler *
engineAriaHandler(void)
{
    return &ariaHandler;
}
