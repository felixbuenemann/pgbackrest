/***********************************************************************************************************************************
Cold-Mode (Offline) Backup Orchestrator
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/redoLog.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/type/string.h"
#include "mysql/coldBackup.h"
#include "mysql/datadir.h"
#include "mysql/engine/engine.h"
#include "mysql/interface.h"
#include "mysql/manifest.h"
#include "mysql/miscFiles.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Engine dispatch table: maps a presence-flag accessor to the engine-name string that engineHandlerLookup expects. The table-driven
form lets us add a row when a new engine ships without growing the orchestrator body.
***********************************************************************************************************************************/
typedef struct ColdBackupEngineRow
{
    bool (*present)(const MysqlDataDirInfo *info);
    const char *name;                                                   // Lookup key for engineHandlerLookup
    const char *logLabel;                                               // Human-readable for log messages
} ColdBackupEngineRow;

static bool hasInnodb(const MysqlDataDirInfo *const i) { return i->hasInnodb; }
static bool hasMyisam(const MysqlDataDirInfo *const i) { return i->hasMyisam; }
static bool hasIsam(const MysqlDataDirInfo *const i)   { return i->hasIsam; }
static bool hasAria(const MysqlDataDirInfo *const i)   { return i->hasAria; }
static bool hasMyrocks(const MysqlDataDirInfo *const i){ return i->hasMyrocks; }
static bool hasTokudb(const MysqlDataDirInfo *const i) { return i->hasTokudb; }
static bool hasCsv(const MysqlDataDirInfo *const i)    { return i->hasCsv; }
static bool hasArchive(const MysqlDataDirInfo *const i){ return i->hasArchive; }
static bool hasMerge(const MysqlDataDirInfo *const i)  { return i->hasMerge; }
static bool hasConnect(const MysqlDataDirInfo *const i){ return i->hasConnect; }
static bool hasMroonga(const MysqlDataDirInfo *const i){ return i->hasMroonga; }

static const ColdBackupEngineRow coldBackupEngines[] = {
    {hasInnodb,  "innodb",     "InnoDB"},
    {hasMyisam,  "myisam",     "MyISAM"},
    {hasIsam,    "isam",       "ISAM"},
    {hasAria,    "aria",       "Aria"},
    {hasMyrocks, "rocksdb",    "RocksDB/MyRocks"},
    {hasTokudb,  "tokudb",     "TokuDB"},
    {hasCsv,     "csv",        "CSV"},
    {hasArchive, "archive",    "Archive"},
    {hasMerge,   "mrg_myisam", "MERGE"},
    {hasConnect, "connect",    "Connect"},
    {hasMroonga, "mroonga",    "Mroonga"},
};

/**********************************************************************************************************************************/
FN_EXTERN MysqlColdBackupResult *
mysqlColdBackup(
    const Storage *const srcStorage, const String *const dataPath,
    const Storage *const dstStorage, const String *const backupPath)
{
    FUNCTION_LOG_BEGIN(logLevelInfo);
        FUNCTION_LOG_PARAM(STORAGE, srcStorage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
        FUNCTION_LOG_PARAM(STORAGE, dstStorage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
    FUNCTION_LOG_END();

    ASSERT(srcStorage != NULL);
    ASSERT(dataPath != NULL);
    ASSERT(dstStorage != NULL);
    ASSERT(backupPath != NULL);

    MysqlColdBackupResult *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlColdBackupResult));
            *result = (MysqlColdBackupResult){0};
        }
        MEM_CONTEXT_PRIOR_END();

        // Step 1: inspect the datadir. Result lives in the parent context so it survives the TEMP_END.
        LOG_INFO_FMT("cold backup: inspecting datadir at %s", strZ(dataPath));
        MysqlDataDirInfo *const info = mysqlDataDirInspect(srcStorage, dataPath);

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result->info = info;
        }
        MEM_CONTEXT_PRIOR_END();

        // Step 2: dispatch to every present engine handler. The cold-safety invariant (server shutdown) means each handler's
        // copyOnline callback is safe to call without a live MysqlClient — the existing handlers don't reference ctx->client.
        //
        // Engine handlers create their own POSIX Storage rooted at ctx->dataPath / ctx->backupPath, which require absolute
        // paths. Resolve the caller's possibly-relative paths against the supplied storages now so handlers don't need to
        // know about the storage abstraction.
        const String *const absoluteDataPath = storagePathP(srcStorage, dataPath);
        const String *const absoluteBackupPath = storagePathP(dstStorage, backupPath);

        EngineBackupCtx ctx = {
            .client = NULL,
            .dataPath = absoluteDataPath,
            .backupPath = absoluteBackupPath,
            .processMax = 1,
            .innodbPageSize = info->pageSize,
            .innodbPageChecksum = info->pageChecksum,
        };

        for (size_t i = 0; i < sizeof(coldBackupEngines) / sizeof(coldBackupEngines[0]); i++)
        {
            const ColdBackupEngineRow *const row = &coldBackupEngines[i];

            if (!row->present(info))
                continue;

            const EngineHandler *const handler = engineHandlerLookup(STR(row->name));

            if (handler == NULL)
            {
                LOG_WARN_FMT("cold backup: %s detected but no handler registered — skipping", row->logLabel);
                continue;
            }

            // Cold mode has no live lock-state machine: with the server shut down there's nothing to race against. Run every
            // non-NULL callback in order. Each engine module decides which of copyOnline / copyUnderLock holds its work and we
            // simply call both when present (engines that split the work between phases — e.g. Aria for online-vs-offline
            // tables — will get the same outcome as one consolidated copy because there's no concurrent writer).
            LOG_INFO_FMT("cold backup: dispatching %s handler", row->logLabel);

            if (handler->prepare != NULL)
                handler->prepare(&ctx);
            if (handler->copyOnline != NULL)
                handler->copyOnline(&ctx);
            if (handler->copyUnderLock != NULL)
                handler->copyUnderLock(&ctx);
            if (handler->finalize != NULL)
                handler->finalize(&ctx);

            result->enginesProcessed++;
        }

        // Step 3: redo log files (cold copy)
        if (info->hasInnodb)
            result->redoFilesCopied = redoLogColdCopy(srcStorage, dataPath, dstStorage, backupPath);

        // Step 4: auto.cnf — copy last (in xtrabackup convention this is what tells the restore the original server-uuid)
        const String *const autoCnfRel = STRDEF(MYSQL_FILE_AUTOCNF);
        const String *const srcAutoCnf = strNewFmt("%s/%s", strZ(dataPath), strZ(autoCnfRel));

        if (storageExistsP(srcStorage, srcAutoCnf))
        {
            const String *const dstAutoCnf = strNewFmt("%s/%s", strZ(backupPath), strZ(autoCnfRel));
            storageCopyP(storageNewReadP(srcStorage, srcAutoCnf), storageNewWriteP(dstStorage, dstAutoCnf));
            result->autoCnfCopied = true;
        }

        // Step 4.5: miscellaneous files (.sdi, .CSV, .CSM, .opt, etc.) + empty schema directories. Without this, mysqld won't
        // start against the restored datadir because Data Dictionary initialization expects mysql/ and performance_schema/
        // to be on disk.
        mysqlMiscFilesCopy(srcStorage, dataPath, dstStorage, backupPath);

        // Step 5: render and write the manifest. No binlog block in cold mode — that's an online concept.
        mysqlBackupManifestWrite(dstStorage, backupPath, info, /*binlog*/ NULL);
        result->manifestWritten = true;

        LOG_INFO_FMT(
            "cold backup complete: %u engine(s), %u redo file(s), auto.cnf=%s, manifest written",
            result->enginesProcessed, result->redoFilesCopied, result->autoCnfCopied ? "yes" : "absent");
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_P(VOID, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlColdBackupResultFree(MysqlColdBackupResult *const this)
{
    // Both the result struct and the info pointer were allocated in the caller's mem context via MEM_CONTEXT_PRIOR_BEGIN. The
    // context owns them — letting it free them when it ends is the idiomatic pgBackRest pattern. This function exists for
    // symmetric API shape; callers can let their parent context handle cleanup instead.
    (void)this;
}
