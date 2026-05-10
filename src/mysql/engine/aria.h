/***********************************************************************************************************************************
Aria Engine Module (MariaDB only)

Aria is MariaDB's crash-safe replacement for MyISAM, used as the system table engine in modern MariaDB. Backup proceeds in two
phases coordinated with BACKUP STAGE:

  copyOnline (during BACKUP STAGE START):  copy stable .MAD/.MAI files plus aria_log_control snapshot
  copyUnderLock (during BLOCK_DDL):        copy aria log tail (aria_log.NNNNNNNN) and any newly created tables

Reference: mariabackup aria_backup_client.cc:BackupImpl, copy_log_tail() line 710. The backup directory holds an aria_log_control
+ aria_log.* tail that lets MariaDB replay outstanding Aria transactions on first start.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_ARIA_H
#define MYSQL_ENGINE_ARIA_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineAriaHandler(void);

#endif
