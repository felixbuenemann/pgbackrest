/***********************************************************************************************************************************
MERGE / MyISAMMRG Engine Module

MERGE is a meta-engine that presents a virtual union over multiple identically-structured MyISAM tables. Available in MySQL,
MariaDB, and Percona. Per-table file:
  <table>.MRG   — newline-separated list of underlying MyISAM table names
  <table>.frm   — schema descriptor (5.7 only; 8.0+ has SDI)

The actual data lives in the child MyISAM tables, which are already copied by the MyISAM engine handler. This handler only
copies the small .MRG definition file so the MERGE table reappears after restore.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_MERGE_H
#define MYSQL_ENGINE_MERGE_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineMergeHandler(void);

#endif
