/***********************************************************************************************************************************
Standalone test for src/mysql/pageChecksumFilter.c

Feeds a synthetic stream of InnoDB pages through the filter and verifies it counts checked / valid / invalid / skipped correctly.
Pages are built with a real CRC32 checksum (matching the algorithm in src/mysql/interface.c).
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/io/bufferRead.h"
#include "common/io/bufferWrite.h"
#include "common/io/filter/filter.h"
#include "common/io/filter/group.h"
#include "common/io/io.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "common/type/buffer.h"
#include "mysql/interface.h"
#include "mysql/pageChecksumFilter.h"

static int testFailures = 0;
static void expect(const char *w, bool ok) { printf("  %s  %s\n", ok ? "PASS" : "FAIL", w); if (!ok) testFailures++; }

static void
stampValidPage(unsigned char *const page, const uint32_t pageNo)
{
    const size_t pageSize = mysqlPageSize16K;
    memset(page, 0, pageSize);

    page[FIL_PAGE_OFFSET + 0] = (unsigned char)((pageNo >> 24) & 0xFF);
    page[FIL_PAGE_OFFSET + 1] = (unsigned char)((pageNo >> 16) & 0xFF);
    page[FIL_PAGE_OFFSET + 2] = (unsigned char)((pageNo >> 8) & 0xFF);
    page[FIL_PAGE_OFFSET + 3] = (unsigned char)(pageNo & 0xFF);

    page[FIL_PAGE_LSN + 7] = (unsigned char)(0x80 + pageNo);

    const size_t trailerOff = pageSize - FIL_PAGE_TRAILER_SIZE;
    page[trailerOff + 7] = page[FIL_PAGE_LSN + 7];

    for (size_t i = FIL_PAGE_DATA; i < pageSize - FIL_PAGE_TRAILER_SIZE; i++)
        page[i] = (unsigned char)(((i + pageNo) * 7) & 0xFF);

    const uint32_t c1 = (uint32_t)crc32(0, page + FIL_PAGE_OFFSET, FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
    const uint32_t c2 = (uint32_t)crc32(0, page + FIL_PAGE_DATA, (uInt)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));
    const uint32_t expected = c1 ^ c2;

    page[0] = (unsigned char)((expected >> 24) & 0xFF);
    page[1] = (unsigned char)((expected >> 16) & 0xFF);
    page[2] = (unsigned char)((expected >> 8) & 0xFF);
    page[3] = (unsigned char)(expected & 0xFF);
}

int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));
    logInit(logLevelOff, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Page-checksum filter tests:\n");

        const size_t pageSize = mysqlPageSize16K;
        const unsigned int pageCount = 4;

        // ---- Test 1: all-valid stream ----
        unsigned char *const buf = (unsigned char *)calloc(pageCount, pageSize);
        for (unsigned int i = 0; i < pageCount; i++) stampValidPage(buf + i * pageSize, i);

        Buffer *const input = bufNewC(buf, pageCount * pageSize);
        Buffer *const output = bufNew(pageCount * pageSize);

        IoFilter *const filter = mysqlPageChecksumFilterNew(pageSize, mysqlPageChecksumCrc32);

        IoWrite *const writer = ioBufferWriteNew(output);
        ioFilterGroupAdd(ioWriteFilterGroup(writer), filter);
        ioWriteOpen(writer);
        ioWrite(writer, input);
        ioWriteClose(writer);

        const Pack *const pack = ioFilterGroupResultPackP(ioWriteFilterGroup(writer), MY_PAGE_CHECKSUM_FILTER_TYPE);
        MysqlPageChecksumFilterStats stats = mysqlPageChecksumFilterStatsFromPack(pack);

        expect("all-valid: 4 pages checked", stats.pagesChecked == 4);
        expect("all-valid: 4 valid", stats.pagesValid == 4);
        expect("all-valid: 0 invalid", stats.pagesInvalid == 0);
        expect("all-valid: pass-through preserved content", bufEq(input, output));

        // ---- Test 2: corruption on page 2 ----
        for (unsigned int i = 0; i < pageCount; i++) stampValidPage(buf + i * pageSize, i);
        buf[2 * pageSize + 100] ^= 0xFF;

        Buffer *const input2 = bufNewC(buf, pageCount * pageSize);
        Buffer *const output2 = bufNew(pageCount * pageSize);
        IoFilter *const filter2 = mysqlPageChecksumFilterNew(pageSize, mysqlPageChecksumCrc32);
        IoWrite *const writer2 = ioBufferWriteNew(output2);
        ioFilterGroupAdd(ioWriteFilterGroup(writer2), filter2);
        ioWriteOpen(writer2);
        ioWrite(writer2, input2);
        ioWriteClose(writer2);

        const Pack *const pack2 = ioFilterGroupResultPackP(ioWriteFilterGroup(writer2), MY_PAGE_CHECKSUM_FILTER_TYPE);
        MysqlPageChecksumFilterStats stats2 = mysqlPageChecksumFilterStatsFromPack(pack2);

        expect("corrupt: 4 checked", stats2.pagesChecked == 4);
        expect("corrupt: 3 valid", stats2.pagesValid == 3);
        expect("corrupt: 1 invalid", stats2.pagesInvalid == 1);

        // ---- Test 3: compressed-page (type 14) → skipped ----
        for (unsigned int i = 0; i < pageCount; i++) stampValidPage(buf + i * pageSize, i);
        // Mark page 1 as FIL_PAGE_TYPE_COMPRESSED (14)
        buf[1 * pageSize + FIL_PAGE_TYPE]     = 0x00;
        buf[1 * pageSize + FIL_PAGE_TYPE + 1] = 14;

        Buffer *const input3 = bufNewC(buf, pageCount * pageSize);
        Buffer *const output3 = bufNew(pageCount * pageSize);
        IoFilter *const filter3 = mysqlPageChecksumFilterNew(pageSize, mysqlPageChecksumCrc32);
        IoWrite *const writer3 = ioBufferWriteNew(output3);
        ioFilterGroupAdd(ioWriteFilterGroup(writer3), filter3);
        ioWriteOpen(writer3);
        ioWrite(writer3, input3);
        ioWriteClose(writer3);

        const Pack *const pack3 = ioFilterGroupResultPackP(ioWriteFilterGroup(writer3), MY_PAGE_CHECKSUM_FILTER_TYPE);
        MysqlPageChecksumFilterStats stats3 = mysqlPageChecksumFilterStatsFromPack(pack3);

        expect("compressed page: 4 checked", stats3.pagesChecked == 4);
        expect("compressed page: >=1 skipped", stats3.pagesSkipped >= 1);

        // ---- Test 4: split-buffer input — feed in tiny chunks to exercise the partial-page accumulator ----
        for (unsigned int i = 0; i < pageCount; i++) stampValidPage(buf + i * pageSize, i);

        IoFilter *const filter4 = mysqlPageChecksumFilterNew(pageSize, mysqlPageChecksumCrc32);
        Buffer *const output4 = bufNew(pageCount * pageSize);
        IoWrite *const writer4 = ioBufferWriteNew(output4);
        ioFilterGroupAdd(ioWriteFilterGroup(writer4), filter4);
        ioWriteOpen(writer4);

        // Feed 1023-byte chunks — deliberately not page-aligned
        const size_t chunkSize = 1023;
        for (size_t off = 0; off < pageCount * pageSize; off += chunkSize)
        {
            const size_t take = (off + chunkSize > pageCount * pageSize) ? (pageCount * pageSize - off) : chunkSize;
            Buffer *const chunk = bufNewC(buf + off, take);
            ioWrite(writer4, chunk);
        }
        ioWriteClose(writer4);

        const Pack *const pack4 = ioFilterGroupResultPackP(ioWriteFilterGroup(writer4), MY_PAGE_CHECKSUM_FILTER_TYPE);
        MysqlPageChecksumFilterStats stats4 = mysqlPageChecksumFilterStatsFromPack(pack4);

        expect("split-buffer: 4 checked", stats4.pagesChecked == 4);
        expect("split-buffer: 4 valid", stats4.pagesValid == 4);

        free(buf);
    }
    CATCH_FATAL() { printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace()); rc = 1; }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); return 1; }
    printf("\nAll assertions passed\n");
    return rc;
}
