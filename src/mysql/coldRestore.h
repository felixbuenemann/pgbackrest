/***********************************************************************************************************************************
Cold-Mode (Offline) Restore Orchestrator

Lays down the files captured by mysqlColdBackup into a target datadir. No live mysqld required during the file-placement phase;
the optional follow-up via prepareInvokeMysqld replays InnoDB redo + shuts down so the operator can start their own server
against a clean datadir.

The restored directory is bit-identical to the source datadir at backup time, modulo:
  - mybackrest_backup_info is preserved (so the operator can re-read it)
  - mybackrest_recovery.cnf and mybackrest_recovery.sql are added by prepareWriteRecoveryFiles
  - any files the engine handlers know they shouldn't restore (e.g. doublewrite files mysqld recreates)
***********************************************************************************************************************************/
#ifndef MYSQL_COLDRESTORE_H
#define MYSQL_COLDRESTORE_H

#include "common/type/string.h"
#include "mysql/manifest.h"
#include "storage/storage.h"

typedef struct MysqlColdRestoreResult
{
    MysqlBackupManifestParsed *manifest;                                // Parsed manifest from the backup
    unsigned int filesCopied;                                           // Total files placed in the destination
    bool recoveryFilesWritten;                                          // True iff prepareWriteRecoveryFiles ran
} MysqlColdRestoreResult;

// Run a cold restore. backupPath is the directory mysqlColdBackup wrote into; restorePath the target datadir (caller is
// responsible for pre-cleaning it). When mysqldPath is non-NULL, writes recovery files so the operator can drive InnoDB redo
// recovery; pass NULL to skip that step (e.g. when restoring purely as a file shuffle).
FN_EXTERN MysqlColdRestoreResult *mysqlColdRestore(
    const Storage *srcStorage, const String *backupPath,
    const Storage *dstStorage, const String *restorePath,
    const String *mysqldPath);

FN_EXTERN void mysqlColdRestoreResultFree(MysqlColdRestoreResult *this);

#endif
