/***********************************************************************************************************************************
Standalone test for src/mysql/engine/rocksdb.c — engineRocksdbCopyOnline against a synthetic RocksDB dir
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
#include "mysql/engine/rocksdb.h"

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
        printf("RocksDB engine offline copy test:\n");

        const char *const src = "/tmp/mybackrest-rocksdb-src";
        const char *const dst = "/tmp/mybackrest-rocksdb-dst";
        rmrf(src); rmrf(dst);

        mkdirP(src);
        // Test BOTH conventions: .rocksdb (Percona) and #rocksdb (mariabackup)
        mkdirP("/tmp/mybackrest-rocksdb-src/.rocksdb");
        mkdirP("/tmp/mybackrest-rocksdb-src/#rocksdb");

        touch("/tmp/mybackrest-rocksdb-src/.rocksdb/000004.sst", "sst data");
        touch("/tmp/mybackrest-rocksdb-src/.rocksdb/MANIFEST-000003", "manifest");
        touch("/tmp/mybackrest-rocksdb-src/.rocksdb/CURRENT", "MANIFEST-000003");
        touch("/tmp/mybackrest-rocksdb-src/.rocksdb/OPTIONS-000005", "options");
        touch("/tmp/mybackrest-rocksdb-src/.rocksdb/LOG", "log");

        touch("/tmp/mybackrest-rocksdb-src/#rocksdb/000007.sst", "mb sst");
        touch("/tmp/mybackrest-rocksdb-src/#rocksdb/IDENTITY", "identity");

        mkdirP(dst);

        EngineBackupCtx ctx = { .client = NULL, .dataPath = STR(src), .backupPath = STR(dst), .processMax = 1 };
        const EngineHandler *const h = engineRocksdbHandler();
        h->copyOnline(&ctx);

        // Output is always under <dst>/#rocksdb/  regardless of source convention
        expect(".rocksdb/000004.sst → #rocksdb/000004.sst", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/000004.sst"));
        expect(".rocksdb/MANIFEST-000003 copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/MANIFEST-000003"));
        expect(".rocksdb/CURRENT copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/CURRENT"));
        expect(".rocksdb/OPTIONS-000005 copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/OPTIONS-000005"));
        expect(".rocksdb/LOG copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/LOG"));
        expect("#rocksdb/000007.sst copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/000007.sst"));
        expect("#rocksdb/IDENTITY copied", fileExists("/tmp/mybackrest-rocksdb-dst/#rocksdb/IDENTITY"));

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
