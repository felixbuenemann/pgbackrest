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
        result.encrypted = (flags & FSP_FLAGS_MASK_ENCRYPTION) != 0;
        result.hasSdi = (flags & FSP_FLAGS_MASK_SDI) != 0;
        result.zipSsize = (flags & FSP_FLAGS_MASK_ZIP_SSIZE) >> FSP_FLAGS_POS_ZIP_SSIZE;

        // POST_ANTELOPE bit: 0 = Antelope (original format, MySQL 4.1 → 5.5.6 default), 1 = Barracuda or later. Old upgraded
        // installations carry Antelope ibdata1 forward forever — backup is fine because the FSP layout + standard checksum
        // are identical, but the orchestrator may want to warn about per-table .ibd files using ROW_FORMAT=COMPRESSED (which
        // requires Barracuda — Antelope can't produce them).
        result.antelope = (flags & FSP_FLAGS_MASK_POST_ANTELOPE) == 0;

        // Definitive checksum-algo signal: MariaDB sets bit 4 to mark full_crc32 mode. For everyone else the algo isn't stored
        // on disk — leave pageChecksum at None and let the adaptive validator probe at copy time.
        if (flags & FSP_FLAGS_MASK_FCRC32_MARKER)
            result.pageChecksum = mysqlPageChecksumFullCrc32;
        else
            result.pageChecksum = mysqlPageChecksumNone;                // Means "unknown — caller should probe adaptively"

        // Layout flavor — presence of mysql.ibd is a hard signal for 8.0+ data dictionary. Without it we return the absolute
        // minimum supported version (5.5) as a SAFE LOWER BOUND; callers (notably mysqlDataDirInspect) refine upward using
        // additional filesystem signals (sys/ schema → 5.7, mysql/gtid_executed.* → 5.6.5+).
        const bool has80Dictionary = strEqZ(strBase(fileToRead), MYSQL_FILE_MYSQL_IBD) ||
            storageExistsP(storage, strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD));

        result.versionNum = has80Dictionary ? 80000 : 50500;

        // Redo layout follows from datadir layout
        result.redoLayout = mysqlRedoLayoutDetect(storage, dataPath);

    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_CONTROL, result);
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

    MysqlRedoCreator result = {.vendor = mysqlVendorUnknown, .versionNum = 0, .raw = NULL};

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const redoPath = mysqlRedoFirstFile(storage, dataPath);

        if (redoPath != NULL)
        {
            // LOG_HEADER_CREATOR is at offset 16, max 32 bytes (matches both MySQL and MariaDB layout). Read 64 bytes total
            // so we comfortably cover the field plus its NUL terminator.
            Buffer *const header = storageGetP(
                storageNewReadP(storage, redoPath, .limit = VARUINT64(64)));

            if (header != NULL && bufUsed(header) >= 48)
            {
                const unsigned char *const data = bufPtrConst(header);

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

                // Vendor detection by prefix + suffix. Per the cloned percona-server source
                // (storage/innobase/include/univ.i): Percona's INNODB_VERSION_STR is
                //   IB_TO_STR(MAJOR) "." IB_TO_STR(MINOR) "." IB_TO_STR(BUGFIX) "-" IB_TO_STR(PERCONA_INNODB_VERSION)
                // so a Percona 8.0.36 redo header says "MySQL 8.0.36-8" (8 is the Percona-specific InnoDB version int),
                // while upstream MySQL 8.0.36 says plain "MySQL 8.0.36". The "-N" suffix after the version is the
                // distinguishing signal.
                if (strncmp(creator, "MariaDB ", 8) == 0)
                {
                    result.vendor = mysqlVendorMariadb;
                }
                else if (strncmp(creator, "MySQL Clone", 11) == 0)
                {
                    // CLONE plugin output — vendor would have matched the source server; treat as MySQL (caller can refine)
                    result.vendor = mysqlVendorMysql;
                }
                else if (strncmp(creator, "MySQL ", 6) == 0)
                {
                    result.vendor = mysqlVendorMysql;                   // default — refined below if Percona suffix found
                }
                else if (strncmp(creator, "MEB ", 4) == 0)
                {
                    // MySQL Enterprise Backup wrote this — we're inspecting a backup directory, not a live datadir
                    result.vendor = mysqlVendorUnknown;
                }
                else if (strstr(creator, "Percona") != NULL || strstr(creator, "Xtra") != NULL)
                {
                    // xtrabackup writes "Percona-XtraBackup X.Y.Z" — we're looking at a backup dir, not a live datadir
                    result.vendor = mysqlVendorPercona;
                }

                // Version: scan past the prefix word for "X.Y.Z" (and check for the Percona "-N" suffix)
                const char *digits = creator;
                while (*digits != '\0' && (*digits < '0' || *digits > '9'))
                    digits++;

                if (*digits != '\0')
                {
                    unsigned int major = 0, minor = 0, patch = 0;
                    int consumed = 0;
                    if (sscanf(digits, "%u.%u.%u%n", &major, &minor, &patch, &consumed) >= 3)
                    {
                        result.versionNum = major * 10000 + minor * 100 + patch;

                        // After the version, Percona appends "-N" where N is PERCONA_INNODB_VERSION (small integer).
                        // Upstream MySQL has nothing after the patch number, so the presence of "-<digit>" promotes the
                        // detected vendor from generic MySQL to Percona.
                        if (consumed > 0 && digits[consumed] == '-' && digits[consumed + 1] >= '0' && digits[consumed + 1] <= '9'
                            && result.vendor == mysqlVendorMysql)
                        {
                            result.vendor = mysqlVendorPercona;
                        }
                    }
                    else if (sscanf(digits, "%u.%u", &major, &minor) >= 2)
                    {
                        result.versionNum = major * 10000 + minor * 100;
                    }
                }

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
        debugLog, "{vendor: %u, versionNum: %u, raw: %s}",
        (unsigned int)this->vendor, this->versionNum, this->raw != NULL ? strZ(this->raw) : "(null)");
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

            const uint32_t stored = (uint32_t)((uint32_t)page[0] << 24) |
                                    (uint32_t)((uint32_t)page[1] << 16) |
                                    (uint32_t)((uint32_t)page[2] << 8)  |
                                    (uint32_t)page[3];

            // BUF_NO_CHECKSUM_MAGIC marker (innodb_checksum_algorithm=none historic)
            if (stored == 0xDEADBEEFu)
                FUNCTION_TEST_RETURN(BOOL, true);

            // ut_fold_binary inlined for the new-checksum range (bytes 4..25 + bytes 38..pageSize-9)
            uint32_t fold = 0;

            // Range 1: bytes 4..25 (22 bytes — exactly 5 4-byte chunks + 2 leftover bytes)
            for (size_t off = FIL_PAGE_OFFSET; off + 4 <= FIL_PAGE_FILE_FLUSH_LSN; off += 4)
            {
                const uint32_t n2 = ((uint32_t)page[off] << 24) | ((uint32_t)page[off + 1] << 16) |
                                    ((uint32_t)page[off + 2] << 8) | (uint32_t)page[off + 3];
                const uint32_t mix = (uint32_t)((((uint64_t)((fold ^ n2 ^ UT_HASH_RANDOM_MASK2)) << 8) + fold) & 0xFFFFFFFFu);
                fold = (mix ^ UT_HASH_RANDOM_MASK) + n2;
            }
            for (size_t off = FIL_PAGE_OFFSET + ((FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET) & ~(size_t)3);
                 off < FIL_PAGE_FILE_FLUSH_LSN; off++)
            {
                const uint32_t n2 = page[off];
                const uint32_t mix = (uint32_t)((((uint64_t)((fold ^ n2 ^ UT_HASH_RANDOM_MASK2)) << 8) + fold) & 0xFFFFFFFFu);
                fold = (mix ^ UT_HASH_RANDOM_MASK) + n2;
            }

            // Range 2: bytes 38..pageSize-9
            const size_t dataEnd = pageSize - FIL_PAGE_TRAILER_SIZE;
            for (size_t off = FIL_PAGE_DATA; off + 4 <= dataEnd; off += 4)
            {
                const uint32_t n2 = ((uint32_t)page[off] << 24) | ((uint32_t)page[off + 1] << 16) |
                                    ((uint32_t)page[off + 2] << 8) | (uint32_t)page[off + 3];
                const uint32_t mix = (uint32_t)((((uint64_t)((fold ^ n2 ^ UT_HASH_RANDOM_MASK2)) << 8) + fold) & 0xFFFFFFFFu);
                fold = (mix ^ UT_HASH_RANDOM_MASK) + n2;
            }
            for (size_t off = FIL_PAGE_DATA + ((dataEnd - FIL_PAGE_DATA) & ~(size_t)3); off < dataEnd; off++)
            {
                const uint32_t n2 = page[off];
                const uint32_t mix = (uint32_t)((((uint64_t)((fold ^ n2 ^ UT_HASH_RANDOM_MASK2)) << 8) + fold) & 0xFFFFFFFFu);
                fold = (mix ^ UT_HASH_RANDOM_MASK) + n2;
            }

            #undef UT_HASH_RANDOM_MASK
            #undef UT_HASH_RANDOM_MASK2

            FUNCTION_TEST_RETURN(BOOL, stored == fold);
        }

        default:
            THROW_FMT(AssertError, "unknown page checksum algorithm %u", (unsigned int)algo);
    }
}
