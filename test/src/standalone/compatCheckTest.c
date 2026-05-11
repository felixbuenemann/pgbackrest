/***********************************************************************************************************************************
Standalone test for src/command/restore/prepare.c — prepareVerifyCompatibility

Composes the writer + reader + binary probe + check function from this session into a single end-to-end exercise:
  - Render a manifest from a known MysqlDataDirInfo
  - Write it to a tmp directory
  - Synthesize a fake mysqld script that prints the matching --version line
  - Call prepareVerifyCompatibility — should pass silently
  - Repeat with an INCOMPATIBLE binary (cross-vendor or cross-major) — should throw OptionInvalidError

Verifies the round-trip-then-validate pipeline a real Phase E restore command will follow.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "command/restore/prepare.h"
#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/manifest.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

static void mkdirP(const char *const p) { mkdir(p, 0755); }
static void rmrf(const char *const p) { char c[1024]; snprintf(c, sizeof(c), "rm -rf '%s'", p); int u __attribute__((unused)) = system(c); }

static void
writeFakeMysqld(const char *const path, const char *const versionLine)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fprintf(fp, "#!/bin/sh\necho '%s'\nexit 0\n", versionLine);
    fclose(fp);
    chmod(path, 0755);
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
        printf("Restore compatibility-check test:\n");

        const char *const root = "/tmp/mybackrest-compat-test";
        rmrf(root);
        mkdirP(root);

        const Storage *const storage = storagePosixNewP(STR(root), .write = true);

        // Write a manifest representing a MySQL 8.0.36 backup
        MysqlDataDirInfo info = {
            .vendor = mysqlVendorMysql,
            .versionNum = 80036,
            .versionExact = true,
            .pageSize = mysqlPageSize16K,
            .pageChecksum = mysqlPageChecksumCrc32,
            .redoLayout = mysqlRedoLayoutFixedIbLogfile,
            .hasInnodb = true,
            .serverUuid = STRDEF("aaaa-bbbb-cccc-dddd-eeeeffffaaaa"),
        };

        mysqlBackupManifestWrite(storage, STRDEF("."), &info, NULL);


        // ---- Test 1: matching binary → passes silently ----
        const char *const mysqlOk = "/tmp/mybackrest-compat-test/fake-mysql-8.0.36";
        writeFakeMysqld(mysqlOk, "mysqld  Ver 8.0.36 for Linux on x86_64 (MySQL Community Server - GPL)");

        bool threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysqlOk), false);
        }
        CATCH_ANY()
        {
            threw = true;
        }
        TRY_END();

        expect("matching MySQL 8.0.36 binary → no throw", !threw);

        // ---- Test 2: Percona Server (-N suffix) for MySQL backup → still compatible (same on-disk) ----
        const char *const mysqlPercona = "/tmp/mybackrest-compat-test/fake-percona-8.0.36";
        writeFakeMysqld(mysqlPercona, "mysqld  Ver 8.0.36-28 for Linux on x86_64 (Percona Server (GPL))");

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysqlPercona), false);
        }
        CATCH_ANY()
        {
            threw = true;
        }
        TRY_END();

        expect("Percona binary for MySQL backup → no throw (interchangeable)", !threw);

        // ---- Test 3: MariaDB binary for MySQL backup → INCOMPATIBLE ----
        const char *const mariadbBin = "/tmp/mybackrest-compat-test/fake-mariadb-10.11";
        writeFakeMysqld(mariadbBin, "mysqld  Ver 10.11.6-MariaDB-0+deb12u1 for debian-linux-gnu on x86_64");

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mariadbBin), false);
        }
        CATCH(OptionInvalidError)
        {
            threw = true;
        }
        TRY_END();

        expect("MariaDB binary for MySQL backup → throws OptionInvalidError", threw);

        // ---- Test 4: 5.7 binary for 8.0 backup → INCOMPATIBLE (DD downgrade) ----
        const char *const mysql57 = "/tmp/mybackrest-compat-test/fake-mysql-5.7";
        writeFakeMysqld(mysql57, "mysqld  Ver 5.7.42 for Linux on x86_64 (MySQL Community Server (GPL))");

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysql57), false);
        }
        CATCH(OptionInvalidError)
        {
            threw = true;
        }
        TRY_END();

        expect("MySQL 5.7 binary for 8.0 backup → throws (DD downgrade unsupported)", threw);

        // ---- Test 4b: backup with redo format 6 (8.0.30+) on 5.7 binary → throws redo-format incompat ----
        // Re-write manifest with redoFormatNum=6 to trigger the redo-format check
        info.redoFormatNum = 6;
        mysqlBackupManifestWrite(storage, STRDEF("."), &info, NULL);

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysql57), false);
        }
        CATCH(OptionInvalidError)
        {
            threw = true;
        }
        TRY_END();

        expect("MySQL 5.7 binary, backup redoFormat=6 → throws redo-format incompat", threw);

        // Reset manifest for next test
        info.redoFormatNum = 0;
        mysqlBackupManifestWrite(storage, STRDEF("."), &info, NULL);

        // ---- Test 4c: engine compat — Aria backup on MySQL binary → throws ----
        info.redoFormatNum = 0;
        info.hasAria = true;
        mysqlBackupManifestWrite(storage, STRDEF("."), &info, NULL);

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysqlOk), false);
        }
        CATCH(OptionInvalidError)
        {
            threw = true;
        }
        TRY_END();

        expect("Aria backup on MySQL binary → throws engine compat", threw);
        info.hasAria = false;
        mysqlBackupManifestWrite(storage, STRDEF("."), &info, NULL);

        // ---- Test 5: missing manifest → throws FileMissingError ----
        unlink("/tmp/mybackrest-compat-test/mybackrest_backup_info");

        threw = false;
        TRY_BEGIN()
        {
            prepareVerifyCompatibility(storage, STRDEF("."), STR(mysqlOk), false);
        }
        CATCH(FileMissingError)
        {
            threw = true;
        }
        TRY_END();

        expect("missing manifest → throws FileMissingError", threw);

        rmrf(root);
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
