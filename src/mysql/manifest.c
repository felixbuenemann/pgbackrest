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
#include "common/type/string.h"
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

        // strNewFmt allocates a fixed-size string; switch to strNew + strCatFmt so the optional [galera] / [binlog] sections
        // can be appended.
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
            info->antelope ? "true" : "false",
            info->zipSsize,
            info->encrypted ? "true" : "false",
            info->serverUuid != NULL ? strZ(info->serverUuid) : "",
            info->hasInnodb ? "true" : "false",
            info->hasMyisam ? "true" : "false",
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
                "seqno = %" PRId64 "\n",
                info->galeraStateUuid != NULL ? strZ(info->galeraStateUuid) : "",
                info->galeraSeqno);
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
