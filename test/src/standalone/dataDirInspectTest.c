/***********************************************************************************************************************************
Standalone test for src/mysql/datadir.c — mysqlDataDirInspect against synthetic datadirs

Synthesizes representative datadirs for every supported flavor + version and asserts the inspector returns the right vendor +
versionNum + engine flags + serverUuid WITHOUT consulting any mysqld binary.

The motivating scenario from the design discussion: a 5.7 datadir on a host where 8.0 is installed. This test verifies the
inspector says "5.7" based on the absence of mysql.ibd, regardless of what binary the host has.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/crc32c.h"
#include "mysql/datadir.h"
#include "mysql/interface.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

static void mkdirP(const char *const p) { mkdir(p, 0755); }
static void touch(const char *const p, const char *const data)
{
    FILE *fp = fopen(p, "w"); if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", p); fputs(data, fp); fclose(fp);
}
static void rmrf(const char *const p) { char c[1024]; snprintf(c, sizeof(c), "rm -rf '%s'", p); int u __attribute__((unused)) = system(c); }

/***********************************************************************************************************************************
Build a minimal valid ibdata1 page 0 with FSP_SPACE_FLAGS encoding the 16K default page size (ssize=0)
***********************************************************************************************************************************/
static void
writeMinimalIbdata1(const char *const path)
{
    unsigned char page[16384];
    memset(page, 0, sizeof(page));

    // FSP_SPACE_FLAGS at offset 54 (FIL_PAGE_DATA + 16) all-zero = 16K legacy default
    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fwrite(page, 1, sizeof(page), fp);
    fclose(fp);
}

/***********************************************************************************************************************************
Build a synthetic auto.cnf for the given UUID
***********************************************************************************************************************************/
static void
writeAutoCnf(const char *const path, const char *const uuid)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fprintf(fp, "[auto]\nserver-uuid=%s\n", uuid);
    fclose(fp);
}

/***********************************************************************************************************************************
Build a redo log file with LOG_HEADER_CREATOR set to the given string. Both MySQL and MariaDB write this at offset 16, length 32
bytes max, NUL-terminated.
***********************************************************************************************************************************/
static void
writeRedoWithCreator(const char *const path, const char *const creator)
{
    unsigned char header[64];
    memset(header, 0, sizeof(header));

    // LOG_HEADER_FORMAT @ 0    LOG_HEADER_LOG_UUID @ 4    LOG_HEADER_START_LSN @ 8
    // LOG_HEADER_CREATOR @ 16  (32 bytes, NUL-terminated)
    const size_t creatorLen = strlen(creator);
    memcpy(header + 16, creator, creatorLen < 32 ? creatorLen : 31);

    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fwrite(header, 1, sizeof(header), fp);
    fclose(fp);
}

/***********************************************************************************************************************************
Build a redo log file with explicit LOG_HEADER_FORMAT (offset 0..3 BE) + creator.
***********************************************************************************************************************************/
static void
writeRedoWithFormat(const char *const path, const uint32_t formatRaw, const char *const creator)
{
    unsigned char header[64];
    memset(header, 0, sizeof(header));

    header[0] = (unsigned char)((formatRaw >> 24) & 0xFF);
    header[1] = (unsigned char)((formatRaw >> 16) & 0xFF);
    header[2] = (unsigned char)((formatRaw >> 8) & 0xFF);
    header[3] = (unsigned char)(formatRaw & 0xFF);

    if (creator != NULL)
    {
        const size_t creatorLen = strlen(creator);
        memcpy(header + 16, creator, creatorLen < 32 ? creatorLen : 31);
    }

    FILE *const fp = fopen(path, "wb");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s)", path);
    fwrite(header, 1, sizeof(header), fp);
    fclose(fp);
}

/**********************************************************************************************************************************/
int
main(void)
{
    static const ErrorHandlerFunction errorHandlerList[] = {stackTraceClean, memContextClean};
    errorHandlerSet(errorHandlerList, LENGTH_OF(errorHandlerList));
    logInit(logLevelOff, logLevelError, logLevelOff, false, 0, 1, false);

    int rc = 0;

    TRY_BEGIN()
    {
        printf("Datadir inspector tests:\n");

        const char *const root = "/tmp/mybackrest-datadir-test";

        // ============================================================================================================================
        // Scenario 1: MySQL 5.7 datadir (the upgrade-workflow case the user raised)
        //    Host might have 8.0 mysqld installed but the datadir hasn't been migrated.
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test/sakila");
        mkdirP("/tmp/mybackrest-datadir-test/sys");                     // sys/ schema is 5.7+
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        touch("/tmp/mybackrest-datadir-test/ib_logfile0", "");
        touch("/tmp/mybackrest-datadir-test/ib_logfile1", "");
        touch("/tmp/mybackrest-datadir-test/sakila/actor.ibd", "");
        touch("/tmp/mybackrest-datadir-test/sakila/actor.frm", "");     // .frm coexisting with .ibd → pre-8.0
        writeAutoCnf("/tmp/mybackrest-datadir-test/auto.cnf", "11111111-2222-3333-4444-555555555555");

        const Storage *const storage = storagePosixNewP(STR(root));
        MysqlDataDirInfo *info1 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[5.7] vendor=MySQL", info1->vendor == mysqlVendorMysql);
        expect("[5.7] versionNum >= 50700", info1->versionNum >= 50700);
        expect("[5.7] versionNum < 80000 (no mysql.ibd!)", info1->versionNum < 80000);
        expect("[5.7] hasInnodb=true", info1->hasInnodb);
        expect("[5.7] hasMyisam=false", !info1->hasMyisam);
        expect("[5.7] hasAria=false", !info1->hasAria);
        expect("[5.7] redoLayout=fixed", info1->redoLayout == mysqlRedoLayoutFixedIbLogfile);
        expect("[5.7] serverUuid extracted",
            info1->serverUuid != NULL && strEqZ(info1->serverUuid, "11111111-2222-3333-4444-555555555555"));

        // ============================================================================================================================
        // Scenario 2: MySQL 8.0.30+ datadir (mysql.ibd present + #innodb_redo dir)
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test/sakila");
        mkdirP("/tmp/mybackrest-datadir-test/#innodb_redo");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/mysql.ibd");
        touch("/tmp/mybackrest-datadir-test/sakila/actor.ibd", "");
        touch("/tmp/mybackrest-datadir-test/undo_001.ibu", "");

        MysqlDataDirInfo *info2 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[8.0.30+] vendor=MySQL", info2->vendor == mysqlVendorMysql);
        expect("[8.0.30+] versionNum >= 80030", info2->versionNum >= 80030);
        expect("[8.0.30+] hasInnodb=true", info2->hasInnodb);
        expect("[8.0.30+] redoLayout=dynamic", info2->redoLayout == mysqlRedoLayoutDynamicInnodbRedo);

        // ============================================================================================================================
        // Scenario 3: MariaDB datadir (Aria control file + .MAD/.MAI tables)
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test/mysql");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        touch("/tmp/mybackrest-datadir-test/aria_log_control", "");
        touch("/tmp/mybackrest-datadir-test/aria_log.00000001", "");
        touch("/tmp/mybackrest-datadir-test/mysql/user.MAD", "");
        touch("/tmp/mybackrest-datadir-test/mysql/user.MAI", "");

        MysqlDataDirInfo *info3 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[MariaDB] vendor=MariaDB", info3->vendor == mysqlVendorMariadb);
        expect("[MariaDB] hasAria=true", info3->hasAria);
        expect("[MariaDB] hasInnodb=true (ibdata1)", info3->hasInnodb);

        // ============================================================================================================================
        // Scenario 4: Percona with MyRocks (.rocksdb subdir)
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test/.rocksdb");
        mkdirP("/tmp/mybackrest-datadir-test/sakila");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/mysql.ibd");
        touch("/tmp/mybackrest-datadir-test/sakila/actor.ibd", "");
        touch("/tmp/mybackrest-datadir-test/.rocksdb/000004.sst", "");
        touch("/tmp/mybackrest-datadir-test/audit.log", "");

        MysqlDataDirInfo *info4 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[Percona+MyRocks] vendor=Percona (.rocksdb + audit.log)", info4->vendor == mysqlVendorPercona);
        expect("[Percona+MyRocks] hasMyrocks=true", info4->hasMyrocks);
        expect("[Percona+MyRocks] hasInnodb=true", info4->hasInnodb);
        expect("[Percona+MyRocks] versionNum >= 80000 (mysql.ibd present)", info4->versionNum >= 80000);

        // ============================================================================================================================
        // Scenario 5: TokuDB (xelabs Percona variant)
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        touch("/tmp/mybackrest-datadir-test/tokudb.environment", "");
        touch("/tmp/mybackrest-datadir-test/tokudb.directory", "");
        touch("/tmp/mybackrest-datadir-test/_test_users_main_00000001.tokudb", "");

        MysqlDataDirInfo *info5 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[TokuDB] hasTokudb=true", info5->hasTokudb);

        // ============================================================================================================================
        // Scenario 6: MySQL 5.5 (no sys schema, no mysql.ibd, no gtid artifacts)
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test/test");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        touch("/tmp/mybackrest-datadir-test/ib_logfile0", "");
        touch("/tmp/mybackrest-datadir-test/test/t1.MYD", "");
        touch("/tmp/mybackrest-datadir-test/test/t1.MYI", "");

        MysqlDataDirInfo *info6 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[5.5] vendor=MySQL", info6->vendor == mysqlVendorMysql);
        expect("[5.5] hasMyisam=true", info6->hasMyisam);
        expect("[5.5] hasInnodb=true", info6->hasInnodb);
        expect("[5.5] versionNum < 50605 (no gtid_executed)", info6->versionNum < 50605);
        expect("[5.5] versionNum < 50700 (no sys schema)", info6->versionNum < 50700);

        // ============================================================================================================================
        // Scenario 6b: redo log creator — exact version detection from LOG_HEADER_CREATOR field
        //   Per the cloned percona-server source (storage/innobase/include/univ.i), Percona writes "MySQL X.Y.Z-N" where
        //   N = PERCONA_INNODB_VERSION (default 8). Upstream MySQL writes plain "MySQL X.Y.Z" with no suffix. MariaDB writes
        //   "MariaDB X.Y.Z-build-suffix".
        // ============================================================================================================================

        // Upstream MySQL 8.0.36 — no Percona suffix
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/mysql.ibd");
        writeRedoWithCreator("/tmp/mybackrest-datadir-test/ib_logfile0", "MySQL 8.0.36");

        MysqlDataDirInfo *infoMysqlExact = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[creator MySQL 8.0.36] versionExact=true", infoMysqlExact->versionExact);
        expect("[creator MySQL 8.0.36] versionNum == 80036", infoMysqlExact->versionNum == 80036);
        expect("[creator MySQL 8.0.36] vendor=MySQL (no suffix)", infoMysqlExact->vendor == mysqlVendorMysql);

        // Percona Server 8.0.36-8 — has the -N suffix that distinguishes it from upstream
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/mysql.ibd");
        writeRedoWithCreator("/tmp/mybackrest-datadir-test/ib_logfile0", "MySQL 8.0.36-8");

        MysqlDataDirInfo *infoPerconaExact = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[creator MySQL 8.0.36-8] versionNum == 80036", infoPerconaExact->versionNum == 80036);
        expect("[creator MySQL 8.0.36-8] vendor=Percona (-N suffix)", infoPerconaExact->vendor == mysqlVendorPercona);

        // MariaDB 10.11.6 — distinct prefix
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithCreator("/tmp/mybackrest-datadir-test/ib_logfile0", "MariaDB 10.11.6-MariaDB-0+deb12u1");

        MysqlDataDirInfo *infoMariaExact = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[creator MariaDB 10.11.6] versionNum == 101106", infoMariaExact->versionNum == 101106);
        expect("[creator MariaDB 10.11.6] vendor=MariaDB", infoMariaExact->vendor == mysqlVendorMariadb);

        // CLONE plugin output — version comes from datadir's other signals, vendor stays MySQL
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithCreator("/tmp/mybackrest-datadir-test/ib_logfile0", "MySQL Clone");

        MysqlDataDirInfo *infoClone = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[creator MySQL Clone] vendor=MySQL", infoClone->vendor == mysqlVendorMysql);
        // Note: version stays at the safe lower bound 50500 because "Clone" has no version digits

        // ============================================================================================================================
        // Scenario 6c: Antelope file format (the upgrade-from-4.1 case raised in code review)
        //   ibdata1 on a server originally installed as MySQL 4.1 still has POST_ANTELOPE bit clear in FSP_SPACE_FLAGS even
        //   after upgrades through 5.0/5.1/5.5/5.6. We must detect this so the backup manifest records it correctly and the
        //   restore-side sanity check can warn if the target server can't read Antelope (none currently can't, but recording
        //   the format is still the right thing to do).
        // ============================================================================================================================

        // Build ibdata1 with all FSP flag bits zero (= POST_ANTELOPE clear = Antelope, no compression, no encryption)
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        touch("/tmp/mybackrest-datadir-test/ib_logfile0", "");

        MysqlDataDirInfo *infoAntelope = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[Antelope] antelope=true (POST_ANTELOPE bit clear)", infoAntelope->antelope);
        expect("[Antelope] zipSsize=0 (no compressed tables)", infoAntelope->zipSsize == 0);
        expect("[Antelope] hasInnodb=true", infoAntelope->hasInnodb);

        // Now flip POST_ANTELOPE bit on (= Barracuda or later)
        unsigned char barracuda[16384];
        memset(barracuda, 0, sizeof(barracuda));
        barracuda[FSP_SPACE_FLAGS + 3] |= 0x01;                         // POST_ANTELOPE = bit 0 of FSP_SPACE_FLAGS
        FILE *fp = fopen("/tmp/mybackrest-datadir-test/ibdata1", "wb");
        if (fp == NULL) THROW(FileWriteError, "fopen");
        fwrite(barracuda, 1, sizeof(barracuda), fp);
        fclose(fp);

        MysqlDataDirInfo *infoBarracuda = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[Barracuda] antelope=false (POST_ANTELOPE bit set)", !infoBarracuda->antelope);

        // ============================================================================================================================
        // Scenario 6d: adaptive checksum probe — inspector reads page 0 and detects the actual algorithm
        //   Build an ibdata1 page 0 with a real CRC32 checksum that the inspector should probe and detect.
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        mkdirP("/tmp/mybackrest-datadir-test");

        unsigned char crc32Page[16384];
        memset(crc32Page, 0, sizeof(crc32Page));

        // Set FIL_PAGE_OFFSET = 0 (already zero)
        // Set FIL_PAGE_LSN trailer match (low half of FIL_PAGE_LSN must match low half of trailer)
        crc32Page[16 + 7] = 0x99;                                       // FIL_PAGE_LSN low byte
        crc32Page[16383] = 0x99;                                        // trailer LSN low byte (last byte of page)

        // Set FSP_SPACE_FLAGS at offset 54 to all-zero (default 16K page, Antelope, no encryption, no FCRC32 marker)

        // Sprinkle data starting at byte 58 (after FSP_SPACE_FLAGS at 54..57). Leaves FSP_SPACE_ID (38..41) zero so it matches
        // the also-zero FIL_PAGE_SPACE_ID (34..37) — required or mysqlControlFromIbdata throws "header is corrupt".
        for (size_t i = 58; i < 16384 - 8; i++)
            crc32Page[i] = (unsigned char)((i * 11) & 0xFF);

        // Compute InnoDB CRC-32C: c1 = crc32c(page[4..25] = 22 bytes), c2 = crc32c(page[38..16375]). Matches the canonical
        // buf_calc_page_crc32() in mysql-server/storage/innobase/buf/checksum.cc — note this is the Castagnoli polynomial
        // (0x82F63B78 reversed), NOT IEEE 802.3 which is what zlib's crc32() implements.
        const uint32_t c1 = mysqlCrc32c(0, crc32Page + FIL_PAGE_OFFSET,
                                        FIL_PAGE_FILE_FLUSH_LSN - FIL_PAGE_OFFSET);
        const uint32_t c2 = mysqlCrc32c(0, crc32Page + FIL_PAGE_DATA,
                                        16384 - FIL_PAGE_DATA - FIL_PAGE_TRAILER_SIZE);
        const uint32_t expected = c1 ^ c2;
        crc32Page[0] = (unsigned char)((expected >> 24) & 0xFF);
        crc32Page[1] = (unsigned char)((expected >> 16) & 0xFF);
        crc32Page[2] = (unsigned char)((expected >> 8) & 0xFF);
        crc32Page[3] = (unsigned char)(expected & 0xFF);

        FILE *fpProbe = fopen("/tmp/mybackrest-datadir-test/ibdata1", "wb");
        if (fpProbe == NULL) THROW(FileWriteError, "fopen");
        fwrite(crc32Page, 1, sizeof(crc32Page), fpProbe);
        fclose(fpProbe);

        MysqlDataDirInfo *infoProbe = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[probe] inspector detected CRC32 algorithm from page 0", infoProbe->pageChecksum == mysqlPageChecksumCrc32);

        // ============================================================================================================================
        // Scenario 6e1: LOG_HEADER_FORMAT exact-version detection (8.0.30 vs 8.0.0 vs MariaDB 10.5)
        // ============================================================================================================================

        // MySQL 8.0.30 — format = 6 (current MySQL VERSION_8_0_30)
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithFormat("/tmp/mybackrest-datadir-test/ib_logfile0", 6, "MySQL 8.0.36");

        MysqlDataDirInfo *info830 = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[fmt 6] redoFormatNum == 6 (MySQL 8.0.30+)", info830->redoFormatNum == 6);
        expect("[fmt 6] encryptedRedo = false", !info830->encryptedRedo);

        // MySQL 5.7.9 — format = 1
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithFormat("/tmp/mybackrest-datadir-test/ib_logfile0", 1, "MySQL 5.7.42");

        MysqlDataDirInfo *info579 = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[fmt 1] redoFormatNum == 1 (MySQL 5.7.9)", info579->redoFormatNum == 1);

        // MariaDB 10.5 — format = 0x50485953 ("PHYS")
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithFormat("/tmp/mybackrest-datadir-test/ib_logfile0", 0x50485953U, "MariaDB 10.5.20");

        MysqlDataDirInfo *info105 = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[fmt PHYS] redoFormatNum == 0x50485953", info105->redoFormatNum == 0x50485953U);
        expect("[fmt PHYS] encryptedRedo = false", !info105->encryptedRedo);

        // MariaDB 10.5 ENCRYPTED — format = 0x50485953 | 0x80000000
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithFormat(
            "/tmp/mybackrest-datadir-test/ib_logfile0", 0x50485953U | 0x80000000U, "MariaDB 10.5.20");

        MysqlDataDirInfo *info105enc = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[fmt ENC PHYS] high bit stripped → formatNum = 0x50485953", info105enc->redoFormatNum == 0x50485953U);
        expect("[fmt ENC PHYS] encryptedRedo = true", info105enc->encryptedRedo);

        // MariaDB 10.8 — format = 0x50687973 ("Phys")
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithFormat("/tmp/mybackrest-datadir-test/ib_logfile0", 0x50687973U, "MariaDB 10.8.0");

        MysqlDataDirInfo *info108 = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[fmt Phys] redoFormatNum == 0x50687973", info108->redoFormatNum == 0x50687973U);

        // ============================================================================================================================
        // Scenario 6e: Galera cluster state files — grastate.dat / gvwstate.dat present
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");

        // Synthesize a realistic grastate.dat
        FILE *fpGra = fopen("/tmp/mybackrest-datadir-test/grastate.dat", "w");
        if (fpGra == NULL) THROW(FileWriteError, "fopen");
        fprintf(fpGra,
            "# GALERA saved state\n"
            "version: 2.1\n"
            "uuid:    aaaa1111-bbbb-2222-cccc-333333333333\n"
            "seqno:   987654321\n"
            "safe_to_bootstrap: 0\n");
        fclose(fpGra);

        touch("/tmp/mybackrest-datadir-test/gvwstate.dat", "view state placeholder");

        MysqlDataDirInfo *infoGalera = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[Galera] hasGalera=true (grastate.dat present)", infoGalera->hasGalera);
        expect(
            "[Galera] cluster UUID extracted",
            infoGalera->galeraStateUuid != NULL &&
                strEqZ(infoGalera->galeraStateUuid, "aaaa1111-bbbb-2222-cccc-333333333333"));
        expect("[Galera] seqno extracted", infoGalera->galeraSeqno == 987654321);
        expect("[Galera] safe_to_bootstrap = 0 (must wait)", infoGalera->safeToBootstrap == 0);

        // Edge case: a node where safe_to_bootstrap=1 (can bootstrap a fresh cluster)
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        FILE *fpGra2 = fopen("/tmp/mybackrest-datadir-test/grastate.dat", "w");
        if (fpGra2 == NULL) THROW(FileWriteError, "fopen");
        fprintf(fpGra2,
            "# GALERA saved state\n"
            "version: 2.1\n"
            "uuid:    bbbb2222-cccc-3333-dddd-444444444444\n"
            "seqno:   42\n"
            "safe_to_bootstrap: 1\n");
        fclose(fpGra2);

        MysqlDataDirInfo *infoGalera2 = mysqlDataDirInspect(storage, STRDEF("."));
        expect("[Galera safe=1] safe_to_bootstrap = 1 (can bootstrap)", infoGalera2->safeToBootstrap == 1);

        // ============================================================================================================================
        // Scenario 6f: human-readable summary
        // ============================================================================================================================
        rmrf(root); mkdirP(root);
        writeMinimalIbdata1("/tmp/mybackrest-datadir-test/ibdata1");
        writeRedoWithCreator("/tmp/mybackrest-datadir-test/ib_logfile0", "MariaDB 10.11.6-MariaDB-0+deb12u1");
        touch("/tmp/mybackrest-datadir-test/aria_log_control", "");
        writeAutoCnf("/tmp/mybackrest-datadir-test/auto.cnf", "11111111-2222-3333-4444-555555555555");

        MysqlDataDirInfo *infoSum = mysqlDataDirInspect(storage, STRDEF("."));
        const String *const summary = mysqlDataDirSummarize(infoSum);
        const char *const sz = strZ(summary);

        printf("\n--- Sample summary output ---\n%s---\n", sz);

        expect("[summary] contains DETECTED line", strstr(sz, "DETECTED") != NULL);
        expect("[summary] mentions MariaDB vendor", strstr(sz, "MariaDB") != NULL);
        expect("[summary] mentions InnoDB section", strstr(sz, "InnoDB") != NULL);
        expect("[summary] mentions Engines section", strstr(sz, "Engines") != NULL);
        expect("[summary] mentions aria engine", strstr(sz, "aria") != NULL);
        expect("[summary] includes server-uuid", strstr(sz, "11111111-2222-3333-4444-555555555555") != NULL);
        expect("[summary] reports exact version (creator string parsed)", strstr(sz, "exact via redo log creator") != NULL);

        // ============================================================================================================================
        // Scenario 7: empty / non-MySQL directory — must not throw, must return all-zero
        // ============================================================================================================================
        rmrf(root);
        mkdirP(root);
        touch("/tmp/mybackrest-datadir-test/random.txt", "");

        MysqlDataDirInfo *info7 = mysqlDataDirInspect(storage, STRDEF("."));

        expect("[empty] no exception", true);                           // we got here
        expect("[empty] versionNum == 0", info7->versionNum == 0);
        expect("[empty] vendor=Unknown", info7->vendor == mysqlVendorUnknown);
        expect("[empty] no engine flags set",
            !info7->hasInnodb && !info7->hasMyisam && !info7->hasAria && !info7->hasMyrocks && !info7->hasTokudb);

        rmrf(root);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); rc = 1; }
    else if (rc == 0)     printf("\nAll assertions passed\n");

    return rc;
}
