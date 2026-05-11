/***********************************************************************************************************************************
Hot-Backup End-to-End Integration Test (with harnessMysql shim)

Drives the full mysqlHotBackup orchestrator against a scripted libmariadb. Covers:
  - Server sanity check (SHOW VARIABLES for log_bin / binlog_format / gtid_mode / server_id, SELECT @@server_uuid /
    @@log_bin_basename)
  - Lock method selection + acquisition sequence
  - SHOW MASTER STATUS at start and stop
  - Engine handler dispatch over a synthetic datadir
  - Manifest written with [binlog] block populated
  - Round-trip parse confirms the captured binlog position survived

The harnessMysql shim intercepts every libmariadb function at link time so client.c's mysql_real_query etc. land in our
scripted dispatcher instead of the real library.
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

#include "harnessMysql.h"

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
        printf("Hot-backup integration test (scripted libmariadb):\n");

        const char *const tmpRoot = "/tmp/mybackrest-hotbackup-int";
        const char *const srcDir = "/tmp/mybackrest-hotbackup-int/src";
        const char *const dstDir = "/tmp/mybackrest-hotbackup-int/dst";

        rmRf(tmpRoot);
        mkdir(tmpRoot, 0755);
        mkdir(srcDir, 0755);
        mkdir(dstDir, 0755);

        // Synthetic datadir: auto.cnf + ibdata1 + #innodb_redo file + a MyISAM table
        writeFile(
            "/tmp/mybackrest-hotbackup-int/src/auto.cnf",
            "[auto]\nserver-uuid=8c0fd6f0-bf8f-11ee-9821-0242ac120002\n", 56);

        unsigned char ibdata[16384] = {0};
        writeFile("/tmp/mybackrest-hotbackup-int/src/ibdata1", ibdata, sizeof(ibdata));

        mkdir("/tmp/mybackrest-hotbackup-int/src/#innodb_redo", 0755);
        writeZeros("/tmp/mybackrest-hotbackup-int/src/#innodb_redo/#ib_redo7_1234", 4096);

        mkdir("/tmp/mybackrest-hotbackup-int/src/myschema", 0755);
        writeZeros("/tmp/mybackrest-hotbackup-int/src/myschema/users.MYD", 512);
        writeZeros("/tmp/mybackrest-hotbackup-int/src/myschema/users.MYI", 512);

        // Script the libmariadb conversation. Order matters — must match what hotBackup.c issues.
        const HrnMysqlScript script[] = {
            // 1. Version + vendor detection happens during mysqlClientOpen
            HRN_MYSQL_VERSION("8.0.36", 80036),

            // 2. mysqlClientOpen issues SELECT @@version_comment after the handshake
            HRN_MYSQL_QUERY_ONE("SELECT @@version_comment", "MySQL Community Server - GPL"),

            // 3. mysqlServerSanityCheck — each variable goes via "SHOW VARIABLES LIKE 'X'" returning (Variable_name, Value).
            //    For MySQL/Percona we query @@gtid_mode; gtid_strict_mode isn't checked. (For MariaDB the sanity check
            //    skips the gtid_mode lookup entirely since GTID is automatic when log_bin is ON.)
            HRN_MYSQL_QUERY_VAR("SHOW VARIABLES LIKE 'log_bin'",          "log_bin",          "ON"),
            HRN_MYSQL_QUERY_VAR("SHOW VARIABLES LIKE 'binlog_format'",    "binlog_format",    "ROW"),
            HRN_MYSQL_QUERY_VAR("SHOW VARIABLES LIKE 'gtid_mode'",        "gtid_mode",        "ON"),
            HRN_MYSQL_QUERY_VAR("SHOW VARIABLES LIKE 'server_id'",        "server_id",        "42"),
            HRN_MYSQL_QUERY_ONE("SELECT @@server_uuid",                   "8c0fd6f0-bf8f-11ee-9821-0242ac120002"),
            HRN_MYSQL_QUERY_ONE("SELECT @@log_bin_basename",              "/var/log/mysql/mysql-bin"),

            // 4. Lock acquisition — MySQL 8.0.16+ uses instance method.
            //    mysqlLockBegin for instance is a no-op (no SQL). mysqlLockBlockDdl issues LOCK INSTANCE FOR BACKUP.
            HRN_MYSQL_QUERY_NONE("LOCK INSTANCE FOR BACKUP"),

            // 5. Start binlog capture — SHOW MASTER STATUS returns File, Position, Binlog_Do_DB, Binlog_Ignore_DB, Executed_Gtid_Set
            HRN_MYSQL_QUERY_MASTER_STATUS("mysql-bin.000123", "4567", "8c0fd6f0-bf8f-11ee-9821-0242ac120002:1-100"),

            // 6. mysqlLockBlockCommit for instance is a no-op (LOCK INSTANCE already blocks new commits)
            // 7. Stop binlog capture
            HRN_MYSQL_QUERY_MASTER_STATUS("mysql-bin.000123", "4567", "8c0fd6f0-bf8f-11ee-9821-0242ac120002:1-100"),

            // 8. mysqlLockRelease — instance method issues UNLOCK INSTANCE
            HRN_MYSQL_QUERY_NONE("UNLOCK INSTANCE"),
        };

        hrnMysqlScriptSet(script, LENGTH_OF(script));

        // Build + open the client. mysqlClientOpen drives the handshake + version detection through the shim.
        MysqlClient *const client = mysqlClientNew(
            STRDEF("127.0.0.1"), 3306, /*socket*/ NULL, /*database*/ NULL,
            STRDEF("backup"), STRDEF("password"), /*timeout*/ 5000);

        mysqlClientOpen(client);

        expect("client vendor detected as MySQL", mysqlClientVendor(client) == mysqlVendorMysql);
        expect("client server version captured", mysqlClientServerVersionNum(client) == 80036);

        // Run the hot backup
        const Storage *const srcStorage = storagePosixNewP(STR(srcDir));
        const Storage *const dstStorage = storagePosixNewP(STR(dstDir), .write = true);

        MysqlHotBackupResult *const result = mysqlHotBackup(
            client, srcStorage, STRDEF("."), dstStorage, STRDEF("."), mysqlLockMethodAuto);

        // ---- Results ----
        expect("hot backup completed", result != NULL && result->manifestWritten);
        expect("lock method used = instance", result->lockMethodUsed == mysqlLockMethodInstance);
        expect("info detected hasInnodb", result->info != NULL && result->info->hasInnodb);
        expect("info detected hasMyisam", result->info != NULL && result->info->hasMyisam);
        expect("at least one redo file copied", result->redoFilesCopied >= 1);
        expect("auto.cnf copied", result->autoCnfCopied);
        expect("at least 2 engines processed", result->enginesProcessed >= 2);

        // ---- Binlog block populated from SHOW MASTER STATUS ----
        expect("binlog start file captured", result->binlog != NULL && result->binlog->startFile != NULL);
        if (result->binlog != NULL && result->binlog->startFile != NULL)
        {
            expect(
                "binlog start file = mysql-bin.000123",
                strEqZ(result->binlog->startFile, "mysql-bin.000123"));
            expect("binlog start position = 4567", result->binlog->startPos == 4567);
            expect(
                "binlog start GTID captured",
                result->binlog->startGtid != NULL &&
                strEqZ(result->binlog->startGtid, "8c0fd6f0-bf8f-11ee-9821-0242ac120002:1-100"));
        }

        // ---- Destination files exist ----
        expect("dst auto.cnf exists", fileExists("/tmp/mybackrest-hotbackup-int/dst/auto.cnf"));
        expect("dst ibdata1 exists", fileExists("/tmp/mybackrest-hotbackup-int/dst/ibdata1"));
        expect(
            "dst #innodb_redo/#ib_redo7_1234 exists",
            fileExists("/tmp/mybackrest-hotbackup-int/dst/#innodb_redo/#ib_redo7_1234"));
        expect("dst myschema/users.MYD exists", fileExists("/tmp/mybackrest-hotbackup-int/dst/myschema/users.MYD"));
        expect("dst manifest exists", fileExists("/tmp/mybackrest-hotbackup-int/dst/mybackrest_backup_info"));

        // ---- Round-trip: manifest preserves binlog block ----
        MysqlBackupManifestParsed *const parsed = mysqlBackupManifestRead(dstStorage, STRDEF("."));

        expect("round-trip: manifest parses", parsed != NULL);
        if (parsed != NULL)
        {
            expect("round-trip: binlog block present", parsed->binlog != NULL);
            if (parsed->binlog != NULL)
            {
                expect(
                    "round-trip: binlog start file preserved",
                    parsed->binlog->startFile != NULL && strEqZ(parsed->binlog->startFile, "mysql-bin.000123"));
                expect("round-trip: binlog start position preserved", parsed->binlog->startPos == 4567);
                expect(
                    "round-trip: binlog start GTID preserved",
                    parsed->binlog->startGtid != NULL &&
                    strEqZ(parsed->binlog->startGtid, "8c0fd6f0-bf8f-11ee-9821-0242ac120002:1-100"));
            }
        }

        // ---- Script must have been fully consumed ----
        hrnMysqlScriptVerifyComplete();
        expect("MySQL script fully consumed", true);

        mysqlClientFree(client);
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
