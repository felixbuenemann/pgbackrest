/***********************************************************************************************************************************
CRC-32C (Castagnoli) — software implementation

Used by InnoDB's page-checksum "crc32" mode and by MariaDB's full_crc32 mode. NOT the same as zlib's crc32() which implements
IEEE 802.3 (the polynomial used by Ethernet / PNG / gzip). The two are wire-incompatible and produce different results for the
same input.
***********************************************************************************************************************************/
#ifndef MYSQL_CRC32C_H
#define MYSQL_CRC32C_H

#include <stddef.h>
#include <stdint.h>

#include "common/macro.h"

// Compute CRC-32C over len bytes. `crc` is the seed (pass 0 for a fresh computation; non-zero to chain across multiple buffers).
FN_EXTERN uint32_t mysqlCrc32c(uint32_t crc, const unsigned char *buf, size_t len);

#endif
