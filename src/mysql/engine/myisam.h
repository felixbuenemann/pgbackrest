/***********************************************************************************************************************************
MyISAM Engine Module

MyISAM tables are non-transactional and require a table-level lock during backup. The lock is provided externally by either
LOCK INSTANCE FOR BACKUP (MySQL 8.0.16+) or BACKUP STAGE BLOCK_DDL (MariaDB 10.4+); this module assumes the lock is already held
when copyUnderLock is invoked. Files copied per table: <db>/<table>.MYD (data) + .MYI (index), plus the schema descriptor (.frm
on 5.7, .sdi inside the per-schema .ibd on 8.0+).

Reference: percona-xtrabackup backup_copy.cc:copy_non_innodb_files() and mariabackup equivalent at line ~5158.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_MYISAM_H
#define MYSQL_ENGINE_MYISAM_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineMyisamHandler(void);

#endif
