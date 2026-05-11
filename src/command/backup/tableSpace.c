/***********************************************************************************************************************************
InnoDB Tablespace Iterator

Walks the datadir once at construction time to enumerate every InnoDB tablespace file the backup needs to copy:
  - System tablespace:        ibdata1, ibdata2, ... (top-level)
  - Data dictionary (8.0+):   mysql.ibd
  - Undo tablespaces (8.0+):  undo_NNN.ibu  (top-level by default; alternate location via innodb-undo-directory not yet handled)
  - Per-table tablespaces:    <schema>/<table>.ibd   (per-schema subdirectories, recurse one level)

Deliberately excluded:
  - #innodb_dblwr  — doublewrite buffer; mysqld recreates it on startup, no need to back it up
  - #innodb_redo   — handled by RedoLogCopier (src/command/backup/redoLog.c)
  - lost+found     — ext filesystem artifact

Not yet handled (deferred to v1.x):
  - innodb-undo-directory pointing outside the datadir
  - innodb-data-home-dir pointing outside the datadir
  - General Tablespaces (CREATE TABLESPACE foo ADD DATAFILE 'foo.ibd' under any user-chosen path)
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/type/list.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/interface.h"
#include "storage/iterator.h"
#include "storage/storage.h"

struct TableSpaceIter
{
    TableSpaceIterPub pub;                                              // Publicly accessible variables (fileTotal)
    StringList *files;
    unsigned int nextIdx;
};

/***********************************************************************************************************************************
Predicate: is this a top-level tablespace file we should back up?

Matches:
  - ibdata*                  — InnoDB system tablespace (MySQL/Percona/MariaDB)
  - mysql.ibd                — MySQL/Percona 8.0+ data dictionary tablespace
  - undo_NNN.ibu             — MySQL/Percona 8.0+ undo tablespaces (note the underscore + .ibu extension)
  - undoNNN                  — MariaDB undo tablespaces (no underscore, no extension; same FSP header layout though)
                               See mariadb-server srv_undo_dir + srv_undo_tablespaces_open logic; default names are
                               undo001 / undo002 / undo003.
***********************************************************************************************************************************/
static bool
tableSpaceIsTopLevelMatch(const String *const name)
{
    if (strBeginsWithZ(name, "ibdata"))
        return true;

    if (strEqZ(name, MYSQL_FILE_MYSQL_IBD))
        return true;

    if (strBeginsWithZ(name, "undo_") && strEndsWithZ(name, ".ibu"))
        return true;

    // MariaDB-style "undoNNN" (no underscore, no extension). Match a fixed prefix + 3+ digits, no extension.
    if (strBeginsWithZ(name, "undo") && strSize(name) >= 7)
    {
        const char *const after = strZ(name) + 4;                       // points past "undo"
        bool allDigits = true;
        for (const char *c = after; *c != '\0'; c++)
        {
            if (*c < '0' || *c > '9')
            {
                allDigits = false;
                break;
            }
        }
        if (allDigits)
            return true;
    }

    return false;
}

/***********************************************************************************************************************************
Predicate: should this top-level entry be skipped entirely (do NOT recurse into the subdir)?
***********************************************************************************************************************************/
static bool
tableSpaceShouldSkipDir(const String *const name)
{
    return
        strEqZ(name, MYSQL_PATH_INNODB_DBLWR) ||
        strEqZ(name, MYSQL_PATH_INNODB_REDO)  ||
        strEqZ(name, "lost+found");
}

/**********************************************************************************************************************************/
FN_EXTERN TableSpaceIter *
tableSpaceIterNew(const Storage *const dataStorage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, dataStorage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(dataStorage != NULL);
    ASSERT(dataPath != NULL);

    OBJ_NEW_BEGIN(TableSpaceIter, .childQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (TableSpaceIter){.pub = {.fileTotal = 0}, .files = strLstNew(), .nextIdx = 0};

        MEM_CONTEXT_TEMP_BEGIN()
        {
            // Pass 1: top-level entries. Capture the matching files; remember the schema directories for pass 2.
            StringList *const schemaDirs = strLstNew();

            StorageIterator *const topItr = storageNewItrP(dataStorage, dataPath, .level = storageInfoLevelType);

            while (storageItrMore(topItr))
            {
                const StorageInfo info = storageItrNext(topItr);

                if (!info.exists)
                    continue;

                if (info.type == storageTypeFile)
                {
                    if (tableSpaceIsTopLevelMatch(info.name))
                        strLstAdd(this->files, info.name);
                }
                else if (info.type == storageTypePath && !tableSpaceShouldSkipDir(info.name))
                {
                    // Don't recurse into hidden / dot-prefixed dirs except #innodb_redo / #innodb_dblwr (handled above).
                    // Schema names start with a regular character.
                    if (strSize(info.name) > 0 && strZ(info.name)[0] != '.')
                        strLstAdd(schemaDirs, info.name);
                }
            }

            // Pass 2: per-schema *.ibd files. Done as a separate listing per directory so we don't build one giant recursive
            // tree (millions of tables × thousands of files each is plausible at scale).
            for (unsigned int schemaIdx = 0; schemaIdx < strLstSize(schemaDirs); schemaIdx++)
            {
                const String *const schema = strLstGet(schemaDirs, schemaIdx);
                const String *const schemaPath = strNewFmt("%s/%s", strZ(dataPath), strZ(schema));

                StorageIterator *const schemaItr = storageNewItrP(
                    dataStorage, schemaPath, .level = storageInfoLevelType, .nullOnMissing = true);

                if (schemaItr == NULL)
                    continue;

                while (storageItrMore(schemaItr))
                {
                    const StorageInfo schemaInfo = storageItrNext(schemaItr);

                    if (schemaInfo.exists && schemaInfo.type == storageTypeFile && strEndsWithZ(schemaInfo.name, ".ibd"))
                    {
                        // Store relative path so the consumer can join it back with dataPath when opening the file.
                        strLstAdd(this->files, strNewFmt("%s/%s", strZ(schema), strZ(schemaInfo.name)));
                    }
                }
            }

            // Sort for determinism (helps tests + log readability)
            strLstSort(this->files, sortOrderAsc);

            this->pub.fileTotal = strLstSize(this->files);
        }
        MEM_CONTEXT_TEMP_END();
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(TABLE_SPACE_ITER, this);
}

/**********************************************************************************************************************************/
FN_EXTERN String *
tableSpaceIterNext(TableSpaceIter *const this)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(TABLE_SPACE_ITER, this);
    FUNCTION_TEST_END();

    ASSERT(this != NULL);

    if (this->nextIdx >= strLstSize(this->files))
        FUNCTION_TEST_RETURN(STRING, NULL);

    String *const result = strLstGet(this->files, this->nextIdx);
    this->nextIdx++;

    FUNCTION_TEST_RETURN(STRING, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
tableSpaceIterToLog(const TableSpaceIter *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(debugLog, "{fileTotal: %u, nextIdx: %u}", tableSpaceIterFileTotal(this), this->nextIdx);
}
