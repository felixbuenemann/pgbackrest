/***********************************************************************************************************************************
MySQL / MariaDB Datadir Inspector

Given a datadir path, returns everything we can derive about the database from filesystem signals alone — no SQL, no mysqld
binary required. This is the SOURCE OF TRUTH for cold-backup metadata: the datadir's own files reflect what was actually written
last, regardless of whether someone has since installed a different mysqld version on the host.

Concrete example that motivates this design (raised in code review): an admin's pre-upgrade workflow goes:

  1. systemctl stop mysql              (5.7 still installed)
  2. apt install mysql-server-8.0      (binary replaces /usr/sbin/mysqld with 8.0)
  3. mybackrest backup --type=cold     (datadir is still 5.7 format)
  4. mysql_upgrade / start 8.0
  5. systemctl start mysql

If we'd trusted `mysqld --version` in step 3 we would have recorded "8.0" for a backup of a 5.7 datadir, picked the wrong page-
checksum default, and possibly mishandled the lack of mysql.ibd. Detecting from the datadir prevents this.

What we can derive from filesystem signals:
  - vendor              MariaDB-only markers (aria_log_control, *.MAD/.MAI, *.TRG/TRN), Percona-only markers (audit.log,
                        xtrabackup_* leftovers); else default to MySQL.
  - versionNum (rough)  8.0.30+ if #innodb_redo/ dir exists; 8.0+ if mysql.ibd present; 5.7 if .frm files coexist with InnoDB
                        without mysql.ibd; 5.6 vs 5.5 distinguished by mysql/gtid_executed.* presence (5.6.5+).
  - versionExact        false unless we can pin to a specific patch (we usually can't from the filesystem alone — but the
                        versionNum returned is always a SAFE LOWER BOUND, never an overstatement).
  - serverUuid          auto.cnf [auto] section.
  - pageSize            FSP_SPACE_FLAGS in ibdata1 page 0 (or mysql.ibd page 0 on 8.0+).
  - redoLayout          mysqlRedoLayoutDetect probe.
  - engine presence     hasAria / hasMyrocks / hasInnodb / hasMyisam booleans drive which engine handlers the cold-backup
                        orchestrator instantiates.

The function is deliberately tolerant: missing files or unrecognized markers don't throw, they just leave the corresponding
field at its zero value. Callers can treat any zero/false field as "not detected".
***********************************************************************************************************************************/
#ifndef MYSQL_DATADIR_H
#define MYSQL_DATADIR_H

#include "common/type/string.h"
#include "mysql/client.h"
#include "mysql/interface.h"
#include "storage/storage.h"

typedef struct MysqlDataDirInfo
{
    // Vendor + version (best-effort from filesystem)
    MysqlVendor vendor;
    unsigned int versionNum;                                            // Best-effort lower bound; 0 if undetermined
    bool versionExact;                                                  // True iff we can pin to a specific patch from disk

    // Cluster identity
    String *serverUuid;                                                 // From auto.cnf, NULL if pre-bootstrap

    // InnoDB layout
    MysqlPageSize pageSize;                                             // 0 if no InnoDB tablespace found
    MysqlRedoLayout redoLayout;                                         // mysqlRedoLayoutUnknown if no redo dir/files

    // Engine presence flags — drive which handlers the cold-backup orchestrator runs
    bool hasInnodb;                                                     // any *.ibd / ibdata* / mysql.ibd
    bool hasMyisam;                                                     // any *.MYD
    bool hasAria;                                                       // any *.MAD or aria_log_control
    bool hasMyrocks;                                                    // .rocksdb/ or #rocksdb/ subdir
    bool hasTokudb;                                                     // any *.tokudb or tokudb.environment
} MysqlDataDirInfo;

// Inspect the datadir at dataPath. Always returns a non-NULL struct (zero-valued fields mean "not detected").
FN_EXTERN MysqlDataDirInfo *mysqlDataDirInspect(const Storage *storage, const String *dataPath);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlDataDirInfoToLog(const MysqlDataDirInfo *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_DATADIR_INFO_TYPE                                                                                          \
    MysqlDataDirInfo *
#define FUNCTION_LOG_MY_DATADIR_INFO_FORMAT(value, buffer, bufferSize)                                                             \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlDataDirInfoToLog, buffer, bufferSize)

#endif
