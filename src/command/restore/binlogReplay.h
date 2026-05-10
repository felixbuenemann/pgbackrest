/***********************************************************************************************************************************
Binlog Replay for PITR (Phase E)

After Phase E's prepareInvokeMysqld leaves the datadir crash-recovered, point-in-time recovery is achieved by replaying the
archived binary logs from `binlogStop` (the position captured at backup end) up to the user's --target. Implementation drives
mysqlbinlog | mysql via common/exec.c rather than parsing the binlog ourselves:

  mysqlbinlog --start-position=<binlogStartPos> [--stop-datetime=... | --stop-position=... | --stop-gtid=...] <files...> \
      | mysql --defaults-file=<recovery_cnf>

The list of <files> is materialized from the archive store using src/mysql/binlog.c's GTID-range index.
***********************************************************************************************************************************/
#ifndef COMMAND_RESTORE_BINLOGREPLAY_H
#define COMMAND_RESTORE_BINLOGREPLAY_H

#include <stdint.h>

#include "common/type/string.h"
#include "common/type/stringList.h"
#include "storage/storage.h"

typedef struct BinlogReplayTarget
{
    enum { binlogTargetNone, binlogTargetTime, binlogTargetGtid, binlogTargetPosition, binlogTargetImmediate } kind;
    String *time;                                                       // YYYY-MM-DD HH:MM:SS
    String *gtid;                                                       // uuid:1-N
    String *binlogFile;                                                 // mysql-bin.000123
    uint64_t binlogPos;
} BinlogReplayTarget;

// Replay binlogs from startFile/startPos up to target. mysqldClient is the running recovered server.
FN_EXTERN void binlogReplay(
    const Storage *archiveStorage, const String *archivePath, const String *startFile, uint64_t startPos,
    const BinlogReplayTarget *target, const String *mysqlbinlogPath, const String *mysqlClientPath,
    const String *recoveryCnf);

#endif
