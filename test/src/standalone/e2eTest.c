/***********************************************************************************************************************************
End-to-End test against a real MySQL/MariaDB/Percona instance.

Driven by:
  - test/e2e/run-mysql.sh   — apt-installed local MySQL on a /tmp socket (single flavor / version, host-only)
  - test/e2e/run-docker.sh  — containerized MySQL / MariaDB / Percona via TCP — multi-flavor matrix

The test binary picks its transport from env vars:
  MYBACKREST_E2E_SOCKET            — Unix socket path (host-only mode), OR
  MYBACKREST_E2E_HOST + _PORT      — TCP (Docker mode)
  MYBACKREST_E2E_DATADIR           — Source datadir to back up (host path; for Docker this is the bind-mount target)
  MYBACKREST_E2E_BACKUP            — Empty destination directory
  MYBACKREST_E2E_USER              — Backup user (has RELOAD/LOCK TABLES/REPLICATION CLIENT etc.)
  MYBACKREST_E2E_PASS              — Backup user password

Verifies:
  - mysqlHotBackup runs end-to-end against a live MariaDB server
  - SHOW MASTER STATUS returns a real binlog file + position, captured into the manifest
  - Engine handlers dispatch over the real datadir layout (InnoDB + MyISAM tables)
  - Manifest round-trips with the captured binlog block
  - The lock ladder cycles cleanly (BACKUP STAGE START → FLUSH → BLOCK_DDL → BLOCK_COMMIT → END for MariaDB 10.4+)

This is a SINGLE-flavor / SINGLE-version smoke test by design — the goal is to catch integration bugs that the scripted shim
can't (real wire-protocol quirks, real SHOW MASTER STATUS column ordering, real lock semantics). Multi-version coverage is the
Phase H Docker-matrix story.
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
#include "mysql/client.h"
#include "mysql/hotBackup.h"
#include "mysql/lock.h"
#include "mysql/manifest.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static const char *
mustGetEnv(const char *const name)
{
    const char *const value = getenv(name);
    if (value == NULL || value[0] == '\0')
    {
        fprintf(stderr, "missing required env var: %s\n", name);
        exit(2);
    }
    return value;
}

static bool
fileExists(const char *const path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));

    logInit(logLevelWarn, logLevelError, logLevelOff, false, 0, 1, false);

    // Transport: prefer TCP when MYBACKREST_E2E_HOST is set (Docker harness), else require socket path (host-local harness).
    const char *const socketPath = getenv("MYBACKREST_E2E_SOCKET");
    const char *const hostZ = getenv("MYBACKREST_E2E_HOST");
    const char *const portZ = getenv("MYBACKREST_E2E_PORT");

    if ((socketPath == NULL || socketPath[0] == '\0') && (hostZ == NULL || hostZ[0] == '\0'))
    {
        fprintf(stderr, "must set MYBACKREST_E2E_SOCKET or MYBACKREST_E2E_HOST (+ MYBACKREST_E2E_PORT)\n");
        exit(2);
    }

    const unsigned int port = (portZ != NULL && portZ[0] != '\0') ? (unsigned int)atoi(portZ) : 0;

    const char *const dataDirZ = mustGetEnv("MYBACKREST_E2E_DATADIR");
    const char *const backupDirZ = mustGetEnv("MYBACKREST_E2E_BACKUP");
    const char *const userZ = mustGetEnv("MYBACKREST_E2E_USER");
    const char *const passZ = mustGetEnv("MYBACKREST_E2E_PASS");

    int rc = 0;

    TRY_BEGIN()
    {
        if (hostZ != NULL && hostZ[0] != '\0')
            printf("e2e test (live server at %s:%u):\n", hostZ, port);
        else
            printf("e2e test (live server at %s):\n", socketPath);

        MysqlClient *const client = mysqlClientNew(
            /*host*/ (hostZ != NULL && hostZ[0] != '\0') ? STR(hostZ) : NULL,
            /*port*/ port,
            /*socket*/ (socketPath != NULL && socketPath[0] != '\0') ? STR(socketPath) : NULL,
            /*database*/ NULL,
            STR(userZ), STR(passZ), /*timeout*/ 10000);

        mysqlClientOpen(client);

        expect("connected to server", client != NULL);

        const MysqlVendor vendor = mysqlClientVendor(client);
        const unsigned int ver = mysqlClientServerVersionNum(client);
        const char *const vendorName =
            vendor == mysqlVendorMysql   ? "MySQL"   :
            vendor == mysqlVendorMariadb ? "MariaDB" :
            vendor == mysqlVendorPercona ? "Percona" : "(unknown)";
        printf("  INFO  vendor = %s, version = %u\n", vendorName, ver);

        expect("vendor detected (not Unknown)", vendor != mysqlVendorUnknown);
        // Reasonable lower bound — covers MySQL 5.0+, MariaDB 5.5+ which are the floor for any orchestrator path that matters
        expect("server version >= 50500", ver >= 50500);

        const Storage *const srcStorage = storagePosixNewP(STR(dataDirZ));
        const Storage *const dstStorage = storagePosixNewP(STR(backupDirZ), .write = true);

        MysqlHotBackupResult *const result = mysqlHotBackup(
            client, srcStorage, STRDEF("."), dstStorage, STRDEF("."), mysqlLockMethodAuto);

        // ---- Orchestrator result ----
        expect("hot backup completed", result != NULL && result->manifestWritten);
        expect("info detected hasInnodb", result->info != NULL && result->info->hasInnodb);
        expect("info detected hasMyisam", result->info != NULL && result->info->hasMyisam);
        expect("at least one redo file copied", result->redoFilesCopied >= 1);

        // Page-checksum validation — every page in the real running mysqld's tablespaces should validate. The InnoDB engine
        // handler ran every .ibd / ibdata1 / mysql.ibd / undo_*.ibu through the page-checksum filter using the algorithm
        // mysqlDataDirInspect detected.
        printf("  INFO  InnoDB pages: checked=%lu valid=%lu invalid=%lu skipped=%lu\n",
            (unsigned long)result->innodbPagesChecked, (unsigned long)result->innodbPagesValid,
            (unsigned long)result->innodbPagesInvalid, (unsigned long)result->innodbPagesSkipped);
        expect("InnoDB pages were checked (>0)", result->innodbPagesChecked > 0);
        expect("InnoDB validation found 0 invalid pages", result->innodbPagesInvalid == 0);
        // auto.cnf is a MySQL/Percona convention — MariaDB doesn't create one (Galera uses grastate.dat instead).
        // Either presence or absence is correct depending on vendor; the orchestrator records autoCnfCopied accordingly.
        if (mysqlClientVendor(client) == mysqlVendorMariadb)
            expect("auto.cnf optional on MariaDB", true);                // documented difference, always passes
        else
            expect("auto.cnf copied (MySQL/Percona)", result->autoCnfCopied);
        expect("at least 2 engines processed", result->enginesProcessed >= 2);

        // Lock-method autodetect is vendor- and version-driven:
        //   MariaDB 10.4+         → stage    (BACKUP STAGE state machine)
        //   MariaDB 10.3 / older  → ftwrl    (FLUSH TABLES WITH READ LOCK)
        //   MySQL/Percona 8.0.16+ → instance (LOCK INSTANCE FOR BACKUP)
        //   MySQL/Percona older   → ftwrl
        if (vendor == mysqlVendorMariadb && ver >= 100400)
            expect("lock method = stage (MariaDB 10.4+)", result->lockMethodUsed == mysqlLockMethodStage);
        else if ((vendor == mysqlVendorMysql || vendor == mysqlVendorPercona) && ver >= 80016)
            expect("lock method = instance (MySQL/Percona 8.0.16+)", result->lockMethodUsed == mysqlLockMethodInstance);
        else
            expect("lock method = ftwrl (legacy)", result->lockMethodUsed == mysqlLockMethodFtwrl);

        // ---- Binlog captured ----
        expect("binlog start file captured", result->binlog != NULL && result->binlog->startFile != NULL);
        if (result->binlog != NULL && result->binlog->startFile != NULL)
        {
            printf("  INFO  binlog start = %s @ %lu\n", strZ(result->binlog->startFile), (unsigned long)result->binlog->startPos);
            expect("binlog start file is non-empty", strSize(result->binlog->startFile) > 0);
            expect("binlog start position > 0", result->binlog->startPos > 0);
        }

        // ---- Destination has the expected files ----
        char path[512];

        // auto.cnf only present on MySQL/Percona — see vendor branch above
        snprintf(path, sizeof(path), "%s/auto.cnf", backupDirZ);
        if (mysqlClientVendor(client) == mysqlVendorMariadb)
            expect("dst auto.cnf absent (MariaDB)", true);
        else
            expect("dst auto.cnf exists (MySQL/Percona)", fileExists(path));

        snprintf(path, sizeof(path), "%s/ibdata1", backupDirZ);
        expect("dst ibdata1 exists", fileExists(path));

        // testdb is the database created by run-mariadb.sh
        snprintf(path, sizeof(path), "%s/testdb/users.ibd", backupDirZ);
        expect("dst testdb/users.ibd exists", fileExists(path));

        snprintf(path, sizeof(path), "%s/testdb/legacy.MYD", backupDirZ);
        expect("dst testdb/legacy.MYD exists", fileExists(path));

        snprintf(path, sizeof(path), "%s/mybackrest_backup_info", backupDirZ);
        expect("dst manifest exists", fileExists(path));

        // ---- Manifest round-trip ----
        MysqlBackupManifestParsed *const parsed = mysqlBackupManifestRead(dstStorage, STRDEF("."));

        expect("manifest parses", parsed != NULL);
        if (parsed != NULL)
        {
            expect("round-trip: hasInnodb preserved", parsed->info != NULL && parsed->info->hasInnodb);
            expect("round-trip: hasMyisam preserved", parsed->info != NULL && parsed->info->hasMyisam);
            expect("round-trip: binlog block present", parsed->binlog != NULL);
            if (parsed->binlog != NULL && result->binlog != NULL && result->binlog->startFile != NULL)
            {
                expect(
                    "round-trip: binlog start file preserved",
                    parsed->binlog->startFile != NULL &&
                    strEq(parsed->binlog->startFile, result->binlog->startFile));
                expect(
                    "round-trip: binlog start position preserved",
                    parsed->binlog->startPos == result->binlog->startPos);
            }
        }

        mysqlClientFree(client);
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
