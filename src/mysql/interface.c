/***********************************************************************************************************************************
MySQL / MariaDB On-Disk Interface (Phase C scaffolding)

Real implementations land in subsequent commits. This stub provides the symbol surface so dependent code (Phase D's tableSpace.c
and pageChecksum.c) can compile against it.

Implementation notes for the real Phase C:
  - mysqlAutoCnfReadUuid: read MYSQL_FILE_AUTOCNF, parse [auto] / server-uuid = <36-char> line, validate format.
  - mysqlControlFromIbdata: storageNewReadP(MYSQL_FILE_IBDATA1, .offset=0, .limit=64K), extract FSP_SPACE_FLAGS at offset 54, decode
    via FSP_FLAGS_GET_PAGE_SSIZE; (2048 << ssize) yields the page size. If MYSQL_FILE_IBDATA1 doesn't exist but MYSQL_FILE_MYSQL_IBD
    does, we are on MySQL 8.0+ — read its page 0 instead (same FSP layout).
  - mysqlRedoLayoutDetect: storageExistsP(#innodb_redo dir) → DynamicInnodbRedo; else if ib_logfile0 exists → FixedIbLogfile.
    MariaDB 10.5+ flagged by checking for the full_crc32 marker in ib_logfile0 header.
  - mysqlPageChecksumValidate: per algo —
      crc32: c1=crc32(page[4..25]), c2=crc32(page[38..pageSize-8]), result=c1^c2; compare to page[0..3] big-endian
      innodb: legacy buf_calc_page_new_checksum (polynomial table from include/ut0crc32.h)
      fullCrc32 (MariaDB 10.5+): single crc32 of page[4..pageSize-4] vs page[pageSize-4..pageSize]
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/string.h"
#include "mysql/interface.h"

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlAutoCnfReadUuid(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    // TODO(myBackRest-C): read MYSQL_FILE_AUTOCNF and grep server-uuid
    THROW(AssertError, "TODO(myBackRest-C): mysqlAutoCnfReadUuid not implemented");
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlControl
mysqlControlFromIbdata(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    // TODO(myBackRest-C): parse FSP_HEADER from page 0 of ibdata1 / mysql.ibd
    THROW(AssertError, "TODO(myBackRest-C): mysqlControlFromIbdata not implemented");
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlRedoLayout
mysqlRedoLayoutDetect(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    // TODO(myBackRest-C): probe #innodb_redo/ then ib_logfile0
    THROW(AssertError, "TODO(myBackRest-C): mysqlRedoLayoutDetect not implemented");
}

/**********************************************************************************************************************************/
FN_EXTERN bool
mysqlPageChecksumValidate(
    const unsigned char *const page, const MysqlPageSize pageSize, const MysqlPageChecksumAlgo algo, const uint32_t pageNo)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, page);
        FUNCTION_TEST_PARAM(UINT, pageSize);
        FUNCTION_TEST_PARAM(STRING_ID, algo);
        FUNCTION_TEST_PARAM(UINT, pageNo);
    FUNCTION_TEST_END();

    ASSERT(page != NULL);
    ASSERT(pageSize > 0);

    (void)algo;
    (void)pageNo;

    // TODO(myBackRest-C): InnoDB CRC32 / legacy innodb / full_crc32
    THROW(AssertError, "TODO(myBackRest-C): mysqlPageChecksumValidate not implemented");
}
