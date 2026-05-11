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
#include "mysql/client.h"
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
    bool encrypted;                                                     // FSP_FLAGS_MASK_ENCRYPTION bit set in FSP_SPACE_FLAGS
    bool hasSdi;                                                        // FSP_FLAGS_MASK_SDI bit (8.0+ Serialized Dictionary Info)
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

// Adaptive validate: try CRC32, then MariaDB full_crc32, then legacy "innodb" — return the algorithm that matched (or
// mysqlPageChecksumNone if none matched). Used during cold backup when the algorithm isn't known up front; the orchestrator
// remembers the matching algorithm after the first successful page so subsequent pages skip the trial loop.
FN_EXTERN MysqlPageChecksumAlgo mysqlPageChecksumValidateAdaptive(
    const unsigned char *page, MysqlPageSize pageSize, uint32_t pageNo);

// FSP_SPACE_FLAGS bit positions — only ones we currently care about. Per
// /home/user/mysql-server/storage/innobase/include/fsp0types.h:
//   POST_ANTELOPE @ 0 (1)  ZIP_SSIZE @ 1..4 (4)  ATOMIC_BLOBS @ 5 (1)  PAGE_SSIZE @ 6..9 (4)
//   DATA_DIR @ 10 (1)  SHARED @ 11 (1)  TEMPORARY @ 12 (1)  ENCRYPTION @ 13 (1)  SDI @ 14 (1)
#define FSP_FLAGS_POS_ENCRYPTION                                    13
#define FSP_FLAGS_MASK_ENCRYPTION                                   (1U << FSP_FLAGS_POS_ENCRYPTION)
#define FSP_FLAGS_POS_SDI                                           14
#define FSP_FLAGS_MASK_SDI                                          (1U << FSP_FLAGS_POS_SDI)

// MariaDB-only: full_crc32 algorithm marker. Per
// /home/user/mariadb-server/storage/innobase/include/fsp0types.h FSP_FLAGS_FCRC32_POS_MARKER, the bit lives at position 4 (mask
// 0x10) of FSP_SPACE_FLAGS. MariaDB defines `fil_space_t::full_crc32(flags)` as `flags & FSP_FLAGS_FCRC32_MASK_MARKER`. When
// this bit is set we know definitively the tablespace uses full_crc32; when clear we have to fall back to the trial-and-error
// adaptive validator (CRC32 vs legacy innodb hash) because pure MySQL/Percona doesn't record the algorithm on disk.
#define FSP_FLAGS_MASK_FCRC32_MARKER                                0x10U

/***********************************************************************************************************************************
Result of parsing the redo log file header's LOG_HEADER_CREATOR string (offset 16, 32 bytes max, NUL-terminated).

Both MySQL and MariaDB write a vendor+version string here when they create a redo log file:
  MySQL:    "MySQL X.Y.Z"               e.g. "MySQL 8.0.36"
  MariaDB:  "MariaDB X.Y.Z-suffix"      e.g. "MariaDB 10.11.6"
  Other:    "MEB X.Y.Z" (mysqlbackup), "MySQL Clone" (CLONE plugin), "Percona-XtraBackup" (xtrabackup)

This gives us the exact version that LAST WROTE the redo log — strictly more authoritative than mysqld --version on the host,
and unaffected by binary upgrades that haven't run a server yet.
***********************************************************************************************************************************/
typedef struct MysqlRedoCreator
{
    MysqlVendor vendor;                                                 // Detected from the prefix
    unsigned int versionNum;                                            // Parsed X.Y.Z, packed as MAJOR*10000+MINOR*100+PATCH
    String *raw;                                                        // The full creator string for logging
} MysqlRedoCreator;

// Read the LOG_HEADER_CREATOR string out of the redo log files under dataPath. Returns all-zero / NULL fields if no redo log
// can be located (server has never run, or 8.0.30+ datadir with the dir not yet populated).
FN_EXTERN MysqlRedoCreator mysqlRedoCreatorRead(const Storage *storage, const String *dataPath);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlControlToLog(const MysqlControl *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_CONTROL_TYPE                                                                                               \
    MysqlControl
#define FUNCTION_LOG_MY_CONTROL_FORMAT(value, buffer, bufferSize)                                                                  \
    FUNCTION_LOG_OBJECT_FORMAT(&value, mysqlControlToLog, buffer, bufferSize)

FN_EXTERN void mysqlRedoCreatorToLog(const MysqlRedoCreator *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_REDO_CREATOR_TYPE                                                                                          \
    MysqlRedoCreator
#define FUNCTION_LOG_MY_REDO_CREATOR_FORMAT(value, buffer, bufferSize)                                                             \
    FUNCTION_LOG_OBJECT_FORMAT(&value, mysqlRedoCreatorToLog, buffer, bufferSize)

#endif
