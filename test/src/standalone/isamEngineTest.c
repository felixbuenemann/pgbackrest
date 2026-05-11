/***********************************************************************************************************************************
Standalone test for src/mysql/engine/isam.c — engineIsamCopyUnderLock against a synthetic 3.x datadir
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
#include "mysql/engine/isam.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

static void mkdirP(const char *const p) { mkdir(p, 0755); }
static void touch(const char *const p, const char *const d) {
    FILE *fp = fopen(p, "w"); if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", p); fputs(d, fp); fclose(fp);
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
        printf("ISAM engine offline copy test (MySQL 3.21..4.0.2):\n");

        const char *const src = "/tmp/mybackrest-isam-src";
        const char *const dst = "/tmp/mybackrest-isam-dst";
        rmrf(src); rmrf(dst);

        mkdirP(src);
        mkdirP("/tmp/mybackrest-isam-src/test");
        mkdirP("/tmp/mybackrest-isam-src/world");

        // Per-table ISAM trio
        touch("/tmp/mybackrest-isam-src/test/users.ISD", "users data");
        touch("/tmp/mybackrest-isam-src/test/users.ISM", "users index");
        touch("/tmp/mybackrest-isam-src/test/users.frm", "users schema");

        touch("/tmp/mybackrest-isam-src/world/cities.ISD", "cities data");
        touch("/tmp/mybackrest-isam-src/world/cities.ISM", "cities index");

        // Decoy: an .MYD (MyISAM) file should NOT be copied by the ISAM handler
        touch("/tmp/mybackrest-isam-src/world/myisam_decoy.MYD", "decoy");

        mkdirP(dst);

        EngineBackupCtx ctx = { .client = NULL, .dataPath = STR(src), .backupPath = STR(dst), .processMax = 1 };
        const EngineHandler *const h = engineIsamHandler();
        ASSERT(h != NULL);
        ASSERT(h->copyUnderLock != NULL);

        h->copyUnderLock(&ctx);

        expect("test/users.ISD copied", fileExists("/tmp/mybackrest-isam-dst/test/users.ISD"));
        expect("test/users.ISM copied", fileExists("/tmp/mybackrest-isam-dst/test/users.ISM"));
        expect("test/users.frm copied", fileExists("/tmp/mybackrest-isam-dst/test/users.frm"));
        expect("world/cities.ISD copied", fileExists("/tmp/mybackrest-isam-dst/world/cities.ISD"));
        expect("world/cities.ISM copied", fileExists("/tmp/mybackrest-isam-dst/world/cities.ISM"));
        expect("MyISAM .MYD decoy NOT copied by ISAM handler", !fileExists("/tmp/mybackrest-isam-dst/world/myisam_decoy.MYD"));

        // Engine handler dispatch: lookup by name "ISAM" returns the right handler
        expect("engineHandlerLookup('ISAM') returns isam handler",
            engineHandlerLookup(STRDEF("ISAM")) == engineIsamHandler());
        expect("engineHandlerLookup('isam') returns isam handler",
            engineHandlerLookup(STRDEF("isam")) == engineIsamHandler());

        rmrf(src); rmrf(dst);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); rc = 1; }
    else if (rc == 0)     printf("\nAll assertions passed\n");

    return rc;
}
