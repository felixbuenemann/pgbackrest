/***********************************************************************************************************************************
Restore Prepare Phase (Phase E)

After cmdRestore puts every file back in <restore_datadir>, the database is in a state equivalent to a server crash. To make it
startable, InnoDB must replay the redo log we copied during backup. Per the user-chosen approach, myBackRest does NOT link
GPLv2 InnoDB recovery code; instead it writes a one-shot mybackrest_recovery.cnf and invokes the user's mysqld to recover and
shut itself down:

  mysqld --defaults-file=<restore>/mybackrest_recovery.cnf
         --datadir=<restore>
         --skip-networking
         --innodb-read-only=0
         --init-file=<restore>/mybackrest_recovery.sql

The init file contains a single SHUTDOWN; statement so the recovered server exits gracefully. The user can then start mysqld
normally against the now-clean datadir.
***********************************************************************************************************************************/
#ifndef COMMAND_RESTORE_PREPARE_H
#define COMMAND_RESTORE_PREPARE_H

#include "common/type/string.h"
#include "storage/storage.h"

// Write mybackrest_recovery.cnf + mybackrest_recovery.sql into the restore datadir
FN_EXTERN void prepareWriteRecoveryFiles(const Storage *restoreStorage, const String *restorePath, const String *mysqldPath);

// Spawn `<mysqldPath> --defaults-file=...` and wait for graceful exit; throws on non-zero exit
FN_EXTERN void prepareInvokeMysqld(const String *mysqldPath, const String *restorePath);

/***********************************************************************************************************************************
Run the full restore-side compatibility check before letting mysqld touch the restored datadir.

Steps:
  1. Read mybackrest_backup_info from <backupPath> (mysqlBackupManifestRead).
  2. Probe <mysqldPath> --version (mysqlBinaryProbe).
  3. Run mysqlBinaryCheckCompatibility(probe, manifest.vendor, manifest.versionNum).
  4. If incompatible: log the issue, throw OptionInvalidError. Operator can override by re-running with a different mysqld.
  5. If only a warning (major-version skew or compatible-but-not-identical): log WARN, continue.

Returns silently when the binary is safe to drive recovery. Throws on hard incompatibility.
***********************************************************************************************************************************/
FN_EXTERN void prepareVerifyCompatibility(
    const Storage *backupStorage, const String *backupPath, const String *mysqldPath, bool allowMajorSkew);

#endif
