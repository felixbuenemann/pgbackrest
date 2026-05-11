/***********************************************************************************************************************************
ARCHIVE Engine Module

ARCHIVE is a built-in storage engine in MySQL, MariaDB, and Percona Server. Designed for append-only data (audit logs, historical
records). Files per table:
  <table>.ARZ   — compressed data (zlib)
  <table>.frm   — schema descriptor (5.7 only; 8.0+ uses .sdi sidecar, captured by the miscellaneous-files pass)

Older versions (5.0) also had a .ARM (auto-increment metadata) file, but that was merged into .ARZ in 5.1+; the file isn't
present on any version this code targets.

ARCHIVE only supports INSERT and SELECT (no UPDATE/DELETE), so the engine's on-disk layout is effectively append-only. Copy
happens under the orchestrator's lock so a concurrent append can't truncate the file mid-read.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_ARCHIVE_H
#define MYSQL_ENGINE_ARCHIVE_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineArchiveHandler(void);

#endif
