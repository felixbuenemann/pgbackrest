/***********************************************************************************************************************************
CONNECT Engine Module
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/connect.h"

// Companion list for the .dnx index file. The .frm carries the table type. Local data files (.csv/.xml/.json/.dat/etc.) are
// captured by the miscellaneous-files pass since their extensions vary too widely to enumerate exhaustively.
static const char *const connectCompanions[] = {".frm", NULL};

static void
engineConnectCopyUnderLock(EngineBackupCtx *const ctx)
{
    engineFlatCopyByExtension(ctx, ".dnx", connectCompanions, "Connect");
}

static const EngineHandler connectHandler =
{
    .kind = mysqlEngineConnect,
    .name = "connect",
    .copyUnderLock = engineConnectCopyUnderLock,
};

FN_EXTERN const EngineHandler *
engineConnectHandler(void)
{
    return &connectHandler;
}
