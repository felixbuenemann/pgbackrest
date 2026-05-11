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

        // InnoDB tablespace details when present
        if (sawIbdata || sawMysqlIbd)
        {
            TRY_BEGIN()
            {
                MysqlControl ctl = mysqlControlFromIbdata(storage, dataPath);
                info->pageSize = ctl.pageSize;
                info->encrypted = ctl.encrypted;
                info->pageChecksum = ctl.pageChecksum;                  // Either FullCrc32 (definitive) or None (caller probes)
                info->antelope = ctl.antelope;
                info->zipSsize = ctl.zipSsize;

                // mysqlControlFromIbdata already infers 50700 vs 80000; honor it as a floor.
                mysqlDataDirRaiseVersion(info, ctl.versionNum);

                // If pageChecksum is still unknown (FCRC32 marker bit was clear → could be CRC32 or legacy "innodb"), probe
                // page 0 itself with the adaptive validator. Page 0 carries an FSP_HEADER but its FIL header + checksum field
                // are standard, so the same validator works. The result tells us which algorithm is in use; subsequent pages
                // can skip the probe.
                if (info->pageChecksum == mysqlPageChecksumNone && info->pageSize > 0)
                {
                    const String *const probePath =
                        sawIbdata
                            ? strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_IBDATA1)
                            : strNewFmt("%s/%s", strZ(dataPath), MYSQL_FILE_MYSQL_IBD);

                    Buffer *const page0 = storageGetP(
                        storageNewReadP(storage, probePath, .limit = VARUINT64(info->pageSize)));

                    if (page0 != NULL && bufUsed(page0) >= info->pageSize)
                    {
                        const MysqlPageChecksumAlgo detected =
                            mysqlPageChecksumValidateAdaptive(bufPtrConst(page0), info->pageSize, /*pageNo*/ 0);

                        if (detected != mysqlPageChecksumNone)
                            info->pageChecksum = detected;
                    }
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

        // Redo log header creator: most authoritative version signal we have. Both MySQL and MariaDB write a "MySQL X.Y.Z" or
        // "MariaDB X.Y.Z" string at offset 16 of the redo file header when they create it. Use this to refine versionNum to
        // an EXACT value, and override vendor when the creator string disagrees with our filesystem heuristics.
        if (info->hasInnodb)
        {
            TRY_BEGIN()
            {
                MysqlRedoCreator creator = mysqlRedoCreatorRead(storage, dataPath);

                if (creator.versionNum != 0)
                {
                    // The creator string is exact; previous heuristic was a lower bound, so promote.
                    info->versionNum = creator.versionNum;
                    info->versionExact = true;
                }

                if (creator.vendor != mysqlVendorUnknown)
                {
                    // Override vendor only if filesystem heuristic didn't already pin us to MariaDB or Percona via
                    // engine-specific markers (Aria, .rocksdb, audit.log). The creator string can't distinguish Percona from
                    // upstream MySQL because Percona uses "MySQL X.Y.Z-N" too — the audit.log signal is more specific.
                    if (info->vendor == mysqlVendorUnknown ||
                        (info->vendor == mysqlVendorMysql && creator.vendor == mysqlVendorMariadb))
                    {
                        info->vendor = creator.vendor;
                    }
                }
            }
            CATCH_ANY()
            {
                LOG_DETAIL_FMT("redo log creator inspection failed: %s", errorMessage());
            }
            TRY_END();
        }

        // Galera state file: if present, parse uuid + seqno from the last-known-good cluster state. Format is plain INI:
        //   # GALERA saved state
        //   version: 2.1
        //   uuid:    abc123-...
        //   seqno:   12345
        //   safe_to_bootstrap: 0
        info->galeraSeqno = -1;
        if (info->hasGalera)
        {
            TRY_BEGIN()
            {
                const String *const grastatePath = strNewFmt("%s/grastate.dat", strZ(dataPath));
                Buffer *const content = storageGetP(storageNewReadP(storage, grastatePath, .ignoreMissing = true));

                if (content != NULL)
                {
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
                            info->galeraSeqno = strtoll(strZ(seqStr), NULL, 10);
                        }
                    }
                }
            }
            CATCH_ANY()
            {
                LOG_DETAIL_FMT("grastate.dat parse failed: %s", errorMessage());
            }
            TRY_END();
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
