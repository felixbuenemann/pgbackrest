/***********************************************************************************************************************************
Standalone test for src/mysql/interface.c — mysqlPageChecksumValidate

Builds two synthetic 16K InnoDB pages:

  1. A "valid" page whose CRC32 (computed via zlib) matches the FIL_PAGE_SPACE_OR_CHKSUM field (offset 0..3, big-endian) and
     whose trailer's LSN low half matches FIL_PAGE_LSN's low half. mysqlPageChecksumValidate(crc32) should return true.

  2. An "invalid" page identical to (1) but with one byte in the data area flipped. The CRC mismatch should trigger
     mysqlPageChecksumValidate(crc32) → false.

Plus a "page-no mismatch" case: ask the validator about page N when the buffer says page N+1.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/interface.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

/***********************************************************************************************************************************
Build a 16K InnoDB page with a synthetic LSN and a CRC32 checksum that the validator will accept.
***********************************************************************************************************************************/
static unsigned char *
buildValidPage(const uint32_t pageNo, const uint32_t lsnHigh, const uint32_t lsnLow)
{
    const size_t pageSize = mysqlPageSize16K;
    unsigned char *const page = (unsigned char *)calloc(1, pageSize);

    if (page == NULL)
        return NULL;

    // FIL_PAGE_OFFSET (4..7) — page number in big-endian
    page[FIL_PAGE_OFFSET + 0] = (unsigned char)((pageNo >> 24) & 0xFF);
    page[FIL_PAGE_OFFSET + 1] = (unsigned char)((pageNo >> 16) & 0xFF);
    page[FIL_PAGE_OFFSET + 2] = (unsigned char)((pageNo >> 8) & 0xFF);
    page[FIL_PAGE_OFFSET + 3] = (unsigned char)(pageNo & 0xFF);

    // FIL_PAGE_LSN (16..23) — high 4 bytes + low 4 bytes, big-endian
    page[FIL_PAGE_LSN + 0] = (unsigned char)((lsnHigh >> 24) & 0xFF);
    page[FIL_PAGE_LSN + 1] = (unsigned char)((lsnHigh >> 16) & 0xFF);
    page[FIL_PAGE_LSN + 2] = (unsigned char)((lsnHigh >> 8) & 0xFF);
    page[FIL_PAGE_LSN + 3] = (unsigned char)(lsnHigh & 0xFF);
    page[FIL_PAGE_LSN + 4] = (unsigned char)((lsnLow >> 24) & 0xFF);
    page[FIL_PAGE_LSN + 5] = (unsigned char)((lsnLow >> 16) & 0xFF);
    page[FIL_PAGE_LSN + 6] = (unsigned char)((lsnLow >> 8) & 0xFF);
    page[FIL_PAGE_LSN + 7] = (unsigned char)(lsnLow & 0xFF);

    // Trailer LSN low half must match FIL_PAGE_LSN low half (torn-page detector)
    const size_t trailerOff = pageSize - FIL_PAGE_TRAILER_SIZE;
    page[trailerOff + 4] = page[FIL_PAGE_LSN + 4];
    page[trailerOff + 5] = page[FIL_PAGE_LSN + 5];
    page[trailerOff + 6] = page[FIL_PAGE_LSN + 6];
    page[trailerOff + 7] = page[FIL_PAGE_LSN + 7];

    // Sprinkle some recognizable data so the page isn't all zeros
    for (size_t i = FIL_PAGE_DATA; i < pageSize - FIL_PAGE_TRAILER_SIZE; i++)
        page[i] = (unsigned char)((i * 7) & 0xFF);

    // Compute the InnoDB CRC32: c1 = crc32(page[4..25]), c2 = crc32(page[38..pageSize-9]); store BE at page[0..3]
    const uint32_t c1 = (uint32_t)crc32(0, page + FIL_PAGE_OFFSET, FIL_PAGE_LSN - FIL_PAGE_OFFSET);
    const uint32_t c2 = (uint32_t)crc32(0, page + FIL_PAGE_DATA, (uInt)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));
    const uint32_t expected = c1 ^ c2;

    page[0] = (unsigned char)((expected >> 24) & 0xFF);
    page[1] = (unsigned char)((expected >> 16) & 0xFF);
    page[2] = (unsigned char)((expected >> 8) & 0xFF);
    page[3] = (unsigned char)(expected & 0xFF);

    return page;
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));

    logInit(logLevelWarn, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Page checksum validation tests:\n");

        // ---- Test 1: valid CRC32 page ----
        unsigned char *const goodPage = buildValidPage(/*pageNo*/42, /*lsnHigh*/0x01020304, /*lsnLow*/0xDEADBEEF);
        const bool goodResult = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumCrc32, /*pageNo*/42);
        expect("valid CRC32 page accepted", goodResult);

        // ---- Test 2: same page, CRC algorithm strict variant (should also accept) ----
        const bool strictResult = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumStrictCrc32, 42);
        expect("strict-CRC32 path accepts the same page", strictResult);

        // ---- Test 3: corrupt one data byte → CRC mismatch ----
        goodPage[1000] ^= 0xFF;
        const bool corruptResult = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumCrc32, 42);
        expect("corrupted page rejected (CRC mismatch)", !corruptResult);
        goodPage[1000] ^= 0xFF;                                         // un-corrupt for next test

        // ---- Test 4: torn page detection (trailer LSN low ≠ header LSN low) ----
        goodPage[mysqlPageSize16K - 1] ^= 0xFF;
        const bool tornResult = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumCrc32, 42);
        expect("torn page rejected (trailer-LSN mismatch)", !tornResult);
        goodPage[mysqlPageSize16K - 1] ^= 0xFF;

        // ---- Test 5: page-no caller mismatch ----
        const bool wrongPageNo = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumCrc32, /*ask for*/99);
        expect("wrong page-no rejected", !wrongPageNo);

        // ---- Test 6: pageChecksumNone always accepts ----
        const bool noneResult = mysqlPageChecksumValidate(goodPage, mysqlPageSize16K, mysqlPageChecksumNone, 42);
        expect("checksum=none always accepts", noneResult);

        free(goodPage);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0)
    {
        printf("\n%d assertion(s) failed\n", testFailures);
        rc = 1;
    }
    else if (rc == 0)
    {
        printf("\nAll assertions passed\n");
    }

    return rc;
}
