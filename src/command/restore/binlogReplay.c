/***********************************************************************************************************************************
Binlog Replay for PITR (Phase E scaffolding)
***********************************************************************************************************************************/
#include <build.h>

#include "command/restore/binlogReplay.h"
#include "common/debug.h"
#include "common/log.h"

/**********************************************************************************************************************************/
FN_EXTERN void
binlogReplay(
    const Storage *const archiveStorage, const String *const archivePath, const String *const startFile, const uint64_t startPos,
    const BinlogReplayTarget *const target, const String *const mysqlbinlogPath, const String *const mysqlClientPath,
    const String *const recoveryCnf)
{
    (void)archiveStorage; (void)archivePath; (void)startFile; (void)startPos; (void)target;
    (void)mysqlbinlogPath; (void)mysqlClientPath; (void)recoveryCnf;
    // 1. Materialize file list from archive (storageList; pull to local tmp via standard pipeline so compression/encryption
    //    apply transparently)
    // 2. Compose mysqlbinlog cmd: --start-position=startPos + target-specific flag + space-joined file list
    // 3. Compose mysql cmd: --defaults-file=recoveryCnf
    // 4. fork() + pipe() + execvp() — child A runs mysqlbinlog, child B runs mysql with stdin = child-A.stdout
    // 5. waitpid both; throw on non-zero exit
    THROW(AssertError, "TODO(myBackRest-E): binlogReplay — pipe mysqlbinlog into mysql via common/exec.c");
}
