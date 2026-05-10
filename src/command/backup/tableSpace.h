/***********************************************************************************************************************************
InnoDB Tablespace Iterator (Phase D)

Enumerates the tablespaces that need to be copied: ibdata*, mysql.ibd (8.0+), per-table .ibd files (under <datadir>/<schema>/),
undo_NNN.ibu (8.0+). Used by Phase D's parallel copier — each worker pulls the next tablespace from this iterator and copies it
page-by-page, validating each page's checksum before writing.

Doublewrite buffer files under #innodb_dblwr/ are intentionally excluded — mysqld recreates them on first start.
***********************************************************************************************************************************/
#ifndef COMMAND_BACKUP_TABLESPACE_H
#define COMMAND_BACKUP_TABLESPACE_H

#include "common/type/object.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "storage/storage.h"

typedef struct TableSpaceIter TableSpaceIter;

typedef struct TableSpaceIterPub
{
    unsigned int fileTotal;                                             // How many tablespace files were enumerated at construction
} TableSpaceIterPub;

FN_INLINE_ALWAYS unsigned int
tableSpaceIterFileTotal(const TableSpaceIter *const this)
{
    return THIS_PUB(TableSpaceIter)->fileTotal;
}

// Build the full list of tablespace files under dataPath. Excludes #innodb_dblwr/, #innodb_redo/, and lost+found.
FN_EXTERN TableSpaceIter *tableSpaceIterNew(const Storage *dataStorage, const String *dataPath);

// Pop the next tablespace path (relative to dataPath); returns NULL when exhausted. Single-consumer; caller serializes access.
FN_EXTERN String *tableSpaceIterNext(TableSpaceIter *this);

FN_INLINE_ALWAYS void
tableSpaceIterFree(TableSpaceIter *const this)
{
    objFree(this);
}

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void tableSpaceIterToLog(const TableSpaceIter *this, StringStatic *debugLog);

#define FUNCTION_LOG_TABLE_SPACE_ITER_TYPE                                                                                         \
    TableSpaceIter *
#define FUNCTION_LOG_TABLE_SPACE_ITER_FORMAT(value, buffer, bufferSize)                                                            \
    FUNCTION_LOG_OBJECT_FORMAT(value, tableSpaceIterToLog, buffer, bufferSize)

#endif