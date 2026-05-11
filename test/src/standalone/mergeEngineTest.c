/***********************************************************************************************************************************
Standalone test for src/mysql/engine/merge.c
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
#include "mysql/engine/engine.h"
#include "mysql/engine/merge.h"

static int testFailures = 0;

static void expect(const char *what, bool ok) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", what); if (!ok) testFailures++; }
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
        printf("Merge engine offline copy test:\n");
        rmrf("/tmp/mybackrest-merge-src"); rmrf("/tmp/mybackrest-merge-dst");
        mkdirP("/tmp/mybackrest-merge-src"); mkdirP("/tmp/mybackrest-merge-dst");
        mkdirP("/tmp/mybackrest-merge-src/big");

        // MERGE table file lists child MyISAM table names
        writeFile("/tmp/mybackrest-merge-src/big/yearly.MRG", "y2024\ny2025\n");
        writeFile("/tmp/mybackrest-merge-src/big/yearly.frm", "merge-schema");
        // Child tables (would be copied by myisam handler, not merge)
        writeFile("/tmp/mybackrest-merge-src/big/y2024.MYD", "y24");
        writeFile("/tmp/mybackrest-merge-src/big/y2024.MYI", "i24");

        EngineBackupCtx ctx = {
            .client = NULL, .dataPath = STRDEF("/tmp/mybackrest-merge-src"),
            .backupPath = STRDEF("/tmp/mybackrest-merge-dst"), .processMax = 1
        };

        const EngineHandler *const h = engineMergeHandler();
        ASSERT(h != NULL && h->copyUnderLock != NULL);
        h->copyUnderLock(&ctx);

        expect(".MRG copied", fileExists("/tmp/mybackrest-merge-dst/big/yearly.MRG"));
        expect(".frm copied", fileExists("/tmp/mybackrest-merge-dst/big/yearly.frm"));
        expect("child .MYD NOT copied (handled by myisam)", !fileExists("/tmp/mybackrest-merge-dst/big/y2024.MYD"));

        rmrf("/tmp/mybackrest-merge-src"); rmrf("/tmp/mybackrest-merge-dst");
    }
    CATCH_FATAL() { printf("FATAL: %s\n", errorMessage()); rc = 1; }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); return 1; }
    printf("\nAll assertions passed\n");
    return rc;
}
