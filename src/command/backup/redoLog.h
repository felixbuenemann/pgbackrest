/***********************************************************************************************************************************
InnoDB Redo Log Copier (Phase D)

Background thread that streams the InnoDB redo log into the backup repository. Started before any data file copy begins (so we
have continuous coverage from the earliest LSN we'll need at recovery time) and stopped after the lock is acquired and a final
LSN is captured.

Two on-disk layouts to handle:
  - Pre-8.0.30 fixed:  ib_logfile0, ib_logfile1 (circular, ~50MB each)
  - 8.0.30+ dynamic:   <datadir>/#innodb_redo/#ib_redoN_<lsn>  (dictionary of files keyed by start LSN)
  - MariaDB 10.5+:     ib_logfile0 only, optional encryption

Output goes through the standard pgBackRest IO pipeline (compression / encryption / blockIncr apply transparently).

Reference: percona-xtrabackup redo_log.h class Redo_Log_Data_Manager, redo_log.cc:Redo_Log_Reader::scan_log_recs.
***********************************************************************************************************************************/
#ifndef COMMAND_BACKUP_REDOLOG_H
#define COMMAND_BACKUP_REDOLOG_H

#include <stdint.h>

#include "common/type/object.h"
#include "common/type/string.h"
#include "storage/storage.h"

typedef struct RedoLogCopier RedoLogCopier;

// Construct a copier ready to start. dataPath is the live datadir, repoPath the backup output root.
FN_EXTERN RedoLogCopier *redoLogCopierNew(const Storage *dataStorage, const String *dataPath, const Storage *repoStorage,
    const String *repoPath, uint64_t startLsn);

// Spawn the background thread and begin copying from startLsn forward.
FN_EXTERN void redoLogCopierStart(RedoLogCopier *this);

// Tell the background thread to stop after it reaches stopLsn (the LSN captured under lock).
FN_EXTERN void redoLogCopierStop(RedoLogCopier *this, uint64_t stopLsn);

// Wait for the background thread to exit. Throws if the thread reported any error.
FN_EXTERN void redoLogCopierJoin(RedoLogCopier *this);

FN_INLINE_ALWAYS void
redoLogCopierFree(RedoLogCopier *const this)
{
    objFree(this);
}

/***********************************************************************************************************************************
Cold-mode redo log copy

Used when the server is known to be shut down (offline backup). With no writer touching the redo log, a flat file copy is safe
and consistent — no concurrency, no protocol-worker plumbing, no stop-at-LSN signalling.

Handles both layouts the live datadir might have:
  - 5.7 / 8.0.0..8.0.29     <datadir>/ib_logfile0, ib_logfile1
  - 8.0.30+                  <datadir>/#innodb_redo/#ib_redoN_<lsn>
  - MariaDB 10.5+            <datadir>/ib_logfile0 (single file)

The destination preserves the same relative path: ib_logfile0 stays at the root, dynamic-format files go under #innodb_redo/.
Returns the number of files copied; emits LOG_INFO summarizing the result. Missing redo files are silently skipped — a brand-new
datadir before any server start has none.
***********************************************************************************************************************************/
FN_EXTERN unsigned int redoLogColdCopy(
    const Storage *srcStorage, const String *srcPath, const Storage *dstStorage, const String *dstPath);

#endif
