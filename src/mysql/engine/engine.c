/***********************************************************************************************************************************
Storage Engine Plugin Registry (Phase D scaffolding)

The lookup table is populated as each engine module ships:
  - innodb.c  : Phase D core (redo + tablespace + system tablespace + DD)
  - myisam.c  : Phase D (.MYD/.MYI under lock)
  - rocksdb.c : Phase D (checkpoint API + SST/log copy)
  - aria.c    : Phase D (MariaDB only; aria_backup API)
  - toku.c    : v1.1 (HOOK API via tokudb-xtrabackup fork)

Until those land this returns NULL for every name; backup orchestration in src/command/backup/backup.c will detect that and
fail loudly with a "no handler for engine X" message.
***********************************************************************************************************************************/
#include <build.h>

#include <strings.h>
#include <string.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/engine/archive.h"
#include "mysql/engine/aria.h"
#include "mysql/engine/connect.h"
#include "mysql/engine/csv.h"
#include "mysql/engine/engine.h"
#include "mysql/engine/innodb.h"
#include "mysql/engine/isam.h"
#include "mysql/engine/merge.h"
#include "mysql/engine/mroonga.h"
#include "mysql/engine/myisam.h"
#include "mysql/engine/rocksdb.h"
#include "mysql/engine/toku.h"
#include "storage/iterator.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Registry of all engines this build supports. Each row maps one or more case-insensitive names (as reported by
INFORMATION_SCHEMA.ENGINES.ENGINE) to a handler vtable. Adding a new engine = appending one row.
***********************************************************************************************************************************/
typedef struct EngineRegistryEntry
{
    const char *const names[4];                                         // NULL-terminated; entries compared case-insensitively
    const EngineHandler *(*const factory)(void);
} EngineRegistryEntry;

static const EngineRegistryEntry engineRegistry[] = {
    {{"innodb", "xtradb", NULL}, engineInnodbHandler},
    {{"myisam", NULL}, engineMyisamHandler},
    {{"isam", NULL}, engineIsamHandler},
    {{"rocksdb", "myrocks", NULL}, engineRocksdbHandler},
    {{"aria", NULL}, engineAriaHandler},
    {{"tokudb", NULL}, engineTokuHandler},
    {{"csv", NULL}, engineCsvHandler},
    {{"archive", NULL}, engineArchiveHandler},
    {{"mrg_myisam", "merge", NULL}, engineMergeHandler},
    {{"connect", NULL}, engineConnectHandler},
    {{"mroonga", NULL}, engineMroongaHandler},
};

/**********************************************************************************************************************************/
FN_EXTERN const EngineHandler *
engineHandlerLookup(const String *const engineName)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, engineName);
    FUNCTION_TEST_END();

    ASSERT(engineName != NULL);

    for (size_t i = 0; i < sizeof(engineRegistry) / sizeof(engineRegistry[0]); i++)
    {
        for (size_t n = 0; engineRegistry[i].names[n] != NULL; n++)
        {
            if (strcasecmp(strZ(engineName), engineRegistry[i].names[n]) == 0)
                FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineRegistry[i].factory());
        }
    }

    // Trivial engines (CSV, MEMORY, FEDERATED, ARCHIVE, BLACKHOLE) deliberately have no handler — orchestrator skips them.
    FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, NULL);
}

/**********************************************************************************************************************************/
FN_EXTERN void
engineFlatCopyByExtension(
    EngineBackupCtx *const ctx, const char *const primaryExt, const char *const companionExts[], const char *const logLabel)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);
    ASSERT(ctx->dataPath != NULL);
    ASSERT(ctx->backupPath != NULL);
    ASSERT(primaryExt != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        const size_t primaryExtLen = strlen(primaryExt);

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

                if (!entry.exists || entry.type != storageTypeFile || !strEndsWithZ(entry.name, primaryExt))
                    continue;

                const String *const tableName = strSubN(entry.name, 0, strSize(entry.name) - primaryExtLen);

                // Primary file
                const String *const primaryPath = strNewFmt("%s/%s%s", strZ(schema), strZ(tableName), primaryExt);
                storageCopyP(storageNewReadP(srcStorage, primaryPath), storageNewWriteP(dstStorage, primaryPath));

                // Companion files (.MYI/.frm/...) — silently skip if absent (.frm optional on 8.0+)
                for (size_t cIdx = 0; companionExts[cIdx] != NULL; cIdx++)
                {
                    const String *const cPath = strNewFmt("%s/%s%s", strZ(schema), strZ(tableName), companionExts[cIdx]);
                    storageCopyP(
                        storageNewReadP(srcStorage, cPath, .ignoreMissing = true),
                        storageNewWriteP(dstStorage, cPath));
                }

                copied++;
            }
        }

        LOG_INFO_FMT("%s: copied %u table(s) from %s", logLabel, copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
