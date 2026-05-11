/***********************************************************************************************************************************
MySQL / MariaDB Binary Probe

Forks `<path> --version`, captures stdout, parses out vendor + numeric version. The probe is OPTIONAL — myBackRest will work
without ever invoking it. Two specific use cases:

  1. Restore-prepare (REQUIRED): we're invoking mysqld for crash recovery anyway, so the path is guaranteed available. The
     probe runs first to log + cross-check vendor/version against the backup's recorded source before letting mysqld touch
     the restored datadir. Refuse a downgrade across the 5.7 → 8.0 dictionary line, warn on major-version skew.

  2. Cold backup (OPTIONAL): IF the operator passes --db-mysqld-path AND we're doing an offline backup of a shutdown server,
     the probe can enrich the recorded backup metadata with vendor/version info. But cold backup must not REQUIRE a local
     mysqld binary — the operator may be backing up a NAS-mounted datadir from a host that doesn't have mysqld installed.
     The cold-backup path therefore prefers filesystem-based detection (mysql.ibd presence → 5.7 vs 8.0+, FSP_HEADER fields
     in ibdata1, server_uuid in auto.cnf) and only falls back to the binary probe when the path is provided AND the
     filesystem signal is ambiguous.

Adaptive page-checksum validation handles the missing-version case during cold backup: try CRC32 first, fall back to legacy
"innodb" hash on a single-page mismatch. The recorded backup metadata is "best-effort" without the probe, "authoritative"
with it.

Output format examples we have to parse:
  MySQL    : "mysqld  Ver 8.0.36 for Linux on x86_64 (MySQL Community Server - GPL)"
  Percona  : "mysqld  Ver 8.0.36-28 for Linux on x86_64 (Percona Server (GPL), Release 28, ...)"
  MariaDB  : "mysqld  Ver 10.11.6-MariaDB-0+deb12u1 for debian-linux-gnu on x86_64 (Debian ...)"
  MariaDB  : "mariadbd  Ver 11.2.2-MariaDB-1:11.2.2+maria~ubu2204 for debian-linux-gnu on x86_64 (mariadb.org ...)"

The vendor + version inference uses the same heuristics as mysqlClientDetectVendor in client.c so the two paths agree on what
flavor a binary belongs to.
***********************************************************************************************************************************/
#ifndef MYSQL_BINARY_H
#define MYSQL_BINARY_H

#include "common/type/string.h"
#include "mysql/client.h"

typedef struct MysqlBinaryInfo
{
    MysqlVendor vendor;                                                 // Detected vendor (mysql / mariadb / percona / unknown)
    unsigned int versionNum;                                            // Numeric, packed as MAJOR*10000 + MINOR*100 + PATCH
    String *versionRaw;                                                 // The raw "Ver ..." token for logging
    String *fullOutput;                                                 // Full first line of --version output (for diagnostics)
} MysqlBinaryInfo;

// Probe the binary at the given path. Throws ExecuteError if the binary can't be invoked, FormatError if the output is unparseable.
FN_EXTERN MysqlBinaryInfo *mysqlBinaryProbe(const String *binaryPath);

// Compatibility check: returns NULL if the binary at probe is safe to drive recovery for a backup taken on the recorded vendor +
// version, or a String describing the incompatibility (caller decides whether to log + warn or throw).
FN_EXTERN String *mysqlBinaryCheckCompatibility(
    const MysqlBinaryInfo *probe, MysqlVendor backupVendor, unsigned int backupVersionNum);

// Redo log format compatibility: a binary can replay only redo formats <= the format it was compiled with. A backup written by
// 8.0.30+ (format 6) on a target 8.0.19 binary (max format 4) → recovery refuses to start. Returns NULL if compatible (binary
// is at the same or newer version that introduced the format), or a warning string if backupRedoFormat > binary's expected
// max. backupRedoFormat = 0 (unknown) returns NULL since we can't validate without a recorded value.
FN_EXTERN String *mysqlBinaryCheckRedoCompat(const MysqlBinaryInfo *probe, uint32_t backupRedoFormat);

/***********************************************************************************************************************************
Macros for function logging
***********************************************************************************************************************************/
FN_EXTERN void mysqlBinaryInfoToLog(const MysqlBinaryInfo *this, StringStatic *debugLog);

#define FUNCTION_LOG_MY_BINARY_INFO_TYPE                                                                                           \
    MysqlBinaryInfo *
#define FUNCTION_LOG_MY_BINARY_INFO_FORMAT(value, buffer, bufferSize)                                                              \
    FUNCTION_LOG_OBJECT_FORMAT(value, mysqlBinaryInfoToLog, buffer, bufferSize)

#endif
