/***********************************************************************************************************************************
MyRocks / RocksDB Engine Module

Backs up MyRocks (Percona / MySQL) and the MariaDB RocksDB plugin via the RocksDB checkpoint API:

  Percona/MySQL: SET SESSION rocksdb_create_checkpoint = '<tmp_path>'
  MariaDB:       same statement (checkpoint also placed under <tmp_path>)

The checkpoint creates hard-link snapshots of the .sst files plus a consistent MANIFEST/CURRENT/OPTIONS view. Files are then copied
from the checkpoint dir to the backup; mariabackup convention places the output under <backup>/#rocksdb/.

Reference: mariabackup backup_copy.cc:has_rocksdb_plugin() line 2123, rocksdb_create_checkpoint() line 2269.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_ROCKSDB_H
#define MYSQL_ENGINE_ROCKSDB_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineRocksdbHandler(void);

#endif
