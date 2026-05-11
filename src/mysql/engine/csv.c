/***********************************************************************************************************************************
CSV Engine Module
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/csv.h"

// Companion files: .CSM (metadata) is always present alongside the .CSV; .frm exists only on MySQL 5.7 (8.0+ has SDI in
// mysql.ibd's DD, which the miscellaneous-files pass copies as a *.sdi sidecar where present).
static const char *const csvCompanions[] = {".CSM", ".frm", NULL};

static void
engineCsvCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".CSV", csvCompanions, "CSV");
}

static const EngineHandler csvHandler =
{
    .kind = mysqlEngineCsv,
    .name = "csv",
    .copyUnderLock = engineCsvCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineCsvHandler(void)
{
    return &csvHandler;
}
