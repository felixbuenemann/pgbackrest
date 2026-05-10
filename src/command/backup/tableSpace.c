/***********************************************************************************************************************************
InnoDB Tablespace Iterator (Phase D scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/log.h"

struct TableSpaceIter
{
    MemContext *memContext;
    StringList *files;
    unsigned int nextIdx;
};

/**********************************************************************************************************************************/
FN_EXTERN TableSpaceIter *
tableSpaceIterNew(const Storage *const dataStorage, const String *const dataPath)
{
    (void)dataStorage; (void)dataPath;
    THROW(AssertError, "TODO(myBackRest-D): tableSpaceIterNew — walk datadir, glob *.ibd / ibdata* / mysql.ibd / undo_*.ibu");
}

/**********************************************************************************************************************************/
FN_EXTERN String *
tableSpaceIterNext(TableSpaceIter *const this)
{
    (void)this;
    THROW(AssertError, "TODO(myBackRest-D): tableSpaceIterNext — atomic-fetch-and-increment over files list");
}
