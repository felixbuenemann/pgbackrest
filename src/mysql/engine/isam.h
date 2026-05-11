/***********************************************************************************************************************************
ISAM Engine Module — predecessor to MyISAM, lives only in MySQL 3.21 → 4.0.2

ISAM (Indexed Sequential Access Method) was MySQL's original storage engine. It was effectively superseded by MyISAM in MySQL
3.23 (which became the default) and removed entirely in 4.0.3 (released March 2003). Existing ISAM tables in a 3.21–4.0.2
datadir use:

  <table>.ISD   data file (analogous to MyISAM's .MYD)
  <table>.ISM   index file (analogous to MyISAM's .MYI)
  <table>.frm   schema descriptor (same format as MyISAM)

Backup is a straight file copy under whatever lock the orchestrator already holds — ISAM tables don't have transactions or
crash recovery, so a flat copy taken under FLUSH TABLES WITH READ LOCK is consistent.

Anyone running MySQL 3.21–4.0.2 in 2026 is doing so deliberately (museum installations, archival systems, vendor-locked
appliances). The supporting code is bounded enough that we may as well include it.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_ISAM_H
#define MYSQL_ENGINE_ISAM_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineIsamHandler(void);

#endif
