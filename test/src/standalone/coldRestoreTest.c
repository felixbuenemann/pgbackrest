/***********************************************************************************************************************************
Standalone test for src/mysql/coldRestore.c — backup → restore round-trip

Builds a synthetic datadir, runs mysqlColdBackup to a backup dir, then mysqlColdRestore from that backup dir to a fresh restore
dir, and verifies the restore dir is byte-identical to the source modulo manifest + recovery files.
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
#include "mysql/coldBackup.h"
#include "mysql/coldRestore.h"
#include "mysql/datadir.h"
#include "storage/posix/storage.h"

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

static bool
filesEqual(const char *const a, const char *const b)
{
    FILE *const fa = fopen(a, "rb");
    FILE *const fb = fopen(b, "rb");
    if (fa == NULL || fb == NULL) { if (fa) fclose(fa); if (fb) fclose(fb); return false; }

    bool equal = true;
    while (equal)
    {
        unsigned char ba[4096], bb[4096];
        const size_t na = fread(ba, 1, sizeof(ba), fa);
        const size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb) { equal = false; break; }
        if (na == 0) break;
        if (memcmp(ba, bb, na) != 0) { equal = false; break; }
    }

    fclose(fa);
    fclose(fb);
    return equal;
}

static void
rmRf(const char *const path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best-effort */ }
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
        printf("Cold-restore + backup round-trip tests:\n");

        const char *const tmpRoot = "/tmp/mybackrest-coldrestore-test";
        const char *const srcDir = "/tmp/mybackrest-coldrestore-test/src";
        const char *const bkpDir = "/tmp/mybackrest-coldrestore-test/bkp";
        const char *const rstDir = "/tmp/mybackrest-coldrestore-test/rst";

        rmRf(tmpRoot);
        mkdir(tmpRoot, 0755);
        mkdir(srcDir, 0755);
        mkdir(bkpDir, 0755);
        mkdir(rstDir, 0755);

        // ---- Synthetic datadir (mirrors coldBackupTest's layout) ----
        writeFile(
            "/tmp/mybackrest-coldrestore-test/src/auto.cnf",
            "[auto]\nserver-uuid=aabbccdd-eeff-1122-3344-556677889900\n", 56);

        unsigned char ibdata[16384] = {0};
        writeFile("/tmp/mybackrest-coldrestore-test/src/ibdata1", ibdata, sizeof(ibdata));

        mkdir("/tmp/mybackrest-coldrestore-test/src/#innodb_redo", 0755);
        writeZeros("/tmp/mybackrest-coldrestore-test/src/#innodb_redo/#ib_redo7_1234", 4096);

        mkdir("/tmp/mybackrest-coldrestore-test/src/myschema", 0755);
        writeZeros("/tmp/mybackrest-coldrestore-test/src/myschema/users.MYD", 512);
        writeZeros("/tmp/mybackrest-coldrestore-test/src/myschema/users.MYI", 512);

        // ---- Backup ----
        const Storage *const srcStorage = storagePosixNewP(STR(srcDir));
        const Storage *const bkpStorage = storagePosixNewP(STR(bkpDir), .write = true);

        MysqlColdBackupResult *const bkp = mysqlColdBackup(srcStorage, STRDEF("."), bkpStorage, STRDEF("."));

        expect("backup succeeded", bkp != NULL && bkp->manifestWritten);
        expect("backup processed >=2 engines", bkp->enginesProcessed >= 2);

        // ---- Restore ----
        // Reopen backup storage as read-only for the restore side (mirrors how a real operator would invoke it)
        const Storage *const bkpStorageRO = storagePosixNewP(STR(bkpDir));
        const Storage *const rstStorage = storagePosixNewP(STR(rstDir), .write = true);

        MysqlColdRestoreResult *const rst =
            mysqlColdRestore(bkpStorageRO, STRDEF("."), rstStorage, STRDEF("."), /*mysqldPath*/ NULL);

        expect("restore result non-null", rst != NULL);
        expect("restore manifest parsed", rst != NULL && rst->manifest != NULL);
        expect(
            "restore preserved serverUuid",
            rst != NULL && rst->manifest != NULL && rst->manifest->info != NULL &&
            rst->manifest->info->serverUuid != NULL &&
            strEqZ(rst->manifest->info->serverUuid, "aabbccdd-eeff-1122-3344-556677889900"));
        expect("restore copied >=4 files", rst != NULL && rst->filesCopied >= 4);
        expect("restore skipped recovery file generation (mysqldPath NULL)", rst != NULL && !rst->recoveryFilesWritten);

        // ---- Byte-identity checks: every source file should reappear in the restore ----
        expect(
            "restore auto.cnf matches source",
            filesEqual(
                "/tmp/mybackrest-coldrestore-test/src/auto.cnf",
                "/tmp/mybackrest-coldrestore-test/rst/auto.cnf"));
        expect(
            "restore ibdata1 matches source",
            filesEqual(
                "/tmp/mybackrest-coldrestore-test/src/ibdata1",
                "/tmp/mybackrest-coldrestore-test/rst/ibdata1"));
        expect(
            "restore #innodb_redo/#ib_redo7_1234 matches source",
            filesEqual(
                "/tmp/mybackrest-coldrestore-test/src/#innodb_redo/#ib_redo7_1234",
                "/tmp/mybackrest-coldrestore-test/rst/#innodb_redo/#ib_redo7_1234"));
        expect(
            "restore myschema/users.MYD matches source",
            filesEqual(
                "/tmp/mybackrest-coldrestore-test/src/myschema/users.MYD",
                "/tmp/mybackrest-coldrestore-test/rst/myschema/users.MYD"));

        // The manifest should NOT be copied into the restore dir (it's a backup-side artefact)
        expect(
            "restore does NOT include the manifest file",
            !fileExists("/tmp/mybackrest-coldrestore-test/rst/mybackrest_backup_info"));

        // ---- Restore with mysqldPath set: recovery files should be written ----
        rmRf(rstDir);
        mkdir(rstDir, 0755);
        const Storage *const rstStorage2 = storagePosixNewP(STR(rstDir), .write = true);

        // Use a fake mysqld path — prepareWriteRecoveryFiles doesn't validate, just writes the cnf/sql referencing it
        MysqlColdRestoreResult *const rst2 = mysqlColdRestore(
            bkpStorageRO, STRDEF("."), rstStorage2, STRDEF("."), STRDEF("/usr/sbin/mysqld"));

        expect("restore w/ mysqld: recovery files written", rst2 != NULL && rst2->recoveryFilesWritten);
        expect(
            "restore w/ mysqld: mybackrest_recovery.cnf exists",
            fileExists("/tmp/mybackrest-coldrestore-test/rst/mybackrest_recovery.cnf"));
        expect(
            "restore w/ mysqld: mybackrest_recovery.sql exists",
            fileExists("/tmp/mybackrest-coldrestore-test/rst/mybackrest_recovery.sql"));

        // ---- Restore from an empty/invalid backup throws ----
        const char *const emptyBkp = "/tmp/mybackrest-coldrestore-test/empty";
        mkdir(emptyBkp, 0755);
        const Storage *const emptyBkpStorage = storagePosixNewP(STR(emptyBkp));

        bool threw = false;
        TRY_BEGIN()
        {
            mysqlColdRestore(emptyBkpStorage, STRDEF("."), rstStorage2, STRDEF("."), NULL);
        }
        CATCH(FileMissingError)
        {
            threw = true;
        }
        TRY_END();

        expect("empty backup: throws FileMissingError", threw);

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
