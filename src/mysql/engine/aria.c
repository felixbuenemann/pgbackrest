/***********************************************************************************************************************************
Aria Engine Module (Phase D scaffolding, MariaDB only)
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/aria.h"

static void
engineAriaCopyOnline(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Copy aria_log_control + .MAD/.MAI files of online tables; reference aria_backup_client.cc:BackupImpl::start() line 597
    THROW(AssertError, "TODO(myBackRest-D): engineAriaCopyOnline — copy stable .MAD/.MAI + aria_log_control snapshot");
}

static void
engineAriaCopyUnderLock(EngineBackupCtx *const ctx)
{
    (void)ctx;
    // Tail of aria_log.NNNNNNNN files; reference copy_log_tail() line 710
    THROW(AssertError, "TODO(myBackRest-D): engineAriaCopyUnderLock — copy aria_log.* tail");
}

static const EngineHandler ariaHandler =
{
    .kind = mysqlEngineAria,
    .name = "aria",
    .prepare = NULL,
    .copyOnline = engineAriaCopyOnline,
    .copyUnderLock = engineAriaCopyUnderLock,
    .finalize = NULL,
};

FN_EXTERN const EngineHandler *
engineAriaHandler(void)
{
    return &ariaHandler;
}
