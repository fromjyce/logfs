#ifndef LOGFS_TXN_H
#define LOGFS_TXN_H

#include <stdint.h>
#include "blockdev.h"
#include "disk_format.h"
#include "journal.h"

/*
 * Transaction manager: the handle protocol and the running/committing/
 * checkpointing pipeline described in docs/DESIGN.md §3 and §6. Built on
 * top of journal.h, which knows nothing about handles, modes, or
 * concurrency — this is where those live.
 *
 * Concurrency contract callers must uphold (see docs/DESIGN.md §7): this
 * module's own mutex protects its internal bookkeeping only — the running
 * generation's dirty-block table, handle counts, and checkpoint list — not
 * the *contents* of a buffer returned by logfs_txn_get_write_access(). Two
 * handles that call get_write_access() on the same block number
 * concurrently get a pointer to the *same* staging buffer with no
 * synchronization between their writes to it. Real filesystems avoid this
 * via a lock that already serializes operations on the same inode/directory
 * before the journal layer is ever reached (VFS's i_rwsem, in Linux's
 * case); this project's FUSE frontend (fs_ops.c) takes a single
 * filesystem-wide lock for the same purpose, documented there as a
 * deliberate scope cut — fine-grained per-inode locking isn't this
 * project's hard part.
 */

typedef struct logfs_txn_mgr logfs_txn_mgr_t; /* opaque, defined in txn.c */

typedef struct {
    logfs_txn_mgr_t *mgr;
    uint64_t txn_id; /* sanity tag: the sequence this handle's generation will commit as */
} logfs_handle_t;

int logfs_txnmgr_create(logfs_txn_mgr_t **out, logfs_journal_t *journal, logfs_bdev_t *dev,
                         logfs_data_mode_t mode, unsigned commit_interval_ms);

/* Assumes no handles are in flight (mirrors requiring a clean unmount — it
 * does not forcibly cancel outstanding handles). Forces a final commit of
 * any pending work and fully checkpoints the journal before returning, so
 * a clean unmount always leaves an empty log. */
void logfs_txnmgr_destroy(logfs_txn_mgr_t *mgr);

int logfs_txn_start(logfs_txn_mgr_t *mgr, logfs_handle_t *out);

/* Returns a pointer to an in-memory staging buffer for blockno, seeded
 * with its current on-disk contents on first access this generation
 * (read-modify-write, matching jbd2's get_write_access) and shared by any
 * other get_write_access() call for the same block in the same
 * generation (write absorption — docs/DESIGN.md §5's journal.c doc
 * references this same idea for descriptor tags; here it's why a block
 * dirtied twice before commit is only ever logged once). The buffer is
 * valid until logfs_txn_stop() is called on this handle. */
int logfs_txn_get_write_access(logfs_handle_t *h, uint64_t blockno, void **buf_out);

/* Validates that blockno was already staged via get_write_access() on
 * this handle and will be included in this generation's commit set.
 * Metadata is always journaled, regardless of data mode. */
int logfs_txn_dirty_metadata(logfs_handle_t *h, uint64_t blockno);

/* Mode-dependent data path (docs/DESIGN.md §9) — this is the single
 * function where the three data modes actually diverge:
 *   JOURNAL:   identical mechanism to metadata (stages + journals it).
 *   ORDERED:   written to its home location synchronously now, and
 *              tracked so this generation's commit forces it durable
 *              before the commit block is written.
 *   WRITEBACK: handed to an independent background writeback queue and
 *              returns immediately — durability is decoupled from any
 *              commit's timing entirely, which is the source of the
 *              mode's weaker guarantee.
 */
int logfs_txn_write_data(logfs_handle_t *h, uint64_t blockno, const void *buf);

int logfs_txn_stop(logfs_handle_t *h);

/* Reads the current, freshest content of blockno: from the running
 * generation's staging table if it's dirty there, else from the
 * committed-but-not-yet-checkpointed list if a recent commit logged it,
 * else straight from disk. This exists because logfs_txn_get_write_access
 * transfers a dirty block's ownership onto the checkpoint list at commit
 * time rather than leaving a single shared buffer object reachable from
 * everywhere (the way a real kernel's buffer_head would be) — without this
 * three-tier lookup, a read of a block in exactly that
 * committed-but-not-checkpointed window would see stale on-disk data. Any
 * fs.c/inode.c code path that reads filesystem metadata (not raw
 * mkfs/recovery I/O, which talk to the block device directly) should go
 * through this, not logfs_bdev_read, for that reason. */
int logfs_txn_read_block(logfs_txn_mgr_t *mgr, uint64_t blockno, void *buf);

/* Forces an immediate commit of the transaction that is running at the
 * moment of the call, and blocks until it is durable in the journal.
 * This is fsync()'s primitive. */
int logfs_txn_commit_sync(logfs_txn_mgr_t *mgr);

#endif /* LOGFS_TXN_H */
