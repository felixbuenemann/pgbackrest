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

// Build the full list of tablespace files under dataPath. Excludes #innodb_dblwr/, performance_schema/, sys/, and lost+found.
FN_EXTERN TableSpaceIter *tableSpaceIterNew(const Storage *dataStorage, const String *dataPath);

// Pop the next tablespace path (relative to dataPath); returns NULL when exhausted. Thread-safe.
FN_EXTERN String *tableSpaceIterNext(TableSpaceIter *this);

FN_INLINE_ALWAYS void
tableSpaceIterFree(TableSpaceIter *const this)
{
    objFree(this);
}

#endif
