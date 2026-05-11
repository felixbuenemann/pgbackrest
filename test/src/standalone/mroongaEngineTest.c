/***********************************************************************************************************************************
Standalone test for src/mysql/engine/mroonga.c

The Mroonga handler has the most complex file-matching logic of any engine: it copies <table>.mrn AND all variable-suffix
sibling files (<table>.mrn.NNNNNNNN segment files, <table>.mrn.c/l/s/i sidecars). Exercise each pattern explicitly.
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
#include "mysql/engine/mroonga.h"

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
        printf("Mroonga engine offline copy test:\n");
        rmrf("/tmp/mybackrest-mroonga-src"); rmrf("/tmp/mybackrest-mroonga-dst");
        mkdirP("/tmp/mybackrest-mroonga-src"); mkdirP("/tmp/mybackrest-mroonga-dst");
        mkdirP("/tmp/mybackrest-mroonga-src/search");

        // Mroonga files for table "docs"
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn",            "main");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.000000001",  "seg1");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.000000002",  "seg2");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.c",          "columns");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.l",          "lexicon");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.s",          "segments");
        writeFile("/tmp/mybackrest-mroonga-src/search/docs.mrn.i",          "index");
        // Non-Mroonga sibling that must NOT be copied by this handler
        writeFile("/tmp/mybackrest-mroonga-src/search/leftover.MYD",        "myisam");

        EngineBackupCtx ctx = {
            .client = NULL, .dataPath = STRDEF("/tmp/mybackrest-mroonga-src"),
            .backupPath = STRDEF("/tmp/mybackrest-mroonga-dst"), .processMax = 1
        };

        const EngineHandler *const h = engineMroongaHandler();
        ASSERT(h != NULL && h->copyUnderLock != NULL);
        h->copyUnderLock(&ctx);

        expect("docs.mrn copied",            fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn"));
        expect("docs.mrn.000000001 copied",  fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.000000001"));
        expect("docs.mrn.000000002 copied",  fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.000000002"));
        expect("docs.mrn.c copied",          fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.c"));
        expect("docs.mrn.l copied",          fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.l"));
        expect("docs.mrn.s copied",          fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.s"));
        expect("docs.mrn.i copied",          fileExists("/tmp/mybackrest-mroonga-dst/search/docs.mrn.i"));
        expect("leftover.MYD NOT copied",   !fileExists("/tmp/mybackrest-mroonga-dst/search/leftover.MYD"));

        rmrf("/tmp/mybackrest-mroonga-src"); rmrf("/tmp/mybackrest-mroonga-dst");
    }
    CATCH_FATAL() { printf("FATAL: %s\n", errorMessage()); rc = 1; }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); return 1; }
    printf("\nAll assertions passed\n");
    return rc;
}
