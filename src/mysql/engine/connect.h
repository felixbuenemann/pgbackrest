/***********************************************************************************************************************************
CONNECT Engine Module (MariaDB only)

MariaDB's CONNECT engine projects external data sources as SQL tables. Local-file variants store data in the datadir; remote
variants (ODBC, MONGO, MYSQL, JDBC) keep no local files.

Per-table local files we always back up:
  <table>.dnx   — CONNECT index file (created for indexed tables; absent for index-less ones)
  <table>.frm   — schema descriptor (5.7-style; 8.0+ has SDI captured by miscFiles)
  <table>.<ext> — actual data file when TABLE_TYPE is a local format (CSV, XML, JSON, INI, DBF, FIXED). Extensions vary per
                  TYPE; common ones are .csv, .xml, .json, .dat. These are captured by the miscellaneous-files pass since
                  they're not on any engine's "claimed extensions" list.

Limitation: when a CREATE TABLE specifies `FILE_NAME=/absolute/path/elsewhere`, the data file is outside the datadir and this
handler can't reach it. Operators using out-of-datadir Connect tables must back those files up separately. Mariabackup has the
same limitation.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_CONNECT_H
#define MYSQL_ENGINE_CONNECT_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineConnectHandler(void);

#endif
