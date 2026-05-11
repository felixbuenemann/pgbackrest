/***********************************************************************************************************************************
Miscellaneous-Files Copier
***********************************************************************************************************************************/
#include <build.h>

#include <strings.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/miscFiles.h"
#include "storage/iterator.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Extensions that engine handlers claim. Files with these suffixes are SKIPPED here — handlers in src/mysql/engine/ copy them.
Case-insensitive match (MyISAM is upper-cased, ISAM upper-cased, Aria upper-cased, InnoDB lower-cased — historical asymmetry).
***********************************************************************************************************************************/
static const char *const engineExtensions[] = {
    ".ibd",                                                             // InnoDB per-table tablespace
    ".MYD", ".MYI",                                                     // MyISAM
    ".MAD", ".MAI",                                                     // Aria
    ".ISD", ".ISM",                                                     // ISAM (MySQL 3.21..4.0.2)
    ".CSV", ".CSM",                                                     // CSV engine
    ".ARZ",                                                             // ARCHIVE engine
    ".MRG",                                                             // MERGE engine
    ".dnx",                                                             // MariaDB CONNECT engine index
    ".tokudb",                                                          // TokuDB main file
    NULL,
};

static bool
isEngineHandledFile(const String *const fileName)
{
    for (size_t i = 0; engineExtensions[i] != NULL; i++)
    {
        const size_t extLen = strlen(engineExtensions[i]);
        if (strSize(fileName) <= extLen)
            continue;

        // Suffix match, case-insensitive
        if (strcasecmp(strZ(fileName) + strSize(fileName) - extLen, engineExtensions[i]) == 0)
            return true;
    }

    // Mroonga uses variable suffixes: <table>.mrn, <table>.mrn.NNNNNNNN, <table>.mrn.c/l/s/i. A pure-suffix list can't capture
    // these — substring check on ".mrn" is the proxy (the Mroonga handler uses the same predicate).
    if (strstr(strZ(fileName), ".mrn") != NULL)
        return true;

    return false;
}

/***********************************************************************************************************************************
Top-level directory filter — same convention as the engine handlers + datadir inspector.
***********************************************************************************************************************************/
static bool
isSchemaDirCandidate(const String *const name)
{
    return strSize(name) > 0 && strZ(name)[0] != '.' &&
           !strEqZ(name, "lost+found") &&
           !strEqZ(name, "#innodb_redo") &&
           !strEqZ(name, "#innodb_dblwr") &&
           !strEqZ(name, "#innodb_temp") &&
           !strEqZ(name, "#rocksdb") &&
           !strEqZ(name, ".rocksdb");
}

/**********************************************************************************************************************************/
FN_EXTERN unsigned int
mysqlMiscFilesCopy(
    const Storage *const srcStorage, const String *const dataPath,
    const Storage *const dstStorage, const String *const backupPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, srcStorage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
        FUNCTION_LOG_PARAM(STORAGE, dstStorage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
    FUNCTION_LOG_END();

    ASSERT(srcStorage != NULL);
    ASSERT(dataPath != NULL);
    ASSERT(dstStorage != NULL);
    ASSERT(backupPath != NULL);

    unsigned int copied = 0;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        StorageIterator *const topItr = storageNewItrP(srcStorage, dataPath, .level = storageInfoLevelType);

        while (storageItrMore(topItr))
        {
            const StorageInfo entry = storageItrNext(topItr);

            if (!entry.exists || entry.type != storageTypePath || !isSchemaDirCandidate(entry.name))
                continue;

            const String *const srcSchema = strNewFmt("%s/%s", strZ(dataPath), strZ(entry.name));
            const String *const dstSchema = strNewFmt("%s/%s", strZ(backupPath), strZ(entry.name));

            // Ensure the schema directory exists in the destination, even if every file in it is engine-handled (mysqld
            // expects sys/, performance_schema/ etc. to be on disk).
            storagePathCreateP(dstStorage, dstSchema, .errorOnExists = false);

            StorageIterator *const schemaItr = storageNewItrP(
                srcStorage, srcSchema, .level = storageInfoLevelType, .nullOnMissing = true);

            if (schemaItr == NULL)
                continue;

            while (storageItrMore(schemaItr))
            {
                const StorageInfo file = storageItrNext(schemaItr);

                if (!file.exists || file.type != storageTypeFile)
                    continue;

                if (isEngineHandledFile(file.name))
                    continue;

                const String *const srcFile = strNewFmt("%s/%s", strZ(srcSchema), strZ(file.name));
                const String *const dstFile = strNewFmt("%s/%s", strZ(dstSchema), strZ(file.name));

                storageCopyP(storageNewReadP(srcStorage, srcFile), storageNewWriteP(dstStorage, dstFile));
                copied++;
            }
        }

        LOG_INFO_FMT("misc-files: copied %u non-engine file(s) across schema directories", copied);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(UINT, copied);
}
