/***********************************************************************************************************************************
Standalone test for src/mysql/engine/csv.c — engineCsvCopyUnderLock against a synthetic datadir

Builds a fake datadir with a CSV-engine table (.CSV + .CSM + .frm), runs the handler, verifies every expected file is copied
and that non-CSV files (e.g. .ibd) are not touched.
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
#include "mysql/engine/csv.h"
#include "mysql/engine/engine.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static void
mkdirP(const char *const path) { mkdir(path, 0755); }

static void
writeFile(const char *const path, const char *const data)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s) failed", path);
    fputs(data, fp);
    fclose(fp);
}

static bool
fileExists(const char *const path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void
rmrf(const char *const path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int u __attribute__((unused)) = system(cmd);
}

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
        printf("CSV engine offline copy test:\n");

        const char *const srcRoot = "/tmp/mybackrest-csv-src";
        const char *const dstRoot = "/tmp/mybackrest-csv-dst";

        rmrf(srcRoot); rmrf(dstRoot);
        mkdirP(srcRoot); mkdirP(dstRoot);
        mkdirP("/tmp/mybackrest-csv-src/mysql");

        writeFile("/tmp/mybackrest-csv-src/mysql/general_log.CSV", "row1\nrow2\n");
        writeFile("/tmp/mybackrest-csv-src/mysql/general_log.CSM", "metadata");
        writeFile("/tmp/mybackrest-csv-src/mysql/general_log.frm", "schema");
        writeFile("/tmp/mybackrest-csv-src/mysql/users.ibd", "innodb-not-mine");      // Must NOT be copied

        EngineBackupCtx ctx = {
            .client = NULL,
            .dataPath = STR(srcRoot),
            .backupPath = STR(dstRoot),
            .processMax = 1,
        };

        const EngineHandler *const handler = engineCsvHandler();
        ASSERT(handler != NULL);
        ASSERT(handler->copyUnderLock != NULL);

        handler->copyUnderLock(&ctx);

        expect(".CSV copied", fileExists("/tmp/mybackrest-csv-dst/mysql/general_log.CSV"));
        expect(".CSM copied", fileExists("/tmp/mybackrest-csv-dst/mysql/general_log.CSM"));
        expect(".frm copied", fileExists("/tmp/mybackrest-csv-dst/mysql/general_log.frm"));
        expect(".ibd NOT copied (not CSV)", !fileExists("/tmp/mybackrest-csv-dst/mysql/users.ibd"));

        rmrf(srcRoot); rmrf(dstRoot);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d assertion(s) failed\n", testFailures); return 1; }
    printf("\nAll assertions passed\n");
    return rc;
}
