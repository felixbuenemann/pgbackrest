/***********************************************************************************************************************************
Datadir Integrity Verifier

Walks every InnoDB tablespace file under a (live or restored) datadir and validates every page. Produces aggregate counts +
a list of files that had any invalid pages.

Two natural use cases:
  - Pre-backup safety net: refuse to back up a datadir that already has corruption (catch it at source, not after a restore)
  - Post-restore confidence: confirm the restored datadir is intact before pointing mysqld at it

Non-InnoDB engines (MyISAM, Aria, RocksDB, etc.) have their own integrity stories that don't fit this validator's "every 16K
chunk is a page" model — they're checked separately by the engine modules at restore time.
***********************************************************************************************************************************/
#ifndef MYSQL_VERIFY_H
#define MYSQL_VERIFY_H

#include <stdint.h>

#include "common/type/string.h"
#include "common/type/stringList.h"
#include "storage/storage.h"

typedef struct MysqlVerifyResult
{
    uint64_t filesScanned;                                              // .ibd / ibdata* / mysql.ibd / undo_*.ibu
    uint64_t pagesChecked;                                              // Sum across all files
    uint64_t pagesValid;
    uint64_t pagesInvalid;                                              // Any non-zero here = corruption
    uint64_t pagesSkipped;                                              // Compressed / encrypted — can't validate uniformly
    StringList *invalidFiles;                                           // Paths (relative to dataPath) that had ≥1 invalid page
} MysqlVerifyResult;

// Walk the datadir's InnoDB tablespace files and validate each one's pages. Throws only on I/O errors — per-page failures are
// recorded in the aggregate counts and invalidFiles list. Returns a result whose internals live in the caller's mem context.
FN_EXTERN MysqlVerifyResult *mysqlVerify(const Storage *storage, const String *dataPath);

#endif
