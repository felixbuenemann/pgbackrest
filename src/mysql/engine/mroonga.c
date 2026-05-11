/***********************************************************************************************************************************
Mroonga Engine Module
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/stringList.h"
#include "mysql/engine/mroonga.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Mroonga files don't fit the engineFlatCopyByExtension shape because segment files use numeric suffixes (.mrn.000000001 etc.)
rather than a single extension. Custom walker: any file whose name has ".mrn" as the first extension after the table name, plus
any further suffix-only variations.
***********************************************************************************************************************************/
static bool
mroongaIsMatch(const String *const name)
{
    // Match patterns: <table>.mrn, <table>.mrn.NNNNNNNN, <table>.mrn.c, <table>.mrn.l, <table>.mrn.s, <table>.mrn.i
    const char *const dot = strstr(strZ(name), ".mrn");
    if (dot == NULL)
        return false;

    const char *const after = dot + 4;                                  // points just past ".mrn"

    // True if the string ends right after ".mrn" or is followed by another dot (segments/column-store sidecars)
    return *after == '\0' || *after == '.';
}

static void
engineMroongaCopyUnderLock(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        StringList *const schemas = strLstNew();
        StorageIterator *const topItr = storageNewItrP(srcStorage, NULL, .level = storageInfoLevelType);

        while (storageItrMore(topItr))
        {
            const StorageInfo info = storageItrNext(topItr);

            if (info.exists && info.type == storageTypePath && strSize(info.name) > 0 &&
                strZ(info.name)[0] != '.' && !strEqZ(info.name, "lost+found"))
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

                if (!entry.exists || entry.type != storageTypeFile || !mroongaIsMatch(entry.name))
                    continue;

                const String *const path = strNewFmt("%s/%s", strZ(schema), strZ(entry.name));
                storageCopyP(storageNewReadP(srcStorage, path), storageNewWriteP(dstStorage, path));
                copied++;
            }
        }

        LOG_INFO_FMT("Mroonga: copied %u file(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler mroongaHandler =
{
    .kind = mysqlEngineMroonga,
    .name = "mroonga",
    .copyUnderLock = engineMroongaCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineMroongaHandler(void)
{
    return &mroongaHandler;
}
