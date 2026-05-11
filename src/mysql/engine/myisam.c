/***********************************************************************************************************************************
MyISAM Engine Module

Backs up MyISAM tables: walks the datadir for *.MYD files and copies each MYD/MYI pair into the backup. The optional .frm schema
descriptor (5.7-only) is also copied when present.

Online/offline: identical algorithm — we trust the orchestrator's lock for online mode.

Reference: percona-xtrabackup backup_copy.cc:copy_non_innodb_files() and mariabackup equivalent at line ~5158.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/myisam.h"

static const char *const myisamCompanions[] = {".MYI", ".frm", NULL};

static void
engineMyisamCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".MYD", myisamCompanions, "MyISAM");
}

static const EngineHandler myisamHandler =
{
    .kind = mysqlEngineMyisam,
    .name = "myisam",
    .copyUnderLock = engineMyisamCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineMyisamHandler(void)
{
    return &myisamHandler;
}
