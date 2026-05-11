/***********************************************************************************************************************************
InnoDB / XtraDB Engine Module

Backs up ibdata*, mysql.ibd, per-table .ibd files, undo_*.ibu. Doublewrite (#innodb_dblwr/) excluded — mysqld recreates.
Redo log handled by RedoLogCopier, not this module.

Online mode coordinates with the redo copier + lock ladder; offline mode (server shutdown) is a clean flat copy because torn
pages can't exist when no one is writing.

Reference: percona-xtrabackup xtrabackup.cc:xtrabackup_backup_func() line 4238 + redo_log.cc:Redo_Log_Data_Manager.
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/io/filter/group.h"
#include "common/io/read.h"
#include "common/log.h"
#include "common/type/pack.h"
#include "common/type/string.h"
#include "mysql/engine/innodb.h"
#include "mysql/pageChecksumFilter.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

static void
engineInnodbCopyOnline(EngineBackupCtx *const ctx)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
    FUNCTION_LOG_END();

    ASSERT(ctx != NULL);
    ASSERT(ctx->dataPath != NULL);
    ASSERT(ctx->backupPath != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const Storage *const srcStorage = storagePosixNewP(ctx->dataPath);
        const Storage *const dstStorage = storagePosixNewP(ctx->backupPath, .write = true);

        TableSpaceIter *const iter = tableSpaceIterNew(srcStorage, ctx->dataPath);

        LOG_INFO_FMT("InnoDB: copying %u tablespace file(s)", tableSpaceIterFileTotal(iter));

        unsigned int copied = 0;
        String *path;

        // Page-checksum filter wiring: when the orchestrator has detected the InnoDB page size + algorithm (via
        // mysqlDataDirInspect), insert the filter into each file's read pipeline so we get streaming page validation alongside
        // the copy. When the algo is None (not detected), fall back to flat copy without validation — this matches the cold-
        // backup-of-an-empty-or-pre-bootstrap-datadir case.
        const bool validate = ctx->innodbPageSize > 0 && ctx->innodbPageChecksum != mysqlPageChecksumNone;

        uint64_t totalInvalid = 0;
        uint64_t totalSkipped = 0;

        while ((path = tableSpaceIterNext(iter)) != NULL)
        {
            StorageRead *const read = storageNewReadP(srcStorage, path);

            if (validate)
            {
                ioFilterGroupAdd(
                    ioReadFilterGroup(storageReadIo(read)),
                    mysqlPageChecksumFilterNew(ctx->innodbPageSize, ctx->innodbPageChecksum));
            }

            storageCopyP(read, storageNewWriteP(dstStorage, path));

            if (validate)
            {
                const Pack *const stats = ioFilterGroupResultPackP(
                    ioReadFilterGroup(storageReadIo(read)), MY_PAGE_CHECKSUM_FILTER_TYPE);
                const MysqlPageChecksumFilterStats s = mysqlPageChecksumFilterStatsFromPack(stats);

                if (s.pagesInvalid > 0)
                {
                    LOG_WARN_FMT(
                        "InnoDB: %s — %" PRIu64 " invalid page(s) of %" PRIu64 " checked", strZ(path), s.pagesInvalid,
                        s.pagesChecked);
                }
                else
                {
                    LOG_DETAIL_FMT(
                        "InnoDB: %s — %" PRIu64 " page(s) OK (%" PRIu64 " skipped)", strZ(path), s.pagesValid, s.pagesSkipped);
                }

                totalInvalid += s.pagesInvalid;
                totalSkipped += s.pagesSkipped;
            }

            copied++;
        }

        if (validate)
        {
            LOG_INFO_FMT(
                "InnoDB: page-checksum validation summary — %" PRIu64 " invalid, %" PRIu64 " skipped across %u file(s)",
                totalInvalid, totalSkipped, copied);
        }

        tableSpaceIterFree(iter);

        LOG_INFO_FMT("InnoDB: copied %u tablespace file(s) from %s", copied, strZ(ctx->dataPath));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

static const EngineHandler innodbHandler =
{
    .kind = mysqlEngineInnodb,
    .name = "innodb",
    .copyOnline = engineInnodbCopyOnline,
};

FN_EXTERN const EngineHandler *
engineInnodbHandler(void)
{
    return &innodbHandler;
}
