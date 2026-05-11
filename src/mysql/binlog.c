/***********************************************************************************************************************************
MySQL / MariaDB Binlog Event-Header Parser

Streams the binlog from disk one event at a time, extracting only what archive-push/get + restore PITR need: file size, GTID
range, format-description event's checksum-alg flag. Does NOT decode row events or query payloads — that work belongs to
mysqlbinlog at restore time. Streaming (vs full-file slurp) matters because production binlogs cap at max_binlog_size = 1 GiB
default; loading multiple in parallel would OOM.

Wire format (libs/mysql/binlog/event/binlog_event.h in mysql-server):
  - File magic: 4 bytes  0xfe 'b' 'i' 'n'
  - Event common header (19 bytes, little-endian):
        offset 0..3   timestamp
        offset 4      type_code
        offset 5..8   server_id
        offset 9..12  event_size  (total bytes including this 19-byte header)
        offset 13..16 log_pos     (absolute file position of next event)
        offset 17..18 flags
  - Optional 4-byte CRC32 trailer per event if binlog_checksum=CRC32 (default since 5.6.6).

GTID_LOG_EVENT (MySQL, type 33) payload after the common header:
        +0       commit flag (1B)
        +1..16   source_id (16 raw UUID bytes; format as 8-4-4-4-12 hex with dashes)
        +17..24  gno (8B LE transaction number)

GTID_EVENT (MariaDB, type 162) payload after the common header:
        +0..7    seq_no (uint64 LE)
        +8..11   domain_id (uint32 LE)
        +12      flags2 (uint8)
        rendered as "<domain_id>-<server_id>-<seq_no>" with server_id from the common header
***********************************************************************************************************************************/
#include <build.h>

#include <inttypes.h>
#include <string.h>

#include "common/debug.h"
#include "common/io/read.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "mysql/binlog.h"

#define BINLOG_SCAN_PAYLOAD_MAX                                     128             // FDE post-header (~96) > GTID (~25)

/***********************************************************************************************************************************
Render 16 raw UUID bytes into the canonical 8-4-4-4-12 hex form
***********************************************************************************************************************************/
static String *
mysqlBinlogFormatUuid(const unsigned char *const raw)
{
    char buf[37];

    snprintf(
        buf, sizeof(buf),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
        raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);

    return strNewZ(buf);
}

/***********************************************************************************************************************************
Decode a little-endian 32-bit integer from a byte stream
***********************************************************************************************************************************/
static uint32_t
mysqlBinlogReadU32Le(const unsigned char *const p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
mysqlBinlogReadU64Le(const unsigned char *const p)
{
    return (uint64_t)p[0]        | ((uint64_t)p[1] << 8)  | ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/***********************************************************************************************************************************
Fill a buffer to exactly `count` bytes from the read stream; throw on truncation. The Buffer is reset before reading.
***********************************************************************************************************************************/
static void
mysqlBinlogReadExactly(IoRead *const read, Buffer *const buf, const size_t count, const String *const binlogPath)
{
    ASSERT(count <= bufSizeAlloc(buf));

    bufUsedZero(buf);
    bufLimitSet(buf, count);

    while (bufUsed(buf) < count)
    {
        const size_t before = bufUsed(buf);
        ioReadSmall(read, buf);

        if (bufUsed(buf) == before)                                     // No progress = EOF before count
        {
            THROW_FMT(
                FormatError, "binlog '%s' truncated: needed %zu more bytes, got %zu", strZ(binlogPath), count - before, before);
        }
    }

    bufLimitClear(buf);
}

/***********************************************************************************************************************************
Discard `count` bytes from the read stream (used to skip events we don't care about).
***********************************************************************************************************************************/
static void
mysqlBinlogSkipExactly(IoRead *const read, Buffer *const scratch, const size_t count, const String *const binlogPath)
{
    size_t remaining = count;

    while (remaining > 0)
    {
        const size_t chunk = remaining < bufSizeAlloc(scratch) ? remaining : bufSizeAlloc(scratch);
        mysqlBinlogReadExactly(read, scratch, chunk, binlogPath);
        remaining -= chunk;
    }
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlBinlogInfo *
mysqlBinlogScan(const Storage *const storage, const String *const binlogPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, binlogPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(binlogPath != NULL);

    MysqlBinlogInfo *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        IoRead *const read = storageReadIo(storageNewReadP(storage, binlogPath));
        ioReadOpen(read);

        Buffer *const evtBuf = bufNew(BINLOG_EVENT_HEADER_SIZE);
        Buffer *const payloadBuf = bufNew(BINLOG_SCAN_PAYLOAD_MAX);
        Buffer *const scratch = bufNew(4096);

        // Validate magic (4 bytes)
        mysqlBinlogReadExactly(read, evtBuf, BINLOG_MAGIC_SIZE, binlogPath);
        if (memcmp(bufPtrConst(evtBuf), BINLOG_MAGIC, BINLOG_MAGIC_SIZE) != 0)
            THROW_FMT(FormatError, "binlog '%s' has invalid magic header", strZ(binlogPath));

        uint64_t totalSize = BINLOG_MAGIC_SIZE;
        String *firstGtid = NULL;
        String *lastGtid = NULL;
        uint32_t formatVersion = 0;
        bool checksumCrc32 = false;

        for (;;)
        {
            // Try to read one 19-byte event header. EOF here is normal end-of-file.
            bufUsedZero(evtBuf);
            bufLimitSet(evtBuf, BINLOG_EVENT_HEADER_SIZE);
            ioReadSmall(read, evtBuf);

            if (bufUsed(evtBuf) == 0)                                   // Clean EOF
                break;

            if (bufUsed(evtBuf) < BINLOG_EVENT_HEADER_SIZE)
            {
                THROW_FMT(
                    FormatError, "binlog '%s' truncated event header: got %zu bytes at offset %" PRIu64,
                    strZ(binlogPath), bufUsed(evtBuf), totalSize);
            }
            bufLimitClear(evtBuf);

            const unsigned char *const hdr = bufPtrConst(evtBuf);
            const uint8_t typeCode = hdr[4];
            const uint32_t serverId = mysqlBinlogReadU32Le(hdr + 5);
            const uint32_t eventSize = mysqlBinlogReadU32Le(hdr + 9);

            if (eventSize < BINLOG_EVENT_HEADER_SIZE)
            {
                THROW_FMT(
                    FormatError, "binlog '%s' impossible event_size %u at offset %" PRIu64, strZ(binlogPath), eventSize,
                    totalSize);
            }

            const size_t payloadSize = eventSize - BINLOG_EVENT_HEADER_SIZE;

            if (typeCode == BINLOG_EVENT_FORMAT_DESCRIPTION && payloadSize >= 57)
            {
                // Read enough payload to reach the trailing checksum_alg byte. The FDE post-header is variable in length but
                // the alg byte sits exactly 5 bytes from the END of the event (1 byte alg + 4 byte trailing CRC32 of the FDE
                // itself if present). Capacity-cap so a pathological FDE doesn't blow the payload buffer.
                const size_t want = payloadSize > BINLOG_SCAN_PAYLOAD_MAX ? BINLOG_SCAN_PAYLOAD_MAX : payloadSize;
                mysqlBinlogReadExactly(read, payloadBuf, want, binlogPath);
                const unsigned char *const payload = bufPtrConst(payloadBuf);

                formatVersion = (uint32_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));

                // checksum_alg is 5 bytes before the end of the event. If `want` already reached the end, we have it; otherwise
                // we'd need to seek which IoRead doesn't support, so cap-cap behavior is: skip the rest of the event and don't
                // try to surface checksumCrc32 (defaults stay false).
                if (want == payloadSize && payloadSize >= 5 + 57)
                    checksumCrc32 = (payload[payloadSize - 5] == 1);

                mysqlBinlogSkipExactly(read, scratch, payloadSize - want, binlogPath);
            }
            else if (typeCode == BINLOG_EVENT_GTID && payloadSize >= 25)
            {
                mysqlBinlogReadExactly(read, payloadBuf, 25, binlogPath);
                const unsigned char *const payload = bufPtrConst(payloadBuf);

                String *const uuid = mysqlBinlogFormatUuid(payload + 1);
                const uint64_t gno = mysqlBinlogReadU64Le(payload + 17);
                String *const composed = strNewFmt("%s:%" PRIu64, strZ(uuid), gno);

                if (firstGtid == NULL)
                    firstGtid = composed;

                lastGtid = composed;

                mysqlBinlogSkipExactly(read, scratch, payloadSize - 25, binlogPath);
            }
            else if (typeCode == BINLOG_EVENT_GTID_MARIADB && payloadSize >= 13)
            {
                mysqlBinlogReadExactly(read, payloadBuf, 13, binlogPath);
                const unsigned char *const payload = bufPtrConst(payloadBuf);

                const uint64_t seqNo = mysqlBinlogReadU64Le(payload + 0);
                const uint32_t domainId = mysqlBinlogReadU32Le(payload + 8);

                String *const composed = strNewFmt("%u-%u-%" PRIu64, domainId, serverId, seqNo);

                if (firstGtid == NULL)
                    firstGtid = composed;

                lastGtid = composed;

                mysqlBinlogSkipExactly(read, scratch, payloadSize - 13, binlogPath);
            }
            else
            {
                // Don't care about this event type — skip its payload entirely
                mysqlBinlogSkipExactly(read, scratch, payloadSize, binlogPath);
            }

            totalSize += eventSize;
        }

        // Materialize the result in the parent context
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlBinlogInfo));
            *result = (MysqlBinlogInfo)
            {
                .fileName = strBase(binlogPath),
                .fileSize = totalSize,
                .firstGtid = firstGtid != NULL ? strDup(firstGtid) : NULL,
                .lastGtid = lastGtid != NULL ? strDup(lastGtid) : NULL,
                .checksumCrc32 = checksumCrc32,
                .formatVersion = formatVersion,
            };
        }
        MEM_CONTEXT_PRIOR_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_BINLOG_INFO, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlBinlogInfoToLog(const MysqlBinlogInfo *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog, "{file: %s, size: %" PRIu64 ", formatVersion: %u, checksumCrc32: %s",
        this->fileName != NULL ? strZ(this->fileName) : "(null)", this->fileSize, this->formatVersion,
        this->checksumCrc32 ? "true" : "false");

    if (this->firstGtid != NULL)
        strStcFmt(debugLog, ", firstGtid: %s", strZ(this->firstGtid));

    if (this->lastGtid != NULL)
        strStcFmt(debugLog, ", lastGtid: %s", strZ(this->lastGtid));

    strStcCat(debugLog, "}");
}
