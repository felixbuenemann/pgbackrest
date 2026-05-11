/***********************************************************************************************************************************
MySQL / MariaDB Backup Manifest

INI-style writer. Each section is independent so a partially-populated info struct (e.g. cold backup with no binlog metadata)
just emits empty/missing fields rather than failing to render.
***********************************************************************************************************************************/
#include <build.h>

#include <inttypes.h>
#include <time.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/buffer.h"
#include "common/type/convert.h"
#include "common/type/keyValue.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "common/type/variant.h"
#include "mysql/manifest.h"
#include "version.h"

/***********************************************************************************************************************************
Render a vendor enum as a short string for the manifest
***********************************************************************************************************************************/
static const char *
manifestVendorName(const MysqlVendor v)
{
    switch (v)
    {
        case mysqlVendorMysql:   return "mysql";
        case mysqlVendorMariadb: return "mariadb";
        case mysqlVendorPercona: return "percona";
        case mysqlVendorUnknown:
        default:                 return "unknown";
    }
}

/***********************************************************************************************************************************
Render a checksum-algorithm enum as a short string
***********************************************************************************************************************************/
static const char *
manifestChecksumName(const MysqlPageChecksumAlgo a)
{
    switch (a)
    {
        case mysqlPageChecksumCrc32:        return "crc32";
        case mysqlPageChecksumStrictCrc32:  return "strict_crc32";
        case mysqlPageChecksumInnodb:       return "innodb";
        case mysqlPageChecksumFullCrc32:    return "full_crc32";
        case mysqlPageChecksumNone:
        default:                            return "none";
    }
}

/***********************************************************************************************************************************
Render a redo-layout enum
***********************************************************************************************************************************/
static const char *
manifestRedoLayoutName(const MysqlRedoLayout r)
{
    switch (r)
    {
        case mysqlRedoLayoutFixedIbLogfile:        return "fixed";
        case mysqlRedoLayoutDynamicInnodbRedo:     return "dynamic";
        case mysqlRedoLayoutMariaDb107:            return "mariadb_10.5+";
        case mysqlRedoLayoutUnknown:
        default:                                   return "unknown";
    }
}

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlBackupManifestRender(const MysqlDataDirInfo *const info, const MysqlBackupBinlog *const binlog)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_DATADIR_INFO, info);
        FUNCTION_LOG_PARAM_P(VOID, binlog);
    FUNCTION_LOG_END();

    ASSERT(info != NULL);

    String *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // ISO 8601 UTC timestamp for backup_time
        char ts[32];
        const time_t now = time(NULL);
        struct tm utc;
        gmtime_r(&now, &utc);
        strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &utc);

        // Optional [galera] / [binlog] sections are appended below — build incrementally with strCatFmt rather than one fixed
        // strNewFmt.
        String *const out = strNew();
        strCatFmt(
            out,
            "[backrest]\n"
            "format = %u\n"
            "version = %s\n"
            "backup_time = %s\n"
            "\n"
            "[datadir]\n"
            "vendor = %s\n"
            "version_num = %u\n"
            "version_exact = %s\n"
            "page_size = %u\n"
            "page_checksum = %s\n"
            "redo_layout = %s\n"
            "redo_format_num = %u\n"
            "encrypted_redo = %s\n"
            "antelope = %s\n"
            "zip_ssize = %u\n"
            "encrypted = %s\n"
            "\n"
            "[identity]\n"
            "server_uuid = %s\n"
            "\n"
            "[engines]\n"
            "innodb = %s\n"
            "myisam = %s\n"
            "isam = %s\n"
            "aria = %s\n"
            "myrocks = %s\n"
            "tokudb = %s\n",
            REPOSITORY_FORMAT,
            PROJECT_VERSION,
            ts,
            manifestVendorName(info->vendor),
            info->versionNum,
            info->versionExact ? "true" : "false",
            (unsigned int)info->pageSize,
            manifestChecksumName(info->pageChecksum),
            manifestRedoLayoutName(info->redoLayout),
            info->redoFormatNum,
            info->encryptedRedo ? "true" : "false",
            info->antelope ? "true" : "false",
            info->zipSsize,
            info->encrypted ? "true" : "false",
            info->serverUuid != NULL ? strZ(info->serverUuid) : "",
            info->hasInnodb ? "true" : "false",
            info->hasMyisam ? "true" : "false",
            info->hasIsam ? "true" : "false",
            info->hasAria ? "true" : "false",
            info->hasMyrocks ? "true" : "false",
            info->hasTokudb ? "true" : "false");

        if (info->hasGalera)
        {
            strCatFmt(
                out,
                "\n[galera]\n"
                "enabled = true\n"
                "state_uuid = %s\n"
                "seqno = %" PRId64 "\n"
                "safe_to_bootstrap = %d\n",
                info->galeraStateUuid != NULL ? strZ(info->galeraStateUuid) : "",
                info->galeraSeqno,
                info->safeToBootstrap);
        }

        if (binlog != NULL && binlog->startFile != NULL)
        {
            strCatFmt(
                out,
                "\n[binlog]\n"
                "start_file = %s\n"
                "start_pos = %" PRIu64 "\n"
                "start_gtid = %s\n"
                "stop_file = %s\n"
                "stop_pos = %" PRIu64 "\n"
                "stop_gtid = %s\n",
                strZ(binlog->startFile),
                binlog->startPos,
                binlog->startGtid != NULL ? strZ(binlog->startGtid) : "",
                binlog->stopFile != NULL ? strZ(binlog->stopFile) : "",
                binlog->stopPos,
                binlog->stopGtid != NULL ? strZ(binlog->stopGtid) : "");
        }

        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = strDup(out);
        }
        MEM_CONTEXT_PRIOR_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(STRING, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlBackupManifestWrite(
    const Storage *const storage, const String *const backupPath, const MysqlDataDirInfo *const info,
    const MysqlBackupBinlog *const binlog)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
        FUNCTION_LOG_PARAM(MY_DATADIR_INFO, info);
        FUNCTION_LOG_PARAM_P(VOID, binlog);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(backupPath != NULL);
    ASSERT(info != NULL);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const text = mysqlBackupManifestRender(info, binlog);
        const String *const path = strNewFmt("%s/%s", strZ(backupPath), MYSQL_FILE_BACKUP_INFO);

        storagePutP(storageNewWriteP(storage, path), BUFSTR(text));

        LOG_INFO_FMT("backup manifest written: %s (%zu bytes)", strZ(path), strSize(text));
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}

/***********************************************************************************************************************************
Reverse mappings — string → enum. The render side has the inverse helpers; these accept any value the writer would emit.
***********************************************************************************************************************************/
static MysqlVendor
parseVendor(const String *const s)
{
    if (s == NULL) return mysqlVendorUnknown;
    if (strEqZ(s, "mysql"))   return mysqlVendorMysql;
    if (strEqZ(s, "mariadb")) return mysqlVendorMariadb;
    if (strEqZ(s, "percona")) return mysqlVendorPercona;
    return mysqlVendorUnknown;
}

static MysqlPageChecksumAlgo
parseChecksum(const String *const s)
{
    if (s == NULL) return mysqlPageChecksumNone;
    if (strEqZ(s, "crc32"))         return mysqlPageChecksumCrc32;
    if (strEqZ(s, "strict_crc32"))  return mysqlPageChecksumStrictCrc32;
    if (strEqZ(s, "innodb"))        return mysqlPageChecksumInnodb;
    if (strEqZ(s, "full_crc32"))    return mysqlPageChecksumFullCrc32;
    return mysqlPageChecksumNone;
}

static MysqlRedoLayout
parseRedoLayout(const String *const s)
{
    if (s == NULL) return mysqlRedoLayoutUnknown;
    if (strEqZ(s, "fixed"))         return mysqlRedoLayoutFixedIbLogfile;
    if (strEqZ(s, "dynamic"))       return mysqlRedoLayoutDynamicInnodbRedo;
    if (strEqZ(s, "mariadb_10.5+")) return mysqlRedoLayoutMariaDb107;
    return mysqlRedoLayoutUnknown;
}

static bool
parseBool(const String *const s)
{
    return s != NULL && strEqZ(s, "true");
}

/***********************************************************************************************************************************
Look up "section.key" in a flat KeyValue using "section\nkey" as the composite string key. Returns NULL if absent.
***********************************************************************************************************************************/
static const String *
lookupKv(const KeyValue *const kv, const char *const section, const char *const key)
{
    String *const composite = strNewFmt("%s\n%s", section, key);
    const Variant *const v = kvGet(kv, VARSTR(composite));
    strFree(composite);

    if (v == NULL || varType(v) != varTypeString)
        return NULL;

    return varStr(v);
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlBackupManifestParsed *
mysqlBackupManifestParse(const String *const text)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, text);
    FUNCTION_LOG_END();

    ASSERT(text != NULL);

    MysqlBackupManifestParsed *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Pass 1: walk lines, build a "section\nkey" → value KeyValue. Section header is "[name]"; key=value lines split on
        // first '='. Comments (lines beginning with '#') and blank lines are skipped. Whitespace around key and value is
        // trimmed. The parser is intentionally tolerant — a malformed line just gets logged at DETAIL and skipped.
        KeyValue *const kv = kvNew();
        StringList *const lines = strLstNewSplit(text, STRDEF("\n"));
        String *currentSection = NULL;

        for (unsigned int i = 0; i < strLstSize(lines); i++)
        {
            const String *const line = strTrim(strDup(strLstGet(lines, i)));

            if (strSize(line) == 0 || strZ(line)[0] == '#')
                continue;

            if (strBeginsWithZ(line, "[") && strEndsWithZ(line, "]"))
            {
                strFree(currentSection);
                currentSection = strSubN(line, 1, strSize(line) - 2);
                continue;
            }

            if (currentSection == NULL)
                continue;                                               // Stray line before the first section — ignore

            const int eqIdx = (int)strChr(line, '=');
            if (eqIdx <= 0)
                continue;

            String *const key = strTrim(strSubN(line, 0, (size_t)eqIdx));
            String *const value = strTrim(strSubN(line, (size_t)eqIdx + 1, strSize(line) - (size_t)eqIdx - 1));

            String *const composite = strNewFmt("%s\n%s", strZ(currentSection), strZ(key));
            kvPut(kv, VARSTR(composite), VARSTR(value));
        }

        // Pass 2: extract typed fields
        const String *const formatStr = lookupKv(kv, "backrest", "format");
        const String *const versionStr = lookupKv(kv, "backrest", "version");
        const String *const timeStr = lookupKv(kv, "backrest", "backup_time");

        const String *const vendorStr = lookupKv(kv, "datadir", "vendor");
        const String *const verNumStr = lookupKv(kv, "datadir", "version_num");
        const String *const verExactStr = lookupKv(kv, "datadir", "version_exact");
        const String *const pageSizeStr = lookupKv(kv, "datadir", "page_size");
        const String *const checksumStr = lookupKv(kv, "datadir", "page_checksum");
        const String *const redoLayoutStr = lookupKv(kv, "datadir", "redo_layout");
        const String *const redoFormatNumStr = lookupKv(kv, "datadir", "redo_format_num");
        const String *const encryptedRedoStr = lookupKv(kv, "datadir", "encrypted_redo");
        const String *const antelopeStr = lookupKv(kv, "datadir", "antelope");
        const String *const zipSsizeStr = lookupKv(kv, "datadir", "zip_ssize");
        const String *const encryptedStr = lookupKv(kv, "datadir", "encrypted");
        const String *const serverUuidStr = lookupKv(kv, "identity", "server_uuid");

        const bool hasGaleraSection = lookupKv(kv, "galera", "enabled") != NULL;
        const String *const galeraUuidStr = lookupKv(kv, "galera", "state_uuid");
        const String *const galeraSeqnoStr = lookupKv(kv, "galera", "seqno");
        const String *const galeraSafeStr = lookupKv(kv, "galera", "safe_to_bootstrap");

        const bool hasBinlogSection = lookupKv(kv, "binlog", "start_file") != NULL;
        const String *const binStartFile = lookupKv(kv, "binlog", "start_file");
        const String *const binStartPos = lookupKv(kv, "binlog", "start_pos");
        const String *const binStartGtid = lookupKv(kv, "binlog", "start_gtid");
        const String *const binStopFile = lookupKv(kv, "binlog", "stop_file");
        const String *const binStopPos = lookupKv(kv, "binlog", "stop_pos");
        const String *const binStopGtid = lookupKv(kv, "binlog", "stop_gtid");

        // Materialize result in the parent context
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlBackupManifestParsed));
            *result = (MysqlBackupManifestParsed){0};

            result->format = formatStr != NULL ? cvtZToUInt(strZ(formatStr)) : 0;
            result->backrestVersion = versionStr != NULL ? strDup(versionStr) : NULL;
            result->backupTime = timeStr != NULL ? strDup(timeStr) : NULL;

            result->info = memNew(sizeof(MysqlDataDirInfo));
            *result->info = (MysqlDataDirInfo){0};
            result->info->vendor = parseVendor(vendorStr);
            result->info->versionNum = verNumStr != NULL ? cvtZToUInt(strZ(verNumStr)) : 0;
            result->info->versionExact = parseBool(verExactStr);
            result->info->pageSize = pageSizeStr != NULL ? (MysqlPageSize)cvtZToUInt(strZ(pageSizeStr)) : 0;
            result->info->pageChecksum = parseChecksum(checksumStr);
            result->info->redoLayout = parseRedoLayout(redoLayoutStr);
            result->info->redoFormatNum = redoFormatNumStr != NULL ? cvtZToUInt(strZ(redoFormatNumStr)) : 0;
            result->info->encryptedRedo = parseBool(encryptedRedoStr);
            result->info->antelope = parseBool(antelopeStr);
            result->info->zipSsize = zipSsizeStr != NULL ? cvtZToUInt(strZ(zipSsizeStr)) : 0;
            result->info->encrypted = parseBool(encryptedStr);
            result->info->serverUuid =
                (serverUuidStr != NULL && strSize(serverUuidStr) > 0) ? strDup(serverUuidStr) : NULL;

            result->info->hasInnodb = parseBool(lookupKv(kv, "engines", "innodb"));
            result->info->hasMyisam = parseBool(lookupKv(kv, "engines", "myisam"));
            result->info->hasIsam = parseBool(lookupKv(kv, "engines", "isam"));
            result->info->hasAria = parseBool(lookupKv(kv, "engines", "aria"));
            result->info->hasMyrocks = parseBool(lookupKv(kv, "engines", "myrocks"));
            result->info->hasTokudb = parseBool(lookupKv(kv, "engines", "tokudb"));

            result->info->hasGalera = hasGaleraSection;
            result->info->galeraStateUuid = (galeraUuidStr != NULL && strSize(galeraUuidStr) > 0) ? strDup(galeraUuidStr) : NULL;
            result->info->galeraSeqno = galeraSeqnoStr != NULL ? (int64_t)cvtZToInt64(strZ(galeraSeqnoStr)) : -1;
            result->info->safeToBootstrap = galeraSafeStr != NULL ? (int)cvtZToInt(strZ(galeraSafeStr)) : -1;

            if (hasBinlogSection)
            {
                result->binlog = memNew(sizeof(MysqlBackupBinlog));
                *result->binlog = (MysqlBackupBinlog){0};
                result->binlog->startFile = binStartFile != NULL ? strDup(binStartFile) : NULL;
                result->binlog->startPos = binStartPos != NULL ? (uint64_t)cvtZToInt64(strZ(binStartPos)) : 0;
                result->binlog->startGtid =
                    (binStartGtid != NULL && strSize(binStartGtid) > 0) ? strDup(binStartGtid) : NULL;
                result->binlog->stopFile =
                    (binStopFile != NULL && strSize(binStopFile) > 0) ? strDup(binStopFile) : NULL;
                result->binlog->stopPos = binStopPos != NULL ? (uint64_t)cvtZToInt64(strZ(binStopPos)) : 0;
                result->binlog->stopGtid =
                    (binStopGtid != NULL && strSize(binStopGtid) > 0) ? strDup(binStopGtid) : NULL;
            }
        }
        MEM_CONTEXT_PRIOR_END();
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_MANIFEST_PARSED, result);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlBackupManifestParsedToLog(const MysqlBackupManifestParsed *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog, "{format: %u, version: %s, hasBinlog: %s}",
        this->format,
        this->backrestVersion != NULL ? strZ(this->backrestVersion) : "(null)",
        this->binlog != NULL ? "true" : "false");
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlBackupManifestParsed *
mysqlBackupManifestRead(const Storage *const storage, const String *const backupPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, backupPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(backupPath != NULL);

    MysqlBackupManifestParsed *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        const String *const path = strNewFmt("%s/%s", strZ(backupPath), MYSQL_FILE_BACKUP_INFO);
        Buffer *const content = storageGetP(storageNewReadP(storage, path, .ignoreMissing = true));

        if (content != NULL)
        {
            // Run Parse with the CALLER's context as its "prior" — otherwise the parsed result allocates inside this Read's
            // temp scope and gets freed when TEMP_END fires. The MEM_CONTEXT_PRIOR_BEGIN here makes Read's prior the active
            // context so Parse's own PRIOR_BEGIN points at the caller.
            MEM_CONTEXT_PRIOR_BEGIN()
            {
                result = mysqlBackupManifestParse(strNewBuf(content));
            }
            MEM_CONTEXT_PRIOR_END();
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_MANIFEST_PARSED, result);
}
