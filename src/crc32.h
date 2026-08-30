#ifndef LOGFS_CRC32_H
#define LOGFS_CRC32_H

#include <stddef.h>
#include <stdint.h>

/* Standard CRC-32 (IEEE 802.3, poly 0xEDB88320), self-contained so the
 * journal's commit checksum has no external dependency. init/update/final
 * are split so callers can checksum several non-contiguous buffers (a
 * descriptor block plus N data blocks) as one running value without
 * copying them into a single buffer first. */

uint32_t logfs_crc32_init(void);
uint32_t logfs_crc32_update(uint32_t crc, const void *buf, size_t len);
uint32_t logfs_crc32_final(uint32_t crc);

#endif /* LOGFS_CRC32_H */
