/***********************************************************************************************************************************
Cold-Mode (Offline) Backup Orchestrator

Drives an end-to-end backup of a known-shut-down MySQL/MariaDB/Percona datadir without needing a live mysqld connection. Glues
together the engine plugin registry, the InnoDB tablespace iterator, the redo-log cold-copy helper, the auto.cnf reader, and the
manifest writer.

Online mode requires the protocol "service worker" extension (see redoLog.c) and is left for the dedicated Phase D commit. Cold
mode is fully implementable today because no writer can race with the copier.

Sequence:
  1. mysqlDataDirInspect — detect vendor, version, engines, layout
  2. Dispatch to every present engine's copyOnline callback (safe to call in cold mode since the server is down)
  3. redoLogColdCopy — flat-copy the InnoDB redo log files
  4. Copy auto.cnf last so the destination looks bit-identical to the source
  5. mysqlBackupManifestWrite — render and persist the manifest
***********************************************************************************************************************************/
#ifndef MYSQL_COLDBACKUP_H
#define MYSQL_COLDBACKUP_H

#include "common/type/string.h"
#include "mysql/datadir.h"
#include "storage/storage.h"

typedef struct MysqlColdBackupResult
{
    MysqlDataDirInfo *info;                                             // What the inspector found
    unsigned int enginesProcessed;                                      // How many engine handlers ran
    unsigned int redoFilesCopied;                                       // From redoLogColdCopy
    bool autoCnfCopied;                                                 // True if auto.cnf existed and was copied
    bool manifestWritten;                                               // True iff the manifest landed on disk

    // Page-checksum validation summary across every InnoDB file copied. innodbPagesInvalid > 0 means the source datadir
    // contained corrupt pages — the backup was still written (faithful copy) but the operator should investigate.
    uint64_t innodbPagesChecked;
    uint64_t innodbPagesValid;
    uint64_t innodbPagesInvalid;
    uint64_t innodbPagesSkipped;
} MysqlColdBackupResult;

// Run a cold backup. dataPath must be the live datadir of a known-shut-down server; backupPath the empty (or
// caller-cleaned) destination. The destination storage must be writable. Returns a heap-allocated result struct owned by the
// caller. Throws on any unrecoverable error (e.g. dataPath missing).
FN_EXTERN MysqlColdBackupResult *mysqlColdBackup(
    const Storage *srcStorage, const String *dataPath,
    const Storage *dstStorage, const String *backupPath);

FN_EXTERN void mysqlColdBackupResultFree(MysqlColdBackupResult *this);

#endif
