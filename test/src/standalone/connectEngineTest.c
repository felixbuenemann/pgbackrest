/***********************************************************************************************************************************
Standalone test for src/mysql/engine/connect.c

The Connect handler copies the .dnx index + .frm. The actual data files (.csv/.xml/.json/etc, extensions vary by TABLE_TYPE)
sit alongside but are claimed by the miscellaneous-files pass since their extensions are too varied to enumerate here.
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
#include "mysql/engine/connect.h"
#include "mysql/engine/engine.h"

static int testFailures = 0;
static void expect(const char *w, bool ok) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) testFailures++; }
static void mkdirP(const char *p) { mkdir(p, 0755); }
static void writeFile(const char *p, const char *d) { FILE *f = fopen(p, "w"); if (!f) THROW_FMT(FileWriteError, "%s", p); fputs(d, f); fclose(f); }
static bool fileExists(const char *p) { struct stat s; return stat(p, &s) == 0 && S_ISREG(s.st_mode); }
static void rmrf(const char *p) { char c[1024]; snprintf(c, sizeof c, "rm -rf '%s'", p); int u __attribute__((unused)) = system(c); }

int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));
    logInit(logLevelOff, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Connect engine offline copy test:\n");
        rmrf("/tmp/mybackrest-connect-src"); rmrf("/tmp/mybackrest-connect-dst");
        mkdirP("/tmp/mybackrest-connect-src"); mkdirP("/tmp/mybackrest-connect-dst");
        mkdirP("/tmp/mybackrest-connect-src/external");

        // Indexed Connect table — .dnx + .frm copied by the engine handler.
        writeFile("/tmp/mybackrest-connect-src/external/feed.dnx", "connect-index");
        writeFile("/tmp/mybackrest-connect-src/external/feed.frm", "connect-schema");
        // .csv is the actual data file — claimed by miscFiles (not tested here)
        writeFile("/tmp/mybackrest-connect-src/external/feed.csv", "data,row\n");
        // Non-Connect siblings
        writeFile("/tmp/mybackrest-connect-src/external/other.MYD", "myisam");

        EngineBackupCtx ctx = {
            .client = NULL, .dataPath = STRDEF("/tmp/mybackrest-connect-src"),
            .backupPath = STRDEF("/tmp/mybackrest-connect-dst"), .processMax = 1
        };

        const EngineHandler *const h = engineConnectHandler();
        ASSERT(h != NULL && h->copyUnderLock != NULL);
        h->copyUnderLock(&ctx);

        expect(".dnx copied", fileExists("/tmp/mybackrest-connect-dst/external/feed.dnx"));
        expect(".frm copied", fileExists("/tmp/mybackrest-connect-dst/external/feed.frm"));
        expect(".csv NOT copied by Connect handler (miscFiles handles it)",
            !fileExists("/tmp/mybackrest-connect-dst/external/feed.csv"));
        expect(".MYD NOT copied", !fileExists("/tmp/mybackrest-connect-dst/external/other.MYD"));

        rmrf("/tmp/mybackrest-connect-src"); rmrf("/tmp/mybackrest-connect-dst");
    }
    CATCH_FATAL() { printf("FATAL: %s\n", errorMessage()); rc = 1; }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); return 1; }
    printf("\nAll assertions passed\n");
    return rc;
}
