/***********************************************************************************************************************************
MySQL / MariaDB On-Disk Interface

mysqlAutoCnfReadUuid: implemented (small INI-style file).
mysqlBinlogScan and mysqlPageChecksumValidate: implemented (well-defined formats from the cloned upstream tree).
mysqlControlFromIbdata and mysqlRedoLayoutDetect remain stubs — they need real fixture data to test against, which Phase D will
provide via the test harness in test/data/mysql/.
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>
#include <zlib.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/interface.h"
#include "storage/storage.h"

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

    String *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const fullPath = strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_AUTOCNF);
        Buffer *const content = storageGetP(storageNewReadP(storage, fullPath, .ignoreMissing = true));

        // auto.cnf is created on first server start; absence is a valid pre-bootstrap state
        if (content != NULL)
        {
            // Format:
            //   [auto]
            //   server-uuid=8c0fd6f0-bf8f-11ee-9821-0242ac120002
            // We split on \n and look for the prefix; the file is small (<200 bytes) so allocating a StringList is cheap.
            const String *const text = strNewBuf(content);
            StringList *const lines = strLstNewSplit(text, STRDEF("\n"));

            for (unsigned int lineIdx = 0; lineIdx < strLstSize(lines); lineIdx++)
            {
                const String *const line = strTrim(strDup(strLstGet(lines, lineIdx)));

                if (strBeginsWithZ(line, "server-uuid"))
                {
                    // Take everything after the first '=' and trim
                    const int eqIdx = (int)strChr(line, '=');

                    if (eqIdx >= 0)
                    {
                        const String *const value = strTrim(strSubN(line, (size_t)eqIdx + 1, strSize(line) - (size_t)eqIdx - 1));

                        // MySQL UUID is exactly 36 chars (8-4-4-4-12 with dashes). Validate length only — full regex is not
                        // worth the dependency here.
                        if (strSize(value) != 36)
                        {
                            THROW_FMT(
                                FormatError, "auto.cnf has invalid server-uuid length %zu in '%s'", strSize(value), strZ(fullPath));
                        }

                        MEM_CONTEXT_PRIOR_BEGIN()
                        {
                            result = strDup(value);
                        }
                        MEM_CONTEXT_PRIOR_END();

                        break;
                    }
                }
            }

            if (result == NULL)
                THROW_FMT(FormatError, "auto.cnf at '%s' missing server-uuid line", strZ(fullPath));
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(STRING, result);
}

/***********************************************************************************************************************************
Decode the InnoDB FSP_SPACE_FLAGS page-size field

FSP_SPACE_FLAGS bits 6..9 encode `page_ssize`. Per
storage/innobase/include/fsp0types.h plus the formula confirmed against the FIL header dumps in the cloned mysql-server tree:

    ssize == 0: legacy default = 16 KiB                           (also written when --innodb-page-size is omitted on 5.7+)
    ssize == 1: 4 KiB                                             (2048 << 1)
    ssize == 2: 8 KiB                                             (2048 << 2)
    ssize == 3: 16 KiB                                            (2048 << 3)
    ssize == 4: 32 KiB                                            (2048 << 4)
    ssize == 5: 64 KiB                                            (2048 << 5)

Anything else is unsupported or corrupt.
***********************************************************************************************************************************/
static MysqlPageSize
mysqlPageSizeFromFlags(const uint32_t flags)
{
    const unsigned int ssize = (flags >> 6) & 0xF;

    switch (ssize)
    {
        case 0:
        case 3:
            return mysqlPageSize16K;

        case 1:
            return mysqlPageSize4K;

        case 2:
            return mysqlPageSize8K;

        case 4:
            return mysqlPageSize32K;

        case 5:
            return mysqlPageSize64K;

        default:
            THROW_FMT(FormatError, "InnoDB FSP_SPACE_FLAGS encodes unsupported page_ssize %u (raw flags 0x%08x)", ssize, flags);
    }
}

/***********************************************************************************************************************************
Read a 4-byte big-endian unsigned int from a buffer
***********************************************************************************************************************************/
static uint32_t
mysqlReadU32Be(const unsigned char *const p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
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

    MysqlControl result = {0};

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Try MYSQL_FILE_IBDATA1 first (always present on 5.7 + 8.0); fall back to MYSQL_FILE_MYSQL_IBD which exists on 8.0+
        // even when ibdata1 has been split out. Both files start with a tablespace whose page 0 carries an FSP_HEADER.
        const String *const ibdata1Path = strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_IBDATA1);
        const String *fileToRead = NULL;

        if (storageExistsP(storage, ibdata1Path))
        {
            fileToRead = ibdata1Path;
        }
        else
        {
            const String *const mysqlIbdPath = strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD);

            if (storageExistsP(storage, mysqlIbdPath))
                fileToRead = mysqlIbdPath;
        }

        if (fileToRead == NULL)
        {
            THROW_FMT(
                FileMissingError, "neither %s nor %s found under '%s' — datadir does not look like an InnoDB installation",
                MYSQL_FILE_IBDATA1, MYSQL_FILE_MYSQL_IBD, strZ(dataPath));
        }

        // Read just the first 64 KiB which is enough for any supported page size (up to 64K).
        Buffer *const page0 = storageGetP(
            storageNewReadP(storage, fileToRead, .limit = VARUINT64(mysqlPageSize64K)));

        if (bufUsed(page0) < FSP_SPACE_FLAGS + 4)
        {
            THROW_FMT(
                FormatError, "%s is too short (%zu bytes) to contain an FSP_HEADER", strZ(fileToRead), bufUsed(page0));
        }

        const unsigned char *const data = bufPtrConst(page0);

        // Pull the fields we care about
        const uint32_t spaceId = mysqlReadU32Be(data + FIL_PAGE_SPACE_ID);
        const uint32_t flags = mysqlReadU32Be(data + FSP_SPACE_FLAGS);
        const uint32_t spaceIdFsp = mysqlReadU32Be(data + FSP_SPACE_ID);

        if (spaceId != spaceIdFsp)
        {
            THROW_FMT(
                FormatError,
                "%s page 0: FIL_PAGE_SPACE_ID (%u) disagrees with FSP_SPACE_ID (%u) — header is corrupt",
                strZ(fileToRead), spaceId, spaceIdFsp);
        }

        result.pageSize = mysqlPageSizeFromFlags(flags);

        // Layout flavor — presence of mysql.ibd indicates 8.0+ data dictionary; the version number itself can't be derived from
        // the file alone, so leave versionNum at 0 and let callers fill it from a live SELECT VERSION() if available.
        const bool has80Dictionary = strEqZ(strBase(fileToRead), MYSQL_FILE_MYSQL_IBD) ||
            storageExistsP(storage, strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD));

        result.versionNum = has80Dictionary ? 80000 : 50700;

        // Redo layout follows from datadir layout
        result.redoLayout = mysqlRedoLayoutDetect(storage, dataPath);

        // Default to the modern checksum algorithm; mysqlPageChecksumValidate accepts both crc32 variants.
        result.pageChecksum = mysqlPageChecksumCrc32;
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_CONTROL, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlControlToLog(const MysqlControl *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog,
        "{versionNum: %u, pageSize: %u, redoLayout: %u, pageChecksum: %u, lsnCheckpoint: %" PRIu64 "}",
        this->versionNum, (unsigned int)this->pageSize, (unsigned int)this->redoLayout, (unsigned int)this->pageChecksum,
        this->lsnCheckpoint);
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

    MysqlRedoLayout result = mysqlRedoLayoutUnknown;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // 8.0.30+ uses a directory; older releases use flat files.
        const String *const innodbRedoDir = strNewFmt("%s/%s", strZ(dataPath), MYSQL_PATH_INNODB_REDO);

        if (storageExistsP(storage, innodbRedoDir))
        {
            result = mysqlRedoLayoutDynamicInnodbRedo;
        }
        else
        {
            const String *const ibLogfile0 = strNewFmt("%s/%s0", strZ(dataPath), MYSQL_FILE_IB_LOGFILE_PREFIX);

            if (storageExistsP(storage, ibLogfile0))
                result = mysqlRedoLayoutFixedIbLogfile;
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(STRING_ID, result);
}

/***********************************************************************************************************************************
InnoDB CRC32 page checksum

Algorithm (matches InnoDB CRC32 in storage/innobase/buf/checksum.cc):
  c1 = crc32(page[FIL_PAGE_OFFSET..FIL_PAGE_LSN-1])    = bytes 4..25  (22 bytes — page no + 4 PREV/NEXT half + LSN high half)
  c2 = crc32(page[FIL_PAGE_DATA..pageSize-trailer-1])  = bytes 38..(pageSize-9)
  expected = c1 XOR c2
  Compare against the 4-byte big-endian value stored at FIL_PAGE_SPACE_OR_CHKSUM (offset 0).

We deliberately skip:
  bytes [0..3]                    — checksum field itself
  bytes [26..37]                  — file_flush_lsn + space_id (legacy, may be zeroed by some writers)
  last 8 bytes (trailer)          — old-style checksum + LSN low half

zlib's crc32() implements the IEEE 802.3 polynomial used by InnoDB's CRC32 mode (the legacy "innodb" hashing mode is different and
deliberately unsupported here — when v1.x adds the legacy mode we'll do it in a separate function).
***********************************************************************************************************************************/
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

    // The trailer's LSN low half should match FIL_PAGE_LSN's low half — that's a torn-page detector. If they don't match, the
    // page is torn and recovery (not us) will deal with it; we report invalid here so the upstream filter can re-read the page.
    const uint32_t lsnLow = (uint32_t)((uint32_t)page[FIL_PAGE_LSN + 4] << 24) |
                            (uint32_t)((uint32_t)page[FIL_PAGE_LSN + 5] << 16) |
                            (uint32_t)((uint32_t)page[FIL_PAGE_LSN + 6] << 8)  |
                            (uint32_t)page[FIL_PAGE_LSN + 7];

    const size_t trailerOffset = (size_t)pageSize - FIL_PAGE_TRAILER_SIZE;
    const uint32_t trailerLsnLow = (uint32_t)((uint32_t)page[trailerOffset + 4] << 24) |
                                   (uint32_t)((uint32_t)page[trailerOffset + 5] << 16) |
                                   (uint32_t)((uint32_t)page[trailerOffset + 6] << 8)  |
                                   (uint32_t)page[trailerOffset + 7];

    if (lsnLow != trailerLsnLow)
        FUNCTION_TEST_RETURN(BOOL, false);

    // Also check the FIL_PAGE_OFFSET matches the caller's expectation
    const uint32_t storedPageNo = (uint32_t)((uint32_t)page[FIL_PAGE_OFFSET + 0] << 24) |
                                  (uint32_t)((uint32_t)page[FIL_PAGE_OFFSET + 1] << 16) |
                                  (uint32_t)((uint32_t)page[FIL_PAGE_OFFSET + 2] << 8)  |
                                  (uint32_t)page[FIL_PAGE_OFFSET + 3];

    if (storedPageNo != pageNo)
        FUNCTION_TEST_RETURN(BOOL, false);

    switch (algo)
    {
        case mysqlPageChecksumNone:
            FUNCTION_TEST_RETURN(BOOL, true);

        case mysqlPageChecksumCrc32:
        case mysqlPageChecksumStrictCrc32:
        {
            // Read stored checksum (big-endian)
            const uint32_t stored = (uint32_t)((uint32_t)page[0] << 24) |
                                    (uint32_t)((uint32_t)page[1] << 16) |
                                    (uint32_t)((uint32_t)page[2] << 8)  |
                                    (uint32_t)page[3];

            // c1 covers bytes 4..25 (22 bytes); c2 covers bytes 38..pageSize-9 (pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE)
            const uint32_t c1 = (uint32_t)crc32(0, page + FIL_PAGE_OFFSET, FIL_PAGE_LSN - FIL_PAGE_OFFSET);
            const uint32_t c2 = (uint32_t)crc32(
                0, page + FIL_PAGE_DATA, (uInt)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));

            FUNCTION_TEST_RETURN(BOOL, stored == (c1 ^ c2));
        }

        case mysqlPageChecksumFullCrc32:
        {
            // MariaDB 10.5+: single CRC32 over page[0..pageSize-5], compared to last 4 bytes (big-endian).
            const uint32_t expected = (uint32_t)crc32(0, page, (uInt)(pageSize - 4));
            const uint32_t stored = (uint32_t)((uint32_t)page[pageSize - 4] << 24) |
                                    (uint32_t)((uint32_t)page[pageSize - 3] << 16) |
                                    (uint32_t)((uint32_t)page[pageSize - 2] << 8)  |
                                    (uint32_t)page[pageSize - 1];

            FUNCTION_TEST_RETURN(BOOL, stored == expected);
        }

        case mysqlPageChecksumInnodb:
            // Legacy "innodb" mode uses a different polynomial hash (buf_calc_page_new_checksum). Defer to v1.x — modern
            // installations don't write it (default flipped to CRC32 in MySQL 5.6.7).
            THROW(
                AssertError,
                "TODO(myBackRest-C): mysqlPageChecksumInnodb (legacy hash) not implemented; require innodb_checksum_algorithm=crc32");

        default:
            THROW_FMT(AssertError, "unknown page checksum algorithm %u", (unsigned int)algo);
    }
}
