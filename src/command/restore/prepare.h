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

#endif
