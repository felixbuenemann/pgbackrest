/***********************************************************************************************************************************
MySQL / MariaDB Binlog Event-Header Parser

Walks a binlog from the magic header to EOF, extracting only fixed-size event headers and the small subset of payload fields that
archive-push/get + restore-PITR need. Does NOT decode row events, query events, or any actual binlog payload — that work belongs
to mysqlbinlog at restore time.

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

GTID_LOG_EVENT (type 33) payload after the common header:
        +0       commit flag (1B)
        +1..16   source_id (16 raw UUID bytes; format as 8-4-4-4-12 hex with dashes)
        +17..24  gno (8B little-endian transaction number)
***********************************************************************************************************************************/
#include <build.h>

#include <inttypes.h>
#include <string.h>

#include "common/debug.h"
#include "common/io/read.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "mysql/binlog.h"

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
        // Pull the entire binlog into memory. Production binlogs are typically capped at 1 GiB by max_binlog_size; if that's too
        // large for a single read we'll switch to streamed scanning. For now keep it simple — most pre-rotation binlogs are
        // tens to hundreds of MB.
        Buffer *const content = storageGetP(storageNewReadP(storage, binlogPath));
        const unsigned char *const data = bufPtrConst(content);
        const size_t totalSize = bufUsed(content);

        if (totalSize < BINLOG_MAGIC_SIZE)
            THROW_FMT(FormatError, "binlog '%s' is too short (%zu bytes) for magic header", strZ(binlogPath), totalSize);

        if (memcmp(data, BINLOG_MAGIC, BINLOG_MAGIC_SIZE) != 0)
            THROW_FMT(FormatError, "binlog '%s' has invalid magic header", strZ(binlogPath));

        // Scan into temporaries; copy the final result into the parent context once parsing succeeds.
        String *firstGtid = NULL;
        String *lastGtid = NULL;
        uint32_t formatVersion = 0;
        bool checksumCrc32 = false;

        size_t offset = BINLOG_MAGIC_SIZE;

        while (offset + BINLOG_EVENT_HEADER_SIZE <= totalSize)
        {
            const unsigned char *const hdr = data + offset;

            const uint8_t typeCode = hdr[4];
            const uint32_t eventSize = mysqlBinlogReadU32Le(hdr + 9);

            if (eventSize < BINLOG_EVENT_HEADER_SIZE)
                THROW_FMT(FormatError, "binlog '%s' has impossible event_size %u at offset %zu", strZ(binlogPath), eventSize, offset);

            if (offset + eventSize > totalSize)
                THROW_FMT(
                    FormatError, "binlog '%s' truncated event of size %u at offset %zu (file size %zu)",
                    strZ(binlogPath), eventSize, offset, totalSize);

            // FORMAT_DESCRIPTION_EVENT: capture format version and detect CRC32 trailer presence.
            // Layout after common header:
            //     +0..1   binlog_version (uint16 LE)
            //     +2..51  server_version (50 bytes, NUL-padded)
            //     +52..55 create_timestamp (uint32 LE)
            //     +56     event_header_length (uint8) — should be 19
            //     +57..   per-event-type post-header lengths (one byte each)
            //     last byte: checksum_alg (1 = CRC32, 0 = NONE) — present in 5.6.1+
            //     final 4 bytes: CRC32 of the event itself if checksum_alg != 0
            if (typeCode == BINLOG_EVENT_FORMAT_DESCRIPTION && eventSize >= BINLOG_EVENT_HEADER_SIZE + 57)
            {
                const unsigned char *const payload = hdr + BINLOG_EVENT_HEADER_SIZE;

                formatVersion = (uint32_t)((uint16_t)payload[0] | ((uint16_t)payload[1] << 8));

                // checksum_alg byte sits 5 bytes from the end (alg byte + 4-byte trailing CRC of the FDE itself if present)
                if (eventSize >= 5 + BINLOG_EVENT_HEADER_SIZE + 57)
                {
                    const uint8_t algByte = hdr[eventSize - 5];
                    checksumCrc32 = (algByte == 1);
                }
            }

            // GTID_LOG_EVENT (MySQL, type 33): payload = 1B flags + 16B uuid + 8B gno → "uuid:gno"
            if (typeCode == BINLOG_EVENT_GTID && eventSize >= BINLOG_EVENT_HEADER_SIZE + 25)
            {
                const unsigned char *const payload = hdr + BINLOG_EVENT_HEADER_SIZE;

                String *const uuid = mysqlBinlogFormatUuid(payload + 1);
                const uint64_t gno = mysqlBinlogReadU64Le(payload + 17);
                String *const composed = strNewFmt("%s:%" PRIu64, strZ(uuid), gno);

                if (firstGtid == NULL)
                    firstGtid = composed;

                lastGtid = composed;
            }

            // GTID_EVENT (MariaDB, type 162): payload = 8B seq_no + 4B domain_id + 1B flags
            // server_id comes from the common header at offset 5..8 (LE). Rendered as "domain-server_id-seq_no".
            if (typeCode == BINLOG_EVENT_GTID_MARIADB && eventSize >= BINLOG_EVENT_HEADER_SIZE + 13)
            {
                const unsigned char *const payload = hdr + BINLOG_EVENT_HEADER_SIZE;
                const uint64_t seqNo = mysqlBinlogReadU64Le(payload + 0);
                const uint32_t domainId = mysqlBinlogReadU32Le(payload + 8);
                const uint32_t serverId = mysqlBinlogReadU32Le(hdr + 5);                // common header offset 5..8 (LE)

                String *const composed = strNewFmt("%u-%u-%" PRIu64, domainId, serverId, seqNo);

                if (firstGtid == NULL)
                    firstGtid = composed;

                lastGtid = composed;
            }

            offset += eventSize;
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
