#ifndef LOGFS_FS_H
#define LOGFS_FS_H

#include "blockdev.h"
#include "disk_format.h"
#include "journal.h"
#include "txn.h"

/* Top-level mounted-filesystem handle: the thing fs_ops.c's FUSE callbacks
 * actually operate on. Ties together the block device, the journal, the
 * transaction manager, and the cached superblock layout. */
typedef struct {
    logfs_bdev_t dev;
    logfs_journal_t journal;
    logfs_txn_mgr_t *txnmgr;
    struct logfs_super sb; /* in-memory copy of block LOGFS_SUPER_BLOCKNO */
} logfs_fs_t;

/* Lays out a fresh filesystem image: journal region, inode/block bitmaps
 * (single block each — see the size-cap note next to LOGFS_FS_MAX_BLOCKS),
 * inode table, and an empty root directory. Writes the superblock last and
 * fsynced, so a crash mid-format never leaves a superblock claiming a
 * filesystem that isn't actually fully laid out yet — mkfs gets the same
 * "durable iff the last write landed" discipline the rest of this project
 * is about. */
int logfs_fs_format(const char *path, uint64_t total_blocks, uint64_t journal_len,
                     logfs_data_mode_t default_mode);

/* A single block-bitmap block covers at most this many bits; both the
 * inode bitmap and the data-block bitmap are exactly one block in this
 * toy design (see docs/DESIGN.md §1 non-goals — a real allocator would use
 * per-group bitmaps, ext2-style, to scale past this). */
#define LOGFS_FS_MAX_BITMAP_UNITS (LOGFS_BLOCK_SIZE * 8u)

/* Opens the backing file, reads and validates the superblock, replays the
 * journal (logfs_journal_recover — a no-op if the journal was already
 * empty at last unmount), and starts the transaction manager. mode
 * overrides the on-disk default if not equal to (logfs_data_mode_t)-1,
 * mirroring ext4's mount-time -o data= override. */
int logfs_fs_mount(logfs_fs_t *fs, const char *path, int mode_override);

/* Stops the transaction manager (which fully checkpoints on the way down
 * — see logfs_txnmgr_destroy) and closes the backing file. */
void logfs_fs_unmount(logfs_fs_t *fs);

#endif /* LOGFS_FS_H */
