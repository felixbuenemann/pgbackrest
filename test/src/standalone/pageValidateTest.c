/***********************************************************************************************************************************
Standalone test for src/mysql/pageValidate.c — mysqlPageValidateFile

Builds synthetic .ibd files page-by-page with valid CRC32 checksums (matching the InnoDB algorithm), runs the validator, and
verifies the counts. Also covers single-corrupt-page detection and empty-file handling.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/crc32c.h"
#include "mysql/interface.h"
#include "mysql/pageValidate.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static void
rmRf(const char *const path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    if (system(cmd) != 0) { /* best-effort */ }
}

/***********************************************************************************************************************************
Stamp a 16K page in-place with a valid CRC32 checksum for the given pageNo. Returns the buffer (already allocated).
***********************************************************************************************************************************/
static void
stampValidPage(unsigned char *const page, const uint32_t pageNo, const uint32_t lsnHigh, const uint32_t lsnLow)
{
    const size_t pageSize = mysqlPageSize16K;

    memset(page, 0, pageSize);

    // FIL_PAGE_OFFSET (4..7) — page number, big-endian
    page[FIL_PAGE_OFFSET + 0] = (unsigned char)((pageNo >> 24) & 0xFF);
    page[FIL_PAGE_OFFSET + 1] = (unsigned char)((pageNo >> 16) & 0xFF);
    page[FIL_PAGE_OFFSET + 2] = (unsigned char)((pageNo >> 8) & 0xFF);
    page[FIL_PAGE_OFFSET + 3] = (unsigned char)(pageNo & 0xFF);

    // FIL_PAGE_LSN (16..23)
    page[FIL_PAGE_LSN + 0] = (unsigned char)((lsnHigh >> 24) & 0xFF);
    page[FIL_PAGE_LSN + 1] = (unsigned char)((lsnHigh >> 16) & 0xFF);
    page[FIL_PAGE_LSN + 2] = (unsigned char)((lsnHigh >> 8) & 0xFF);
    page[FIL_PAGE_LSN + 3] = (unsigned char)(lsnHigh & 0xFF);
    page[FIL_PAGE_LSN + 4] = (unsigned char)((lsnLow >> 24) & 0xFF);
    page[FIL_PAGE_LSN + 5] = (unsigned char)((lsnLow >> 16) & 0xFF);
    page[FIL_PAGE_LSN + 6] = (unsigned char)((lsnLow >> 8) & 0xFF);
    page[FIL_PAGE_LSN + 7] = (unsigned char)(lsnLow & 0xFF);

    // Trailer LSN low half (last 4 bytes) — must mirror FIL_PAGE_LSN low half
    const size_t trailerOff = pageSize - FIL_PAGE_TRAILER_SIZE;
    page[trailerOff + 4] = page[FIL_PAGE_LSN + 4];
    page[trailerOff + 5] = page[FIL_PAGE_LSN + 5];
    page[trailerOff + 6] = page[FIL_PAGE_LSN + 6];
    page[trailerOff + 7] = page[FIL_PAGE_LSN + 7];

    // Some recognizable data in the body
    for (size_t i = FIL_PAGE_DATA; i < pageSize - FIL_PAGE_TRAILER_SIZE; i++)
        page[i] = (unsigned char)(((i + pageNo) * 7) & 0xFF);

    // Page 0 carries the FSP_HEADER — leave FSP_SPACE_ID (offset 38) and FSP_SPACE_FLAGS (offset 54) zeroed so it parses as a
    // consistent header (FIL_PAGE_SPACE_ID @ 34 is also zero from the calloc). Flags=0 → 16K page size.
    if (pageNo == 0)
    {
        memset(page + FIL_PAGE_DATA, 0, 24);                            // bytes 38..61 — covers FSP_SPACE_ID, FSP_SIZE, FREE_LIMIT, FSP_SPACE_FLAGS, +pad
    }

    // Compute and stamp CRC32 at offset 0..3 (big-endian)
    const uint32_t c1 = mysqlCrc32c(0, page + FIL_PAGE_OFFSET, FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
    const uint32_t c2 = mysqlCrc32c(0, page + FIL_PAGE_DATA, (size_t)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));
    const uint32_t expected = c1 ^ c2;

    page[0] = (unsigned char)((expected >> 24) & 0xFF);
    page[1] = (unsigned char)((expected >> 16) & 0xFF);
    page[2] = (unsigned char)((expected >> 8) & 0xFF);
    page[3] = (unsigned char)(expected & 0xFF);
}

static void
writeFile(const char *const path, const void *const data, const size_t size)
{
    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen failed: %s", path);
    if (fwrite(data, 1, size, fp) != size) THROW(FileWriteError, "fwrite short");
    fclose(fp);
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
        printf("Page validator tests:\n");

        const char *const tmpDir = "/tmp/mybackrest-pagevalidate-test";
        rmRf(tmpDir);
        mkdir(tmpDir, 0755);

        const Storage *const storage = storagePosixNewP(STR(tmpDir));

        const size_t pageSize = mysqlPageSize16K;
        const unsigned int pageCount = 5;
        unsigned char *const buf = (unsigned char *)calloc(pageCount, pageSize);

        // ---- Test 1: all-valid 5-page file ----
        for (unsigned int i = 0; i < pageCount; i++)
            stampValidPage(buf + i * pageSize, i, /*lsnHigh*/ 1, /*lsnLow*/ (uint32_t)(0x10000 + i));

        writeFile("/tmp/mybackrest-pagevalidate-test/valid.ibd", buf, pageCount * pageSize);

        MysqlPageValidateResult r1 = mysqlPageValidateFile(storage, STRDEF("valid.ibd"), 0);
        expect("valid file: 5 pages checked", r1.pagesChecked == 5);
        expect("valid file: 5 pages valid", r1.pagesValid == 5);
        expect("valid file: 0 invalid", r1.pagesInvalid == 0);
        expect("valid file: 0 skipped", r1.pagesSkipped == 0);
        expect("valid file: detected pageSize=16K", r1.pageSize == mysqlPageSize16K);

        // ---- Test 2: one corrupted page (flip a body byte on page 2) ----
        for (unsigned int i = 0; i < pageCount; i++)
            stampValidPage(buf + i * pageSize, i, 1, (uint32_t)(0x10000 + i));

        buf[2 * pageSize + 100] ^= 0xFF;                                // flip a byte in page 2's data area

        writeFile("/tmp/mybackrest-pagevalidate-test/corrupt.ibd", buf, pageCount * pageSize);

        MysqlPageValidateResult r2 = mysqlPageValidateFile(storage, STRDEF("corrupt.ibd"), 0);
        expect("corrupt file: 5 pages checked", r2.pagesChecked == 5);
        expect("corrupt file: 4 pages valid", r2.pagesValid == 4);
        expect("corrupt file: 1 invalid", r2.pagesInvalid == 1);

        // ---- Test 3: empty file ----
        writeFile("/tmp/mybackrest-pagevalidate-test/empty.ibd", "", 0);

        MysqlPageValidateResult r3 = mysqlPageValidateFile(storage, STRDEF("empty.ibd"), 0);
        expect("empty file: 0 pages checked", r3.pagesChecked == 0);

        // ---- Test 4: pageSize hint provided ----
        MysqlPageValidateResult r4 = mysqlPageValidateFile(storage, STRDEF("valid.ibd"), mysqlPageSize16K);
        expect("with hint: 5 pages checked", r4.pagesChecked == 5);
        expect("with hint: 5 pages valid", r4.pagesValid == 5);

        // ---- Test 5: compressed page (type 14) is skipped ----
        for (unsigned int i = 0; i < pageCount; i++)
            stampValidPage(buf + i * pageSize, i, 1, (uint32_t)(0x10000 + i));

        // Mark page 3 as FIL_PAGE_TYPE_COMPRESSED (type=14, 2 bytes big-endian @ offset 24)
        buf[3 * pageSize + FIL_PAGE_TYPE]     = 0x00;
        buf[3 * pageSize + FIL_PAGE_TYPE + 1] = 14;

        writeFile("/tmp/mybackrest-pagevalidate-test/compressed.ibd", buf, pageCount * pageSize);

        MysqlPageValidateResult r5 = mysqlPageValidateFile(storage, STRDEF("compressed.ibd"), 0);
        expect("with compressed page: 5 checked", r5.pagesChecked == 5);
        expect("with compressed page: >=1 skipped", r5.pagesSkipped >= 1);
        expect("with compressed page: <=4 valid", r5.pagesValid <= 4);

        free(buf);
        rmRf(tmpDir);
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
        return 1;
    }

    printf("\nAll assertions passed\n");
    return rc;
}
