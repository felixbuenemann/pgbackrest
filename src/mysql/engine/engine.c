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
#include "mysql/engine/engine.h"

/**********************************************************************************************************************************/
FN_EXTERN const EngineHandler *
engineHandlerLookup(const String *const engineName)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(STRING, engineName);
    FUNCTION_TEST_END();

    ASSERT(engineName != NULL);

    // TODO(myBackRest-D): populate as engine modules ship; until then, every engine is unimplemented
    FUNCTION_TEST_RETURN_TYPE_CONST_P(EngineHandler, NULL);
}
