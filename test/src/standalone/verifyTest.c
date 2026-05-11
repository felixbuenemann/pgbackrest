/***********************************************************************************************************************************
Standalone test for src/mysql/verify.c — mysqlVerify

Builds a synthetic datadir with multiple InnoDB tablespaces (ibdata1 + a per-schema .ibd file) and verifies the walker enumerates
all of them, validates every page, and reports aggregate counts + the corrupt-files list.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "common/type/stringList.h"
#include "mysql/interface.h"
#include "mysql/verify.h"
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

// Same stamping logic as pageValidateTest — keeps the test self-contained
static void
stampValidPage(unsigned char *const page, const uint32_t pageNo)
{
    const size_t pageSize = mysqlPageSize16K;
    memset(page, 0, pageSize);

    page[FIL_PAGE_OFFSET + 0] = (unsigned char)((pageNo >> 24) & 0xFF);
    page[FIL_PAGE_OFFSET + 1] = (unsigned char)((pageNo >> 16) & 0xFF);
    page[FIL_PAGE_OFFSET + 2] = (unsigned char)((pageNo >> 8) & 0xFF);
    page[FIL_PAGE_OFFSET + 3] = (unsigned char)(pageNo & 0xFF);

    // FIL_PAGE_LSN — pick a per-page value
    page[FIL_PAGE_LSN + 7] = (unsigned char)(0x80 + pageNo);

    const size_t trailerOff = pageSize - FIL_PAGE_TRAILER_SIZE;
    page[trailerOff + 7] = page[FIL_PAGE_LSN + 7];

    for (size_t i = FIL_PAGE_DATA; i < pageSize - FIL_PAGE_TRAILER_SIZE; i++)
        page[i] = (unsigned char)(((i + pageNo) * 7) & 0xFF);

    if (pageNo == 0)
        memset(page + FIL_PAGE_DATA, 0, 24);                            // zero FSP header bytes for page 0

    const uint32_t c1 = (uint32_t)crc32(0, page + FIL_PAGE_OFFSET, FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
    const uint32_t c2 = (uint32_t)crc32(0, page + FIL_PAGE_DATA, (uInt)(pageSize - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE));
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

static void
writeIbd(const char *const path, const unsigned int pageCount, const bool corruptPage)
{
    const size_t pageSize = mysqlPageSize16K;
    unsigned char *const buf = (unsigned char *)calloc(pageCount, pageSize);

    for (unsigned int i = 0; i < pageCount; i++)
        stampValidPage(buf + i * pageSize, i);

    if (corruptPage && pageCount > 1)
        buf[1 * pageSize + 200] ^= 0xFF;                                // flip a byte on page 1

    writeFile(path, buf, pageCount * pageSize);
    free(buf);
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
        printf("Datadir verifier tests:\n");

        const char *const tmpDir = "/tmp/mybackrest-verify-test";
        rmRf(tmpDir);
        mkdir(tmpDir, 0755);

        // ---- Test 1: clean datadir ----
        // ibdata1 (3 pages) + myschema/users.ibd (4 pages, all valid)
        writeIbd("/tmp/mybackrest-verify-test/ibdata1", 3, /*corrupt*/ false);

        mkdir("/tmp/mybackrest-verify-test/myschema", 0755);
        writeIbd("/tmp/mybackrest-verify-test/myschema/users.ibd", 4, /*corrupt*/ false);

        const Storage *const storage = storagePosixNewP(STR(tmpDir));

        MysqlVerifyResult *const r1 = mysqlVerify(storage, STRDEF("."));
        expect("clean: 2 files scanned", r1->filesScanned == 2);
        expect("clean: 7 pages checked", r1->pagesChecked == 7);
        expect("clean: 7 pages valid", r1->pagesValid == 7);
        expect("clean: 0 invalid", r1->pagesInvalid == 0);
        expect("clean: no invalid files in list", strLstSize(r1->invalidFiles) == 0);

        // ---- Test 2: one corrupt page in users.ibd ----
        writeIbd("/tmp/mybackrest-verify-test/myschema/users.ibd", 4, /*corrupt*/ true);

        MysqlVerifyResult *const r2 = mysqlVerify(storage, STRDEF("."));
        expect("corrupt: 2 files scanned", r2->filesScanned == 2);
        expect("corrupt: 7 pages checked", r2->pagesChecked == 7);
        expect("corrupt: 6 valid", r2->pagesValid == 6);
        expect("corrupt: 1 invalid", r2->pagesInvalid == 1);
        expect("corrupt: 1 file in invalid list", strLstSize(r2->invalidFiles) == 1);
        if (strLstSize(r2->invalidFiles) >= 1)
        {
            const String *const f = strLstGet(r2->invalidFiles, 0);
            expect("corrupt: invalid file is myschema/users.ibd", strstr(strZ(f), "users.ibd") != NULL);
        }

        // ---- Test 3: empty datadir → 0 files, 0 pages, no error ----
        rmRf(tmpDir);
        mkdir(tmpDir, 0755);

        MysqlVerifyResult *const r3 = mysqlVerify(storage, STRDEF("."));
        expect("empty: 0 files scanned", r3->filesScanned == 0);
        expect("empty: 0 pages checked", r3->pagesChecked == 0);
        expect("empty: no invalid files", strLstSize(r3->invalidFiles) == 0);

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
