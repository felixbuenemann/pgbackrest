/***********************************************************************************************************************************
ISAM Engine Module (MySQL 3.21 → 4.0.2)

Same shape as engineMyisam: walk per-schema dirs for *.ISD files, copy the .ISD/.ISM/.frm trio. Pure file copy under the
caller's lock; no engine-specific SQL.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/engine/isam.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static void
isamCopyOne(
    const Storage *const srcStorage, const String *const srcPath, const Storage *const dstStorage, const String *const dstPath)
{
    Buffer *const content = storageGetP(storageNewReadP(srcStorage, srcPath, .ignoreMissing = true));

    if (content != NULL)
        storagePutP(storageNewWriteP(dstStorage, dstPath), content);
}

/**********************************************************************************************************************************/
static void
engineIsamCopyUnderLock(EngineBackupCtx *const ctx)
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

        // Walk top-level for schema dirs
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

        for (unsigned int idx = 0; idx < strLstSize(schemas); idx++)
        {
            const String *const schema = strLstGet(schemas, idx);
            StorageIterator *const itr = storageNewItrP(
                srcStorage, schema, .level = storageInfoLevelType, .nullOnMissing = true);

            if (itr == NULL)
                continue;

            while (storageItrMore(itr))
            {
                const StorageInfo entry = storageItrNext(itr);

                if (!entry.exists || entry.type != storageTypeFile || !strEndsWithZ(entry.name, ".ISD"))
                    continue;

                const String *const tableName = strSubN(entry.name, 0, strSize(entry.name) - 4);
                const String *const isd = strNewFmt("%s/%s.ISD", strZ(schema), strZ(tableName));
                const String *const ism = strNewFmt("%s/%s.ISM", strZ(schema), strZ(tableName));
                const String *const frm = strNewFmt("%s/%s.frm", strZ(schema), strZ(tableName));

                isamCopyOne(srcStorage, isd, dstStorage, isd);
                isamCopyOne(srcStorage, ism, dstStorage, ism);
                isamCopyOne(srcStorage, frm, dstStorage, frm);

                copied++;
            }
        }

        LOG_INFO_FMT("ISAM: copied %u table(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler isamHandler =
{
    .kind = mysqlEngineIsam,
    .name = "isam",
    .prepare = NULL,
    .copyOnline = NULL,
    .copyUnderLock = engineIsamCopyUnderLock,
    .finalize = NULL,
};

FN_EXTERN const EngineHandler *
engineIsamHandler(void)
{
    return &isamHandler;
}
