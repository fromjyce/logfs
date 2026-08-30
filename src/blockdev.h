#ifndef LOGFS_BLOCKDEV_H
#define LOGFS_BLOCKDEV_H

#include <stdint.h>
#include "disk_format.h"

/*
 * Thin wrapper over a regular backing file standing in for a block device.
 *
 * Simplification, stated plainly (see docs/DESIGN.md §3): real FLUSH/FUA
 * (REQ_PREFLUSH/REQ_FUA) are block-layer primitives that act on a device's
 * volatile write cache and aren't reachable from a plain file opened in
 * userspace. fsync()/fdatasync() is the closest portable stand-in and is
 * what bdev_flush/bdev_write_fua use. This preserves the *ordering*
 * property the journal's correctness depends on (nothing after a flush is
 * acknowledged as durable before everything issued ahead of it), which is
 * the property that matters here — it does not reproduce a real device's
 * cache behavior or its performance characteristics. See docs/DESIGN.md §7's
 * risk note before treating throughput numbers measured on this layer as
 * representative of a real block device.
 */

typedef struct {
    int fd;
    uint64_t nblocks;
} logfs_bdev_t;

/* create=1: create/truncate path to exactly nblocks blocks (mkfs path;
 *   nblocks must be nonzero).
 * create=0: open an existing backing file and derive dev->nblocks from
 *   its actual size via fstat — nblocks is ignored. This is what lets
 *   logfs_fs_mount() open the device before it has read the superblock
 *   that would otherwise be the only source of truth for block count. */
int logfs_bdev_open(logfs_bdev_t *dev, const char *path, uint64_t nblocks, int create);
void logfs_bdev_close(logfs_bdev_t *dev);

/* buf must be LOGFS_BLOCK_SIZE bytes. Returns 0 or -errno. */
int logfs_bdev_read(logfs_bdev_t *dev, uint64_t blockno, void *buf);
int logfs_bdev_write(logfs_bdev_t *dev, uint64_t blockno, const void *buf);

/* Drains any buffered writes issued so far; stands in for REQ_PREFLUSH. */
int logfs_bdev_flush(logfs_bdev_t *dev);

/* Write + drain in one call; stands in for a single REQ_FUA write. */
int logfs_bdev_write_fua(logfs_bdev_t *dev, uint64_t blockno, const void *buf);

#endif /* LOGFS_BLOCKDEV_H */
