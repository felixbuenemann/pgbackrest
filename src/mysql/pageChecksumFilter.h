/***********************************************************************************************************************************
InnoDB Page Checksum IoFilter

Inline IoFilter that observes (does NOT modify) an InnoDB tablespace file as it streams through pgBackRest's storage pipeline.
For every complete page-sized chunk, runs mysqlPageChecksumValidate; pass-through behavior is identical to the input.

Two natural integration points:
  1. Backup-side: insert into the read pipeline of an InnoDB-engine .ibd / ibdata1 / mysql.ibd copy. The backup proceeds even
     on validation failure; per-file counts surface via mysqlPageChecksumFilterResult so the caller can decide whether to
     refuse the backup or log a warning.
  2. Restore-side: insert into the read pipeline of a restored file to catch any corruption introduced during transit.

Skip-handling:
  - Compressed (FIL_PAGE_TYPE 14) and encrypted (15, 16) pages report as "skipped" — their layout differs from a standard
    page so the uniform checksum function can't validate them.
  - Sub-page trailing bytes (only at end of file) are silently ignored.

Drop-in replacement for the older src/command/backup/pageChecksum.c filter which is still PostgreSQL-shaped; this one is the
InnoDB equivalent and is what the cold/hot backup engine handlers will use once integration lands.
***********************************************************************************************************************************/
#ifndef MYSQL_PAGECHECKSUMFILTER_H
#define MYSQL_PAGECHECKSUMFILTER_H

#include <stdint.h>

#include "common/io/filter/filter.h"
#include "common/type/pack.h"
#include "mysql/interface.h"

// Build a filter that validates every InnoDB page passing through. pageSize must be one of MysqlPageSize values (4K..64K).
// algo is the checksum algorithm to apply per page — CRC32 / StrictCRC32 / FullCrc32 / legacy "innodb" / None.
FN_EXTERN IoFilter *mysqlPageChecksumFilterNew(MysqlPageSize pageSize, MysqlPageChecksumAlgo algo);

// Decode the filter's result Pack into typed counts. The pack is produced by IoFilter's standard "result()" mechanism and is
// accessible after the filter pipeline completes (typically via ioFilterGroupResultP(filterGroup, MY_PAGE_CHECKSUM_FILTER_TYPE)).
typedef struct MysqlPageChecksumFilterStats
{
    uint64_t pagesChecked;
    uint64_t pagesValid;
    uint64_t pagesInvalid;                                              // Non-zero = corruption detected
    uint64_t pagesSkipped;                                              // Compressed/encrypted; not validatable here
} MysqlPageChecksumFilterStats;

FN_EXTERN MysqlPageChecksumFilterStats mysqlPageChecksumFilterStatsFromPack(const Pack *pack);

// Filter type identifier — used by callers that look up the filter's result in a filter group
#define MY_PAGE_CHECKSUM_FILTER_TYPE                                STRID5("mypgck", 0x73e8ed9d0)

#endif
