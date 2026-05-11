/***********************************************************************************************************************************
Miscellaneous-Files Copier

Engine handlers (innodb, myisam, aria, rocksdb, isam, toku) copy files matching their engine-specific extensions. But a working
datadir contains other files that none of those handlers claim:

  - Per-schema metadata: .sdi (8.0+ Serialized Dictionary Info), .opt (collation), .par (partitioning), .TRG/.TRN (triggers),
    .db (legacy schema marker)
  - CSV engine tables in mysql/ — general_log.CSV / general_log.CSM / slow_log.CSV / slow_log.CSM
  - Schema directories themselves (mysql/, performance_schema/, sys/) which mysqld expects to exist on disk even when their
    contents have moved into mysql.ibd

Without these, a restored MySQL 8.0+ datadir won't start — Data Dictionary initialization tries to CREATE SCHEMA mysql and
fails with "System schema directory does not exist".

This module sweeps each schema directory and copies every file the engine handlers didn't take. It's run as the LAST step of
the cold / hot backup orchestrators so any engine-specific copy semantics (lock state, page validation) have already applied
to the files they own.
***********************************************************************************************************************************/
#ifndef MYSQL_MISCFILES_H
#define MYSQL_MISCFILES_H

#include "common/type/string.h"
#include "storage/storage.h"

// Copy every non-engine-handled file from each schema directory under dataPath to the matching path under backupPath.
// Engine-handled extensions are recognized case-insensitively: .ibd / .MYD / .MYI / .MAD / .MAI / .ISD / .ISM / .tokudb.
// Returns the number of files copied. Schema directories that exist in the source but contain only engine-handled files are
// still created in the destination (so mysqld sees them on startup).
FN_EXTERN unsigned int mysqlMiscFilesCopy(
    const Storage *srcStorage, const String *dataPath, const Storage *dstStorage, const String *backupPath);

#endif
