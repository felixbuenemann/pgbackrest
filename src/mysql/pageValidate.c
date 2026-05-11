/***********************************************************************************************************************************
InnoDB Page-by-Page File Validator
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/io/io.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "mysql/interface.h"
#include "mysql/pageValidate.h"
#include "storage/read.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Detect page size + algorithm from page 0. Returns false if the page isn't a valid FSP_HEADER page (caller should treat the file
as corrupt or non-tablespace).
***********************************************************************************************************************************/
static bool
pageValidateDetect(const unsigned char *const page0, const size_t page0Bytes, MysqlPageSize *const pageSizeOut,
    MysqlPageChecksumAlgo *const algoOut)
{
    // Need enough bytes to reach FSP_SPACE_FLAGS at offset 54
    if (page0Bytes < FSP_SPACE_FLAGS + 4)
        return false;

    // Parse the FSP header. has80Dictionary is irrelevant here — we only want pageSize + checksum hints.
    const MysqlControl ctl = mysqlControlFromPage0(page0, page0Bytes, /*has80Dictionary*/ false);

    if (ctl.pageSize == 0)
        return false;

    *pageSizeOut = ctl.pageSize;

    // FCRC32 marker bit is definitive when set. When not set we have to probe page 0 itself with the adaptive validator.
    if (ctl.pageChecksum == mysqlPageChecksumFullCrc32)
    {
        *algoOut = mysqlPageChecksumFullCrc32;
        return true;
    }

    // Probe with adaptive validator — picks CRC32 / strict CRC32 / legacy innodb based on which one matches page 0.
    if (page0Bytes < ctl.pageSize)
        return false;

    const MysqlPageChecksumAlgo probed = mysqlPageChecksumValidateAdaptive(page0, ctl.pageSize, /*pageNo*/ 0);

    *algoOut = probed;
    return true;
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlPageValidateResult
mysqlPageValidateFile(
    const Storage *const storage, const String *const filePath, const MysqlPageSize pageSizeHint)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, filePath);
        FUNCTION_LOG_PARAM(UINT, pageSizeHint);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(filePath != NULL);

    MysqlPageValidateResult result = {0};

    MEM_CONTEXT_TEMP_BEGIN()
    {
        StorageRead *const read = storageNewReadP(storage, filePath);
        ioReadOpen(storageReadIo(read));

        // Step 1: read page 0 (up to 64 KiB, the largest supported page size)
        Buffer *const firstChunk = bufNew(mysqlPageSize64K);
        ioRead(storageReadIo(read), firstChunk);

        if (bufUsed(firstChunk) == 0)
        {
            // Empty file — treat as zero pages, no error
            MEM_CONTEXT_PRIOR_BEGIN()
            {
                // Nothing to set; result stays zero
            }
            MEM_CONTEXT_PRIOR_END();
        }
        else
        {
            // Step 2: detect page size + algorithm. If caller provided a hint, use it without re-parsing.
            MysqlPageSize pageSize = pageSizeHint;
            MysqlPageChecksumAlgo algo = mysqlPageChecksumNone;

            if (pageSize == 0)
            {
                if (!pageValidateDetect(bufPtrConst(firstChunk), bufUsed(firstChunk), &pageSize, &algo))
                {
                    THROW_FMT(
                        FormatError, "page-validate: '%s' page 0 doesn't carry a recognizable FSP header — file is not an"
                        " InnoDB tablespace, or page 0 is corrupt",
                        strZ(filePath));
                }
            }
            else
            {
                // Caller supplied page size; still probe for algorithm
                if (!pageValidateDetect(bufPtrConst(firstChunk), bufUsed(firstChunk), &pageSize, &algo))
                    algo = mysqlPageChecksumNone;
            }

            result.pageSize = pageSize;
            result.algo = algo;

            // Step 3: walk every page. Start with the buffer we already have, then read more in pageSize-aligned chunks.
            const unsigned char *cursor = bufPtrConst(firstChunk);
            size_t available = bufUsed(firstChunk);
            uint32_t pageNo = 0;

            for (;;)
            {
                while (available >= pageSize)
                {
                    // Skip non-validatable pages (compressed/encrypted have different layouts)
                    if (!mysqlPageIsValidatable(cursor))
                    {
                        result.pagesSkipped++;
                    }
                    else
                    {
                        const bool ok = mysqlPageChecksumValidate(cursor, pageSize, algo, pageNo);

                        if (ok)
                            result.pagesValid++;
                        else
                            result.pagesInvalid++;
                    }

                    result.pagesChecked++;
                    cursor += pageSize;
                    available -= pageSize;
                    pageNo++;
                }

                // Refill the buffer — shift leftover (sub-page) to front, then read more
                if (available > 0)
                {
                    memmove(bufPtr(firstChunk), cursor, available);
                }

                bufUsedSet(firstChunk, available);
                bufLimitSet(firstChunk, bufSize(firstChunk));

                ioRead(storageReadIo(read), firstChunk);

                if (bufUsed(firstChunk) == available)                   // No new bytes — EOF
                    break;

                cursor = bufPtrConst(firstChunk);
                available = bufUsed(firstChunk);
            }

            // A trailing partial page is silent — InnoDB files are always page-size aligned in practice; if we see a leftover
            // it usually means a torn write at the tail which downstream redo recovery will sort out.
            if (available > 0)
                LOG_DETAIL_FMT("page-validate: '%s' trailing %zu bytes (sub-page); ignored", strZ(filePath), available);
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_PAGE_VALIDATE_RESULT, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlPageValidateResultToLog(const MysqlPageValidateResult *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog,
        "{checked: %" PRIu64 ", valid: %" PRIu64 ", invalid: %" PRIu64 ", skipped: %" PRIu64 ", algo: %u, pageSize: %u}",
        this->pagesChecked, this->pagesValid, this->pagesInvalid, this->pagesSkipped,
        (unsigned int)this->algo, (unsigned int)this->pageSize);
}
