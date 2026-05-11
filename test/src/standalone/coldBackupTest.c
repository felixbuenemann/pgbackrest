/***********************************************************************************************************************************
Standalone test for src/mysql/coldBackup.c

Builds a synthetic datadir (auto.cnf + ibdata1 + #innodb_redo + a schema dir with .MYD/.MYI) and verifies the cold-backup
orchestrator inspects it, dispatches engine handlers, copies the redo log + auto.cnf, and writes the manifest.

Cold-mode focus: every file in the destination should be a flat copy of the source. No locking, no client, no live mysqld.
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
#include "common/type/buffer.h"
#include "mysql/coldBackup.h"
#include "mysql/datadir.h"
#include "mysql/interface.h"
#include "mysql/manifest.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static void
writeFile(const char *const path, const void *const data, const size_t size)
{
    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen failed: %s", path);
    if (fwrite(data, 1, size, fp) != size) THROW(FileWriteError, "fwrite short");
    fclose(fp);
}

static void
writeZeros(const char *const path, const size_t size)
{
    void *const buf = calloc(1, size);
    writeFile(path, buf, size);
    free(buf);
}

static bool
fileExists(const char *const path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void
rmRf(const char *const path)
{
    // Best-effort cleanup; we use system() to dodge writing a manual walker
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0)
    {
        // Ignore — temp dir may already be gone
    }
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
        printf("Cold-backup orchestrator tests:\n");

        const char *const tmpRoot = "/tmp/mybackrest-coldbackup-test";
        const char *const srcDir = "/tmp/mybackrest-coldbackup-test/src";
        const char *const dstDir = "/tmp/mybackrest-coldbackup-test/dst";

        rmRf(tmpRoot);
        mkdir(tmpRoot, 0755);
        mkdir(srcDir, 0755);
        mkdir(dstDir, 0755);

        // ---- Synthetic datadir ----
        // auto.cnf
        writeFile(
            "/tmp/mybackrest-coldbackup-test/src/auto.cnf",
            "[auto]\nserver-uuid=8c0fd6f0-bf8f-11ee-9821-0242ac120002\n", 56);

        // ibdata1 — minimal 16 KiB page 0 with FSP_SPACE_FLAGS encoding 16K pages. Zero everything; FSP_SPACE_FLAGS=0 decodes
        // to 16K pages (legacy default) which is what we want.
        unsigned char ibdata[16384] = {0};
        // FIL_PAGE_SPACE_ID @ 34 = 0 (system tablespace); FSP_SPACE_ID @ 38 = 0 — both stay zero.
        writeFile("/tmp/mybackrest-coldbackup-test/src/ibdata1", ibdata, sizeof(ibdata));

        // Redo log: 8.0.30+ dynamic layout. Create #innodb_redo/ with one #ib_redoN file.
        mkdir("/tmp/mybackrest-coldbackup-test/src/#innodb_redo", 0755);
        writeZeros("/tmp/mybackrest-coldbackup-test/src/#innodb_redo/#ib_redo7_1234", 4096);

        // MyISAM table in a schema dir
        mkdir("/tmp/mybackrest-coldbackup-test/src/myschema", 0755);
        writeZeros("/tmp/mybackrest-coldbackup-test/src/myschema/users.MYD", 512);
        writeZeros("/tmp/mybackrest-coldbackup-test/src/myschema/users.MYI", 512);
        writeZeros("/tmp/mybackrest-coldbackup-test/src/myschema/users.frm", 256);

        const Storage *const srcStorage = storagePosixNewP(STR(srcDir));
        const Storage *const dstStorage = storagePosixNewP(STR(dstDir), .write = true);

        MysqlColdBackupResult *const result = mysqlColdBackup(srcStorage, STRDEF("."), dstStorage, STRDEF("."));

        // ---- Result struct ----
        expect("result is non-null", result != NULL);
        expect("info captured", result->info != NULL);
        expect("info detected hasInnodb", result->info->hasInnodb);
        expect("info detected hasMyisam", result->info->hasMyisam);
        expect("info captured serverUuid", result->info->serverUuid != NULL);
        expect("manifest written", result->manifestWritten);
        expect("auto.cnf copied", result->autoCnfCopied);
        expect("at least one redo file copied", result->redoFilesCopied >= 1);
        expect("at least two engines processed", result->enginesProcessed >= 2);

        // ---- Destination layout ----
        expect("dst auto.cnf exists", fileExists("/tmp/mybackrest-coldbackup-test/dst/auto.cnf"));
        expect("dst ibdata1 exists", fileExists("/tmp/mybackrest-coldbackup-test/dst/ibdata1"));
        expect(
            "dst #innodb_redo/#ib_redo7_1234 exists",
            fileExists("/tmp/mybackrest-coldbackup-test/dst/#innodb_redo/#ib_redo7_1234"));
        expect("dst myschema/users.MYD exists", fileExists("/tmp/mybackrest-coldbackup-test/dst/myschema/users.MYD"));
        expect("dst myschema/users.MYI exists", fileExists("/tmp/mybackrest-coldbackup-test/dst/myschema/users.MYI"));

        // The manifest writer emits mybackrest_backup_info (MYSQL_FILE_BACKUP_INFO) at the backup root
        expect("dst manifest exists", fileExists("/tmp/mybackrest-coldbackup-test/dst/mybackrest_backup_info"));

        // ---- Round-trip: read the manifest back and check core fields ----
        MysqlBackupManifestParsed *const parsed = mysqlBackupManifestRead(dstStorage, STRDEF("."));

        expect("round-trip: manifest parses", parsed != NULL);
        if (parsed != NULL)
        {
            expect("round-trip: hasInnodb preserved", parsed->info != NULL && parsed->info->hasInnodb);
            expect("round-trip: hasMyisam preserved", parsed->info != NULL && parsed->info->hasMyisam);
            expect(
                "round-trip: serverUuid preserved",
                parsed->info != NULL && parsed->info->serverUuid != NULL &&
                strEqZ(parsed->info->serverUuid, "8c0fd6f0-bf8f-11ee-9821-0242ac120002"));
        }

        mysqlColdBackupResultFree(result);

        // ---- Cold backup with no engines: empty source datadir ----
        rmRf(srcDir);
        mkdir(srcDir, 0755);
        rmRf(dstDir);
        mkdir(dstDir, 0755);

        const Storage *const emptySrcStorage = storagePosixNewP(STR(srcDir));
        const Storage *const emptyDstStorage = storagePosixNewP(STR(dstDir), .write = true);

        MysqlColdBackupResult *const emptyResult =
            mysqlColdBackup(emptySrcStorage, STRDEF("."), emptyDstStorage, STRDEF("."));

        expect("empty datadir: result is non-null", emptyResult != NULL);
        expect("empty datadir: 0 engines processed", emptyResult->enginesProcessed == 0);
        expect("empty datadir: 0 redo files copied", emptyResult->redoFilesCopied == 0);
        expect("empty datadir: no auto.cnf", !emptyResult->autoCnfCopied);
        expect("empty datadir: manifest still written", emptyResult->manifestWritten);

        mysqlColdBackupResultFree(emptyResult);

        // Cleanup
        rmRf(tmpRoot);
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
