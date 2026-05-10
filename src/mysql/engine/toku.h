/***********************************************************************************************************************************
TokuDB Engine Module (Post-v1, low priority)

Percona deprecated TokuDB; the only remaining live reference is the xelabs fork at https://github.com/xelabs/tokudb-xtrabackup.
This stub exists to keep the engine plugin table complete — Phase D's orchestrator will refuse to back up a server that has TokuDB
tables until this module is implemented.

Algorithm sketch (post-v1):
  - dlopen libHotBackup.so from the TokuDB plugin install path
  - SET GLOBAL tokudb_checkpoint_lock = 1                 (blocks new TokuDB checkpoints)
  - call HotBackup::do_backup(<datadir>, <backup>)        (TokuDB's own consistent file copier)
  - SET GLOBAL tokudb_checkpoint_lock = 0
  - Files copied: tokudb.environment, tokudb.directory, tokudb.rollback,
                  __tokudb_lock_dont_delete_me_*, log*.tokulog*, per-table _main_/_status_*.tokudb
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_TOKU_H
#define MYSQL_ENGINE_TOKU_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineTokuHandler(void);

#endif
