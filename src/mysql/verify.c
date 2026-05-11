/***********************************************************************************************************************************
Datadir Integrity Verifier
***********************************************************************************************************************************/
#include <build.h>

#include "command/backup/tableSpace.h"
#include "common/debug.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/type/string.h"
#include "common/type/stringList.h"
#include "mysql/pageValidate.h"
#include "mysql/verify.h"
#include "storage/storage.h"

/**********************************************************************************************************************************/
FN_EXTERN MysqlVerifyResult *
mysqlVerify(const Storage *const storage, const String *const dataPath)
{
    FUNCTION_LOG_BEGIN(logLevelInfo);
        FUNCTION_LOG_PARAM(STORAGE, storage);
        FUNCTION_LOG_PARAM(STRING, dataPath);
    FUNCTION_LOG_END();

    ASSERT(storage != NULL);
    ASSERT(dataPath != NULL);

    MysqlVerifyResult *result = NULL;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        MEM_CONTEXT_PRIOR_BEGIN()
        {
            result = memNew(sizeof(MysqlVerifyResult));
            *result = (MysqlVerifyResult){0};
            result->invalidFiles = strLstNew();
        }
        MEM_CONTEXT_PRIOR_END();

        TableSpaceIter *const iter = tableSpaceIterNew(storage, dataPath);
        const unsigned int fileTotal = tableSpaceIterFileTotal(iter);

        LOG_INFO_FMT("verify: scanning %u InnoDB tablespace file(s) under %s", fileTotal, strZ(dataPath));

        String *relPath;

        while ((relPath = tableSpaceIterNext(iter)) != NULL)
        {
            const String *const absPath = strNewFmt("%s/%s", strZ(dataPath), strZ(relPath));

            const MysqlPageValidateResult r = mysqlPageValidateFile(storage, absPath, /*pageSizeHint*/ 0);

            result->filesScanned++;
            result->pagesChecked += r.pagesChecked;
            result->pagesValid += r.pagesValid;
            result->pagesInvalid += r.pagesInvalid;
            result->pagesSkipped += r.pagesSkipped;

            if (r.pagesInvalid > 0)
            {
                LOG_WARN_FMT(
                    "verify: %s — %" PRIu64 " invalid page(s) of %" PRIu64 " checked",
                    strZ(relPath), r.pagesInvalid, r.pagesChecked);

                MEM_CONTEXT_PRIOR_BEGIN()
                {
                    strLstAdd(result->invalidFiles, relPath);
                }
                MEM_CONTEXT_PRIOR_END();
            }
            else
            {
                LOG_DETAIL_FMT(
                    "verify: %s — %" PRIu64 " page(s) OK (%" PRIu64 " skipped)",
                    strZ(relPath), r.pagesValid, r.pagesSkipped);
            }
        }

        tableSpaceIterFree(iter);

        LOG_INFO_FMT(
            "verify complete: %" PRIu64 " files, %" PRIu64 " pages (%" PRIu64 " valid, %" PRIu64 " invalid, %" PRIu64 " skipped)",
            result->filesScanned, result->pagesChecked, result->pagesValid, result->pagesInvalid, result->pagesSkipped);
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_P(VOID, result);
}
