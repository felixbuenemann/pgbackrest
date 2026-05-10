/***********************************************************************************************************************************
MySQL / MariaDB On-Disk Interface

Equivalent of src/postgres/interface.h: parses the InnoDB system tablespace header for cluster identity (server UUID, page size,
space id flags, last checkpoint LSN), enumerates redo log files (handles both the pre-8.0.30 fixed ib_logfile{0,1} layout and the
8.0.30+ #innodb_redo/#ib_redoN dynamic layout), and detects vendor + version from on-disk artefacts so an offline backup can be
inspected without a live server.

All byte offsets are facts of InnoDB's on-disk format (see /home/user/mysql-server/storage/innobase/include/fil0types.h and
fsp0types.h) and are therefore not copyrightable. Implementation lives in interface.c with helper modules under interface/.
***********************************************************************************************************************************/
#ifndef MYSQL_INTERFACE_H
#define MYSQL_INTERFACE_H

#include <stdint.h>
#include <sys/types.h>

#include "common/debug.h"
#include "common/type/string.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Defines for various MySQL/MariaDB paths and files
***********************************************************************************************************************************/
#define MYSQL_FILE_AUTOCNF                                          "auto.cnf"
#define MYSQL_FILE_IBDATA1                                          "ibdata1"
#define MYSQL_FILE_MYSQL_IBD                                        "mysql.ibd"     // Data dictionary tablespace (8.0+)
#define MYSQL_FILE_IB_LOGFILE_PREFIX                                "ib_logfile"    // Pre-8.0.30 redo
#define MYSQL_FILE_IB_REDO_PREFIX                                   "#ib_redo"      // 8.0.30+ redo
#define MYSQL_FILE_BACKUP_INFO                                      "mybackrest_backup_info"
#define MYSQL_FILE_BACKUP_BINLOG_INFO                               "mybackrest_binlog_info"
#define MYSQL_FILE_BACKUP_CHECKPOINTS                               "mybackrest_checkpoints"
#define MYSQL_FILE_RECOVERY_CNF                                     "mybackrest_recovery.cnf"

#define MYSQL_PATH_INNODB_REDO                                      "#innodb_redo"  // 8.0.30+ dynamic redo dir
#define MYSQL_PATH_INNODB_DBLWR                                     "#innodb_dblwr" // Doublewrite (NOT backed up; mysqld recreates)
#define MYSQL_PATH_PERFORMANCE_SCHEMA                               "performance_schema"
#define MYSQL_PATH_SYS                                              "sys"

/***********************************************************************************************************************************
InnoDB FIL page header offsets (from storage/innobase/include/fil0types.h)
***********************************************************************************************************************************/
#define FIL_PAGE_SPACE_OR_CHKSUM                                    0       // 4B CRC32 checksum
#define FIL_PAGE_OFFSET                                             4       // 4B page number
#define FIL_PAGE_PREV                                               8       // 4B
#define FIL_PAGE_NEXT                                               12      // 4B
#define FIL_PAGE_LSN                                                16      // 8B last-modification LSN
#define FIL_PAGE_TYPE                                               24      // 2B page type
#define FIL_PAGE_FILE_FLUSH_LSN                                     26      // 8B
#define FIL_PAGE_SPACE_ID                                           34      // 4B
#define FIL_PAGE_DATA                                               38      // Header total bytes; data starts here
#define FIL_PAGE_TRAILER_SIZE                                       8       // Last 8 bytes: legacy checksum (4) + LSN low (4)

/***********************************************************************************************************************************
FSP_HEADER offsets within page 0 of a tablespace (from include/fsp0types.h)
***********************************************************************************************************************************/
#define FSP_SPACE_ID                                                (FIL_PAGE_DATA + 0)     // 4B
#define FSP_SIZE                                                    (FIL_PAGE_DATA + 8)     // 4B page count
#define FSP_FREE_LIMIT                                              (FIL_PAGE_DATA + 12)    // 4B
#define FSP_SPACE_FLAGS                                             (FIL_PAGE_DATA + 16)    // 4B (offset 54 from page start)

/***********************************************************************************************************************************
InnoDB redo log block layout (constant across 5.7 / 8.0 / 8.0.30+, 512 bytes per block)
***********************************************************************************************************************************/
#define LOG_BLOCK_SIZE                                              512
#define LOG_BLOCK_HDR_NO                                            0       // 4B (high bit = flush marker)
#define LOG_BLOCK_HDR_DATA_LEN                                      4       // 2B
#define LOG_BLOCK_FIRST_REC_GROUP                                   6       // 2B
#define LOG_BLOCK_EPOCH_NO                                          8       // 4B
#define LOG_BLOCK_DATA_OFFSET                                       12
#define LOG_BLOCK_CHECKSUM                                          508     // 4B CRC32 of bytes 0..507
#define LOG_BLOCK_TRAILER_SIZE                                      4

// 8.0.30+ redo log file header size (4 blocks)
#define LOG_FILE_HDR_SIZE                                           (4 * LOG_BLOCK_SIZE)

/***********************************************************************************************************************************
Allowed InnoDB page sizes (decoded from FSP_FLAGS_GET_PAGE_SSIZE: (2048 << ssize) yields page size)
***********************************************************************************************************************************/
typedef enum
{
    mysqlPageSize4K = 4 * 1024,
    mysqlPageSize8K = 8 * 1024,
    mysqlPageSize16K = 16 * 1024,                                       // Default
    mysqlPageSize32K = 32 * 1024,
    mysqlPageSize64K = 64 * 1024,
} MysqlPageSize;

/***********************************************************************************************************************************
Redo log layout flavor — autodetected from the datadir
***********************************************************************************************************************************/
typedef enum
{
    mysqlRedoLayoutUnknown = 0,
    mysqlRedoLayoutFixedIbLogfile,                                      // 5.7 / 8.0.0–8.0.29: ib_logfile0, ib_logfile1
    mysqlRedoLayoutDynamicInnodbRedo,                                   // 8.0.30+: #innodb_redo/#ib_redoN_<lsn>
    mysqlRedoLayoutMariaDb107,                                          // MariaDB 10.5+: ib_logfile0 with full_crc32 page checksum
} MysqlRedoLayout;

/***********************************************************************************************************************************
Page checksum algorithm — selectable at backup time, autodetected during validate
***********************************************************************************************************************************/
typedef enum
{
    mysqlPageChecksumNone = 0,
    mysqlPageChecksumCrc32,                                             // InnoDB CRC32 (default since MySQL 5.7)
    mysqlPageChecksumInnodb,                                            // Legacy "innodb" polynomial hashing
    mysqlPageChecksumStrictCrc32,                                       // Strict variant: requires CRC32 only
    mysqlPageChecksumFullCrc32,                                         // MariaDB 10.5+ full_crc32 (full page CRC32)
} MysqlPageChecksumAlgo;

/***********************************************************************************************************************************
MySQL/MariaDB cluster identity — analogue of PgControl
***********************************************************************************************************************************/
typedef struct MysqlControl
{
    uint64_t serverUuidHigh;                                            // 16-byte UUID split high/low
    uint64_t serverUuidLow;
    uint32_t serverId;                                                  // server-id from auto.cnf or my.cnf
    unsigned int versionNum;                                            // e.g. 80400 for MySQL 8.4.0
    MysqlPageSize pageSize;                                             // From FSP_SPACE_FLAGS
    MysqlRedoLayout redoLayout;                                         // Redo files location/format
    MysqlPageChecksumAlgo pageChecksum;                                 // From backup_my.cnf or detected
    uint64_t lsnCheckpoint;                                             // Last checkpoint LSN at backup start
} MysqlControl;

/***********************************************************************************************************************************
Functions
***********************************************************************************************************************************/
// Read auto.cnf and return server-uuid as a 36-char string (or NULL if missing)
FN_EXTERN String *mysqlAutoCnfReadUuid(const Storage *storage, const String *dataPath);

// Parse FSP_HEADER of page 0 of ibdata1 (or mysql.ibd on 8.0+) for page size + flags + space id
FN_EXTERN MysqlControl mysqlControlFromIbdata(const Storage *storage, const String *dataPath);

// Detect redo log layout by probing #innodb_redo/ vs ib_logfile0
FN_EXTERN MysqlRedoLayout mysqlRedoLayoutDetect(const Storage *storage, const String *dataPath);

// Validate one InnoDB page's checksum; returns true if the page is intact.
FN_EXTERN bool mysqlPageChecksumValidate(
    const unsigned char *page, MysqlPageSize pageSize, MysqlPageChecksumAlgo algo, uint32_t pageNo);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlControlToLog(const MysqlControl *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_CONTROL_TYPE                                                                                               \
    MysqlControl
#define FUNCTION_LOG_MY_CONTROL_FORMAT(value, buffer, bufferSize)                                                                  \
    FUNCTION_LOG_OBJECT_FORMAT(&value, mysqlControlToLog, buffer, bufferSize)

#endif
