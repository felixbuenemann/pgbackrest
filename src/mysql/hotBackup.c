/***********************************************************************************************************************************
Hot-Mode (Online) Backup Orchestrator
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/redoLog.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/type/pack.h"
#include "common/type/string.h"
#include "mysql/client.h"
#include "mysql/datadir.h"
#include "mysql/engine/engine.h"
#include "mysql/hotBackup.h"
#include "mysql/interface.h"
#include "mysql/lock.h"
#include "mysql/manifest.h"
#include "mysql/sanity.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Engine dispatch table — same shape as coldBackup's, kept independent so future per-mode tweaks (e.g. hot mode skipping the InnoDB
engine handler's flat copy in favor of a redo-coordinated copier) don't have to fork the cold module.
***********************************************************************************************************************************/
typedef struct HotBackupEngineRow
{
    bool (*present)(const MysqlDataDirInfo *info);
    const char *name;
    const char *logLabel;
} HotBackupEngineRow;

static bool hbHasInnodb(const MysqlDataDirInfo *const i) { return i->hasInnodb; }
static bool hbHasMyisam(const MysqlDataDirInfo *const i) { return i->hasMyisam; }
static bool hbHasIsam(const MysqlDataDirInfo *const i)   { return i->hasIsam; }
static bool hbHasAria(const MysqlDataDirInfo *const i)   { return i->hasAria; }
static bool hbHasMyrocks(const MysqlDataDirInfo *const i){ return i->hasMyrocks; }
static bool hbHasTokudb(const MysqlDataDirInfo *const i) { return i->hasTokudb; }

static const HotBackupEngineRow hotBackupEngines[] = {
    {hbHasInnodb,  "innodb",  "InnoDB"},
    {hbHasMyisam,  "myisam",  "MyISAM"},
    {hbHasIsam,    "isam",    "ISAM"},
    {hbHasAria,    "aria",    "Aria"},
    {hbHasMyrocks, "rocksdb", "RocksDB/MyRocks"},
    {hbHasTokudb,  "tokudb",  "TokuDB"},
};

/***********************************************************************************************************************************
Capture the result of SHOW MASTER STATUS into a MysqlBackupBinlog struct. SHOW MASTER STATUS columns vary slightly by vendor:

  MySQL 5.7+:   File | Position | Binlog_Do_DB | Binlog_Ignore_DB | Executed_Gtid_Set
  MariaDB:      File | Position | Binlog_Do_DB | Binlog_Ignore_DB                       (no Executed_Gtid_Set)

We parse File + Position (always present) and Executed_Gtid_Set if it's there. For MariaDB the GTID comes from
@@gtid_binlog_pos instead (queried separately).

If SHOW MASTER STATUS returns no rows (server has log_bin OFF) the caller already failed the sanity check; this function
should still tolerate an empty result and produce a binlog struct with NULL fields.

Sets startOrStop slot on the supplied binlog — both passes share the same parser.
***********************************************************************************************************************************/
typedef enum
{
    hotBackupCaptureStart,
    hotBackupCaptureStop,
} HotBackupCaptureSlot;

static void
hotBackupCaptureBinlog(MysqlClient *const client, MysqlBackupBinlog *const binlog, const HotBackupCaptureSlot slot)
{
    ASSERT(client != NULL);
    ASSERT(binlog != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // SHOW MASTER STATUS returns AT MOST one row. We tolerate zero rows by using mysqlClientQueryResultAny instead of Row
        // so the caller doesn't throw — log_bin=OFF servers return no row, and that case is already caught by sanity check.
        Pack *const pack = mysqlClientQuery(client, STRDEF("SHOW MASTER STATUS"), mysqlClientQueryResultAny);

        if (pack != NULL)
        {
            PackRead *const read = pckReadNew(pack);

            // QueryResultAny wraps each row in an array. We read one (or zero).
            if (pckReadNext(read))
            {
                pckReadArrayBeginP(read);

                String *const file = pckReadStrP(read);
                const int64_t pos = pckReadI64P(read);
                pckReadStrP(read);                                      // Binlog_Do_DB — ignored
                pckReadStrP(read);                                      // Binlog_Ignore_DB — ignored
                String *const gtidSet = pckReadStrP(read);              // NULL on MariaDB (column absent)

                pckReadArrayEndP(read);

                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    if (slot == hotBackupCaptureStart)
                    {
                        binlog->startFile = file != NULL ? strDup(file) : NULL;
                        binlog->startPos = (uint64_t)pos;
                        binlog->startGtid = gtidSet != NULL && strSize(gtidSet) > 0 ? strDup(gtidSet) : NULL;
                    }
                    else
                    {
                        binlog->stopFile = file != NULL ? strDup(file) : NULL;
                        binlog->stopPos = (uint64_t)pos;
                        binlog->stopGtid = gtidSet != NULL && strSize(gtidSet) > 0 ? strDup(gtidSet) : NULL;
                    }
                }
                MEM_CONTEXT_PRIOR_END();
            }
        }

        // For MariaDB, the GTID isn't in SHOW MASTER STATUS — pull it from @@gtid_binlog_pos. The query succeeds with empty
        // string when GTID isn't enabled; we keep whatever value we already got from SHOW MASTER STATUS in that case.
        if (mysqlClientVendor(client) == mysqlVendorMariadb)
        {
            Pack *const gtidPack = mysqlClientQuery(
                client, STRDEF("SELECT @@gtid_binlog_pos"), mysqlClientQueryResultColumn);

            if (gtidPack != NULL)
            {
                PackRead *const gtidRead = pckReadNew(gtidPack);
                String *const gtid = pckReadStrP(gtidRead);

                if (gtid != NULL && strSize(gtid) > 0)
                {
                    MEM_CONTEXT_PRIOR_BEGIN()
                    {
                        if (slot == hotBackupCaptureStart)
                            binlog->startGtid = strDup(gtid);
                        else
                            binlog->stopGtid = strDup(gtid);
                    }
                    MEM_CONTEXT_PRIOR_END();
                }
            }
        }
    }
    MEM_CONTEXT_TEMP_END();
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlHotBackupResult *
mysqlHotBackup(
    MysqlClient *const client,
    const Storage *const srcStorage, const String *const dataPath,
    const Storage *const dstStorage, const String *const backupPath,
    const MysqlLockMethod lockPreference)
{
    FUNCTION_LOG_BEGIN(logLevelInfo);
        FUNCTION_LOG_PARAM(MY_CLIENT, client);
        FUNCTION_LOG_PARAM(STORAGE, srcStorage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
        FUNCTION_LOG_PARAM(STORAGE, dstStorage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
        FUNCTION_LOG_PARAM(STRING_ID, lockPreference);
    FUNCTION_LOG_END();

    ASSERT(client != NULL);
    ASSERT(srcStorage != NULL);
    ASSERT(dataPath != NULL);
    ASSERT(dstStorage != NULL);
    ASSERT(backupPath != NULL);

    MysqlHotBackupResult *result = NULL;
    MysqlLockMethod lockMethod = mysqlLockMethodAuto;
    bool lockAcquired = false;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlHotBackupResult));
            *result = (MysqlHotBackupResult){0};
            result->binlog = memNew(sizeof(MysqlBackupBinlog));
            *result->binlog = (MysqlBackupBinlog){0};
        }
        MEM_CONTEXT_PRIOR_END();

        // Step 1: sanity check the server. Throws AssertError on missing log_bin / binlog_format / gtid_mode — refusing to
        // back up a misconfigured server is the right default; an --allow-unsafe-config flag would relax this in the future.
        LOG_INFO("hot backup: running server sanity check");
        const MysqlSanityResult sanity = mysqlServerSanityCheck(client);

        if (sanity.errorCount > 0)
        {
            THROW_FMT(
                ConfigError,
                "hot backup: server sanity check failed (%u issue(s)) — log_bin=%s, binlog_format=%s,"
                " gtid_enabled=%s, server_id_set=%s; refusing to back up a misconfigured server",
                sanity.errorCount,
                sanity.logBin ? "on" : "off",
                sanity.binlogFormat != NULL ? strZ(sanity.binlogFormat) : "(missing)",
                sanity.gtidEnabled ? "yes" : "no",
                sanity.serverIdSet ? "yes" : "no");
        }

        // Step 2: inspect the live datadir
        LOG_INFO_FMT("hot backup: inspecting datadir at %s", strZ(dataPath));
        MysqlDataDirInfo *const info = mysqlDataDirInspect(srcStorage, dataPath);

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result->info = info;
        }
        MEM_CONTEXT_PRIOR_END();

        // Step 3: select lock method
        lockMethod = mysqlLockMethodSelect(client, lockPreference);
        result->lockMethodUsed = lockMethod;
        LOG_INFO_FMT("hot backup: lock method = %s", mysqlLockMethodName(lockMethod));

        // Resolve absolute paths for engine handlers (they create their own POSIX storages internally)
        const String *const absoluteDataPath = storagePathP(srcStorage, dataPath);
        const String *const absoluteBackupPath = storagePathP(dstStorage, backupPath);

        EngineBackupCtx ctx = {
            .client = client,
            .dataPath = absoluteDataPath,
            .backupPath = absoluteBackupPath,
            .processMax = 1,
        };

        // Wrap the lock/copy section in a TRY so any failure goes through best-effort UNLOCK before propagating
        TRY_BEGIN()
        {
            // Step 4: begin + block-DDL transitions. After these calls, DDL is quiesced and existing transactions can still
            // commit (instance method) or are blocked (stage with BLOCK_DDL). New writes against InnoDB still happen but
            // are durable to the redo log.
            LOG_INFO("hot backup: acquiring lock");
            mysqlLockBegin(client, lockMethod);
            mysqlLockBlockDdl(client, lockMethod);
            lockAcquired = true;

            // Step 5: capture START binlog position. We do this AFTER block-ddl so we have a consistent snapshot of the
            // server state at the moment the lock took effect.
            LOG_INFO("hot backup: capturing start binlog position");
            hotBackupCaptureBinlog(client, result->binlog, hotBackupCaptureStart);

            // Step 6: engine handler dispatch. Same convention as cold mode: run every non-NULL callback in order.
            for (size_t i = 0; i < sizeof(hotBackupEngines) / sizeof(hotBackupEngines[0]); i++)
            {
                const HotBackupEngineRow *const row = &hotBackupEngines[i];

                if (!row->present(info))
                    continue;

                const EngineHandler *const handler = engineHandlerLookup(STR(row->name));

                if (handler == NULL)
                {
                    LOG_WARN_FMT("hot backup: %s detected but no handler registered — skipping", row->logLabel);
                    continue;
                }

                LOG_INFO_FMT("hot backup: dispatching %s handler", row->logLabel);

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

            // Step 7: redo log files (cold copy is safe since writes are quiesced by the lock)
            if (info->hasInnodb)
                result->redoFilesCopied = redoLogColdCopy(srcStorage, dataPath, dstStorage, backupPath);

            // Step 8: auto.cnf
            const String *const srcAutoCnf = strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_AUTOCNF);
            if (storageExistsP(srcStorage, srcAutoCnf))
            {
                const String *const dstAutoCnf = strNewFmt("%s/%s", strZ(backupPath), MYSQL_FILE_AUTOCNF);
                storageCopyP(storageNewReadP(srcStorage, srcAutoCnf), storageNewWriteP(dstStorage, dstAutoCnf));
                result->autoCnfCopied = true;
            }

            // Step 9: block-commit transition. After this no new transactions commit; the binlog stops advancing.
            LOG_INFO("hot backup: blocking commits");
            mysqlLockBlockCommit(client, lockMethod);

            // Step 10: capture STOP binlog position. Should be == start position in practice (no commits happened during the
            // copy because BLOCK_DDL prevented new ones from committing) but we capture it anyway for the manifest.
            LOG_INFO("hot backup: capturing stop binlog position");
            hotBackupCaptureBinlog(client, result->binlog, hotBackupCaptureStop);
        }
        CATCH_ANY()
        {
            // Best-effort lock release, then re-throw the original error. mysqlLockRelease swallows its own errors.
            if (lockAcquired)
            {
                LOG_WARN_FMT("hot backup: error during copy — releasing lock: %s", errorMessage());
                mysqlLockRelease(client, lockMethod);
                lockAcquired = false;
            }
            RETHROW();
        }
        TRY_END();

        // Step 11: clean release of the lock
        LOG_INFO("hot backup: releasing lock");
        mysqlLockRelease(client, lockMethod);
        lockAcquired = false;

        // Step 12: write the manifest with the binlog block populated. If neither start nor stop file was captured (log_bin
        // OFF would already have failed sanity, so this is defensive), pass NULL so the [binlog] section is omitted.
        const bool haveBinlog = result->binlog->startFile != NULL || result->binlog->stopFile != NULL;

        mysqlBackupManifestWrite(dstStorage, backupPath, info, haveBinlog ? result->binlog : NULL);
        result->manifestWritten = true;

        LOG_INFO_FMT(
            "hot backup complete: lock=%s, %u engine(s), %u redo file(s), auto.cnf=%s, binlog=[%s..%s], manifest written",
            mysqlLockMethodName(lockMethod), result->enginesProcessed, result->redoFilesCopied,
            result->autoCnfCopied ? "yes" : "absent",
            result->binlog->startFile != NULL ? strZ(result->binlog->startFile) : "(none)",
            result->binlog->stopFile != NULL ? strZ(result->binlog->stopFile) : "(none)");
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_P(VOID, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlHotBackupResultFree(MysqlHotBackupResult *const this)
{
    // Symmetric with coldBackup — result + nested structs live in the caller's mem context. No-op kept for API symmetry.
    (void)this;
}
