/***********************************************************************************************************************************
MySQL / MariaDB Binary Probe

Algorithm:
  1. pipe() + fork()
  2. Child: dup the pipe write end to stdout, exec `<path> --version`
  3. Parent: read up to 4096 bytes from the read end (more than enough for a single --version line, which never exceeds ~300 chars)
  4. waitpid the child
  5. Parse the captured line for "Ver X.Y.Z" + vendor markers
  6. Return MysqlBinaryInfo
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

        // Materialize result in the parent context
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlBinaryInfo));
            *result = (MysqlBinaryInfo)
            {
                .vendor = vendor,
                .versionNum = versionNum,
                .versionRaw = strNewZ(verToken + 5),
                .fullOutput = strNewZ(buf),
            };

            // versionRaw includes everything after "Ver " up to end-of-line — trim trailing space/tab
            String *const trimmed = strTrim(result->versionRaw);
            if (trimmed != result->versionRaw)
            {
                strFree(result->versionRaw);
                result->versionRaw = trimmed;
            }
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
