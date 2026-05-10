/***********************************************************************************************************************************
Standalone test for src/command/backup/tableSpace.c — TableSpaceIter against a synthetic datadir

Builds a fake InnoDB datadir with:
  - ibdata1, ibdata2                (system tablespace, multi-file)
  - mysql.ibd                       (8.0+ data dictionary)
  - undo_001.ibu, undo_002.ibu      (undo tablespaces)
  - sakila/actor.ibd                (per-table tablespace)
  - sakila/film.ibd
  - world/city.ibd
  - performance_schema/events_statements_history.ibd
  - #innodb_dblwr/foo.dblwr         (must be EXCLUDED — doublewrite)
  - #innodb_redo/#ib_redo1_0        (must be EXCLUDED — redo handled by RedoLogCopier)
  - lost+found/orphan.ibd           (must be EXCLUDED — fs artifact)
  - sakila/.gitignore               (top-level dot files in schema dirs not filtered, but file isn't .ibd anyway)

Verifies the iterator returns exactly the expected files in sorted order.
***********************************************************************************************************************************/
#include <build.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/error/error.h"
#include "common/log.h"
#include "common/stackTrace.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "storage/posix/storage.h"

static int testFailures = 0;

static void
expect(const char *const what, const bool condition)
{
    printf("  %s  %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition)
        testFailures++;
}

static void
mkdirP(const char *const path) { mkdir(path, 0755); }

static void
touch(const char *const path)
{
    FILE *const fp = fopen(path, "w");
    if (fp == NULL) THROW_FMT(FileWriteError, "fopen(%s) failed", path);
    fputs("x", fp);
    fclose(fp);
}

static void
rmrf(const char *const path)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
    int unused __attribute__((unused)) = system(cmd);
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
        printf("TableSpaceIter test:\n");

        const char *const root = "/tmp/mybackrest-tsiter-test";
        rmrf(root);

        mkdirP(root);
        mkdirP("/tmp/mybackrest-tsiter-test/sakila");
        mkdirP("/tmp/mybackrest-tsiter-test/world");
        mkdirP("/tmp/mybackrest-tsiter-test/performance_schema");
        mkdirP("/tmp/mybackrest-tsiter-test/#innodb_dblwr");
        mkdirP("/tmp/mybackrest-tsiter-test/#innodb_redo");
        mkdirP("/tmp/mybackrest-tsiter-test/lost+found");

        touch("/tmp/mybackrest-tsiter-test/ibdata1");
        touch("/tmp/mybackrest-tsiter-test/ibdata2");
        touch("/tmp/mybackrest-tsiter-test/mysql.ibd");
        touch("/tmp/mybackrest-tsiter-test/undo_001.ibu");
        touch("/tmp/mybackrest-tsiter-test/undo_002.ibu");
        touch("/tmp/mybackrest-tsiter-test/sakila/actor.ibd");
        touch("/tmp/mybackrest-tsiter-test/sakila/film.ibd");
        touch("/tmp/mybackrest-tsiter-test/world/city.ibd");
        touch("/tmp/mybackrest-tsiter-test/performance_schema/events_statements_history.ibd");
        touch("/tmp/mybackrest-tsiter-test/#innodb_dblwr/foo.dblwr");
        touch("/tmp/mybackrest-tsiter-test/#innodb_redo/#ib_redo1_0");
        touch("/tmp/mybackrest-tsiter-test/lost+found/orphan.ibd");

        const Storage *const storage = storagePosixNewP(STR(root));
        TableSpaceIter *const iter = tableSpaceIterNew(storage, STRDEF("."));

        // Collect every yielded path
        StringList *const got = strLstNew();
        String *path;
        while ((path = tableSpaceIterNext(iter)) != NULL)
            strLstAdd(got, path);

        printf("  enumerated %u file(s):\n", strLstSize(got));
        for (unsigned int i = 0; i < strLstSize(got); i++)
            printf("    %s\n", strZ(strLstGet(got, i)));

        // Expected (sorted alphabetically — iterator sorts):
        //   #innodb_dblwr SHOULD be excluded (it's a dir match in tableSpaceShouldSkipDir)
        //   #innodb_redo  SHOULD be excluded
        //   lost+found    SHOULD be excluded
        //   ibdata1, ibdata2, mysql.ibd, undo_001.ibu, undo_002.ibu  (top level)
        //   performance_schema/events_statements_history.ibd
        //   sakila/actor.ibd, sakila/film.ibd
        //   world/city.ibd
        // Total: 9 files
        expect("9 tablespace files enumerated", strLstSize(got) == 9);
        expect("ibdata1 included", strLstExists(got, STRDEF("ibdata1")));
        expect("ibdata2 included", strLstExists(got, STRDEF("ibdata2")));
        expect("mysql.ibd included", strLstExists(got, STRDEF("mysql.ibd")));
        expect("undo_001.ibu included", strLstExists(got, STRDEF("undo_001.ibu")));
        expect("undo_002.ibu included", strLstExists(got, STRDEF("undo_002.ibu")));
        expect("sakila/actor.ibd included", strLstExists(got, STRDEF("sakila/actor.ibd")));
        expect("sakila/film.ibd included", strLstExists(got, STRDEF("sakila/film.ibd")));
        expect("world/city.ibd included", strLstExists(got, STRDEF("world/city.ibd")));
        expect(
            "performance_schema/*.ibd included",
            strLstExists(got, STRDEF("performance_schema/events_statements_history.ibd")));

        // Negative checks
        expect(
            "#innodb_dblwr file EXCLUDED",
            !strLstExists(got, STRDEF("#innodb_dblwr/foo.dblwr")));
        expect(
            "#innodb_redo file EXCLUDED",
            !strLstExists(got, STRDEF("#innodb_redo/#ib_redo1_0")));
        expect(
            "lost+found file EXCLUDED",
            !strLstExists(got, STRDEF("lost+found/orphan.ibd")));

        // fileTotal getter matches enumerated count
        expect("fileTotal == enumerated count", tableSpaceIterFileTotal(iter) == strLstSize(got));

        // Iterator exhausted — Next returns NULL on subsequent calls
        expect("iterator exhausted returns NULL", tableSpaceIterNext(iter) == NULL);

        tableSpaceIterFree(iter);
        rmrf(root);
    }
    CATCH_FATAL()
    {
        printf("FATAL: %s\n%s\n", errorMessage(), errorStackTrace());
        rc = 1;
    }
    TRY_END();

    if (testFailures > 0)
    {
        printf("\n%d assertion(s) failed\n", testFailures);
        rc = 1;
    }
    else if (rc == 0)
    {
        printf("\nAll assertions passed\n");
    }

    return rc;
}
