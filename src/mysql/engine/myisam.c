/***********************************************************************************************************************************
MyISAM Engine Module

Backs up MyISAM tables by walking the datadir for *.MYD files and copying each MYD/MYI pair into the backup. The optional .frm
schema descriptor (5.7-only; replaced by .sdi inside mysql.ibd on 8.0+) is also copied when present.

Online vs offline mode:
  - Online (ctx->client != NULL): the lock is already held by Phase D's orchestrator (LOCK INSTANCE FOR BACKUP / BACKUP STAGE
    BLOCK_DDL / FLUSH TABLES WITH READ LOCK), so files are guaranteed quiescent.
  - Offline (ctx->client == NULL): assumes the server is shut down. This is myBackRest's cold-backup path.

Either way, the walk algorithm is identical — we don't query INFORMATION_SCHEMA; we just trust the filesystem.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/engine/myisam.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Copy one file from the source storage to the dest storage. Uses storageGet/storagePut so the standard IO pipeline (compression /
encryption / blockIncr) applies if the dest storage was constructed with those filters.
***********************************************************************************************************************************/
static void
myisamCopyOne(
    const Storage *const srcStorage, const String *const srcPath, const Storage *const dstStorage, const String *const dstPath)
{
    Buffer *const content = storageGetP(storageNewReadP(srcStorage, srcPath, .ignoreMissing = true));

    if (content != NULL)
        storagePutP(storageNewWriteP(dstStorage, dstPath), content);
}

/**********************************************************************************************************************************/
static void
engineMyisamCopyUnderLock(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);
    ASSERT(ctx->dataPath != NULL);
    ASSERT(ctx->backupPath != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Source + dest storage. Production wires these through pgBackRest's repo abstraction; for now we use POSIX directly so
        // this module is exercisable in isolation.
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        // Pass 1: enumerate schema directories at the datadir top level
        StringList *const schemas = strLstNew();
        StorageIterator *const topItr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(topItr))
        {
            const StorageInfo info = storageItrNext(topItr);

            if (info.exists && info.type == storageTypePath && strSize(info.name) > 0 && strZ(info.name)[0] != '.' &&
                !strEqZ(info.name, "lost+found"))
            {
                strLstAdd(schemas, info.name);
            }
        }

        unsigned int copied = 0;

        // Pass 2: walk each schema for *.MYD files; for each MYD found, also copy the matching .MYI and (optional) .frm
        for (unsigned int schemaIdx = 0; schemaIdx < strLstSize(schemas); schemaIdx++)
        {
            const String *const schema = strLstGet(schemas, schemaIdx);
            StorageIterator *const schemaItr = storageNewItrP(
                srcStorage, schema, .level = storageInfoLevelType, .nullOnMissing = true);

            if (schemaItr == NULL)
                continue;

            while (storageItrMore(schemaItr))
            {
                const StorageInfo entry = storageItrNext(schemaItr);

                if (!entry.exists || entry.type != storageTypeFile || !strEndsWithZ(entry.name, ".MYD"))
                    continue;

                // Strip the .MYD suffix to derive the base table name
                const String *const tableName = strSubN(entry.name, 0, strSize(entry.name) - 4);

                // Source paths
                const String *const myd = strNewFmt("%s/%s.MYD", strZ(schema), strZ(tableName));
                const String *const myi = strNewFmt("%s/%s.MYI", strZ(schema), strZ(tableName));
                const String *const frm = strNewFmt("%s/%s.frm", strZ(schema), strZ(tableName));

                myisamCopyOne(srcStorage, myd, dstStorage, myd);
                myisamCopyOne(srcStorage, myi, dstStorage, myi);
                myisamCopyOne(srcStorage, frm, dstStorage, frm);                            // .frm is 5.7-only — ignoreMissing handles 8.0+

                copied++;
            }
        }

        LOG_INFO_FMT("MyISAM: copied %u table(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler myisamHandler =
{
    .kind = mysqlEngineMyisam,
    .name = "myisam",
    .prepare = NULL,
    .copyOnline = NULL,
    .copyUnderLock = engineMyisamCopyUnderLock,
    .finalize = NULL,
};

FN_EXTERN const EngineHandler *
engineMyisamHandler(void)
{
    return &myisamHandler;
}
