/***********************************************************************************************************************************
Hot-Mode (Online) Backup Orchestrator

Drives an end-to-end backup of a LIVE MySQL/MariaDB/Percona server. Unlike cold mode, this assumes the server is running and that
writes may happen during the backup. The correctness story is "lock the server for the entire copy window" — when the lock is
held, no engine writes new data, no DDL happens, and the on-disk state is effectively frozen. This avoids needing the streaming
redo log copier (which would otherwise be required for low-lock-window xtrabackup-style backups), at the cost of holding a
lock for the duration of the copy.

Sequence:
  1. mysqlServerSanityCheck — log_bin, binlog_format, gtid_mode, server_id
  2. mysqlDataDirInspect — autodetect vendor, version, engines, layout
  3. mysqlLockMethodSelect — pick instance/stage/ftwrl based on vendor/version + caller preference
  4. mysqlLockBegin + mysqlLockBlockDdl — quiesce DDL + new transactions
  5. Capture START binlog position via SHOW MASTER STATUS (atomic snapshot of {File, Position, Executed_Gtid_Set})
  6. Dispatch every detected engine's prepare/copyOnline/copyUnderLock callbacks
  7. redoLogColdCopy — flat-copy the InnoDB redo log files (quiescent under lock)
  8. Copy auto.cnf
  9. mysqlLockBlockCommit — block new commits (instance method noop; ftwrl issues the actual FLUSH TABLES WITH READ LOCK here)
  10. Capture STOP binlog position
  11. mysqlLockRelease — UNLOCK INSTANCE / BACKUP STAGE END / UNLOCK TABLES
  12. mysqlBackupManifestWrite with the [binlog] block populated

The PROTOCOL service-worker extension needed by the streaming redo copier (see redoLog.c) is NOT required by this orchestrator —
since the lock is held for the entire file copy window, the redo log doesn't grow during the copy. A future "short-lock" mode
that releases the lock after copying just the metadata would need that extension.
***********************************************************************************************************************************/
#ifndef MYSQL_HOTBACKUP_H
#define MYSQL_HOTBACKUP_H

#include "common/type/string.h"
#include "mysql/client.h"
#include "mysql/datadir.h"
#include "mysql/lock.h"
#include "mysql/manifest.h"
#include "storage/storage.h"

typedef struct MysqlHotBackupResult
{
    MysqlDataDirInfo *info;                                             // Detected at backup time
    MysqlBackupBinlog *binlog;                                          // Captured under lock
    MysqlLockMethod lockMethodUsed;                                     // Method autodetected or specified
    unsigned int enginesProcessed;
    unsigned int redoFilesCopied;
    bool autoCnfCopied;
    bool manifestWritten;
} MysqlHotBackupResult;

// Run a hot (online) backup. The MysqlClient must already be opened via mysqlClientOpen. lockPreference of
// mysqlLockMethodAuto selects the strongest method the server supports. Throws on any unrecoverable error AFTER attempting
// best-effort lock cleanup.
FN_EXTERN MysqlHotBackupResult *mysqlHotBackup(
    MysqlClient *client,
    const Storage *srcStorage, const String *dataPath,
    const Storage *dstStorage, const String *backupPath,
    MysqlLockMethod lockPreference);

FN_EXTERN void mysqlHotBackupResultFree(MysqlHotBackupResult *this);

#endif
