/***********************************************************************************************************************************
ISAM Engine Module (MySQL 3.21 → 4.0.2)

ISAM (Indexed Sequential Access Method) was MySQL's original storage engine, replaced by MyISAM in 3.23 and removed entirely in
4.0.3. Files: <table>.ISD (data), <table>.ISM (index), <table>.frm (schema).

Anyone running MySQL 3.21–4.0.2 in 2026 is doing so deliberately (museum installations, vendor-locked appliances); the supporting
code is bounded enough that we may as well include it.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/isam.h"

static const char *const isamCompanions[] = {".ISM", ".frm", NULL};

static void
engineIsamCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".ISD", isamCompanions, "ISAM");
}

static const EngineHandler isamHandler =
{
    .kind = mysqlEngineIsam,
    .name = "isam",
    .copyUnderLock = engineIsamCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineIsamHandler(void)
{
    return &isamHandler;
}
