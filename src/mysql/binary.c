/***********************************************************************************************************************************
MySQL / MariaDB Binary Probe

Algorithm:
  1. pipe() + fork()
  2. Child: dup the pipe write end to stdout, exec `<path> --version`
  3. Parent: read up to 4096 bytes from the read end (more than enough for a single --version line, which never exceeds ~300 chars)
  4. waitpid the child
  5. Parse the captured line for "Ver X.Y.Z" + vendor markers
  6. Return MysqlBinaryInfo

Implementation note (don't swap to common/exec.c): The Exec wrapper in src/common/exec.c is designed for LONG-LIVED processes
(ssh sessions, remote workers) and treats a child exiting during normal stream consumption as `execCheckStatusError("terminated
unexpectedly")` — even with a 0 exit code. For one-shot commands that exit immediately after writing their output, the manual
fork+pipe+waitpid pattern below is correct and ~50 lines is a reasonable cost.
***********************************************************************************************************************************/
#include <build.h>

#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/fork.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/binary.h"
#include "mysql/interface.h"

#define MYSQL_BINARY_PROBE_MAX_OUTPUT                               4096
#define MYSQL_VERSION_DICTIONARY_BOUNDARY                           80000           // 8.0+ moved DD into mysql.ibd

/***********************************************************************************************************************************
Detect vendor from the version line. Mirrors mysqlClientDetectVendor in client.c — same heuristics, different input.
***********************************************************************************************************************************/
static MysqlVendor
mysqlBinaryDetectVendor(const char *const line)
{
    if (strstr(line, "MariaDB") != NULL)
        return mysqlVendorMariadb;

    if (strstr(line, "Percona") != NULL)
        return mysqlVendorPercona;

    if (strstr(line, "MySQL") != NULL)
        return mysqlVendorMysql;

    return mysqlVendorUnknown;
}

/***********************************************************************************************************************************
Parse "X.Y.Z" out of a string starting at the given offset. Returns 0 if no version found at that position.

Pack as MAJOR*10000 + MINOR*100 + PATCH (matches mysql_get_server_version() encoding).
***********************************************************************************************************************************/
static unsigned int
mysqlBinaryParseVersion(const char *const text)
{
    unsigned int major = 0, minor = 0, patch = 0;

    if (sscanf(text, "%u.%u.%u", &major, &minor, &patch) >= 2)
        return major * 10000 + minor * 100 + patch;

    return 0;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlBinaryInfo *
mysqlBinaryProbe(const String *const binaryPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, binaryPath);
    FUNCTION_LOG_END();

    ASSERT(binaryPath != NULL);

    MysqlBinaryInfo *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        int pipeFd[2];
        THROW_ON_SYS_ERROR(pipe(pipeFd) == -1, ExecuteError, "binary-probe pipe() failed");

        const pid_t pid = forkSafe();

        if (pid == 0)
        {
            // Child: redirect stdout to pipe write end, close read end. stderr left alone so noise still surfaces in logs.
            close(pipeFd[0]);

            if (dup2(pipeFd[1], STDOUT_FILENO) == -1)
                exit(127);

            close(pipeFd[1]);

            const char *argv[3] = {strZ(binaryPath), "--version", NULL};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
            execvp(argv[0], (char **)argv);
#pragma GCC diagnostic pop
            exit(127);
        }

        // Parent: close write end so child's eventual close gives us EOF, then drain stdout
        close(pipeFd[1]);

        char buf[MYSQL_BINARY_PROBE_MAX_OUTPUT + 1];
        ssize_t totalRead = 0;
        ssize_t got;

        while ((got = read(pipeFd[0], buf + totalRead, (size_t)(MYSQL_BINARY_PROBE_MAX_OUTPUT - totalRead))) > 0)
        {
            totalRead += got;
            if (totalRead >= MYSQL_BINARY_PROBE_MAX_OUTPUT)
                break;
        }
        close(pipeFd[0]);

        buf[totalRead] = '\0';

        int status;
        const pid_t waited = waitpid(pid, &status, 0);
        THROW_ON_SYS_ERROR(waited == -1, ExecuteError, "waitpid failed for binary probe");

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        {
            THROW_FMT(
                ExecuteError, "binary probe '%s --version' exited abnormally (status=%d, output='%s')",
                strZ(binaryPath), status, buf);
        }

        // Truncate at the first newline — --version typically prints one line + a license blurb on subsequent lines
        for (ssize_t i = 0; i < totalRead; i++)
        {
            if (buf[i] == '\n')
            {
                buf[i] = '\0';
                break;
            }
        }

        // Find " Ver " (the version-token marker; same on every flavor we support)
        const char *const verToken = strstr(buf, " Ver ");

        if (verToken == NULL)
            THROW_FMT(FormatError, "binary probe output missing 'Ver' token: '%s'", buf);

        const unsigned int versionNum = mysqlBinaryParseVersion(verToken + 5);

        if (versionNum == 0)
            THROW_FMT(FormatError, "binary probe couldn't parse version number from: '%s'", buf);

        const MysqlVendor vendor = mysqlBinaryDetectVendor(buf);

        // Materialize result in the parent context. strTrim mutates versionRaw in place to drop the trailing space/tab that
        // follows the version token.
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlBinaryInfo));
            *result = (MysqlBinaryInfo)
            {
                .vendor = vendor,
                .versionNum = versionNum,
                .versionRaw = strTrim(strNewZ(verToken + 5)),
                .fullOutput = strNewZ(buf),
            };
        }
        MEM_CONTEXT_PRIOR_END();

        LOG_DETAIL_FMT(
            "binary probe: %s → vendor=%u version=%u (raw: %s)",
            strZ(binaryPath), (unsigned int)vendor, versionNum, strZ(result->versionRaw));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_BINARY_INFO, result);
}

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlBinaryCheckCompatibility(
    const MysqlBinaryInfo *const probe, const MysqlVendor backupVendor, const unsigned int backupVersionNum)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_BINARY_INFO, probe);
        FUNCTION_LOG_PARAM(STRING_ID, backupVendor);
        FUNCTION_LOG_PARAM(UINT, backupVersionNum);
    FUNCTION_LOG_END();

    ASSERT(probe != NULL);

    // Vendor mismatch is the most serious issue — different on-disk formats, locking primitives, plugin sets.
    if (probe->vendor != backupVendor && backupVendor != mysqlVendorUnknown && probe->vendor != mysqlVendorUnknown)
    {
        // Percona and MySQL share the same on-disk format and are interchangeable for restore — others aren't.
        const bool perconaMysqlSwap =
            (probe->vendor == mysqlVendorMysql && backupVendor == mysqlVendorPercona) ||
            (probe->vendor == mysqlVendorPercona && backupVendor == mysqlVendorMysql);

        if (!perconaMysqlSwap)
        {
            FUNCTION_LOG_RETURN(
                STRING,
                strNewFmt(
                    "vendor mismatch: backup taken on vendor %u, restore mysqld is vendor %u",
                    (unsigned int)backupVendor, (unsigned int)probe->vendor));
        }
    }

    // Downgrade across the dictionary boundary is unsafe — 8.0+ has the InnoDB DD in mysql.ibd which 5.7 cannot read.
    if (backupVersionNum >= MYSQL_VERSION_DICTIONARY_BOUNDARY && probe->versionNum < MYSQL_VERSION_DICTIONARY_BOUNDARY)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "downgrade across data dictionary boundary unsupported: backup is %u, restore mysqld is %u",
                backupVersionNum, probe->versionNum));
    }

    // Major-version downgrade in any direction is generally unsafe but only warn — the operator may know what they're doing
    const unsigned int probeMajor = probe->versionNum / 10000;
    const unsigned int backupMajor = backupVersionNum / 10000;

    if (probeMajor != backupMajor && backupMajor > 0)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "major-version mismatch: backup is %u, restore mysqld is %u (cross-version restore may need additional"
                " --skip-grant-tables / mysql_upgrade steps)", backupVersionNum, probe->versionNum));
    }

    FUNCTION_LOG_RETURN(STRING, NULL);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlBinaryInfoToLog(const MysqlBinaryInfo *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog, "{vendor: %u, versionNum: %u, raw: %s}",
        (unsigned int)this->vendor, this->versionNum,
        this->versionRaw != NULL ? strZ(this->versionRaw) : "(null)");
}

/***********************************************************************************************************************************
Map a (vendor, versionNum) to the maximum LOG_HEADER_FORMAT value that vendor's binary at that version can replay. Per
mysql-server/storage/innobase/include/log0types.h Log_format::CURRENT and equivalent in MariaDB.

  MySQL/Percona < 5.7.9         → 0  (LEGACY)
  MySQL/Percona 5.7.9 → 8.0.0   → 1
  MySQL/Percona 8.0.1 → 8.0.2   → 2
  MySQL/Percona 8.0.3 → 8.0.18  → 3
  MySQL/Percona 8.0.19 → 8.0.27 → 4
  MySQL/Percona 8.0.28 → 8.0.29 → 5
  MySQL/Percona 8.0.30+         → 6

  MariaDB < 10.5                → 104     (FORMAT_10_4 max; older formats: 1, 103, 104)
  MariaDB 10.5 → 10.7           → "PHYS"  (0x50485953)
  MariaDB 10.8+                 → "Phys"  (0x50687973)

We don't validate at-format-equality (a 10.5 binary can read 10.4) — only that the backup format isn't NEWER than the binary's
max. The mapping returns the binary's max-supported value for the comparison.
***********************************************************************************************************************************/
static uint32_t
binaryMaxRedoFormat(const MysqlVendor vendor, const unsigned int versionNum)
{
    if (vendor == mysqlVendorMysql || vendor == mysqlVendorPercona)
    {
        if (versionNum >= 80030) return MYSQL_REDO_FORMAT_8_0_30;
        if (versionNum >= 80028) return MYSQL_REDO_FORMAT_8_0_28;
        if (versionNum >= 80019) return MYSQL_REDO_FORMAT_8_0_19;
        if (versionNum >= 80003) return MYSQL_REDO_FORMAT_8_0_3;
        if (versionNum >= 80001) return MYSQL_REDO_FORMAT_8_0_1;
        if (versionNum >= 50709) return MYSQL_REDO_FORMAT_5_7_9;
        return MYSQL_REDO_FORMAT_LEGACY;
    }

    if (vendor == mysqlVendorMariadb)
    {
        if (versionNum >= 100800) return MARIADB_REDO_FORMAT_10_8;
        if (versionNum >= 100500) return MARIADB_REDO_FORMAT_10_5;
        if (versionNum >= 100400) return MARIADB_REDO_FORMAT_10_4;
        if (versionNum >= 100300) return MARIADB_REDO_FORMAT_10_3;
        return MARIADB_REDO_FORMAT_10_2;
    }

    return 0;                                                           // Unknown vendor — can't validate
}

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlBinaryCheckRedoCompat(const MysqlBinaryInfo *const probe, const uint32_t backupRedoFormat)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_BINARY_INFO, probe);
        FUNCTION_LOG_PARAM(UINT, backupRedoFormat);
    FUNCTION_LOG_END();

    ASSERT(probe != NULL);

    // backupRedoFormat=0 means the manifest didn't record one (older mybackrest output) — can't validate, accept silently
    if (backupRedoFormat == 0)
        FUNCTION_LOG_RETURN(STRING, NULL);

    // Unknown vendor → can't validate. Note we can't use binaryMax==0 as the "unknown" signal because format 0 (LEGACY) is a
    // VALID supported format for very old MySQL — so test the vendor directly.
    if (probe->vendor == mysqlVendorUnknown)
        FUNCTION_LOG_RETURN(STRING, NULL);

    const uint32_t binaryMax = binaryMaxRedoFormat(probe->vendor, probe->versionNum);

    // MySQL formats are sequential integers (0..6); MariaDB formats are unique magic numbers (1, 103, 104, 0x50485953,
    // 0x50687973). For MariaDB the comparison is "is backupFormat in the SET of formats the binary supports?" not "is it
    // numerically <=". Since the binary's max is the LATEST format it supports and it can read all earlier ones, we order
    // them by introduction date.
    static const uint32_t mariadbFormatOrder[] = {
        MARIADB_REDO_FORMAT_10_2, MARIADB_REDO_FORMAT_10_3, MARIADB_REDO_FORMAT_10_4,
        MARIADB_REDO_FORMAT_10_5, MARIADB_REDO_FORMAT_10_8,
    };

    if (probe->vendor == mysqlVendorMariadb)
    {
        // Find positions of backupRedoFormat and binaryMax in the order array; backup must be <= binary
        int backupPos = -1, binaryPos = -1;
        for (size_t i = 0; i < sizeof(mariadbFormatOrder) / sizeof(mariadbFormatOrder[0]); i++)
        {
            if (mariadbFormatOrder[i] == backupRedoFormat) backupPos = (int)i;
            if (mariadbFormatOrder[i] == binaryMax) binaryPos = (int)i;
        }

        if (backupPos < 0)
            FUNCTION_LOG_RETURN(STRING, strNewFmt("backup redo_format_num 0x%08x is not a recognized MariaDB format", backupRedoFormat));

        if (binaryPos >= 0 && backupPos > binaryPos)
        {
            FUNCTION_LOG_RETURN(
                STRING,
                strNewFmt(
                    "backup uses MariaDB redo format 0x%08x but the binary at version %u only supports up to 0x%08x —"
                    " the binary cannot replay this backup's redo log",
                    backupRedoFormat, probe->versionNum, binaryMax));
        }

        FUNCTION_LOG_RETURN(STRING, NULL);
    }

    // MySQL/Percona: numerical comparison
    if (backupRedoFormat > binaryMax)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "backup uses MySQL redo format %u but the binary at version %u only supports up to format %u —"
                " upgrade the binary to at least the version that introduced format %u (or take a fresh backup on"
                " a clean shutdown so the redo log is empty)",
                backupRedoFormat, probe->versionNum, binaryMax, backupRedoFormat));
    }

    FUNCTION_LOG_RETURN(STRING, NULL);
}

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlBinaryCheckEngineCompat(
    const MysqlBinaryInfo *const probe, const bool hasAria, const bool hasIsam, const bool hasTokudb, const bool hasMyrocks)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_BINARY_INFO, probe);
        FUNCTION_LOG_PARAM(BOOL, hasAria);
        FUNCTION_LOG_PARAM(BOOL, hasIsam);
        FUNCTION_LOG_PARAM(BOOL, hasTokudb);
        FUNCTION_LOG_PARAM(BOOL, hasMyrocks);
    FUNCTION_LOG_END();

    ASSERT(probe != NULL);

    // Aria is exclusively a MariaDB engine. Trying to load .MAD/.MAI files on MySQL or Percona will fail at startup.
    if (hasAria && probe->vendor != mysqlVendorMariadb)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "backup contains Aria-engine tables (.MAD/.MAI files) but restore binary is vendor %u, not MariaDB —"
                " Aria can only be loaded by mariadbd",
                (unsigned int)probe->vendor));
    }

    // ISAM was removed from MySQL in 4.0.3 (March 2003). A modern binary refuses to recognize .ISD/.ISM files. The user
    // would need to either restore on a museum binary (4.0.2 or earlier) or run a migration through a 4.x → MyISAM ALTER
    // chain on the source side before backing up.
    if (hasIsam && probe->versionNum >= 40003)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "backup contains ISAM tables (.ISD/.ISM files) but restore binary version %u is at or after the 4.0.3"
                " ISAM removal — restore on MySQL 4.0.2 or earlier, or migrate the source through ALTER TABLE … ENGINE=MyISAM"
                " before re-backing up",
                probe->versionNum));
    }

    // MyRocks introduced in MySQL 5.7 (Facebook → MariaDB → Percona). On older targets the .rocksdb subdir restoration
    // succeeds but mysqld's MyRocks plugin won't load.
    if (hasMyrocks && probe->versionNum < 50700)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "backup contains MyRocks/RocksDB data but restore binary version %u is pre-5.7 — the MyRocks plugin"
                " requires MySQL/Percona/MariaDB 5.7+",
                probe->versionNum));
    }

    // TokuDB: deprecated by Percona in 8.0; only available via the xelabs fork or older Percona Server 5.6/5.7. Issue a
    // warning rather than throwing — the operator may have a compatible binary even if we can't auto-detect it.
    if (hasTokudb && probe->vendor != mysqlVendorPercona)
    {
        FUNCTION_LOG_RETURN(
            STRING,
            strNewFmt(
                "backup contains TokuDB tables (.tokudb files) but restore binary is vendor %u — TokuDB is typically"
                " only available on Percona Server 5.6/5.7 or the xelabs tokudb-xtrabackup fork",
                (unsigned int)probe->vendor));
    }

    FUNCTION_LOG_RETURN(STRING, NULL);
}
