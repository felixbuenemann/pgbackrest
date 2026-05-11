/***********************************************************************************************************************************
Standalone test for src/mysql/lock.c — mysqlLockMethodSelect (no DB connection required)

The selection logic is pure: it inspects mysqlClientVendor() and mysqlClientServerVersionNum() and returns the appropriate lock
method. We construct a MysqlClient via mysqlClientNew() (which does NOT connect) and poke its public vendor/version fields to
exercise every branch of the selection table.

Because MysqlClientPub is a public struct, the test can mutate it directly — that's the point of pgBackRest's PUB pattern.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>

#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "mysql/client.h"
#include "mysql/lock.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) testFailures++;
}

/***********************************************************************************************************************************
Build a MysqlClient and poke its vendor/version fields. mysqlClientNew doesn't connect, so this is safe.
***********************************************************************************************************************************/
static MysqlClient *
makeFakeClient(const MysqlVendor vendor, const unsigned int version)
{
    MysqlClient *const client = mysqlClientNew(
        STRDEF("localhost"), 3306, NULL, STRDEF("test"), STRDEF("u"), NULL, 30000);

    // The MysqlClientPub is the first field of the opaque MysqlClient struct (pgBackRest object convention), so casting
    // the object pointer to MysqlClientPub* lets the test mutate vendor + version without an extra setter.
    MysqlClientPub *const pub = (MysqlClientPub *)client;
    pub->vendor = vendor;
    pub->serverVersionNum = version;

    return client;
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
        printf("Lock method selection test:\n");

        MEM_CONTEXT_TEMP_BEGIN()
        {
            // ---- auto path ----
            MysqlClient *c;

            c = makeFakeClient(mysqlVendorMysql, 80035);
            expect("MySQL 8.0.35 → instance", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodInstance);

            c = makeFakeClient(mysqlVendorMysql, 80015);                // <8.0.16 → ftwrl
            expect("MySQL 8.0.15 → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 50742);
            expect("MySQL 5.7.42 → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 50651);
            expect("MySQL 5.6.51 → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 50562);
            expect("MySQL 5.5.62 → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 50196);
            expect("MySQL 5.0.96 (pre-Plugin Antelope) → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 40128);
            expect("MySQL 4.1.28 → ftwrl (lowest practical floor)", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorMysql, 40031);
            expect("MySQL 4.0.31 → ftwrl (auto-retry handles old password)", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorPercona, 80016);
            expect("Percona 8.0.16 → instance", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodInstance);

            c = makeFakeClient(mysqlVendorMariadb, 100406);
            expect("MariaDB 10.4.6 → stage", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodStage);

            c = makeFakeClient(mysqlVendorMariadb, 100329);
            expect("MariaDB 10.3.29 → ftwrl", mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            c = makeFakeClient(mysqlVendorUnknown, 99999);
            expect("Unknown vendor → ftwrl (most conservative)",
                mysqlLockMethodSelect(c, mysqlLockMethodAuto) == mysqlLockMethodFtwrl);

            // ---- explicit user choice that the server supports ----
            c = makeFakeClient(mysqlVendorMariadb, 110402);
            expect("explicit stage on MariaDB 11.4 → stage",
                mysqlLockMethodSelect(c, mysqlLockMethodStage) == mysqlLockMethodStage);

            c = makeFakeClient(mysqlVendorMysql, 80020);
            expect("explicit ftwrl always allowed",
                mysqlLockMethodSelect(c, mysqlLockMethodFtwrl) == mysqlLockMethodFtwrl);

            // ---- explicit user choice that the server doesn't support — must throw ----
            c = makeFakeClient(mysqlVendorMariadb, 110000);
            bool threw = false;
            TRY_BEGIN()
            {
                mysqlLockMethodSelect(c, mysqlLockMethodInstance);
            }
            CATCH(OptionInvalidError)
            {
                threw = true;
            }
            TRY_END();
            expect("explicit instance on MariaDB → throws (vendor mismatch)", threw);

            c = makeFakeClient(mysqlVendorMysql, 50742);
            threw = false;
            TRY_BEGIN()
            {
                mysqlLockMethodSelect(c, mysqlLockMethodInstance);
            }
            CATCH(OptionInvalidError)
            {
                threw = true;
            }
            TRY_END();
            expect("explicit instance on MySQL 5.7 → throws (version mismatch)", threw);

            c = makeFakeClient(mysqlVendorMysql, 80020);
            threw = false;
            TRY_BEGIN()
            {
                mysqlLockMethodSelect(c, mysqlLockMethodStage);
            }
            CATCH(OptionInvalidError)
            {
                threw = true;
            }
            TRY_END();
            expect("explicit stage on MySQL → throws (vendor mismatch)", threw);

            // Render-name spot check
            expect("name(stage) == 'stage'", strcmp(mysqlLockMethodName(mysqlLockMethodStage), "stage") == 0);
            expect("name(instance) == 'instance'", strcmp(mysqlLockMethodName(mysqlLockMethodInstance), "instance") == 0);
        }
        MEM_CONTEXT_TEMP_END();
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n", errorMessage());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0) { printf("\n%d failed\n", testFailures); rc = 1; }
    else if (rc == 0)     printf("\nAll assertions passed\n");

    return rc;
}
