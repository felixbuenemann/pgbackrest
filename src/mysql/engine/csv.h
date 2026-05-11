/***********************************************************************************************************************************
CSV Engine Module

CSV is a built-in storage engine in both MySQL and MariaDB that stores table data as a plain text CSV file. Files per table:
  <table>.CSV   — data, comma-separated, one row per line
  <table>.CSM   — metadata (row count, etc.)
  <table>.frm   — schema descriptor (5.7 only; 8.0+ uses .sdi sidecar or mysql.ibd's DD)

Used by every MySQL install for the general_log and slow_log tables in the mysql/ schema. Even when users never touch the CSV
engine themselves, the system schema relies on it — skipping it means restored datadirs lose those log tables.

Like MyISAM, CSV is non-transactional. Copy happens under the orchestrator's lock so concurrent writers can't append mid-copy.
A safe flat copy is all that's needed; CSV has no transaction log or rollback structure.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_CSV_H
#define MYSQL_ENGINE_CSV_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineCsvHandler(void);

#endif
