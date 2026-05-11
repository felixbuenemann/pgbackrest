/***********************************************************************************************************************************
InnoDB Page Checksum IoFilter
***********************************************************************************************************************************/
#include <build.h>

#include "common/debug.h"
#include "common/io/filter/filter.h"
#include "common/io/filter/filter.intern.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/type/object.h"
#include "common/type/pack.h"
#include "mysql/interface.h"
#include "mysql/pageChecksumFilter.h"

/***********************************************************************************************************************************
Filter state. Input arrives in arbitrarily-sized chunks; we accumulate into a single pageBuf big enough for one full page +
whatever tail bytes from the previous call need to be re-prepended. When we have a complete page, validate + drop it.
***********************************************************************************************************************************/
typedef struct MysqlPageChecksumFilter
{
    MysqlPageSize pageSize;
    MysqlPageChecksumAlgo algo;

    unsigned char *pageBuf;                                             // pageSize-sized scratch buffer
    size_t buffered;                                                    // bytes currently sitting in pageBuf
    uint32_t pageNo;                                                    // next page number to validate

    uint64_t pagesChecked;
    uint64_t pagesValid;
    uint64_t pagesInvalid;
    uint64_t pagesSkipped;
} MysqlPageChecksumFilter;

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
static void
mysqlPageChecksumFilterToLog(const MysqlPageChecksumFilter *const this, StringStatic *const debugLog)
{
    strStcFmt(
        debugLog,
        "{pageSize: %u, algo: %u, checked: %" PRIu64 ", valid: %" PRIu64 ", invalid: %" PRIu64 ", skipped: %" PRIu64 "}",
        (unsigned int)this->pageSize, (unsigned int)this->algo,
        this->pagesChecked, this->pagesValid, this->pagesInvalid, this->pagesSkipped);
}

#define FUNCTION_LOG_MY_PAGE_CHECKSUM_FILTER_TYPE                                                                                  \
    MysqlPageChecksumFilter *
#define FUNCTION_LOG_MY_PAGE_CHECKSUM_FILTER_FORMAT(value, buffer, bufferSize)                                                     \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlPageChecksumFilterToLog, buffer, bufferSize)

/***********************************************************************************************************************************
Process a single complete page: classify + count
***********************************************************************************************************************************/
static void
mysqlPageChecksumFilterValidatePage(MysqlPageChecksumFilter *const this)
{
    if (!mysqlPageIsValidatable(this->pageBuf))
    {
        this->pagesSkipped++;
    }
    else
    {
        const bool ok = mysqlPageChecksumValidate(this->pageBuf, this->pageSize, this->algo, this->pageNo);

        if (ok)
            this->pagesValid++;
        else
            this->pagesInvalid++;
    }

    this->pagesChecked++;
    this->pageNo++;
}

/***********************************************************************************************************************************
Filter input handler — accumulate bytes, validate complete pages
***********************************************************************************************************************************/
static void
mysqlPageChecksumFilterProcess(THIS_VOID, const Buffer *const input)
{
    THIS(MysqlPageChecksumFilter);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(MY_PAGE_CHECKSUM_FILTER, this);
        FUNCTION_LOG_PARAM(BUFFER, input);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);
    ASSERT(input != NULL);

    const unsigned char *cursor = bufPtrConst(input);
    size_t remaining = bufUsed(input);

    while (remaining > 0)
    {
        const size_t need = (size_t)this->pageSize - this->buffered;
        const size_t take = remaining < need ? remaining : need;

        memcpy(this->pageBuf + this->buffered, cursor, take);
        this->buffered += take;
        cursor += take;
        remaining -= take;

        if (this->buffered == (size_t)this->pageSize)
        {
            mysqlPageChecksumFilterValidatePage(this);
            this->buffered = 0;
        }
    }

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Filter result — emit the four counts as a Pack
***********************************************************************************************************************************/
static Pack *
mysqlPageChecksumFilterResult(THIS_VOID)
{
    THIS(MysqlPageChecksumFilter);

    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(MY_PAGE_CHECKSUM_FILTER, this);
    FUNCTION_LOG_END();

    ASSERT(this != NULL);

    Pack *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        PackWrite *const packWrite = pckWriteNewP();

        pckWriteU64P(packWrite, this->pagesChecked);
        pckWriteU64P(packWrite, this->pagesValid);
        pckWriteU64P(packWrite, this->pagesInvalid);
        pckWriteU64P(packWrite, this->pagesSkipped);
        pckWriteEndP(packWrite);

        result = pckMove(pckWriteResult(packWrite), memContextPrior());
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(PACK, result);
}

/**********************************************************************************************************************************/
FN_EXTERN IoFilter *
mysqlPageChecksumFilterNew(const MysqlPageSize pageSize, const MysqlPageChecksumAlgo algo)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(UINT, pageSize);
        FUNCTION_LOG_PARAM(UINT, algo);
    FUNCTION_LOG_END();

    ASSERT(pageSize >= mysqlPageSize4K && pageSize <= mysqlPageSize64K);

    OBJ_NEW_BEGIN(MysqlPageChecksumFilter, .childQty = MEM_CONTEXT_QTY_MAX, .allocQty = MEM_CONTEXT_QTY_MAX)
    {
        *this = (MysqlPageChecksumFilter)
        {
            .pageSize = pageSize,
            .algo = algo,
            .pageBuf = memNew((size_t)pageSize),
        };
    }
    OBJ_NEW_END();

    FUNCTION_LOG_RETURN(
        IO_FILTER,
        ioFilterNewP(
            MY_PAGE_CHECKSUM_FILTER_TYPE, this, NULL,
            .in = mysqlPageChecksumFilterProcess, .result = mysqlPageChecksumFilterResult));
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlPageChecksumFilterStats
mysqlPageChecksumFilterStatsFromPack(const Pack *const pack)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(PACK, pack);
    FUNCTION_LOG_END();

    MysqlPageChecksumFilterStats result = {0};

    if (pack != NULL)
    {
        PackRead *const read = pckReadNew(pack);

        result.pagesChecked = (uint64_t)pckReadU64P(read);
        result.pagesValid = (uint64_t)pckReadU64P(read);
        result.pagesInvalid = (uint64_t)pckReadU64P(read);
        result.pagesSkipped = (uint64_t)pckReadU64P(read);
    }

    FUNCTION_LOG_RETURN_STRUCT(result);
}
