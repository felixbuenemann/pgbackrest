/***********************************************************************************************************************************
MySQL / MariaDB Backup Lock Ladder

Encapsulates the SQL sequence for taking and releasing the consistency lock during backup. Three methods supported, picked
either by --backup-lock-method or autodetected from the connected server's vendor and version:

  instance   — LOCK INSTANCE FOR BACKUP                  (MySQL 8.0.16+, Percona 8.0.16+)
  stage      — BACKUP STAGE START / FLUSH / BLOCK_DDL    (MariaDB 10.4+; four-step state machine)
               / BLOCK_COMMIT / END
  ftwrl      — FLUSH TABLES WITH READ LOCK               (legacy; MySQL 5.7, Percona <8.0.16, MariaDB 10.3)

The lock module exposes one call per protocol step, so Phase D's backup orchestrator can interleave file copies between the
states (essential for the BACKUP STAGE machinery — Aria + RocksDB tables are copied at different stages than InnoDB).
***********************************************************************************************************************************/
#ifndef MYSQL_LOCK_H
#define MYSQL_LOCK_H

#include "mysql/client.h"

typedef enum
{
    mysqlLockMethodAuto = 0,
    mysqlLockMethodInstance,                                            // LOCK INSTANCE FOR BACKUP
    mysqlLockMethodStage,                                               // BACKUP STAGE
    mysqlLockMethodFtwrl,                                               // FLUSH TABLES WITH READ LOCK
} MysqlLockMethod;

/***********************************************************************************************************************************
Pick a lock method given the user's preference and the live server's vendor/version. Throws if the user explicitly requested
a method the server doesn't support.
***********************************************************************************************************************************/
FN_EXTERN MysqlLockMethod mysqlLockMethodSelect(MysqlClient *client, MysqlLockMethod userPreference);

/***********************************************************************************************************************************
Acquire the early lock — for BACKUP STAGE this issues START + FLUSH; for instance/ftwrl this is a no-op (the real lock comes in
mysqlLockBlockDdl). Caller must invoke mysqlLockBlockDdl + mysqlLockBlockCommit + mysqlLockRelease in order.
***********************************************************************************************************************************/
FN_EXTERN void mysqlLockBegin(MysqlClient *client, MysqlLockMethod method);

/***********************************************************************************************************************************
BACKUP STAGE BLOCK_DDL transition. For instance method this issues LOCK INSTANCE FOR BACKUP; for ftwrl this is a no-op (the
final FLUSH happens in mysqlLockBlockCommit).
***********************************************************************************************************************************/
FN_EXTERN void mysqlLockBlockDdl(MysqlClient *client, MysqlLockMethod method);

/***********************************************************************************************************************************
BACKUP STAGE BLOCK_COMMIT transition. For instance method this is a no-op (LOCK INSTANCE already blocks new transactions). For
ftwrl this issues FLUSH TABLES WITH READ LOCK.
***********************************************************************************************************************************/
FN_EXTERN void mysqlLockBlockCommit(MysqlClient *client, MysqlLockMethod method);

/***********************************************************************************************************************************
Release everything. For BACKUP STAGE issues BACKUP STAGE END; for instance issues UNLOCK INSTANCE; for ftwrl issues UNLOCK TABLES.
Safe to call even if no earlier step ran (cleans up partial state from an aborted backup).
***********************************************************************************************************************************/
FN_EXTERN void mysqlLockRelease(MysqlClient *client, MysqlLockMethod method);

/***********************************************************************************************************************************
Render the method as a short string for logging
***********************************************************************************************************************************/
FN_EXTERN const char *mysqlLockMethodName(MysqlLockMethod method);

#endif
