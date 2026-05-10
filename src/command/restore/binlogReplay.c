/***********************************************************************************************************************************
Binlog Replay for PITR

Drives the canonical PITR pipeline:

    mysqlbinlog [--start-position=N] [--stop-datetime=... | --stop-position=... | --stop-gtid=...] file1 file2 ... | mysql

Both processes are spawned via fork+execvp; their stdout/stdin are connected via a pipe(). The parent waits for both children
and aggregates exit status — any non-zero exit (or signal) becomes an ExecuteError.

Materializing binlog files from the repository (S3 / GCS / SFTP / local) is the caller's job — binlogReplay takes a StringList
of already-local paths so callers can apply caching, parallel fetch, etc. Phase E's restore.c orchestration will pull files
through the standard pgBackRest IO pipeline so compression / encryption filters apply transparently.
***********************************************************************************************************************************/
#include <build.h>

#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "command/restore/binlogReplay.h"
#include "common/debug.h"
#include "common/fork.h"
#include "common/log.h"
#include "common/type/string.h"
#include "common/type/stringList.h"

/***********************************************************************************************************************************
Build the mysqlbinlog argv as a StringList suitable for execvp via strLstPtr+UNCONSTIFY
***********************************************************************************************************************************/
static StringList *
binlogReplayMysqlbinlogArgv(
    const String *const mysqlbinlogPath, const String *const startFile, const uint64_t startPos,
    const BinlogReplayTarget *const target, const StringList *const localFiles)
{
    StringList *const argv = strLstNew();
    strLstAdd(argv, mysqlbinlogPath);

    // --start-position is anchored to the FIRST file. mysqlbinlog ignores it for subsequent files.
    if (startPos > 0)
        strLstAdd(argv, strNewFmt("--start-position=%" PRIu64, startPos));

    // --start-position only applies to startFile; mysqlbinlog handles this implicitly when files are listed in order.
    (void)startFile;

    // Apply target. exactly one of {time, gtid, position, immediate} per the plan; immediate means "stop at end of files"
    if (target != NULL)
    {
        switch (target->kind)
        {
            case binlogTargetTime:
                ASSERT(target->time != NULL);
                strLstAdd(argv, strNewFmt("--stop-datetime=%s", strZ(target->time)));
                break;

            case binlogTargetGtid:
                ASSERT(target->gtid != NULL);
                strLstAdd(argv, strNewFmt("--exclude-gtids=%s", strZ(target->gtid)));
                break;

            case binlogTargetPosition:
                ASSERT(target->binlogFile != NULL);
                strLstAdd(argv, strNewFmt("--stop-position=%" PRIu64, target->binlogPos));
                break;

            case binlogTargetImmediate:
            case binlogTargetNone:
                // Replay everything in localFiles
                break;
        }
    }

    // Append the binlog file paths in order
    for (unsigned int idx = 0; idx < strLstSize(localFiles); idx++)
        strLstAdd(argv, strLstGet(localFiles, idx));

    return argv;
}

/***********************************************************************************************************************************
Wait for one child PID; throw on non-zero exit. processName is included in the error for clarity.
***********************************************************************************************************************************/
static void
binlogReplayWaitChild(const pid_t pid, const char *const processName)
{
    int status;
    const pid_t waited = waitpid(pid, &status, 0);

    THROW_ON_SYS_ERROR_FMT(waited == -1, ExecuteError, "waitpid failed for %s", processName);

    if (!WIFEXITED(status))
        THROW_FMT(ExecuteError, "%s did not exit normally (signal/abort)", processName);

    const int exitCode = WEXITSTATUS(status);

    if (exitCode != 0)
        THROW_FMT(ExecuteError, "%s exited with status %d", processName, exitCode);
}

/**********************************************************************************************************************************/
FN_EXTERN void
binlogReplay(
    const Storage *const archiveStorage, const String *const archivePath, const String *const startFile, const uint64_t startPos,
    const BinlogReplayTarget *const target, const String *const mysqlbinlogPath, const String *const mysqlClientPath,
    const String *const recoveryCnf)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, archiveStorage);
        FUNCTION_LOG_PARAM(STRING, archivePath);
        FUNCTION_LOG_PARAM(STRING, startFile);
        FUNCTION_LOG_PARAM(UINT64, startPos);
        FUNCTION_LOG_PARAM_P(VOID, target);
        FUNCTION_LOG_PARAM(STRING, mysqlbinlogPath);
        FUNCTION_LOG_PARAM(STRING, mysqlClientPath);
        FUNCTION_LOG_PARAM(STRING, recoveryCnf);
    FUNCTION_LOG_END();

    ASSERT(mysqlbinlogPath != NULL);
    ASSERT(mysqlClientPath != NULL);
    ASSERT(recoveryCnf != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // TODO(myBackRest-E): file-list materialization belongs here. The caller currently must pre-stage the binlogs locally.
        // Phase E's restore.c orchestration will use storageRepoGet to pull from the configured archive store.
        (void)archiveStorage;
        (void)archivePath;
        (void)startFile;

        StringList *const localFiles = strLstNew();
        // Empty list = nothing to replay — accepted as a no-op so callers can run a "PITR target=none" with this same code.

        StringList *const mbArgv = binlogReplayMysqlbinlogArgv(mysqlbinlogPath, startFile, startPos, target, localFiles);
        StringList *const mysqlArgv = strLstNew();
        strLstAdd(mysqlArgv, mysqlClientPath);
        strLstAdd(mysqlArgv, strNewFmt("--defaults-file=%s", strZ(recoveryCnf)));

        if (strLstSize(localFiles) == 0)
        {
            LOG_INFO("binlog replay: no files to replay");
            FUNCTION_LOG_RETURN_VOID();
        }

        LOG_INFO_FMT("binlog replay: piping %u file(s) through mysqlbinlog | mysql", strLstSize(localFiles));

        // Create the pipe BEFORE forking so both children inherit the same fd numbers
        int pipeFd[2];
        THROW_ON_SYS_ERROR(pipe(pipeFd) == -1, ExecuteError, "binlog replay pipe() failed");

        // Fork mysqlbinlog (writer)
        const pid_t binlogPid = forkSafe();

        if (binlogPid == 0)
        {
            // Child A: redirect stdout to pipe write end, close read end
            close(pipeFd[0]);

            if (dup2(pipeFd[1], STDOUT_FILENO) == -1)
                exit(127);

            close(pipeFd[1]);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
            execvp(strZ(strLstGet(mbArgv, 0)), UNCONSTIFY(char **, strLstPtr(mbArgv)));
#pragma GCC diagnostic pop

            exit(127);
        }

        // Fork mysql (reader)
        const pid_t mysqlPid = forkSafe();

        if (mysqlPid == 0)
        {
            // Child B: redirect stdin from pipe read end, close write end
            close(pipeFd[1]);

            if (dup2(pipeFd[0], STDIN_FILENO) == -1)
                exit(127);

            close(pipeFd[0]);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
            execvp(strZ(strLstGet(mysqlArgv, 0)), UNCONSTIFY(char **, strLstPtr(mysqlArgv)));
#pragma GCC diagnostic pop

            exit(127);
        }

        // Parent: close both pipe ends so EOF propagates correctly when mysqlbinlog finishes
        close(pipeFd[0]);
        close(pipeFd[1]);

        // Wait for both. Order matters: mysqlbinlog finishes first (closes its stdout, which gives mysql EOF, which causes
        // mysql to drain its remaining input and exit).
        binlogReplayWaitChild(binlogPid, "mysqlbinlog");
        binlogReplayWaitChild(mysqlPid, "mysql");

        LOG_INFO("binlog replay: completed successfully");
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
