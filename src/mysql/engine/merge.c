/***********************************************************************************************************************************
MERGE Engine Module
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/merge.h"

static const char *const mergeCompanions[] = {".frm", NULL};

static void
engineMergeCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".MRG", mergeCompanions, "MERGE");
}

static const EngineHandler mergeHandler =
{
    .kind = mysqlEngineMerge,
    .name = "mrg_myisam",                                               // INFORMATION_SCHEMA.ENGINES.ENGINE reports this as MRG_MyISAM
    .copyUnderLock = engineMergeCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineMergeHandler(void)
{
    return &mergeHandler;
}
