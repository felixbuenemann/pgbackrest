/***********************************************************************************************************************************
Standalone test for src/mysql/engine/aria.c — engineAriaCopyOnline against a synthetic MariaDB datadir
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
#include "mysql/engine/aria.h"
#include "mysql/engine/engine.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

static void mkdirP(const char *const p) { mkdir(p, 0755); }

static void
touch(const char *const path, const char *const data)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fputs(data, fp);
    fclose(fp);
}

static bool fileExists(const char *const p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
static void rmrf(const char *const p) { char c[1024]; snprintf(c, sizeof(c), "rm -rf '%s'", p); int u __attribute__((unused)) = system(c); }

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));
    logInit(logLevelOff, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Aria engine offline copy test:\n");

        const char *const src = "/tmp/mybackrest-aria-src";
        const char *const dst = "/tmp/mybackrest-aria-dst";
        rmrf(src); rmrf(dst);

        mkdirP(src);
        mkdirP("/tmp/mybackrest-aria-src/mysql");

        // Top-level Aria artefacts
        touch("/tmp/mybackrest-aria-src/aria_log_control", "control state");
        touch("/tmp/mybackrest-aria-src/aria_log.00000001", "log 1");
        touch("/tmp/mybackrest-aria-src/aria_log.00000002", "log 2");

        // Per-table Aria files
        touch("/tmp/mybackrest-aria-src/mysql/user.MAD", "user data");
        touch("/tmp/mybackrest-aria-src/mysql/user.MAI", "user index");
        touch("/tmp/mybackrest-aria-src/mysql/db.MAD", "db data");
        touch("/tmp/mybackrest-aria-src/mysql/db.MAI", "db index");

        // A non-Aria file that should be ignored
        touch("/tmp/mybackrest-aria-src/mysql/random.ibd", "should not be copied");

        mkdirP(dst);

        EngineBackupCtx ctx = { .client = NULL, .dataPath = STR(src), .backupPath = STR(dst), .processMax = 1 };
        const EngineHandler *const h = engineAriaHandler();
        h->copyOnline(&ctx);

        expect("aria_log_control copied", fileExists("/tmp/mybackrest-aria-dst/aria_log_control"));
        expect("aria_log.00000001 copied", fileExists("/tmp/mybackrest-aria-dst/aria_log.00000001"));
        expect("aria_log.00000002 copied", fileExists("/tmp/mybackrest-aria-dst/aria_log.00000002"));
        expect("mysql/user.MAD copied", fileExists("/tmp/mybackrest-aria-dst/mysql/user.MAD"));
        expect("mysql/user.MAI copied", fileExists("/tmp/mybackrest-aria-dst/mysql/user.MAI"));
        expect("mysql/db.MAD copied", fileExists("/tmp/mybackrest-aria-dst/mysql/db.MAD"));
        expect("mysql/db.MAI copied", fileExists("/tmp/mybackrest-aria-dst/mysql/db.MAI"));
        expect("random.ibd NOT copied (not Aria)", !fileExists("/tmp/mybackrest-aria-dst/mysql/random.ibd"));

        rmrf(src); rmrf(dst);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n", errorMessage());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); rc = 1; }
    else if (rc == 0)     printf("\nAll assertions passed\n");

    return rc;
}
