/***********************************************************************************************************************************
Standalone test for src/mysql/interface.c — mysqlControlFromIbdata against a synthetic ibdata1

Builds a 16K-page ibdata1 with the FSP_HEADER fields the parser reads:
  - FIL_PAGE_SPACE_ID @ 34       (4B big-endian) = 0
  - FSP_SPACE_ID @ FIL_PAGE_DATA + 0   = 0  (must agree with FIL_PAGE_SPACE_ID)
  - FSP_SPACE_FLAGS @ FIL_PAGE_DATA + 16  encodes page_ssize for the desired page size

Tests every supported page size (4K/8K/16K/32K/64K) plus the layout-detection (mysql.ibd absent → 5.7; present → 8.0+).
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
#include "common/type/buffer.h"
#include "mysql/interface.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

static void mkdirP(const char *const p) { mkdir(p, 0755); }
static void rmrf(const char *const p) { char c[1024]; snprintf(c, sizeof(c), "rm -rf '%s'", p); int u __attribute__((unused)) = system(c); }

/***********************************************************************************************************************************
Build a 16K page 0 with the given page_ssize encoded in FSP_SPACE_FLAGS bits 6..9.

ssize values per the InnoDB convention:
  0 = legacy 16K (default when --innodb-page-size not specified)
  1 = 4K, 2 = 8K, 3 = 16K, 4 = 32K, 5 = 64K
***********************************************************************************************************************************/
static unsigned char *
buildIbdata(const unsigned int ssize)
{
    const size_t pageSize = 16384;                                      // page 0 itself is always one page; size doesn't matter for the parser
    unsigned char *const page = (unsigned char *)calloc(1, pageSize);

    // FIL_PAGE_SPACE_ID = 0 (system tablespace), big-endian 4 bytes at offset 34
    // (already zero from calloc)

    // FSP_SPACE_ID at FIL_PAGE_DATA + 0 = offset 38, also = 0
    // (already zero)

    // FSP_SPACE_FLAGS at FIL_PAGE_DATA + 16 = offset 54, big-endian 4 bytes
    // bits 6..9 hold page_ssize
    const uint32_t flags = (ssize & 0xF) << 6;
    page[FSP_SPACE_FLAGS + 0] = (unsigned char)((flags >> 24) & 0xFF);
    page[FSP_SPACE_FLAGS + 1] = (unsigned char)((flags >> 16) & 0xFF);
    page[FSP_SPACE_FLAGS + 2] = (unsigned char)((flags >> 8) & 0xFF);
    page[FSP_SPACE_FLAGS + 3] = (unsigned char)(flags & 0xFF);

    return page;
}

static void
writePage(const char *const path, const unsigned char *const page)
{
    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fwrite(page, 1, 16384, fp);
    fclose(fp);
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));
    logInit(logLevelOff, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("ibdata1 / mysql.ibd parser test:\n");

        const char *const root = "/tmp/mybackrest-ibdata-test";
        rmrf(root);
        mkdirP(root);

        const Storage *const storage = storagePosixNewP(STR(root));

        // Page-size decoding: build ibdata1 with various ssize values
        const struct { unsigned int ssize; MysqlPageSize expected; const char *name; } cases[] = {
            {0, mysqlPageSize16K, "ssize=0 (legacy default)"},
            {1, mysqlPageSize4K,  "ssize=1 (4K)"},
            {2, mysqlPageSize8K,  "ssize=2 (8K)"},
            {3, mysqlPageSize16K, "ssize=3 (16K)"},
            {4, mysqlPageSize32K, "ssize=4 (32K)"},
            {5, mysqlPageSize64K, "ssize=5 (64K)"},
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        {
            unsigned char *const page = buildIbdata(cases[i].ssize);
            writePage("/tmp/mybackrest-ibdata-test/ibdata1", page);
            free(page);

            MysqlControl ctl = mysqlControlFromIbdata(storage, STRDEF("."));
            expect(cases[i].name, ctl.pageSize == cases[i].expected);
        }

        // Layout flavor: 5.7 (no mysql.ibd) vs 8.0+ (mysql.ibd present)
        unsigned char *const page = buildIbdata(0);
        writePage("/tmp/mybackrest-ibdata-test/ibdata1", page);
        free(page);

        MysqlControl ctlNo80 = mysqlControlFromIbdata(storage, STRDEF("."));
        expect("no mysql.ibd → versionNum 50500 (safe lower bound)", ctlNo80.versionNum == 50500);

        writePage("/tmp/mybackrest-ibdata-test/mysql.ibd", buildIbdata(0));   // synthetic mysql.ibd just so existence is true
        MysqlControl ctlWith80 = mysqlControlFromIbdata(storage, STRDEF("."));
        expect("mysql.ibd present → versionNum 80000 (8.0+)", ctlWith80.versionNum == 80000);

        // Without the FCRC32 marker bit, the algorithm is not on-disk-recordable for plain MySQL/Percona — caller must probe.
        expect("plain MySQL/Percona pageChecksum = None (caller probes adaptively)", ctlWith80.pageChecksum == mysqlPageChecksumNone);
        expect("encrypted = false (no encryption bit set)", !ctlWith80.encrypted);

        // ---- FSP_FLAGS_MASK_FCRC32_MARKER (MariaDB) ----
        // Build a page with bit 4 set in FSP_SPACE_FLAGS — definitively signals MariaDB full_crc32 mode.
        unsigned char *const fcrc32Page = buildIbdata(0);
        fcrc32Page[FSP_SPACE_FLAGS + 3] |= 0x10;                        // bit 4 = FSP_FLAGS_FCRC32_MASK_MARKER
        writePage("/tmp/mybackrest-ibdata-test/ibdata1", fcrc32Page);
        unlink("/tmp/mybackrest-ibdata-test/mysql.ibd");
        free(fcrc32Page);

        MysqlControl fcrc32Ctl = mysqlControlFromIbdata(storage, STRDEF("."));
        expect("FCRC32 marker bit → pageChecksum = FullCrc32", fcrc32Ctl.pageChecksum == mysqlPageChecksumFullCrc32);

        // ---- FSP_FLAGS_MASK_ENCRYPTION ----
        // Build a page with the encryption bit set (bit 13 = 0x2000).
        unsigned char *const encPage = buildIbdata(0);
        // FSP_SPACE_FLAGS is 4 bytes big-endian; bit 13 is in the top half. 0x2000 = 0x00 0x00 0x20 0x00 BE
        encPage[FSP_SPACE_FLAGS + 2] |= 0x20;
        writePage("/tmp/mybackrest-ibdata-test/ibdata1", encPage);
        free(encPage);

        MysqlControl encCtl = mysqlControlFromIbdata(storage, STRDEF("."));
        expect("ENCRYPTION bit set → encrypted=true", encCtl.encrypted);

        // Missing ibdata1 AND mysql.ibd → throws FileMissingError
        unlink("/tmp/mybackrest-ibdata-test/ibdata1");
        unlink("/tmp/mybackrest-ibdata-test/mysql.ibd");
        bool threw = false;
        TRY_BEGIN()
        {
            mysqlControlFromIbdata(storage, STRDEF("."));
        }
        CATCH(FileMissingError)
        {
            threw = true;
        }
        TRY_END();
        expect("missing ibdata1+mysql.ibd → FileMissingError", threw);

        rmrf(root);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n", errorMessage());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); rc = 1; }
    else if (rc == 0)     printf("\nAll assertions passed\n");

    return rc;
}
