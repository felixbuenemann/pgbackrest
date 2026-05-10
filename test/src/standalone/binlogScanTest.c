/***********************************************************************************************************************************
Standalone test for src/mysql/binlog.c — mysqlBinlogScan against synthetic binlog data

Generates a tiny in-memory binlog buffer with:
  - The 4-byte BINLOG_MAGIC header
  - One FORMAT_DESCRIPTION_EVENT (type 15) carrying server_version + checksum_alg=1 (CRC32 enabled)
  - Two GTID_LOG_EVENTs (type 33) with distinct GTIDs

Writes the buffer to a tmp file, calls mysqlBinlogScan, asserts the returned MysqlBinlogInfo matches expectations.

Run via:
    meson compile -C build standalone-binlog-test && build/test/src/standalone-binlog-test
or just `build/test/src/standalone-binlog-test` after the regular meson compile.

The synthetic binlog is intentionally minimal — no real query/row payloads — but the event header walk + GTID payload extraction
is exactly what mysqlBinlogScan does in production.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "common/type/buffer.h"
#include "common/type/string.h"
#include "mysql/binlog.h"
#include "storage/posix/storage.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Append a little-endian 32-bit integer to a byte array
***********************************************************************************************************************************/
static void
writeU32Le(unsigned char *const buf, const size_t off, const uint32_t value)
{
    buf[off + 0] = (unsigned char)(value & 0xFF);
    buf[off + 1] = (unsigned char)((value >> 8) & 0xFF);
    buf[off + 2] = (unsigned char)((value >> 16) & 0xFF);
    buf[off + 3] = (unsigned char)((value >> 24) & 0xFF);
}

static void
writeU64Le(unsigned char *const buf, const size_t off, const uint64_t value)
{
    for (int i = 0; i < 8; i++)
        buf[off + (size_t)i] = (unsigned char)((value >> (i * 8)) & 0xFF);
}

/***********************************************************************************************************************************
Build a BINLOG_EVENT_HEADER (19 bytes) at the given offset
***********************************************************************************************************************************/
static void
writeEventHeader(
    unsigned char *const buf, const size_t off, const uint32_t timestamp, const uint8_t typeCode, const uint32_t serverId,
    const uint32_t eventSize, const uint32_t logPos, const uint16_t flags)
{
    writeU32Le(buf, off + 0, timestamp);
    buf[off + 4] = typeCode;
    writeU32Le(buf, off + 5, serverId);
    writeU32Le(buf, off + 9, eventSize);
    writeU32Le(buf, off + 13, logPos);
    buf[off + 17] = (unsigned char)(flags & 0xFF);
    buf[off + 18] = (unsigned char)((flags >> 8) & 0xFF);
}

/***********************************************************************************************************************************
Pretty-print one assertion result
***********************************************************************************************************************************/
static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    if (condition)
    {
        printf("  PASS  %s\n", what);
    }
    else
    {
        printf("  FAIL  %s\n", what);
        testFailures++;
    }
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
        // ---- Construct a synthetic binlog buffer ----
        // Layout:
        //   [0..3]      BINLOG_MAGIC = 0xfe 'b' 'i' 'n'
        //   [4..118]    FORMAT_DESCRIPTION_EVENT  (header 19 + payload 96 = 115 bytes total)
        //   [119..162]  GTID_LOG_EVENT #1         (header 19 + payload 25 = 44 bytes)
        //   [163..206]  GTID_LOG_EVENT #2         (header 19 + payload 25 = 44 bytes)
        // FORMAT_DESCRIPTION payload (96 bytes) = 2 binlog_version + 50 server_version + 4 timestamp + 1 hdr_len +
        //   34 post-header-lengths + 1 checksum_alg + 4 trailing CRC32 of FDE = 96
        //
        // For this synthetic test we don't compute real CRC32s or real per-event-type post-header lengths; the parser only
        // looks at the header, the FDE's binlog_version and checksum_alg, and the GTID payload's first 25 bytes — none of
        // which require a valid full FDE.

        const size_t fdeSize = 19 + 96;
        const size_t gtidSize = 19 + 25;
        const size_t totalSize = 4 + fdeSize + gtidSize + gtidSize;

        unsigned char *const buf = (unsigned char *)calloc(1, totalSize);

        if (buf == NULL)
            THROW(MemoryError, "calloc failed");

        // Magic
        memcpy(buf, "\xfe""bin", 4);

        // FORMAT_DESCRIPTION_EVENT
        const size_t fdeOff = 4;
        writeEventHeader(buf, fdeOff, /*ts*/0, /*type*/15, /*srvId*/1, /*size*/(uint32_t)fdeSize, /*pos*/(uint32_t)(fdeOff + fdeSize), /*flags*/0);

        const size_t fdePayload = fdeOff + 19;
        // binlog_version = 4
        buf[fdePayload + 0] = 4;
        buf[fdePayload + 1] = 0;
        // server_version[50] — leave as zero; parser doesn't validate
        // checksum_alg byte sits 5 bytes from the END of the event (alg byte + 4-byte CRC)
        const size_t fdeEnd = fdeOff + fdeSize;
        buf[fdeEnd - 5] = 1;                                            // checksum_alg = 1 = CRC32 enabled

        // GTID_LOG_EVENT #1
        const size_t g1Off = fdeEnd;
        writeEventHeader(buf, g1Off, /*ts*/100, /*type*/33, /*srvId*/1, /*size*/(uint32_t)gtidSize, /*pos*/(uint32_t)(g1Off + gtidSize), /*flags*/0);

        const size_t g1Payload = g1Off + 19;
        buf[g1Payload + 0] = 0;                                         // commit flag
        // 16-byte server-uuid: 11223344-5566-7788-99aa-bbccddeeff00
        static const unsigned char uuidBytes[16] = {
            0x11, 0x22, 0x33, 0x44,
            0x55, 0x66,
            0x77, 0x88,
            0x99, 0xaa,
            0xbb, 0xcc, 0xdd, 0xee, 0xff, 0x00};
        memcpy(buf + g1Payload + 1, uuidBytes, 16);
        writeU64Le(buf, g1Payload + 17, 42);                            // gno = 42

        // GTID_LOG_EVENT #2
        const size_t g2Off = g1Off + gtidSize;
        writeEventHeader(buf, g2Off, /*ts*/200, /*type*/33, /*srvId*/1, /*size*/(uint32_t)gtidSize, /*pos*/(uint32_t)(g2Off + gtidSize), /*flags*/0);

        const size_t g2Payload = g2Off + 19;
        buf[g2Payload + 0] = 0;
        memcpy(buf + g2Payload + 1, uuidBytes, 16);
        writeU64Le(buf, g2Payload + 17, 99);                            // gno = 99

        // Write to /tmp
        const char *const path = "/tmp/mybackrest-binlog-scan-test.bin";
        FILE *const fp = fopen(path, "wb");

        if (fp == NULL)
            THROW(FileWriteError, "unable to open tmp file");

        fwrite(buf, 1, totalSize, fp);
        fclose(fp);
        free(buf);

        // ---- Invoke mysqlBinlogScan ----
        const Storage *const storage = storagePosixNewP(STRDEF("/tmp"));
        MysqlBinlogInfo *const info = mysqlBinlogScan(storage, STRDEF("mybackrest-binlog-scan-test.bin"));

        // ---- Assert ----
        printf("Synthetic binlog scan results:\n");
        printf("  fileName:       %s\n", info->fileName != NULL ? strZ(info->fileName) : "(null)");
        printf("  fileSize:       %" PRIu64 "\n", info->fileSize);
        printf("  formatVersion:  %u\n", info->formatVersion);
        printf("  checksumCrc32:  %s\n", info->checksumCrc32 ? "true" : "false");
        printf("  firstGtid:      %s\n", info->firstGtid != NULL ? strZ(info->firstGtid) : "(null)");
        printf("  lastGtid:       %s\n", info->lastGtid != NULL ? strZ(info->lastGtid) : "(null)");
        printf("\nAssertions:\n");

        expect("file size matches synthetic input", info->fileSize == totalSize);
        expect("format version = 4", info->formatVersion == 4);
        expect("checksum CRC32 detected", info->checksumCrc32);
        expect(
            "first GTID = 11223344-5566-7788-99aa-bbccddeeff00:42",
            info->firstGtid != NULL && strEqZ(info->firstGtid, "11223344-5566-7788-99aa-bbccddeeff00:42"));
        expect(
            "last GTID  = 11223344-5566-7788-99aa-bbccddeeff00:99",
            info->lastGtid != NULL && strEqZ(info->lastGtid, "11223344-5566-7788-99aa-bbccddeeff00:99"));

        // Cleanup
        unlink(path);
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
