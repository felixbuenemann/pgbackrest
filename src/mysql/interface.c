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
#include "common/ini.h"
#include "common/io/bufferRead.h"
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

        // auto.cnf is created on first server start; absence is a valid pre-bootstrap state. Format:
        //   [auto]
        //   server-uuid=8c0fd6f0-bf8f-11ee-9821-0242ac120002
        if (content != NULL)
        {
            // iniNewP(.store=true) drains the IoRead during construction; no separate iniValid call needed.
            Ini *const ini = iniNewP(ioBufferReadNew(content), .store = true);
            const String *const value = iniGet(ini, STRDEF("auto"), STRDEF("server-uuid"));

            if (value == NULL || strSize(value) != 36)
            {
                THROW_FMT(
                    FormatError, "auto.cnf at '%s' has invalid or missing server-uuid (length %zu, expected 36)",
                    strZ(fullPath), value != NULL ? strSize(value) : 0);
            }

            MEM_CONTEXT_PRIOR_BEGIN()
            {
                result = strDup(value);
            }
            MEM_CONTEXT_PRIOR_END();
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
mysqlControlFromPage0(const unsigned char *const page, const size_t pageBytes, const bool has80Dictionary)
{
    ASSERT(page != NULL);

    if (pageBytes < FSP_SPACE_FLAGS + 4)
        THROW_FMT(FormatError, "page 0 buffer too short (%zu bytes) to contain FSP_HEADER", pageBytes);

    const uint32_t spaceId = mysqlReadU32Be(page + FIL_PAGE_SPACE_ID);
    const uint32_t flags = mysqlReadU32Be(page + FSP_SPACE_FLAGS);
    const uint32_t spaceIdFsp = mysqlReadU32Be(page + FSP_SPACE_ID);

    if (spaceId != spaceIdFsp)
    {
        THROW_FMT(
            FormatError, "page 0: FIL_PAGE_SPACE_ID (%u) disagrees with FSP_SPACE_ID (%u) — header is corrupt",
            spaceId, spaceIdFsp);
    }

    MysqlControl result = {0};
    result.pageSize = mysqlPageSizeFromFlags(flags);
    result.encrypted = (flags & FSP_FLAGS_MASK_ENCRYPTION) != 0;
    result.hasSdi = (flags & FSP_FLAGS_MASK_SDI) != 0;
    result.zipSsize = (flags & FSP_FLAGS_MASK_ZIP_SSIZE) >> FSP_FLAGS_POS_ZIP_SSIZE;
    result.antelope = (flags & FSP_FLAGS_MASK_POST_ANTELOPE) == 0;
    result.pageChecksum = (flags & FSP_FLAGS_MASK_FCRC32_MARKER) ? mysqlPageChecksumFullCrc32 : mysqlPageChecksumNone;
    result.versionNum = has80Dictionary ? 80000 : 50500;

    return result;
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

        const bool has80Dictionary = strEqZ(strBase(fileToRead), MYSQL_FILE_MYSQL_IBD) ||
            storageExistsP(storage, strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD));

        result = mysqlControlFromPage0(bufPtrConst(page0), bufUsed(page0), has80Dictionary);

        // Redo layout follows from datadir layout
        result.redoLayout = mysqlRedoLayoutDetect(storage, dataPath);

    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_CONTROL, result);
}

/**********************************************************************************************************************************/
FN_EXTERN unsigned int
mysqlPageType(const unsigned char *const page)
{
    ASSERT(page != NULL);

    return ((unsigned int)page[FIL_PAGE_TYPE] << 8) | (unsigned int)page[FIL_PAGE_TYPE + 1];
}

/**********************************************************************************************************************************/
FN_EXTERN bool
mysqlPageIsValidatable(const unsigned char *const page)
{
    ASSERT(page != NULL);

    const unsigned int t = mysqlPageType(page);

    // Compressed pages have a non-standard checksum layout — trust them; the next page's LSN will catch torn-write fallout
    if (t == FIL_PAGE_TYPE_COMPRESSED || t == FIL_PAGE_TYPE_COMPRESSED_AND_ENCRYPTED)
        return false;

    // Encrypted pages: stored checksum is over ciphertext, which would fail without decryption. Mark non-validatable; the
    // orchestrator will use the LSN-trailer pre-check (mysqlPageChecksumValidate's first guard) as a torn-write detector.
    if (t == FIL_PAGE_TYPE_ENCRYPTED)
        return false;

    return true;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlPageChecksumAlgo
mysqlPageChecksumValidateAdaptive(const unsigned char *const page, const MysqlPageSize pageSize, const uint32_t pageNo)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM_P(VOID, page);
        FUNCTION_TEST_PARAM(UINT, pageSize);
        FUNCTION_TEST_PARAM(UINT, pageNo);
    FUNCTION_TEST_END();

    ASSERT(page != NULL);

    // Order matters: try the most-likely-to-succeed first to keep the hot path cheap.
    //   1. CRC32 — default for MySQL 5.7+ and Percona; vast majority of modern datadirs match here on the first try.
    //   2. legacy "innodb" — MySQL 5.5/5.6 default; older installations.
    //   3. full_crc32 — MariaDB 10.5+ with --innodb-checksum-algorithm=full_crc32; rarer but unmistakable on match.
    //
    // The validator already has a torn-page (FIL_PAGE_LSN low ↔ trailer LSN low) and page-no precheck, so a true torn page
    // shows up as "all algorithms fail" rather than spuriously matching some other algo. False positives across algorithms
    // are vanishingly unlikely given the 32-bit checksum domain.
    if (mysqlPageChecksumValidate(page, pageSize, mysqlPageChecksumCrc32, pageNo))
        FUNCTION_TEST_RETURN(STRING_ID, mysqlPageChecksumCrc32);

    if (mysqlPageChecksumValidate(page, pageSize, mysqlPageChecksumInnodb, pageNo))
        FUNCTION_TEST_RETURN(STRING_ID, mysqlPageChecksumInnodb);

    if (mysqlPageChecksumValidate(page, pageSize, mysqlPageChecksumFullCrc32, pageNo))
        FUNCTION_TEST_RETURN(STRING_ID, mysqlPageChecksumFullCrc32);

    FUNCTION_TEST_RETURN(STRING_ID, mysqlPageChecksumNone);
}

/***********************************************************************************************************************************
Find the first redo log file under <dataPath> regardless of layout flavor.
  - 8.0.30+: any file in <dataPath>/#innodb_redo/ (skip the *_tmp pre-allocated ones if a non-tmp exists)
  - Pre-8.0.30: <dataPath>/ib_logfile0
Returns NULL relative path if no redo log file is present.
***********************************************************************************************************************************/
static String *
mysqlRedoFirstFile(const Storage *const storage, const String *const dataPath)
{
    String *result = NULL;

    const String *const innodbRedoDir = strNewFmt("%s/%s", strZ(dataPath), MYSQL_PATH_INNODB_REDO);

    if (storageExistsP(storage, innodbRedoDir))
    {
        // Pick the first non-tmp file in the redo dir
        StorageIterator *const itr = storageNewItrP(
            storage, innodbRedoDir, .level = storageInfoLevelType, .nullOnMissing = true);

        if (itr != NULL)
        {
            String *fallback = NULL;

            while (storageItrMore(itr))
            {
                const StorageInfo info = storageItrNext(itr);

                if (info.exists && info.type == storageTypeFile && strBeginsWithZ(info.name, MYSQL_FILE_IB_REDO_PREFIX))
                {
                    if (strstr(strZ(info.name), "_tmp") == NULL)
                    {
                        result = strNewFmt("%s/%s", strZ(innodbRedoDir), strZ(info.name));
                        break;
                    }
                    else if (fallback == NULL)
                    {
                        fallback = strNewFmt("%s/%s", strZ(innodbRedoDir), strZ(info.name));
                    }
                }
            }

            if (result == NULL && fallback != NULL)
                result = fallback;
        }
    }

    if (result == NULL)
    {
        const String *const ibLogfile0 = strNewFmt("%s/%s0", strZ(dataPath), MYSQL_FILE_IB_LOGFILE_PREFIX);

        if (storageExistsP(storage, ibLogfile0))
            result = strDup(ibLogfile0);
    }

    return result;
}

/***********************************************************************************************************************************
Parse the LOG_HEADER_CREATOR string. Returns the detected vendor and version (writes 0 for unparseable). Handles the four prefix
forms ("MariaDB X.Y.Z", "MySQL Clone …", "MySQL X.Y.Z[-N]", "MEB …") plus the substring fallback for Percona-XtraBackup output.

The Percona "-N" suffix after the patch number is the distinguishing signal vs upstream MySQL — per percona-server
storage/innobase/include/univ.i, Percona's INNODB_VERSION_STR appends PERCONA_INNODB_VERSION as "MAJOR.MINOR.BUGFIX-N".
***********************************************************************************************************************************/
static void
mysqlRedoCreatorParseString(const char *const creator, MysqlVendor *const vendorOut, unsigned int *const versionOut)
{
    // Prefix-based vendor signal
    if (strncmp(creator, "MariaDB ", 8) == 0)
    {
        *vendorOut = mysqlVendorMariadb;
    }
    else if (strncmp(creator, "MySQL Clone", 11) == 0)
    {
        // CLONE plugin output — vendor would have matched the source server; treat as MySQL (caller can refine)
        *vendorOut = mysqlVendorMysql;
    }
    else if (strncmp(creator, "MySQL ", 6) == 0)
    {
        *vendorOut = mysqlVendorMysql;                                          // default — promoted to Percona below if "-N" suffix
    }
    else if (strncmp(creator, "MEB ", 4) == 0)
    {
        // MySQL Enterprise Backup wrote this — we're inspecting a backup directory, not a live datadir
        *vendorOut = mysqlVendorUnknown;
    }
    else if (strstr(creator, "Percona") != NULL || strstr(creator, "Xtra") != NULL)
    {
        // xtrabackup writes "Percona-XtraBackup X.Y.Z" — we're looking at a backup dir, not a live datadir
        *vendorOut = mysqlVendorPercona;
    }

    // Scan past the prefix word to the first digit, then read "X.Y.Z" / "X.Y"
    const char *digits = creator;
    while (*digits != '\0' && (*digits < '0' || *digits > '9'))
        digits++;

    if (*digits == '\0')
        return;

    unsigned int major = 0, minor = 0, patch = 0;
    int consumed = 0;

    if (sscanf(digits, "%u.%u.%u%n", &major, &minor, &patch, &consumed) >= 3)
    {
        *versionOut = major * 10000 + minor * 100 + patch;

        // Percona "-N" suffix → promote generic MySQL detection to Percona
        if (consumed > 0 && digits[consumed] == '-' && digits[consumed + 1] >= '0' && digits[consumed + 1] <= '9'
            && *vendorOut == mysqlVendorMysql)
        {
            *vendorOut = mysqlVendorPercona;
        }
    }
    else if (sscanf(digits, "%u.%u", &major, &minor) >= 2)
    {
        *versionOut = major * 10000 + minor * 100;
    }
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlRedoCreator
mysqlRedoCreatorRead(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    MysqlRedoCreator result = {.vendor = mysqlVendorUnknown, .versionNum = 0, .raw = NULL, .formatNum = 0, .encryptedRedo = false};

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const redoPath = mysqlRedoFirstFile(storage, dataPath);

        if (redoPath != NULL)
        {
            // LOG_HEADER_FORMAT is at offset 0 (4 bytes BE), LOG_HEADER_CREATOR at offset 16 (max 32 bytes NUL-term). Read 64
            // bytes total to comfortably cover both. The format field disambiguates 8.0.0 from 8.0.30 + MariaDB 10.5 from 10.8
            // when the creator string is missing or truncated.
            Buffer *const header = storageGetP(
                storageNewReadP(storage, redoPath, .limit = VARUINT64(64)));

            if (header != NULL && bufUsed(header) >= 48)
            {
                const unsigned char *const data = bufPtrConst(header);

                // Format number — big-endian uint32 at offset 0
                const uint32_t rawFormat = mysqlReadU32Be(data);
                result.formatNum = rawFormat & ~MARIADB_REDO_FORMAT_ENCRYPTED_BIT;
                result.encryptedRedo = (rawFormat & MARIADB_REDO_FORMAT_ENCRYPTED_BIT) != 0;

                // The field is NUL-terminated within its 32-byte slot. Find the NUL or hit the slot boundary.
                char creator[33];
                size_t len = 0;
                for (size_t i = 0; i < 32; i++)
                {
                    const unsigned char c = data[16 + i];
                    if (c == 0)
                        break;
                    creator[len++] = (char)c;
                }
                creator[len] = '\0';

                mysqlRedoCreatorParseString(creator, &result.vendor, &result.versionNum);

                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    result.raw = strNewZ(creator);
                }
                MEM_CONTEXT_PRIOR_END();
            }
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_REDO_CREATOR, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlRedoCreatorToLog(const MysqlRedoCreator *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog, "{vendor: %u, versionNum: %u, formatNum: 0x%08x, encryptedRedo: %s, raw: %s}",
        (unsigned int)this->vendor, this->versionNum, this->formatNum,
        this->encryptedRedo ? "true" : "false",
        this->raw != NULL ? strZ(this->raw) : "(null)");
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
    const size_t trailerOffset = (size_t)pageSize - FIL_PAGE_TRAILER_SIZE;

    if (mysqlReadU32Be(page + FIL_PAGE_LSN + 4) != mysqlReadU32Be(page + trailerOffset + 4))
        FUNCTION_TEST_RETURN(BOOL, false);

    // Also check the FIL_PAGE_OFFSET matches the caller's expectation
    if (mysqlReadU32Be(page + FIL_PAGE_OFFSET) != pageNo)
        FUNCTION_TEST_RETURN(BOOL, false);

    switch (algo)
    {
        case mysqlPageChecksumNone:
            FUNCTION_TEST_RETURN(BOOL, true);

        case mysqlPageChecksumCrc32:
        case mysqlPageChecksumStrictCrc32:
        {
            // Per buf_calc_page_crc32() in mysql-server/storage/innobase/buf/checksum.cc:
            //   c1 = crc32(page[FIL_PAGE_OFFSET..FIL_PAGE_FILE_FLUSH_LSN-1])      = bytes 4..25 (22 bytes)
            //   c2 = crc32(page[FIL_PAGE_DATA..pageSize-FIL_PAGE_END_LSN_OLD_CHKSUM-1]) = bytes 38..pageSize-9
            //   stored = c1 ^ c2
            const uint32_t c1 = (uint32_t)crc32(0, page + FIL_PAGE_OFFSET, FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
            const uint32_t c2 = (uint32_t)crc32(
                0, page + FIL_PAGE_DATA, (uInt)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));

            FUNCTION_TEST_RETURN(BOOL, mysqlReadU32Be(page) == (c1 ^ c2));
        }

        case mysqlPageChecksumFullCrc32:
        {
            // MariaDB 10.5+: single CRC32 over page[0..pageSize-5], compared to last 4 bytes (big-endian).
            const uint32_t expected = (uint32_t)crc32(0, page, (uInt)(pageSize - 4));
            FUNCTION_TEST_RETURN(BOOL, mysqlReadU32Be(page + pageSize - 4) == expected);
        }

        case mysqlPageChecksumInnodb:
        {
            // Legacy "innodb" mode (default in MySQL 5.5 and 5.6, optional in 5.7). Algorithm is a polynomial hash from
            // storage/innobase/include/ut0rnd.h:
            //
            //   UT_HASH_RANDOM_MASK  = 1463735687
            //   UT_HASH_RANDOM_MASK2 = 1653893711
            //
            //   ut_fold_ulint_pair(n1, n2):
            //       return ((((((n1 ^ n2 ^ UT_HASH_RANDOM_MASK2) << 8) + n1) & 0xFFFFFFFF) ^ UT_HASH_RANDOM_MASK) + n2
            //
            //   ut_fold_binary(buf, len):
            //       fold = 0
            //       process 4-byte chunks: fold = ut_fold_ulint_pair(fold, ut4(chunk))
            //       leftover bytes processed individually with ut_fold_ulint_pair(fold, byte)
            //       return fold
            //
            //   buf_calc_page_new_checksum(page):
            //       hdr   = ut_fold_binary(page[FIL_PAGE_OFFSET..FIL_PAGE_FILE_FLUSH_LSN-1])      // bytes 4..25
            //       data  = ut_fold_binary(page[FIL_PAGE_DATA..pageSize-FIL_PAGE_END_LSN_OLD_CHKSUM-1])  // 38..pageSize-9
            //       return (hdr + data) & 0xFFFFFFFF
            //
            //   buf_calc_page_old_checksum(page):
            //       return ut_fold_binary(page, FIL_PAGE_FILE_FLUSH_LSN) & 0xFFFFFFFF       // bytes 0..25
            //
            // Page is valid if EITHER:
            //   stored_at_offset_0     matches buf_calc_page_new_checksum(page)
            //   OR stored_in_trailer_4 matches buf_calc_page_old_checksum(page)
            // Some pages were also written with stored == BUF_NO_CHECKSUM_MAGIC (0xDEADBEEF) when checksums were disabled.
            #define UT_HASH_RANDOM_MASK   ((uint32_t)1463735687u)
            #define UT_HASH_RANDOM_MASK2  ((uint32_t)1653893711u)

            const uint32_t stored = mysqlReadU32Be(page);

            // BUF_NO_CHECKSUM_MAGIC marker (innodb_checksum_algorithm=none historic)
            if (stored == 0xDEADBEEFu)
                FUNCTION_TEST_RETURN(BOOL, true);

            // ut_fold_binary inlined for the new-checksum range (bytes 4..25 + bytes 38..pageSize-9), maintaining a single fold
            // accumulator across both ranges.
            #define UT_HASH_ROUND(fold, n2)                                                                                        \
                ((uint32_t)((((uint64_t)((fold) ^ (n2) ^ UT_HASH_RANDOM_MASK2)) << 8) + (fold)) ^ UT_HASH_RANDOM_MASK) + (n2)

            uint32_t fold = 0;

            for (size_t i = 0; i < 2; i++)
            {
                const size_t start = i == 0 ? FIL_PAGE_OFFSET : FIL_PAGE_DATA;
                const size_t end   = i == 0 ? FIL_PAGE_FILE_FLUSH_LSN : (size_t)pageSize - FIL_PAGE_TRAILER_SIZE;

                size_t off = start;
                for (; off + 4 <= end; off += 4)
                    fold = UT_HASH_ROUND(fold, mysqlReadU32Be(page + off));
                for (; off < end; off++)
                    fold = UT_HASH_ROUND(fold, (uint32_t)page[off]);
            }

            #undef UT_HASH_ROUND
            #undef UT_HASH_RANDOM_MASK
            #undef UT_HASH_RANDOM_MASK2

            FUNCTION_TEST_RETURN(BOOL, stored == fold);
        }

        default:
            THROW_FMT(AssertError, "unknown page checksum algorithm %u", (unsigned int)algo);
    }
}
