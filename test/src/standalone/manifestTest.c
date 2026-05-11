/***********************************************************************************************************************************
Standalone test for src/mysql/manifest.c — round-trip the manifest writer
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/manifest.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
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
        printf("Manifest writer test:\n");

        // Compose a representative MariaDB Galera datadir info struct
        MysqlDataDirInfo info = {
            .vendor = mysqlVendorMariadb,
            .versionNum = 101106,
            .versionExact = true,
            .serverUuid = STRDEF("aaaa-bbbb-cccc-dddd-eeeeffffaaaa"),
            .pageSize = mysqlPageSize16K,
            .redoLayout = mysqlRedoLayoutFixedIbLogfile,
            .hasInnodb = true,
            .hasMyisam = false,
            .hasAria = true,
            .hasMyrocks = false,
            .hasTokudb = false,
            .encrypted = false,
            .pageChecksum = mysqlPageChecksumCrc32,
            .antelope = false,
            .zipSsize = 0,
            .hasGalera = true,
            .galeraStateUuid = STRDEF("11111111-2222-3333-4444-555555555555"),
            .galeraSeqno = 987654321,
        };

        MysqlBackupBinlog binlog = {
            .startFile = STRDEF("mariadb-bin.000007"),
            .startPos = 4,
            .startGtid = STRDEF("0-1-100"),
            .stopFile = STRDEF("mariadb-bin.000008"),
            .stopPos = 12345,
            .stopGtid = STRDEF("0-1-200"),
        };

        const String *const text = mysqlBackupManifestRender(&info, &binlog);
        const char *const z = strZ(text);

        // Spot-check the rendered text
        expect("output contains [backrest] section", strstr(z, "[backrest]") != NULL);
        expect("format = 6", strstr(z, "format = 6") != NULL);
        expect("vendor = mariadb", strstr(z, "vendor = mariadb") != NULL);
        expect("version_num = 101106", strstr(z, "version_num = 101106") != NULL);
        expect("version_exact = true", strstr(z, "version_exact = true") != NULL);
        expect("page_size = 16384", strstr(z, "page_size = 16384") != NULL);
        expect("page_checksum = crc32", strstr(z, "page_checksum = crc32") != NULL);
        expect("redo_layout = fixed", strstr(z, "redo_layout = fixed") != NULL);
        expect("antelope = false", strstr(z, "antelope = false") != NULL);
        expect("encrypted = false", strstr(z, "encrypted = false") != NULL);
        expect("server_uuid line present", strstr(z, "server_uuid = aaaa-bbbb") != NULL);

        expect("[engines] section", strstr(z, "[engines]") != NULL);
        expect("innodb = true", strstr(z, "innodb = true") != NULL);
        expect("aria = true", strstr(z, "aria = true") != NULL);
        expect("myisam = false", strstr(z, "myisam = false") != NULL);

        expect("[galera] section emitted", strstr(z, "[galera]") != NULL);
        expect("galera state_uuid line", strstr(z, "11111111-2222-3333-4444-555555555555") != NULL);
        expect("galera seqno = 987654321", strstr(z, "seqno = 987654321") != NULL);

        expect("[binlog] section emitted", strstr(z, "[binlog]") != NULL);
        expect("start_file = mariadb-bin.000007", strstr(z, "start_file = mariadb-bin.000007") != NULL);
        expect("start_pos = 4", strstr(z, "start_pos = 4") != NULL);
        expect("stop_pos = 12345", strstr(z, "stop_pos = 12345") != NULL);

        // ---- Test 2: minimal info (no Galera, no binlog) — sections should be absent ----
        MysqlDataDirInfo minimal = {
            .vendor = mysqlVendorMysql,
            .versionNum = 80036,
            .pageSize = mysqlPageSize16K,
            .pageChecksum = mysqlPageChecksumCrc32,
            .redoLayout = mysqlRedoLayoutDynamicInnodbRedo,
            .hasInnodb = true,
        };

        const String *const minimalText = mysqlBackupManifestRender(&minimal, NULL);
        const char *const mz = strZ(minimalText);

        expect("[minimal] no [galera] section", strstr(mz, "[galera]") == NULL);
        expect("[minimal] no [binlog] section", strstr(mz, "[binlog]") == NULL);
        expect("[minimal] vendor = mysql", strstr(mz, "vendor = mysql") != NULL);
        expect("[minimal] redo_layout = dynamic", strstr(mz, "redo_layout = dynamic") != NULL);
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
