#ifndef LOGFS_INODE_H
#define LOGFS_INODE_H

#include <stdint.h>
#include "disk_format.h"
#include "fs.h"

/* Inode table, allocator, and flat-directory operations built on top of
 * fs.h/txn.h. Every function that mutates filesystem state takes a
 * logfs_handle_t and stages its changes into that transaction rather than
 * writing straight to disk — allocation bitmaps included, since a bitmap
 * update that isn't atomic with the inode/dirent write it's paired with is
 * exactly the kind of crash-consistency bug this project exists to avoid
 * (docs/DESIGN.md §3). Read-only functions take just logfs_fs_t and go
 * through logfs_txn_read_block so they see any not-yet-checkpointed write
 * a concurrent transaction has already committed.
 *
 * No indirect blocks: files are capped at LOGFS_NDIR_BLOCKS *
 * LOGFS_BLOCK_SIZE (48KiB). See disk_format.h's comment on struct
 * logfs_inode for why that's a deliberate scope cut, not an oversight. */

int logfs_inode_read(logfs_fs_t *fs, uint32_t ino, struct logfs_inode *out);
int logfs_inode_write(logfs_handle_t *h, logfs_fs_t *fs, uint32_t ino, const struct logfs_inode *in);

int logfs_alloc_inode(logfs_handle_t *h, logfs_fs_t *fs, uint32_t *ino_out);
int logfs_free_inode(logfs_handle_t *h, logfs_fs_t *fs, uint32_t ino);
int logfs_alloc_block(logfs_handle_t *h, logfs_fs_t *fs, uint64_t *blockno_out);
int logfs_free_block(logfs_handle_t *h, logfs_fs_t *fs, uint64_t blockno);

int logfs_dir_lookup(logfs_fs_t *fs, uint32_t dir_ino, const char *name,
                      uint32_t *ino_out, uint8_t *type_out);
int logfs_dir_add(logfs_handle_t *h, logfs_fs_t *fs, uint32_t dir_ino,
                   const char *name, uint32_t ino, uint8_t type);
int logfs_dir_remove(logfs_handle_t *h, logfs_fs_t *fs, uint32_t dir_ino, const char *name);
/* Tri-state, not the usual 0/-errno: returns 1 if dir_ino has no entries
 * besides "." and "..", 0 if it has more, negative errno on lookup failure. */
int logfs_dir_is_empty(logfs_fs_t *fs, uint32_t dir_ino);

/* Resolves an absolute, '/'-separated path from the root inode. */
int logfs_path_lookup(logfs_fs_t *fs, const char *path, uint32_t *ino_out);

#endif /* LOGFS_INODE_H */
