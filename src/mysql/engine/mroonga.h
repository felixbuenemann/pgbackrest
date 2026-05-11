/***********************************************************************************************************************************
Mroonga Engine Module (MariaDB only — and only when the plugin is installed)

Mroonga is a MariaDB storage engine for Japanese-language full-text search, backed by the Groonga library. Per-table files in
the schema directory:
  <table>.mrn          — Groonga's main database file
  <table>.mrn.NNNNNNNN — segment files (000000001, 000000002, ...)
  <table>.mrn.c        — column store
  <table>.mrn.l        — lexicon
  <table>.mrn.s        — segment metadata
  <table>.mrn.i        — index segment
  <table>.frm          — schema descriptor

The actual on-disk format is opaque (Groonga's). For backup purposes we treat it as a flat copy: with the table held under
BACKUP STAGE BLOCK_DDL no writer can append, so a straight file copy is consistent.
***********************************************************************************************************************************/
#ifndef MYSQL_ENGINE_MROONGA_H
#define MYSQL_ENGINE_MROONGA_H

#include "mysql/engine/engine.h"

FN_EXTERN const EngineHandler *engineMroongaHandler(void);

#endif
