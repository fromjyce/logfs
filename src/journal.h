#ifndef LOGFS_JOURNAL_H
#define LOGFS_JOURNAL_H

#include <stddef.h>
#include <stdint.h>
#include "blockdev.h"
#include "disk_format.h"

/*
 * Journal I/O layer: descriptor/commit/revoke block sequencing over a
 * circular log region, plus the two-pass scan+replay recovery algorithm.
 * See docs/DESIGN.md §5, §6 step "Commit", and §8 for the design this
 * implements. This module knows nothing about filesystem semantics
 * (inodes, directories) — it only ever moves opaque LOGFS_BLOCK_SIZE
 * blocks between the log and wherever the caller's install callback
 * decides "home location" means.
 *
 * head/tail are monotonically increasing logical offsets into the journal
 * region, not physical block numbers — logfs_journal_commit() and recovery
 * both translate logical offset -> physical block as `first + (off %
 * maxlen)`. Keeping them unbounded avoids ever having to reason about a
 * commit's own blocks wrapping past its own tail mid-write.
 */

typedef struct {
    logfs_bdev_t *dev;
    uint64_t first;          /* physical block of the journal region's block 0 */
    uint64_t maxlen;          /* journal region length, in blocks */
    uint64_t head;             /* logical offset of the next free slot */
    uint64_t tail;              /* logical offset of the oldest not-yet-checkpointed slot */
    uint64_t next_sequence;      /* sequence number to assign to the next commit */
} logfs_journal_t;

struct logfs_pending_block {
    uint64_t target_block;   /* home-location block number */
    const void *data;         /* exactly LOGFS_BLOCK_SIZE bytes, caller-owned */
};

/* Writes a fresh, empty journal superblock. Called once at mkfs time. */
int logfs_journal_format(logfs_bdev_t *dev, uint64_t first, uint64_t maxlen);

/* Loads journal state from the on-disk journal superblock. Caller MUST
 * call logfs_journal_recover() immediately afterward, even if the journal
 * superblock claims the log is empty — recover() is a no-op in that case,
 * and making it unconditional removes a mount-ordering footgun. */
int logfs_journal_open(logfs_journal_t *j, logfs_bdev_t *dev, uint64_t first, uint64_t maxlen);

/* Free space in the circular region, in blocks. */
uint64_t logfs_journal_free_space(const logfs_journal_t *j);

/* Blocks a transaction with this many data blocks and revoked-block
 * entries will occupy: 1 descriptor + ndata + (nrevoke > 0 ? 1 : 0) +
 * 1 commit. Returns 0 if ndata/nrevoke exceed what fits in a single
 * descriptor/revoke block — this journal only ever writes one of each
 * per transaction (see docs/DESIGN.md §5's checksum-design note; a
 * multi-descriptor-block transaction is a real jbd2 feature this project
 * deliberately doesn't need, since txn.c caps transaction size to fit). */
uint64_t logfs_journal_space_needed(size_t ndata, size_t nrevoke);

/* Appends one transaction: descriptor [+ revoke] + data blocks + commit,
 * with a PREFLUSH before the FUA commit write (docs/DESIGN.md §6 step
 * "Commit", steps 2-4). Caller must have already confirmed
 * logfs_journal_free_space() >= logfs_journal_space_needed(ndata, nrevoke).
 * On success, advances j->head and j->next_sequence, and reports the
 * committed sequence number and this transaction's descriptor offset —
 * the caller needs both to track it on an in-memory checkpoint list. */
int logfs_journal_commit(logfs_journal_t *j,
                          const struct logfs_pending_block *data, size_t ndata,
                          const uint64_t *revoke, size_t nrevoke,
                          uint64_t *out_sequence, uint64_t *out_desc_offset);

/* Called once the caller has copied a run of oldest transactions back to
 * their home locations and fsynced those writes. Persists the new journal
 * "oldest still-valid" pointer. Ordering contract: this must only be
 * called after the home-location writes it accounts for are durable —
 * calling it earlier would let recovery skip data that isn't actually
 * safe on disk yet. Pass new_tail_offset == j->head and
 * new_tail_sequence == j->next_sequence when nothing remains uncheckpointed. */
int logfs_journal_advance_tail(logfs_journal_t *j, uint64_t new_tail_offset,
                                uint64_t new_tail_sequence);

typedef int (*logfs_journal_install_fn)(void *ctx, uint64_t target_block, const void *data);

/* Two-pass scan+replay from the persisted "oldest valid" pointer forward
 * (docs/DESIGN.md §8). install() is invoked once per surviving
 * (target_block, data) pair, in log order, oldest transaction first; the
 * journal module doesn't know what "home location" means for a given fs,
 * that's the caller's job. On return, the log is fully replayed and
 * therefore empty: j->head == j->tail, and the on-disk journal superblock
 * has been updated to reflect that. */
int logfs_journal_recover(logfs_journal_t *j, logfs_journal_install_fn install, void *ctx);

#endif /* LOGFS_JOURNAL_H */
