/***********************************************************************************************************************************
MySQL / MariaDB Backup Manifest

Writes mybackrest_backup_info into the backup output directory — a small key=value INI file that records everything needed for
the restore-side compatibility check + audit trail. Format mirrors xtrabackup_info's intent but is myBackRest-specific.

Sample output:
  [backrest]
  format = 6
  version = 0.1.0dev
  backup_time = 2026-05-11T14:30:25Z

  [datadir]
  vendor = mariadb
  version_num = 101106
  version_exact = true
  page_size = 16384
  page_checksum = crc32
  redo_layout = fixed
  antelope = false
  encrypted = false

  [identity]
  server_uuid = 8c0fd6f0-bf8f-11ee-9821-0242ac120002

  [engines]
  innodb = true
  myisam = false
  aria = true
  myrocks = false
  tokudb = false

  [galera]
  enabled = true
  state_uuid = aaaa1111-bbbb-2222-cccc-333333333333
  seqno = 987654321

  [binlog]
  start_file = mysql-bin.000123
  start_pos = 4
  start_gtid = 11223344-5566-7788-99aa-bbccddeeff00:42
  stop_file = mysql-bin.000125
  stop_pos = 8421
  stop_gtid = 11223344-5566-7788-99aa-bbccddeeff00:99

The binlog section is populated by the orchestrator after backupStart/backupStop; the rest comes from mysqlDataDirInspect.
***********************************************************************************************************************************/
#ifndef MYSQL_MANIFEST_H
#define MYSQL_MANIFEST_H

#include "common/type/string.h"
#include "mysql/datadir.h"
#include "storage/storage.h"

typedef struct MysqlBackupBinlog
{
    String *startFile;                                                  // mysql-bin.000123
    uint64_t startPos;
    String *startGtid;                                                  // uuid:gno (NULL if pre-5.6 / GTID disabled)
    String *stopFile;
    uint64_t stopPos;
    String *stopGtid;
} MysqlBackupBinlog;

// Compose the manifest text from a populated inspector result + optional binlog metadata. Returns a String the caller can
// write through any storage backend.
FN_EXTERN String *mysqlBackupManifestRender(const MysqlDataDirInfo *info, const MysqlBackupBinlog *binlog);

// Write the rendered manifest to <backupPath>/mybackrest_backup_info via the given storage.
FN_EXTERN void mysqlBackupManifestWrite(
    const Storage *storage, const String *backupPath, const MysqlDataDirInfo *info, const MysqlBackupBinlog *binlog);

/***********************************************************************************************************************************
Parsed manifest. Mirrors what was written: the inspector result is reconstructed (so restore-side compatibility checks can run
against it), the optional binlog block is populated when the source manifest had a [binlog] section, and the [backrest] header
gives the format/version/timestamp for sanity logging.
***********************************************************************************************************************************/
typedef struct MysqlBackupManifestParsed
{
    unsigned int format;                                                // [backrest] format = N
    String *backrestVersion;                                            // [backrest] version
    String *backupTime;                                                 // [backrest] backup_time (ISO 8601)

    MysqlDataDirInfo *info;                                             // Reconstructed datadir info (always non-NULL on success)
    MysqlBackupBinlog *binlog;                                          // NULL if the manifest had no [binlog] section
} MysqlBackupManifestParsed;

// Parse an INI text buffer and reconstruct the structured form. Throws FormatError on malformed input.
FN_EXTERN MysqlBackupManifestParsed *mysqlBackupManifestParse(const String *text);

// Read <backupPath>/mybackrest_backup_info and parse it. Returns NULL if the file is missing.
FN_EXTERN MysqlBackupManifestParsed *mysqlBackupManifestRead(const Storage *storage, const String *backupPath);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlBackupManifestParsedToLog(const MysqlBackupManifestParsed *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_MANIFEST_PARSED_TYPE                                                                                       \
    MysqlBackupManifestParsed *
#define FUNCTION_LOG_MY_MANIFEST_PARSED_FORMAT(value, buffer, bufferSize)                                                          \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlBackupManifestParsedToLog, buffer, bufferSize)

#endif
