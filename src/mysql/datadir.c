/***********************************************************************************************************************************
MySQL / MariaDB Datadir Inspector

Implements detection rules described in the header. The order of operations matters:

  1. Walk the top level once collecting filename-based markers (cheap).
  2. Walk one level down into each schema dir to find table-level markers (also cheap on small datadirs; bounded by
     #schemas × #tables-per-schema).
  3. Read auto.cnf for serverUuid (one short file).
  4. Run mysqlControlFromIbdata + mysqlRedoLayoutDetect when InnoDB is present (each does one open).

We never throw on missing files — the function's contract is "return what we found, leave the rest zero/false". Callers decide
whether absence is acceptable for their use case.
***********************************************************************************************************************************/
#include <build.h>

#include <string.h>

#include "common/debug.h"
#include "common/log.h"
#include "common/type/convert.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/datadir.h"
#include "mysql/interface.h"
#include "storage/iterator.h"
#include "storage/storage.h"

/***********************************************************************************************************************************
Refine versionNum based on additional file markers. Always picks the HIGHER of the existing value and the new one — so we never
regress as more evidence accumulates.
***********************************************************************************************************************************/
static void
mysqlDataDirRaiseVersion(MysqlDataDirInfo *const info, const unsigned int candidate)
{
    if (candidate > info->versionNum)
        info->versionNum = candidate;
}

/***********************************************************************************************************************************
Parse <dataPath>/grastate.dat for the Galera cluster state. Format is YAML-ish (key:value, not INI's key=value), small (5-7
lines). Sets info->galeraStateUuid, galeraSeqno, safeToBootstrap. Caller is responsible for the parent-context allocation
expected of strDup'd fields. Silently tolerates a missing or malformed file.
***********************************************************************************************************************************/
static void
dataDirReadGalera(const Storage *const storage, const String *const dataPath, MysqlDataDirInfo *const info)
{
    info->galeraSeqno = -1;
    info->safeToBootstrap = -1;

    TRY_BEGIN()
    {
        const String *const grastatePath = strNewFmt("%s/grastate.dat", strZ(dataPath));
        Buffer *const content = storageGetP(storageNewReadP(storage, grastatePath, .ignoreMissing = true));

        if (content == NULL)
            return;

        const String *const text = strNewBuf(content);
        StringList *const lines = strLstNewSplit(text, STRDEF("\n"));

        for (unsigned int i = 0; i < strLstSize(lines); i++)
        {
            const String *const line = strTrim(strDup(strLstGet(lines, i)));

            if (strBeginsWithZ(line, "uuid:"))
            {
                const String *const uuid = strTrim(strSubN(line, 5, strSize(line) - 5));

                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    info->galeraStateUuid = strDup(uuid);
                }
                MEM_CONTEXT_PRIOR_END();
            }
            else if (strBeginsWithZ(line, "seqno:"))
            {
                const String *const seqStr = strTrim(strSubN(line, 6, strSize(line) - 6));
                info->galeraSeqno = cvtZToInt64(strZ(seqStr));
            }
            else if (strBeginsWithZ(line, "safe_to_bootstrap:"))
            {
                const String *const stbStr = strTrim(strSubN(line, 18, strSize(line) - 18));
                info->safeToBootstrap = cvtZToInt(strZ(stbStr));
            }
        }
    }
    CATCH_ANY()
    {
        LOG_DETAIL_FMT("grastate.dat parse failed: %s", errorMessage());
    }
    TRY_END();
}

/***********************************************************************************************************************************
Refine info from the InnoDB redo log header's LOG_HEADER_CREATOR string + LOG_HEADER_FORMAT field. The creator string ("MySQL
X.Y.Z" / "MariaDB X.Y.Z" / "MySQL X.Y.Z-N" for Percona) gives us EXACT version + a vendor signal we couldn't get from
filesystem heuristics alone.
***********************************************************************************************************************************/
static void
dataDirReadRedoCreator(const Storage *const storage, const String *const dataPath, MysqlDataDirInfo *const info)
{
    TRY_BEGIN()
    {
        MysqlRedoCreator creator = mysqlRedoCreatorRead(storage, dataPath);

        if (creator.versionNum != 0)
        {
            info->versionNum = creator.versionNum;
            info->versionExact = true;
        }

        info->redoFormatNum = creator.formatNum;
        info->encryptedRedo = creator.encryptedRedo;

        // Override vendor only if filesystem heuristic didn't already pin us to MariaDB or Percona via engine-specific markers.
        // The creator string can't distinguish Percona from upstream MySQL because Percona uses "MySQL X.Y.Z-N" too — the
        // audit.log signal is more specific, so don't override Percona to Mysql.
        if (creator.vendor != mysqlVendorUnknown &&
            (info->vendor == mysqlVendorUnknown ||
             (info->vendor == mysqlVendorMysql && creator.vendor == mysqlVendorMariadb)))
        {
            info->vendor = creator.vendor;
        }
    }
    CATCH_ANY()
    {
        LOG_DETAIL_FMT("redo log creator inspection failed: %s", errorMessage());
    }
    TRY_END();
}

/**********************************************************************************************************************************/
FN_EXTERN MysqlDataDirInfo *
mysqlDataDirInspect(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    MysqlDataDirInfo *info = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        // Result struct lives in parent context so the caller owns it
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            info = memNew(sizeof(MysqlDataDirInfo));
            *info = (MysqlDataDirInfo){0};
        }
        MEM_CONTEXT_PRIOR_END();

        // Pass 1: top level — collect markers + remember schema-looking dirs for pass 2
        StringList *const schemaDirs = strLstNew();
        bool sawIbdata = false;
        bool sawMysqlIbd = false;
        bool sawAriaControl = false;
        bool sawAuditLog = false;
        bool sawTokudbEnv = false;

        StorageIterator *const topItr = storageNewItrP(storage, dataPath, .level = storageInfoLevelType);

        while (storageItrMore(topItr))
        {
            const StorageInfo entry = storageItrNext(topItr);

            if (!entry.exists)
                continue;

            if (entry.type == storageTypeFile)
            {
                if (strBeginsWithZ(entry.name, "ibdata"))
                {
                    sawIbdata = true;
                    info->hasInnodb = true;
                }
                else if (strEqZ(entry.name, MYSQL_FILE_MYSQL_IBD))
                {
                    sawMysqlIbd = true;
                    info->hasInnodb = true;
                    // mysql.ibd → 8.0+ data dictionary
                    mysqlDataDirRaiseVersion(info, 80000);
                }
                else if (strEqZ(entry.name, "aria_log_control") || strBeginsWithZ(entry.name, "aria_log."))
                {
                    sawAriaControl = true;
                    info->hasAria = true;
                    info->vendor = mysqlVendorMariadb;
                }
                else if (strBeginsWithZ(entry.name, "audit.log"))
                {
                    sawAuditLog = true;
                    if (info->vendor == mysqlVendorUnknown)
                        info->vendor = mysqlVendorPercona;
                }
                else if (strEqZ(entry.name, "tokudb.environment") || strEqZ(entry.name, "tokudb.directory") ||
                         strEqZ(entry.name, "tokudb.rollback"))
                {
                    sawTokudbEnv = true;
                    info->hasTokudb = true;
                }
                else if (strEndsWithZ(entry.name, ".tokudb"))
                {
                    info->hasTokudb = true;
                }
                else if (strEqZ(entry.name, "grastate.dat") || strEqZ(entry.name, "gvwstate.dat"))
                {
                    info->hasGalera = true;
                    // Default vendor when only the Galera signal is seen — both MariaDB Galera Cluster and Percona XtraDB
                    // Cluster write these files; we can't disambiguate from the file alone. Don't override if a more
                    // specific signal already set vendor.
                }
            }
            else if (entry.type == storageTypePath)
            {
                if (strEqZ(entry.name, MYSQL_PATH_INNODB_REDO))
                {
                    info->hasInnodb = true;
                    info->redoLayout = mysqlRedoLayoutDynamicInnodbRedo;
                    // #innodb_redo/ → 8.0.30+
                    mysqlDataDirRaiseVersion(info, 80030);
                }
                else if (strEqZ(entry.name, ".rocksdb"))
                {
                    info->hasMyrocks = true;
                    if (info->vendor == mysqlVendorUnknown)
                        info->vendor = mysqlVendorPercona;                        // .rocksdb (no hash prefix) is Percona convention
                }
                else if (strEqZ(entry.name, "#rocksdb"))
                {
                    info->hasMyrocks = true;
                    info->vendor = mysqlVendorMariadb;                            // #rocksdb (hash prefix) is mariabackup convention
                }
                else if (strSize(entry.name) > 0 && strZ(entry.name)[0] != '.' && !strEqZ(entry.name, "lost+found") &&
                         !strEqZ(entry.name, MYSQL_PATH_INNODB_DBLWR) && !strEqZ(entry.name, MYSQL_PATH_INNODB_REDO))
                {
                    // The presence of certain schema dirs is itself a version signal — raise here so empty dirs still count.
                    if (strEqZ(entry.name, "sys"))
                        mysqlDataDirRaiseVersion(info, 50700);          // sys schema introduced in MySQL 5.7
                    else if (strEqZ(entry.name, "performance_schema"))
                        mysqlDataDirRaiseVersion(info, 50500);          // performance_schema introduced in 5.5

                    strLstAdd(schemaDirs, entry.name);
                }
            }
        }

        // Pass 2: per-schema markers — InnoDB (.ibd), MyISAM (.MYD), Aria (.MAD/.MAI), and 5.6 GTID artifact (mysql/gtid_executed.*)
        for (unsigned int i = 0; i < strLstSize(schemaDirs); i++)
        {
            const String *const schema = strLstGet(schemaDirs, i);
            const String *const schemaPath = strNewFmt("%s/%s", strZ(dataPath), strZ(schema));

            StorageIterator *const itr = storageNewItrP(
                storage, schemaPath, .level = storageInfoLevelType, .nullOnMissing = true);

            if (itr == NULL)
                continue;

            while (storageItrMore(itr))
            {
                const StorageInfo file = storageItrNext(itr);

                if (!file.exists || file.type != storageTypeFile)
                    continue;

                if (strEndsWithZ(file.name, ".ibd"))
                {
                    info->hasInnodb = true;
                }
                else if (strEndsWithZ(file.name, ".MYD") || strEndsWithZ(file.name, ".MYI"))
                {
                    info->hasMyisam = true;
                }
                else if (strEndsWithZ(file.name, ".ISD") || strEndsWithZ(file.name, ".ISM"))
                {
                    info->hasIsam = true;                               // MySQL 3.21 → 4.0.2 — predates MyISAM
                }
                else if (strEndsWithZ(file.name, ".MAD") || strEndsWithZ(file.name, ".MAI"))
                {
                    info->hasAria = true;
                    info->vendor = mysqlVendorMariadb;
                }
                else if (strEqZ(schema, "mysql") &&
                         (strBeginsWithZ(file.name, "gtid_executed.") || strBeginsWithZ(file.name, "gtid_slave_pos.")))
                {
                    // mysql/gtid_executed.frm/.ibd was added in 5.6.5; mysql/gtid_slave_pos in MariaDB 10.0
                    mysqlDataDirRaiseVersion(info, 50605);
                }
                else if (strEqZ(schema, "sys"))
                {
                    // sys schema → 5.7+
                    mysqlDataDirRaiseVersion(info, 50700);
                }
            }

            // 5.7-vs-pre check: any .frm files alongside .ibd in a schema dir means pre-8.0
            // (8.0+ removed .frm; schema lives in mysql.ibd's SDI). If we already raised to 80000+ this is a contradiction
            // we shouldn't lower; keep the higher value.
        }

        // Server UUID via auto.cnf — never throws
        TRY_BEGIN()
        {
            String *const uuid = mysqlAutoCnfReadUuid(storage, dataPath);

            if (uuid != NULL)
            {
                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    info->serverUuid = strDup(uuid);
                }
                MEM_CONTEXT_PRIOR_END();
            }
        }
        CATCH_ANY()
        {
            // Malformed auto.cnf — don't propagate; leave serverUuid NULL
            LOG_DETAIL_FMT("auto.cnf inspection failed: %s", errorMessage());
        }
        TRY_END();

        // InnoDB tablespace details when present. Single page-0 read feeds both the FSP-flag decode and the adaptive checksum
        // probe — previously the probe re-opened the same file and read the same bytes a second time.
        if (sawIbdata || sawMysqlIbd)
        {
            TRY_BEGIN()
            {
                const String *const probePath =
                    sawIbdata
                        ? strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_IBDATA1)
                        : strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD);

                Buffer *const page0 = storageGetP(
                    storageNewReadP(storage, probePath, .limit = VARUINT64(mysqlPageSize64K)));

                MysqlControl ctl = mysqlControlFromPage0(bufPtrConst(page0), bufUsed(page0), sawMysqlIbd);

                info->pageSize = ctl.pageSize;
                info->encrypted = ctl.encrypted;
                info->pageChecksum = ctl.pageChecksum;                  // Either FullCrc32 (definitive) or None (caller probes)
                info->antelope = ctl.antelope;
                info->zipSsize = ctl.zipSsize;
                mysqlDataDirRaiseVersion(info, ctl.versionNum);

                // Adaptive checksum probe — runs on the SAME buffer we just decoded the FSP header from. Skipped if
                // FCRC32_MARKER already pinned the algorithm above.
                if (info->pageChecksum == mysqlPageChecksumNone && info->pageSize > 0 && bufUsed(page0) >= info->pageSize)
                {
                    const MysqlPageChecksumAlgo detected =
                        mysqlPageChecksumValidateAdaptive(bufPtrConst(page0), info->pageSize, /*pageNo*/ 0);

                    if (detected != mysqlPageChecksumNone)
                        info->pageChecksum = detected;
                }
            }
            CATCH_ANY()
            {
                LOG_DETAIL_FMT("ibdata1 inspection failed: %s", errorMessage());
            }
            TRY_END();
        }

        // Redo layout (if not already set by #innodb_redo dir presence)
        if (info->redoLayout == mysqlRedoLayoutUnknown && info->hasInnodb)
            info->redoLayout = mysqlRedoLayoutDetect(storage, dataPath);

        // Redo log header creator + format-num refinement (covers InnoDB datadirs only)
        if (info->hasInnodb)
            dataDirReadRedoCreator(storage, dataPath, info);

        // Galera cluster state (grastate.dat parse)
        if (info->hasGalera)
            dataDirReadGalera(storage, dataPath, info);
        else
        {
            info->galeraSeqno = -1;
            info->safeToBootstrap = -1;
        }

        // If we still haven't set vendor, default to MySQL
        if (info->vendor == mysqlVendorUnknown && info->hasInnodb)
            info->vendor = mysqlVendorMysql;

        // Suppress unused-variable warnings — these are kept for future refinements
        (void)sawAriaControl;
        (void)sawAuditLog;
        (void)sawTokudbEnv;
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(MY_DATADIR_INFO, info);
}

/**********************************************************************************************************************************/
FN_EXTERN void
mysqlDataDirInfoToLog(const MysqlDataDirInfo *const this, StringStatic *const debugLog)
{
    if (this == NULL)
    {
        strStcCat(debugLog, "null");
        return;
    }

    strStcFmt(
        debugLog,
        "{vendor: %u, versionNum: %u, exact: %s, pageSize: %u, redoLayout: %u, engines:[innodb=%s,myisam=%s,aria=%s,myrocks=%s,toku=%s]}",
        (unsigned int)this->vendor, this->versionNum, this->versionExact ? "true" : "false",
        (unsigned int)this->pageSize, (unsigned int)this->redoLayout,
        this->hasInnodb ? "y" : "n", this->hasMyisam ? "y" : "n", this->hasAria ? "y" : "n",
        this->hasMyrocks ? "y" : "n", this->hasTokudb ? "y" : "n");
}

/***********************************************************************************************************************************
Render a vendor enum as a human-readable string for the summary
***********************************************************************************************************************************/
static const char *
summarizeVendor(const MysqlVendor v)
{
    switch (v)
    {
        case mysqlVendorMysql:   return "MySQL Community";
        case mysqlVendorMariadb: return "MariaDB";
        case mysqlVendorPercona: return "Percona Server";
        case mysqlVendorUnknown: default: return "(unknown)";
    }
}

/***********************************************************************************************************************************
Render the version number as M.m.p (e.g., 80036 → "8.0.36"). Returns "(unknown)" for 0.
***********************************************************************************************************************************/
static String *
summarizeVersion(const unsigned int v)
{
    if (v == 0)
        return strNewZ("(unknown)");

    return strNewFmt("%u.%u.%u", v / 10000, (v / 100) % 100, v % 100);
}

/**********************************************************************************************************************************/
FN_EXTERN String *
mysqlDataDirSummarize(const MysqlDataDirInfo *const info)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(MY_DATADIR_INFO, info);
    FUNCTION_LOG_END();

    ASSERT(info != NULL);

    String *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        String *const out = strNew();
        const String *const versionStr = summarizeVersion(info->versionNum);

        // Top line — vendor + version + exactness
        strCatFmt(
            out,
            "DETECTED  %s %s%s\n",
            summarizeVendor(info->vendor),
            strZ(versionStr),
            info->versionExact ? "  [exact via redo log creator]" : "  [inferred from filesystem]");

        if (info->serverUuid != NULL)
            strCatFmt(out, "          server-uuid: %s\n", strZ(info->serverUuid));

        // InnoDB layout
        if (info->hasInnodb)
        {
            const char *const layoutName =
                info->redoLayout == mysqlRedoLayoutFixedIbLogfile      ? "ib_logfile{0,1} (pre-8.0.30)" :
                info->redoLayout == mysqlRedoLayoutDynamicInnodbRedo   ? "#innodb_redo/ (8.0.30+)" :
                info->redoLayout == mysqlRedoLayoutMariaDb107          ? "MariaDB 10.5+" : "(unknown)";

            const char *const checksumName =
                info->pageChecksum == mysqlPageChecksumCrc32       ? "crc32" :
                info->pageChecksum == mysqlPageChecksumStrictCrc32 ? "strict_crc32" :
                info->pageChecksum == mysqlPageChecksumInnodb      ? "innodb (legacy)" :
                info->pageChecksum == mysqlPageChecksumFullCrc32   ? "full_crc32 (MariaDB)" :
                                                                     "(probe at copy time)";

            strCatFmt(
                out,
                "InnoDB    page-size: %u, file-format: %s, checksum: %s, redo-layout: %s",
                (unsigned int)info->pageSize,
                info->antelope ? "Antelope" : "Barracuda+",
                checksumName,
                layoutName);

            if (info->redoFormatNum != 0)
            {
                strCatFmt(out, ", redo-format: 0x%08x", info->redoFormatNum);
                if (info->encryptedRedo)
                    strCatZ(out, " [ENCRYPTED]");
            }

            strCatChr(out, '\n');

            if (info->encrypted)
                strCatZ(out, "          ENCRYPTION enabled — keyring required at restore\n");

            if (info->zipSsize > 0)
                strCatFmt(out, "          compressed-pages possible (zip_ssize=%u)\n", info->zipSsize);
        }

        // Engine summary line — comma-separated list of present engines (or "(none detected)" when empty)
        strCatZ(out, "Engines   ");

        const struct { bool present; const char *name; } engineRow[] = {
            {info->hasInnodb,  "innodb"},
            {info->hasMyisam,  "myisam"},
            {info->hasIsam,    "isam"},
            {info->hasAria,    "aria"},
            {info->hasMyrocks, "myrocks"},
            {info->hasTokudb,  "tokudb"},
        };

        bool first = true;
        for (size_t i = 0; i < sizeof(engineRow) / sizeof(engineRow[0]); i++)
        {
            if (!engineRow[i].present)
                continue;

            strCatFmt(out, "%s%s", first ? "" : ", ", engineRow[i].name);
            first = false;
        }

        if (first)
            strCatZ(out, "(none detected)");

        strCatChr(out, '\n');

        // Galera summary
        if (info->hasGalera)
        {
            strCatFmt(
                out,
                "Galera    cluster member; state-uuid: %s, last-seqno: %" PRId64 ", safe-to-bootstrap: %s\n",
                info->galeraStateUuid != NULL ? strZ(info->galeraStateUuid) : "(unknown)",
                info->galeraSeqno,
                info->safeToBootstrap == 1 ? "YES (can bootstrap new cluster)" :
                info->safeToBootstrap == 0 ? "no (must wait for primary node)" : "(unrecorded)");
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
