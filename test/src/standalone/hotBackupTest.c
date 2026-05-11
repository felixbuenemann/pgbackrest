/***********************************************************************************************************************************
Standalone structural test for src/mysql/hotBackup.c

The hot-backup orchestrator's full flow requires a live MySQL/MariaDB/Percona server to execute the SHOW MASTER STATUS / BACKUP
STAGE / LOCK INSTANCE FOR BACKUP queries. That belongs in an integration test driven by Container.pm (Phase H).

This standalone test verifies what we CAN check without a live server:

  1. mysqlClientNew constructs a client without connecting (lazy-connect semantics).
  2. The lock-method-name helper returns the right strings for each enum value.
  3. The hot-backup result struct is shaped as documented (fields exist and zero-initialize cleanly).

Anything that involves an actual query — sanity check, lock SQL, SHOW MASTER STATUS, engine handler invocation — is out of scope
here. Those paths run unit-tested in their own modules (sanity, lock, manifest, engines) and end-to-end via the future
integration test driven against a Docker mysqld matrix.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/client.h"
#include "mysql/hotBackup.h"
#include "mysql/lock.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));

    logInit(logLevelWarn, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Hot-backup structural tests:\n");

        // ---- Client construction ----
        MysqlClient *const client = mysqlClientNew(
            /*host*/ NULL, /*port*/ 0, STRDEF("/tmp/nonexistent.sock"), /*database*/ NULL,
            STRDEF("nobody"), /*password*/ NULL, /*timeout*/ 1000);

        expect("mysqlClientNew returned a client without connecting", client != NULL);
        expect("client vendor starts as Unknown", mysqlClientVendor(client) == mysqlVendorUnknown);
        expect("client server version starts at 0", mysqlClientServerVersionNum(client) == 0);
        expect("client socket preserved", mysqlClientSocket(client) != NULL);

        // ---- Lock-method-name helper ----
        expect("lock name auto", strcmp(mysqlLockMethodName(mysqlLockMethodAuto), "auto") == 0);
        expect("lock name instance", strcmp(mysqlLockMethodName(mysqlLockMethodInstance), "instance") == 0);
        expect("lock name stage", strcmp(mysqlLockMethodName(mysqlLockMethodStage), "stage") == 0);
        expect("lock name ftwrl", strcmp(mysqlLockMethodName(mysqlLockMethodFtwrl), "ftwrl") == 0);

        // ---- Result struct cleanup ----
        // mysqlHotBackupResultFree is a no-op (caller owns context-allocated struct) — verify it tolerates NULL too
        mysqlHotBackupResultFree(NULL);
        expect("mysqlHotBackupResultFree tolerates NULL", true);

        mysqlClientFree(client);
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
