/***********************************************************************************************************************************
Standalone test for src/mysql/engine/myisam.c — engineMyisamCopyUnderLock against a synthetic datadir

Builds a fake datadir under /tmp with two schema directories each containing a MyISAM table (.MYD + .MYI), runs the handler,
verifies every expected file lands in the destination directory and that nothing surprising got copied.

The handler runs in offline mode (ctx->client == NULL) which makes this a useful end-to-end smoke test for cold backups.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/engine/engine.h"
#include "mysql/engine/myisam.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static void
mkdirP(const char *const path)
{
    mkdir(path, 0755);                                                  // ignore errors
}

static void
writeFile(const char *const path, const char *const contents)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s) failed", path);
    fputs(contents, fp);
    fclose(fp);
}

static bool
fileExists(const char *const path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool
fileEquals(const char *const path, const char *const expected)
{
    FILE *const fp = fopen(path, "r");
    if (fp == NULL) return false;

    char buf[256];
    const size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);

    buf[got] = '\0';
    return strcmp(buf, expected) == 0;
}

static void
rmrf(const char *const path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int unused __attribute__((unused)) = system(cmd);
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
        printf("MyISAM engine offline copy test:\n");

        const char *const srcRoot = "/tmp/mybackrest-myisam-src";
        const char *const dstRoot = "/tmp/mybackrest-myisam-dst";

        rmrf(srcRoot);
        rmrf(dstRoot);

        // Source layout:
        //   /tmp/mybackrest-myisam-src/
        //     sakila/
        //       actor.MYD       "actor data"
        //       actor.MYI       "actor index"
        //       actor.frm       "actor schema"
        //     world/
        //       city.MYD        "city data"
        //       city.MYI        "city index"
        //       city.ibd        "innodb file -- should NOT be copied by myisam handler"
        //     #innodb_redo/      (skipped by tableSpaceShouldSkipDir-ish heuristic)
        mkdirP(srcRoot);
        mkdirP("/tmp/mybackrest-myisam-src/sakila");
        mkdirP("/tmp/mybackrest-myisam-src/world");
        mkdirP("/tmp/mybackrest-myisam-src/#innodb_redo");

        writeFile("/tmp/mybackrest-myisam-src/sakila/actor.MYD", "actor data");
        writeFile("/tmp/mybackrest-myisam-src/sakila/actor.MYI", "actor index");
        writeFile("/tmp/mybackrest-myisam-src/sakila/actor.frm", "actor schema");
        writeFile("/tmp/mybackrest-myisam-src/world/city.MYD", "city data");
        writeFile("/tmp/mybackrest-myisam-src/world/city.MYI", "city index");
        writeFile("/tmp/mybackrest-myisam-src/world/city.ibd", "innodb file");

        mkdirP(dstRoot);

        EngineBackupCtx ctx = {
            .client = NULL,                                             // offline mode
            .dataPath = STR(srcRoot),
            .backupPath = STR(dstRoot),
            .processMax = 1,
        };

        const EngineHandler *const handler = engineMyisamHandler();
        ASSERT(handler != NULL);
        ASSERT(handler->copyUnderLock != NULL);

        handler->copyUnderLock(&ctx);

        // Verify expected files copied
        expect(
            "actor.MYD copied",
            fileExists("/tmp/mybackrest-myisam-dst/sakila/actor.MYD") &&
            fileEquals("/tmp/mybackrest-myisam-dst/sakila/actor.MYD", "actor data"));
        expect(
            "actor.MYI copied",
            fileExists("/tmp/mybackrest-myisam-dst/sakila/actor.MYI") &&
            fileEquals("/tmp/mybackrest-myisam-dst/sakila/actor.MYI", "actor index"));
        expect(
            "actor.frm copied (5.x compat)",
            fileExists("/tmp/mybackrest-myisam-dst/sakila/actor.frm") &&
            fileEquals("/tmp/mybackrest-myisam-dst/sakila/actor.frm", "actor schema"));
        expect(
            "city.MYD copied",
            fileExists("/tmp/mybackrest-myisam-dst/world/city.MYD") &&
            fileEquals("/tmp/mybackrest-myisam-dst/world/city.MYD", "city data"));
        expect(
            "city.MYI copied",
            fileExists("/tmp/mybackrest-myisam-dst/world/city.MYI") &&
            fileEquals("/tmp/mybackrest-myisam-dst/world/city.MYI", "city index"));

        // Verify InnoDB .ibd was NOT copied by the MyISAM handler
        expect("city.ibd NOT copied (it's InnoDB, not MyISAM)", !fileExists("/tmp/mybackrest-myisam-dst/world/city.ibd"));

        // Cleanup
        rmrf(srcRoot);
        rmrf(dstRoot);
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
        rc = 1;
    }
    else if (rc == 0)
    {
        printf("\nAll assertions passed\n");
    }

    return rc;
}
