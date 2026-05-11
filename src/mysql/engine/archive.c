/***********************************************************************************************************************************
ARCHIVE Engine Module
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/archive.h"

static const char *const archiveCompanions[] = {".frm", NULL};

static void
engineArchiveCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".ARZ", archiveCompanions, "Archive");
}

static const EngineHandler archiveHandler =
{
    .kind = mysqlEngineArchive,
    .name = "archive",
    .copyUnderLock = engineArchiveCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineArchiveHandler(void)
{
    return &archiveHandler;
}
