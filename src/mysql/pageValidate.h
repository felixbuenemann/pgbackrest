/***********************************************************************************************************************************
InnoDB Page-by-Page File Validator

Streams a .ibd / ibdata1 / mysql.ibd / undo_*.ibu file through mysqlPageChecksumValidate one page at a time, returning aggregate
counts. Auto-detects the checksum algorithm from page 0 (FCRC32 marker bit) or via mysqlPageChecksumValidateAdaptive trial-and-
error if the marker is absent.

Compressed / encrypted pages are reported as "skipped" — their on-disk layout differs from a standard page so a uniform validator
can't check them without decompression / decryption support.

Use cases:
  - Pre-flight before a cold backup: refuse to back up a known-corrupt file
  - Post-restore verify: confirm the restored datadir's InnoDB files are intact
  - Standalone integrity check command (future "mybackrest verify --file=...")
***********************************************************************************************************************************/
#ifndef MYSQL_PAGEVALIDATE_H
#define MYSQL_PAGEVALIDATE_H

#include <stdint.h>

#include "common/type/string.h"
#include "mysql/interface.h"
#include "storage/storage.h"

typedef struct MysqlPageValidateResult
{
    uint64_t pagesChecked;                                              // Pages read (= file_size / pageSize)
    uint64_t pagesValid;                                                // Passed mysqlPageChecksumValidate
    uint64_t pagesInvalid;                                              // Failed checksum (corrupt)
    uint64_t pagesSkipped;                                              // Compressed/encrypted — can't validate without decode
    MysqlPageChecksumAlgo algo;                                         // Algorithm detected/used
    MysqlPageSize pageSize;                                             // Page size detected from page 0
} MysqlPageValidateResult;

// Validate every page in filePath. Returns aggregate counts. Throws on I/O errors but NOT on per-page checksum failure (those
// are counted, not thrown). If pageSizeHint is 0, page 0 is parsed for FSP_SPACE_FLAGS to determine the size; otherwise the
// hint is used (callers that already have a MysqlControl may skip the re-parse).
FN_EXTERN MysqlPageValidateResult mysqlPageValidateFile(
    const Storage *storage, const String *filePath, MysqlPageSize pageSizeHint);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
#include "common/debug.h"

FN_EXTERN void mysqlPageValidateResultToLog(const MysqlPageValidateResult *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_PAGE_VALIDATE_RESULT_TYPE                                                                                  \
    MysqlPageValidateResult
#define FUNCTION_LOG_MY_PAGE_VALIDATE_RESULT_FORMAT(value, buffer, bufferSize)                                                     \
    FUNCTION_LOG_OBJECT_FORMAT(&value, mysqlPageValidateResultToLog, buffer, bufferSize)

#endif
