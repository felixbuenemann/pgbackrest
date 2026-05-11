/***********************************************************************************************************************************
End-to-End restore test — drives mysqlColdRestore against a real backup directory.

The companion to e2eTest.c (which does the backup). The harness script runs this AFTER stopping the original mysqld:

  1. e2eTest.c with a running mysqld:    snapshot data, run hot backup
  2. (script)                            stop the original mysqld
  3. e2eRestoreTest.c:                   restore the backup into a fresh datadir
  4. (script)                            start a NEW mysqld on the restored datadir, query, compare

Environment variables (set by run-mysql.sh):
  MYBACKREST_E2E_BACKUP    — directory the backup was written into
  MYBACKREST_E2E_RESTORE   — empty destination for the restored datadir
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/coldRestore.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static const char *
mustGetEnv(const char *const name)
{
    const char *const value = getenv(name);
    if (value == NULL || value[0] == '\0')
    {
        fprintf(stderr, "missing required env var: %s\n", name);
        exit(2);
    }
    return value;
}

static bool
fileExists(const char *const path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));

    logInit(logLevelWarn, logLevelError, logLevelOff, false, 0, 1, false);

    const char *const backupDirZ = mustGetEnv("MYBACKREST_E2E_BACKUP");
    const char *const restoreDirZ = mustGetEnv("MYBACKREST_E2E_RESTORE");

    int rc = 0;

    TRY_BEGIN()
    {
        printf("e2e restore test:\n");
        printf("  INFO  backup=%s\n", backupDirZ);
        printf("  INFO  restore=%s\n", restoreDirZ);

        const Storage *const bkpStorage = storagePosixNewP(STR(backupDirZ));
        const Storage *const rstStorage = storagePosixNewP(STR(restoreDirZ), .write = true);

        // mysqldPath = NULL — we don't want prepareWriteRecoveryFiles to emit a recovery.cnf. mysqld's automatic crash recovery
        // on first start will handle redo replay; the e2e harness invokes mysqld directly afterwards.
        MysqlColdRestoreResult *const result = mysqlColdRestore(
            bkpStorage, STRDEF("."), rstStorage, STRDEF("."), /*mysqldPath*/ NULL);

        expect("restore result non-null", result != NULL);
        expect("restore manifest parsed", result != NULL && result->manifest != NULL);
        expect("restore preserves vendor", result != NULL && result->manifest != NULL && result->manifest->info != NULL &&
            result->manifest->info->vendor != mysqlVendorUnknown);
        expect("restore copied files", result != NULL && result->filesCopied > 0);
        expect("recovery files NOT generated (mysqldPath=NULL)", result != NULL && !result->recoveryFilesWritten);

        // Sanity checks on the restored datadir's structure
        char path[512];

        snprintf(path, sizeof(path), "%s/ibdata1", restoreDirZ);
        expect("restored ibdata1 exists", fileExists(path));

        snprintf(path, sizeof(path), "%s/testdb/users.ibd", restoreDirZ);
        expect("restored testdb/users.ibd exists", fileExists(path));

        snprintf(path, sizeof(path), "%s/testdb/legacy.MYD", restoreDirZ);
        expect("restored testdb/legacy.MYD exists", fileExists(path));

        snprintf(path, sizeof(path), "%s/testdb/legacy.MYI", restoreDirZ);
        expect("restored testdb/legacy.MYI exists", fileExists(path));

        // The manifest is a backup-side artifact and SHOULD NOT appear in the restored datadir
        snprintf(path, sizeof(path), "%s/mybackrest_backup_info", restoreDirZ);
        expect("manifest NOT in restored datadir", !fileExists(path));
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0)
    {
        printf("\n%d assertion(s) failed\n", testFailures);
        return 1;
    }

    printf("\nAll assertions passed\n");
    return rc;
}
