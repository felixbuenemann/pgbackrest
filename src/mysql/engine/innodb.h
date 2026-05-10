/***********************************************************************************************************************************
InnoDB / XtraDB Engine Module

Hot-backup support for the canonical InnoDB engine and Percona XtraDB (which is a drop-in replacement with identical on-disk
format). Backs up:

  - System tablespace: ibdata1 (and ibdata2... if multiple)
  - Per-table tablespaces: <schema>/<table>.ibd
  - Data dictionary tablespace (8.0+): mysql.ibd
  - Undo tablespaces (8.0+): undo_NNN.ibu (default count 2)
  - Redo log: pre-8.0.30 ib_logfile{0,1} or 8.0.30+ #innodb_redo/#ib_redoN

Doublewrite buffer files (#innodb_dblwr/) are intentionally NOT copied — mysqld recreates them at startup.

Reference: percona-xtrabackup xtrabackup.cc:xtrabackup_backup_func() line 4238 and redo_log.cc:Redo_Log_Data_Manager.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_INNODB_H
#define MYSQL_ENGINE_INNODB_H

#include "mysql/engine/engine.h"

// Singleton handler exposed via engineHandlerLookup("innodb"/"xtradb")
FN_EXTERN const EngineHandler *engineInnodbHandler(void);

#endif
