/***********************************************************************************************************************************
Restore Prepare Phase (Phase E scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "command/restore/prepare.h"
#include "common/debug.h"
#include "common/log.h"

/**********************************************************************************************************************************/
FN_EXTERN void
prepareWriteRecoveryFiles(const Storage *const restoreStorage, const String *const restorePath, const String *const mysqldPath)
{
    (void)restoreStorage; (void)restorePath; (void)mysqldPath;
    // Compose the cnf:
    //   [mysqld]
    //   datadir = <restorePath>
    //   skip-networking = ON
    //   innodb-read-only = OFF
    //   init-file = <restorePath>/mybackrest_recovery.sql
    //   pid-file = <restorePath>/mybackrest_recovery.pid
    //   log-error = <restorePath>/mybackrest_recovery.log
    // Compose the sql: SHUTDOWN;
    THROW(AssertError, "TODO(myBackRest-E): prepareWriteRecoveryFiles — emit mybackrest_recovery.cnf + .sql");
}

/**********************************************************************************************************************************/
FN_EXTERN void
prepareInvokeMysqld(const String *const mysqldPath, const String *const restorePath)
{
    (void)mysqldPath; (void)restorePath;
    // Use common/exec.c to fork+exec; capture stdout/stderr; check exit code; clean up the pid file.
    THROW(AssertError, "TODO(myBackRest-E): prepareInvokeMysqld — fork+exec mysqld + wait for SHUTDOWN exit");
}
