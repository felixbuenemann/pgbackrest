/***********************************************************************************************************************************
Standalone test for src/mysql/engine/toku.c — engineTokuCopyOnline (offline mode) against a synthetic TokuDB datadir
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
#include "mysql/engine/toku.h"

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
        printf("TokuDB engine offline copy test:\n");

        const char *const src = "/tmp/mybackrest-toku-src";
        const char *const dst = "/tmp/mybackrest-toku-dst";
        rmrf(src); rmrf(dst);

        mkdirP(src);

        // Top-level metadata
        touch("/tmp/mybackrest-toku-src/tokudb.environment", "env");
        touch("/tmp/mybackrest-toku-src/tokudb.directory", "dir");
        touch("/tmp/mybackrest-toku-src/tokudb.rollback", "rb");

        // Lock state markers
        touch("/tmp/mybackrest-toku-src/__tokudb_lock_dont_delete_me_data", "lock data");
        touch("/tmp/mybackrest-toku-src/__tokudb_lock_dont_delete_me_environment", "lock env");

        // Per-table data (TokuDB stores .tokudb files at the datadir root by default)
        touch("/tmp/mybackrest-toku-src/_test_users_main_00000001.tokudb", "users data");
        touch("/tmp/mybackrest-toku-src/_test_users_status_00000001.tokudb", "users status");
        touch("/tmp/mybackrest-toku-src/_test_users_key_email_00000001.tokudb", "users idx");

        // Recovery logs
        touch("/tmp/mybackrest-toku-src/log000000000001.tokulog25", "log gen 1");
        touch("/tmp/mybackrest-toku-src/log000000000002.tokulog25", "log gen 2");

        // Decoys (must be ignored)
        touch("/tmp/mybackrest-toku-src/random.txt", "decoy");
        touch("/tmp/mybackrest-toku-src/somefile.ibd", "innodb decoy");

        mkdirP(dst);

        EngineBackupCtx ctx = { .client = NULL, .dataPath = STR(src), .backupPath = STR(dst), .processMax = 1 };
        const EngineHandler *const h = engineTokuHandler();
        h->copyOnline(&ctx);

        // Top-level metadata
        expect("tokudb.environment copied", fileExists("/tmp/mybackrest-toku-dst/tokudb.environment"));
        expect("tokudb.directory copied",   fileExists("/tmp/mybackrest-toku-dst/tokudb.directory"));
        expect("tokudb.rollback copied",    fileExists("/tmp/mybackrest-toku-dst/tokudb.rollback"));

        // Lock markers
        expect(
            "__tokudb_lock_dont_delete_me_data copied",
            fileExists("/tmp/mybackrest-toku-dst/__tokudb_lock_dont_delete_me_data"));
        expect(
            "__tokudb_lock_dont_delete_me_environment copied",
            fileExists("/tmp/mybackrest-toku-dst/__tokudb_lock_dont_delete_me_environment"));

        // Per-table .tokudb files
        expect(
            "_test_users_main_00000001.tokudb copied",
            fileExists("/tmp/mybackrest-toku-dst/_test_users_main_00000001.tokudb"));
        expect(
            "_test_users_status_00000001.tokudb copied",
            fileExists("/tmp/mybackrest-toku-dst/_test_users_status_00000001.tokudb"));
        expect(
            "_test_users_key_email_00000001.tokudb copied",
            fileExists("/tmp/mybackrest-toku-dst/_test_users_key_email_00000001.tokudb"));

        // Recovery logs
        expect("log000000000001.tokulog25 copied", fileExists("/tmp/mybackrest-toku-dst/log000000000001.tokulog25"));
        expect("log000000000002.tokulog25 copied", fileExists("/tmp/mybackrest-toku-dst/log000000000002.tokulog25"));

        // Decoys must NOT be copied
        expect("random.txt NOT copied", !fileExists("/tmp/mybackrest-toku-dst/random.txt"));
        expect("somefile.ibd NOT copied", !fileExists("/tmp/mybackrest-toku-dst/somefile.ibd"));

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
