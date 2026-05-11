/***********************************************************************************************************************************
MySQL / MariaDB Binlog Event-Header Parser

Walks a binlog file and extracts only what archive-push / archive-get / restore PITR need: file size, GTID range covered, format
description event checksum flag. Does NOT decode event payloads — that work belongs to mysqlbinlog at restore time.

On-disk format reference: storage/innobase/include/log0constants.h and libs/mysql/binlog/event/binlog_event.h in the cloned
mysql-server tree. Magic header is 4 bytes 0xfe 0x62 0x69 0x6e ("\xfebin"); each subsequent event begins with a 19-byte common
header (timestamp + type + server_id + event_size + log_pos + flags). Optional 4-byte CRC32 trailer per event when
binlog_checksum=CRC32 (default since MySQL 5.6.6).
***********************************************************************************************************************************/
#ifndef MYSQL_BINLOG_H
#define MYSQL_BINLOG_H

#include <stdint.h>

#include "common/debug.h"
#include "common/type/string.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Binlog event type codes (subset we care about)
***********************************************************************************************************************************/
#define BINLOG_MAGIC                                                "\xfe\x62\x69\x6e"  // "\xfebin"
#define BINLOG_MAGIC_SIZE                                           4
#define BINLOG_EVENT_HEADER_SIZE                                    19
#define BINLOG_EVENT_CRC32_SIZE                                     4

#define BINLOG_EVENT_FORMAT_DESCRIPTION                             15
#define BINLOG_EVENT_GTID                                           33      // MySQL GTID_LOG_EVENT
#define BINLOG_EVENT_PREVIOUS_GTIDS                                 35
#define BINLOG_EVENT_ANONYMOUS_GTID                                 34
#define BINLOG_EVENT_ROTATE                                         4

// MariaDB-specific event type, from /home/user/mariadb-server/sql/log_event.h:
//   GTID_EVENT = 162  — payload after the 19-byte common header is:
//     bytes 0..7   seq_no (uint64 LE)
//     bytes 8..11  domain_id (uint32 LE)
//     byte 12      flags2 (uint8)
//   Rendered as "domain-server_id-seq_no" where server_id comes from the common header (offset 5..8 LE).
#define BINLOG_EVENT_GTID_MARIADB                                   162

/***********************************************************************************************************************************
Binlog file metadata captured during a single forward scan
***********************************************************************************************************************************/
typedef struct MysqlBinlogInfo
{
    String *fileName;                                                   // mysql-bin.000123
    uint64_t fileSize;
    String *firstGtid;                                                  // First GTID in the file (or NULL if non-GTID mode)
    String *lastGtid;                                                   // Last GTID in the file
    bool checksumCrc32;                                                 // True if events carry CRC32 trailers
    uint32_t formatVersion;                                             // 4 = 5.0+; 6 = MariaDB; 5.7/8.0 also report 4
} MysqlBinlogInfo;

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Scan a binlog file from beginning to end, returning summary metadata. Reads only event headers (19 bytes per event).
FN_EXTERN MysqlBinlogInfo *mysqlBinlogScan(const Storage *storage, const String *binlogPath);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlBinlogInfoToLog(const MysqlBinlogInfo *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_BINLOG_INFO_TYPE                                                                                           \
    MysqlBinlogInfo *
#define FUNCTION_LOG_MY_BINLOG_INFO_FORMAT(value, buffer, bufferSize)                                                              \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlBinlogInfoToLog, buffer, bufferSize)

#endif
