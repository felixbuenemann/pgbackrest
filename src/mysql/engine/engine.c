/***********************************************************************************************************************************
Storage Engine Plugin Registry (Phase D scaffolding)

The lookup table is populated as each engine module ships:
  - innodb.c  : Phase D core (redo + tablespace + system tablespace + DD)
  - myisam.c  : Phase D (.MYD/.MYI under lock)
  - rocksdb.c : Phase D (checkpoint API + SST/log copy)
  - aria.c    : Phase D (MariaDB only; aria_backup API)
  - toku.c    : v1.1 (HOOK API via tokudb-xtrabackup fork)

Until those land this returns NULL for every name; backup orchestration in src/command/backup/backup.c will detect that and
fail loudly with a "no handler for engine X" message.
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "mysql/engine/aria.h"
#include "mysql/engine/engine.h"
#include "mysql/engine/innodb.h"
#include "mysql/engine/isam.h"
#include "mysql/engine/myisam.h"
#include "mysql/engine/rocksdb.h"
#include "mysql/engine/toku.h"

/**********************************************************************************************************************************/
FN_EXTERN const EngineHandler *
engineHandlerLookup(const String *const engineName)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, engineName);
    FUNCTION_TEST_END();

    ASSERT(engineName != NULL);

    // INFORMATION_SCHEMA.ENGINES.ENGINE column is uppercase; compare case-insensitively
    if (strEqZ(engineName, "InnoDB") || strEqZ(engineName, "INNODB") || strEqZ(engineName, "innodb") ||
        strEqZ(engineName, "XtraDB") || strEqZ(engineName, "XTRADB") || strEqZ(engineName, "xtradb"))
    {
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineInnodbHandler());
    }

    if (strEqZ(engineName, "MyISAM") || strEqZ(engineName, "MYISAM") || strEqZ(engineName, "myisam"))
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineMyisamHandler());

    if (strEqZ(engineName, "ISAM") || strEqZ(engineName, "isam"))
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineIsamHandler());

    if (strEqZ(engineName, "RocksDB") || strEqZ(engineName, "ROCKSDB") || strEqZ(engineName, "rocksdb") ||
        strEqZ(engineName, "MyRocks") || strEqZ(engineName, "MYROCKS") || strEqZ(engineName, "myrocks"))
    {
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineRocksdbHandler());
    }

    if (strEqZ(engineName, "Aria") || strEqZ(engineName, "ARIA") || strEqZ(engineName, "aria"))
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineAriaHandler());

    if (strEqZ(engineName, "TokuDB") || strEqZ(engineName, "TOKUDB") || strEqZ(engineName, "tokudb"))
        FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, engineTokuHandler());

    // Trivial engines (CSV, MEMORY, FEDERATED, ARCHIVE, BLACKHOLE) deliberately have no handler — Phase D's orchestrator
    // treats a NULL handler as either "skip silently" (MEMORY/FEDERATED/BLACKHOLE) or "flat-copy by extension" (CSV/ARCHIVE)
    // based on the engine kind.
    FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, NULL);
}
