/***********************************************************************************************************************************
MyISAM Engine Module (Phase D scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/myisam.h"

static void
engineMyisamCopyUnderLock(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // SELECT TABLE_SCHEMA, TABLE_NAME FROM INFORMATION_SCHEMA.TABLES WHERE ENGINE='MyISAM'
    // For each: copy .MYD + .MYI from <datadir>/<schema>/<table>.{MYD,MYI}. Lock is already held by Phase D orchestrator.
    THROW(AssertError, "TODO(myBackRest-D): engineMyisamCopyUnderLock — enumerate + flat-copy .MYD/.MYI files");
}

static const EngineHandler myisamHandler =
{
    .kind = mysqlEngineMyisam,
    .name = "myisam",
    .prepare = NULL,
    .copyOnline = NULL,
    .copyUnderLock = engineMyisamCopyUnderLock,
    .finalize = NULL,
};

FN_EXTERN const EngineHandler *
engineMyisamHandler(void)
{
    return &myisamHandler;
}
