/***********************************************************************************************************************************
Storage Engine Plugin Interface

Each storage engine myBackRest supports (InnoDB / XtraDB, MyISAM, Aria, MyRocks, MariaDB-RocksDB, TokuDB) implements this small
struct of callbacks. The Phase D backup orchestrator queries INFORMATION_SCHEMA.ENGINES + .TABLES at startup, instantiates one
EngineHandler per detected engine, and drives them through the lock-state machine in lock-step:

  prepare(ctx)       - early setup before any locking; e.g. RocksDB checkpoint dir creation
  copyOnline(ctx)    - copy the engine's online-safe artefacts during BACKUP STAGE START
  copyUnderLock(ctx) - copy the engine's lock-required artefacts during BACKUP STAGE BLOCK_DDL
  finalize(ctx)      - post-BACKUP_STAGE_END cleanup; e.g. drop the RocksDB checkpoint

Reference algorithms:
  - InnoDB: percona-xtrabackup xtrabackup.cc:xtrabackup_backup_func() line 4238 + redo_log.cc:Redo_Log_Data_Manager
  - MyISAM: copy .MYD/.MYI under FLUSH TABLES <t> FOR EXPORT or LOCK TABLES FOR BACKUP
  - Aria: mariadb-server extra/mariabackup/aria_backup_client.cc:BackupImpl
  - MyRocks: SET SESSION rocksdb_create_checkpoint = '<path>'; copy SST/log/MANIFEST; reference backup_copy.cc:rocksdb_create_checkpoint() line 2269
  - MariaDB-RocksDB: same checkpoint API; output to #rocksdb/
  - TokuDB (post-v1, low priority): tokudb-xtrabackup HOOK API; SET tokudb_checkpoint_lock=1; copy .tokudb files and tokulog
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_ENGINE_H
#define MYSQL_ENGINE_ENGINE_H

#include "common/type/string.h"
#include "mysql/client.h"

/***********************************************************************************************************************************
Engine identifiers (matching INFORMATION_SCHEMA.ENGINES.ENGINE column values, lowercased)
***********************************************************************************************************************************/
typedef enum
{
    mysqlEngineUnknown = 0,
    mysqlEngineInnodb,
    mysqlEngineXtradb,                                                  // Percona XtraDB; behaves identically to InnoDB
    mysqlEngineMyisam,
    mysqlEngineIsam,                                                    // Predecessor to MyISAM; MySQL 3.21 → 4.0.2 only
    mysqlEngineAria,                                                    // MariaDB only
    mysqlEngineMyrocks,                                                 // MyRocks (Percona / MySQL)
    mysqlEngineRocksdb,                                                 // MariaDB-RocksDB
    mysqlEngineTokudb,                                                  // Post-v1; deprecated upstream but supported via xelabs fork
    mysqlEngineCsv,                                                     // Trivial flat-file copy
    mysqlEngineMemory,                                                  // No state to back up
    mysqlEngineFederated,                                               // No local state
    mysqlEngineArchive,                                                 // .ARZ flat copy
    mysqlEngineBlackhole,                                               // No state
} MysqlEngineKind;

/***********************************************************************************************************************************
Backup-context handle passed to every engine callback
***********************************************************************************************************************************/
typedef struct EngineBackupCtx
{
    MysqlClient *client;                                                // Live connection (for engine-specific SQL)
    const String *dataPath;                                             // Source datadir
    const String *backupPath;                                           // Destination
    unsigned int processMax;                                            // From --process-max
} EngineBackupCtx;

/***********************************************************************************************************************************
Engine plugin vtable
***********************************************************************************************************************************/
typedef struct EngineHandler
{
    MysqlEngineKind kind;
    const char *name;                                                   // Logging label

    void (*prepare)(EngineBackupCtx *ctx);                              // Optional; may be NULL
    void (*copyOnline)(EngineBackupCtx *ctx);                           // During BACKUP STAGE START
    void (*copyUnderLock)(EngineBackupCtx *ctx);                        // During BACKUP STAGE BLOCK_DDL
    void (*finalize)(EngineBackupCtx *ctx);                             // After BACKUP STAGE END
} EngineHandler;

/***********************************************************************************************************************************
Lookup by engine name as reported by INFORMATION_SCHEMA.ENGINES.ENGINE (case-insensitive)
***********************************************************************************************************************************/
FN_EXTERN const EngineHandler *engineHandlerLookup(const String *engineName);

/***********************************************************************************************************************************
Shared helper used by the simple per-schema flat-copy engines (MyISAM, ISAM, Aria).

Walks the datadir, identifies schema directories (top-level dirs with non-dot/non-lost+found names), then enumerates each schema
for files ending in primaryExt. For every match it copies the file with primaryExt and the corresponding files with each
companion extension (NULL-terminated list). Copies stream through storageCopyP so memory stays bounded regardless of file size.

  primaryExt   ".MYD" / ".ISD" / ".MAD"
  companions   {".MYI", ".frm", NULL}
  logLabel     "MyISAM" / "ISAM" / "Aria" — used for the LOG_INFO summary line

The .frm companion is optional on 8.0+ — companions are tried with .ignoreMissing so silent skips are normal.
***********************************************************************************************************************************/
FN_EXTERN void engineFlatCopyByExtension(
    EngineBackupCtx *ctx, const char *primaryExt, const char *const companionExts[], const char *logLabel);

#endif
