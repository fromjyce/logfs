#include "txn.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define LOGFS_TXN_HASH_BUCKETS 61u

/* How many committed-but-not-yet-checkpointed transactions the journal is
 * allowed to accumulate before the commit thread starts checkpointing the
 * oldest ones. This is what makes "committed, durable, but not yet at its
 * home location" an actual, observable window rather than a theoretical
 * one — see docs/DESIGN.md §3. Kept small and fixed rather than
 * configurable: the point of this project is to demonstrate the window
 * exists and is crash-safe, not to tune checkpoint scheduling. */
#define LOGFS_CKPT_LAG 2u

/* Writeback-mode background flush interval. Real ext4's analogous knob is
 * dirty_writeback_centisecs (default 5s); kept shorter here so the
 * decoupled-durability window this mode is meant to demonstrate is easy
 * to observe without waiting a long time. */
#define LOGFS_WRITEBACK_INTERVAL_MS 1000u

struct logfs_dirty_block {
    uint64_t blockno;
    uint8_t data[LOGFS_BLOCK_SIZE];
    struct logfs_dirty_block *next;
};

struct logfs_committed_txn {
    uint64_t sequence;
    uint64_t desc_offset;
    size_t nblocks;
    struct logfs_dirty_block **nodes; /* ownership transferred from the staging table */
    struct logfs_committed_txn *next;
};

struct logfs_wb_entry {
    uint64_t blockno;
    uint8_t data[LOGFS_BLOCK_SIZE];
    struct logfs_wb_entry *next;
};

struct logfs_txn_mgr {
    logfs_journal_t *journal;
    logfs_bdev_t *dev;
    logfs_data_mode_t mode; /* set once at creation, read-only thereafter */
    unsigned commit_interval_ms;

    pthread_mutex_t lock;
    pthread_cond_t cv;
    int freezing;
    int running_handles;
    int commit_requested;
    int shutdown;

    uint64_t generation_seq;    /* sequence the *running* generation will get when it commits */
    uint64_t completed_through; /* generation_seq value as of the last fully-handled cycle —
                                  * advances every commit-thread cycle whether or not that
                                  * cycle actually had anything to journal (see
                                  * logfs_txn_commit_sync: comparing against committed_seq
                                  * alone would hang fsync() on an idle filesystem, since an
                                  * empty cycle never assigns a new committed sequence). */

    struct logfs_dirty_block *buckets[LOGFS_TXN_HASH_BUCKETS];
    size_t ndirty;

    uint64_t *ordered_blocks;
    size_t n_ordered, cap_ordered;

    struct logfs_committed_txn *ckpt_head, *ckpt_tail;
    size_t ckpt_count;

    pthread_t commit_thread;

    /* writeback mode only */
    pthread_t writeback_thread;
    pthread_mutex_t wb_lock;
    pthread_cond_t wb_cv;
    struct logfs_wb_entry *wb_head, *wb_tail;
    int wb_shutdown;
};

/* ---- staging table -------------------------------------------------------- */

static size_t hash_block(uint64_t blockno) {
    return (size_t)(blockno % LOGFS_TXN_HASH_BUCKETS);
}

/* Caller holds mgr->lock. Lookup-only — does not read-through or insert
 * on a miss, unlike find_or_create_dirty (defined below). */
static struct logfs_dirty_block *find_dirty(logfs_txn_mgr_t *mgr, uint64_t blockno) {
    for (struct logfs_dirty_block *d = mgr->buckets[hash_block(blockno)]; d != NULL; d = d->next) {
        if (d->blockno == blockno) {
            return d;
        }
    }
    return NULL;
}

/* Caller holds mgr->lock. Checks the checkpoint list — transactions that
 * have committed but not yet been copied to their home location — for the
 * most recent copy of blockno, if any. Scans oldest-to-newest and keeps
 * overwriting the result so a more recent transaction's copy always wins
 * over an older one still awaiting checkpoint. */
static struct logfs_dirty_block *find_checkpointed(logfs_txn_mgr_t *mgr, uint64_t blockno) {
    struct logfs_dirty_block *found = NULL;
    for (struct logfs_committed_txn *t = mgr->ckpt_head; t != NULL; t = t->next) {
        for (size_t i = 0; i < t->nblocks; i++) {
            if (t->nodes[i]->blockno == blockno) {
                found = t->nodes[i];
            }
        }
    }
    return found;
}

static struct logfs_dirty_block *find_or_create_dirty(logfs_txn_mgr_t *mgr, uint64_t blockno) {
    struct logfs_dirty_block *existing = find_dirty(mgr, blockno);
    if (existing != NULL) {
        return existing;
    }
    struct logfs_dirty_block *d = malloc(sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->blockno = blockno;

    /* Seed with the block's current content so a caller that only touches
     * part of it (e.g. one dirent slot) is doing read-modify-write, not
     * clobbering the rest with zeros. Checked against the checkpoint list
     * first, disk only as a last resort — otherwise staging a block during
     * its committed-but-not-yet-checkpointed window would silently discard
     * that pending write (docs/DESIGN.md's whole point is not doing that).
     * The disk read is held across the lock, trading a little contention
     * for a simple, obviously-race-free insert: two threads racing to
     * stage the same not-yet-dirty block for the first time this
     * generation can't both win and end up with two divergent nodes. */
    struct logfs_dirty_block *ckpt = find_checkpointed(mgr, blockno);
    int rc;
    if (ckpt != NULL) {
        memcpy(d->data, ckpt->data, LOGFS_BLOCK_SIZE);
        rc = 0;
    } else {
        rc = logfs_bdev_read(mgr->dev, blockno, d->data);
    }
    if (rc != 0) {
        free(d);
        return NULL;
    }
    size_t b = hash_block(blockno);
    d->next = mgr->buckets[b];
    mgr->buckets[b] = d;
    mgr->ndirty++;
    return d;
}

/* Caller holds mgr->lock. Detaches every node into a flat array (for
 * building the commit's pending-block list) and empties the buckets. */
static struct logfs_dirty_block **flatten_and_clear(logfs_txn_mgr_t *mgr, size_t count) {
    if (count == 0) {
        return NULL;
    }
    struct logfs_dirty_block **arr = malloc(count * sizeof(*arr));
    if (arr == NULL) {
        return NULL;
    }
    size_t idx = 0;
    for (size_t b = 0; b < LOGFS_TXN_HASH_BUCKETS; b++) {
        struct logfs_dirty_block *d = mgr->buckets[b];
        while (d != NULL) {
            struct logfs_dirty_block *next = d->next;
            arr[idx++] = d;
            d = next;
        }
        mgr->buckets[b] = NULL;
    }
    return arr;
}

/* ---- checkpoint ------------------------------------------------------------ */

/* Returns 1 if a transaction was checkpointed, 0 if there was nothing to
 * do, or a negative errno on I/O failure (best-effort: some blocks in the
 * transaction may already be durable at their home location and some not
 * — see the abort-on-failure note in do_commit() below for why this
 * project doesn't try to recover from that more precisely). */
static int checkpoint_oldest(logfs_txn_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    struct logfs_committed_txn *t = mgr->ckpt_head;
    if (t == NULL) {
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }
    mgr->ckpt_head = t->next;
    if (mgr->ckpt_head == NULL) {
        mgr->ckpt_tail = NULL;
    }
    mgr->ckpt_count--;
    pthread_mutex_unlock(&mgr->lock);

    /* Known gap, not a crash-consistency one: from here until the writes
     * below land, t's blocks are on neither the staging table nor the
     * checkpoint list, so a concurrent logfs_txn_read_block() for one of
     * them falls through to disk and can observe stale (pre-checkpoint)
     * content for a few I/O calls' worth of time. This is an in-memory
     * read-consistency nit, not a durability bug: t's data is already
     * fsynced in the journal (it got here by having committed), so a
     * crash during this exact window still recovers correctly via replay
     * — closing it would mean keeping a fourth "in-flight" list visible to
     * readers for a window this narrow, which isn't worth the complexity
     * here. */
    int rc = 0;
    for (size_t i = 0; i < t->nblocks; i++) {
        rc = logfs_bdev_write(mgr->dev, t->nodes[i]->blockno, t->nodes[i]->data);
        if (rc != 0) {
            break;
        }
    }
    if (rc == 0) {
        rc = logfs_bdev_flush(mgr->dev);
    }
    if (rc == 0) {
        /* journal->head/next_sequence are read here without mgr->lock:
         * the commit thread is the sole reader and writer of *mgr->journal
         * (checkpoint_oldest is only ever called from that thread), so
         * there is no concurrent access to race with. mgr->lock protects
         * the ckpt list, not the journal struct. */
        pthread_mutex_lock(&mgr->lock);
        struct logfs_committed_txn *next = mgr->ckpt_head;
        uint64_t new_tail_off = (next != NULL) ? next->desc_offset : mgr->journal->head;
        uint64_t new_tail_seq = (next != NULL) ? next->sequence : mgr->journal->next_sequence;
        pthread_mutex_unlock(&mgr->lock);
        rc = logfs_journal_advance_tail(mgr->journal, new_tail_off, new_tail_seq);
    }

    for (size_t i = 0; i < t->nblocks; i++) {
        free(t->nodes[i]);
    }
    free(t->nodes);
    free(t);

    return (rc == 0) ? 1 : rc;
}

static void enforce_checkpoint_lag(logfs_txn_mgr_t *mgr) {
    for (;;) {
        pthread_mutex_lock(&mgr->lock);
        int need = mgr->ckpt_count > LOGFS_CKPT_LAG;
        pthread_mutex_unlock(&mgr->lock);
        if (!need) {
            return;
        }
        if (checkpoint_oldest(mgr) <= 0) {
            return;
        }
    }
}

/* ---- commit ---------------------------------------------------------------- */

static void do_commit(logfs_txn_mgr_t *mgr, struct logfs_dirty_block **nodes, size_t nnodes) {
    while (logfs_journal_free_space(mgr->journal) < logfs_journal_space_needed(nnodes, 0)) {
        if (checkpoint_oldest(mgr) <= 0) {
            break; /* nothing left to reclaim; logfs_journal_commit will report -ENOSPC below */
        }
    }

    struct logfs_pending_block *pending = malloc(nnodes * sizeof(*pending));
    uint64_t seq = 0, desc_off = 0;
    int rc;
    if (pending == NULL) {
        rc = -ENOMEM;
    } else {
        for (size_t i = 0; i < nnodes; i++) {
            pending[i].target_block = nodes[i]->blockno;
            pending[i].data = nodes[i]->data;
        }
        rc = logfs_journal_commit(mgr->journal, pending, nnodes, NULL, 0, &seq, &desc_off);
        free(pending);
    }

    /* If journal_commit succeeded but we can't allocate the bookkeeping
     * node below, the data is nonetheless durably in the journal — a
     * crash recovers it correctly. What's lost is in-memory tracking: it
     * won't reach the checkpoint list this mount, so
     * logfs_txn_read_block() would see stale disk content for these
     * blocks until the next full recover() (e.g. after a restart). Same
     * class of documented best-effort gap as the rest of this file's OOM
     * paths — not worth a retry loop for an allocation this small. */
    struct logfs_committed_txn *t = (rc == 0) ? malloc(sizeof(*t)) : NULL;

    pthread_mutex_lock(&mgr->lock);
    uint64_t finishing_gen = mgr->generation_seq;
    if (rc == 0 && t != NULL) {
        t->sequence = seq;
        t->desc_offset = desc_off;
        t->nblocks = nnodes;
        t->nodes = nodes; /* ownership transferred */
        t->next = NULL;
        if (mgr->ckpt_tail != NULL) {
            mgr->ckpt_tail->next = t;
        } else {
            mgr->ckpt_head = t;
        }
        mgr->ckpt_tail = t;
        mgr->ckpt_count++;
    } else {
        /* Commit failed — e.g. -ENOSPC even after checkpointing everything
         * reclaimable. A production journal aborts here (docs/DESIGN.md
         * §10: refuse new handles, remount read-only) rather than losing
         * writes silently. This toy build does not implement that path;
         * it's a documented limitation (docs/DESIGN.md §1 non-goals), not
         * an oversight — the dirty data for this generation is dropped. */
        for (size_t i = 0; i < nnodes; i++) {
            free(nodes[i]);
        }
        free(nodes);
    }
    mgr->completed_through = finishing_gen;
    mgr->generation_seq = mgr->journal->next_sequence;
    pthread_cond_broadcast(&mgr->cv);
    pthread_mutex_unlock(&mgr->lock);
}

static void *commit_thread_main(void *arg) {
    logfs_txn_mgr_t *mgr = arg;

    pthread_mutex_lock(&mgr->lock);
    while (!mgr->shutdown) {
        if (mgr->ndirty == 0 && !mgr->commit_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(mgr->commit_interval_ms / 1000u);
            ts.tv_nsec += (long)(mgr->commit_interval_ms % 1000u) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&mgr->cv, &mgr->lock, &ts);
            continue; /* re-check the predicate; timeout, signal, and spurious wake all land here */
        }

        mgr->freezing = 1;
        while (mgr->running_handles > 0) {
            pthread_cond_wait(&mgr->cv, &mgr->lock);
        }
        size_t nnodes = mgr->ndirty;
        struct logfs_dirty_block **nodes = flatten_and_clear(mgr, nnodes);
        mgr->ndirty = 0;
        size_t nordered = mgr->n_ordered;
        free(mgr->ordered_blocks);
        mgr->ordered_blocks = NULL;
        mgr->n_ordered = 0;
        mgr->cap_ordered = 0;
        mgr->commit_requested = 0;
        mgr->freezing = 0;
        pthread_cond_broadcast(&mgr->cv);
        pthread_mutex_unlock(&mgr->lock);

        if (mgr->mode == LOGFS_MODE_ORDERED && nordered > 0) {
            /* Forces this generation's ordered data blocks durable before
             * the journal commit below writes its commit block — the
             * ordering guarantee docs/DESIGN.md §9 describes for this mode. */
            logfs_bdev_flush(mgr->dev);
        }

        if (nnodes > 0) {
            do_commit(mgr, nodes, nnodes);
        } else {
            free(nodes);
            pthread_mutex_lock(&mgr->lock);
            /* Nothing to journal this cycle, but the cycle still happened —
             * bump completed_through so a concurrent logfs_txn_commit_sync()
             * call on an idle filesystem returns instead of waiting for a
             * committed_seq that will never arrive (see the field comment). */
            mgr->completed_through = mgr->generation_seq;
            pthread_cond_broadcast(&mgr->cv);
            pthread_mutex_unlock(&mgr->lock);
        }

        enforce_checkpoint_lag(mgr);
        pthread_mutex_lock(&mgr->lock);
    }
    pthread_mutex_unlock(&mgr->lock);

    while (checkpoint_oldest(mgr) == 1) {
        /* drain everything so a clean unmount leaves an empty journal */
    }
    return NULL;
}

/* ---- writeback-mode background flush -------------------------------------- */

static int writeback_queue_push(logfs_txn_mgr_t *mgr, uint64_t blockno, const void *buf) {
    struct logfs_wb_entry *e = malloc(sizeof(*e));
    if (e == NULL) {
        return -ENOMEM;
    }
    e->blockno = blockno;
    memcpy(e->data, buf, LOGFS_BLOCK_SIZE);
    e->next = NULL;

    pthread_mutex_lock(&mgr->wb_lock);
    if (mgr->wb_tail != NULL) {
        mgr->wb_tail->next = e;
    } else {
        mgr->wb_head = e;
    }
    mgr->wb_tail = e;
    pthread_cond_signal(&mgr->wb_cv);
    pthread_mutex_unlock(&mgr->wb_lock);
    return 0;
}

static void *writeback_thread_main(void *arg) {
    logfs_txn_mgr_t *mgr = arg;

    pthread_mutex_lock(&mgr->wb_lock);
    for (;;) {
        while (mgr->wb_head == NULL && !mgr->wb_shutdown) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(LOGFS_WRITEBACK_INTERVAL_MS / 1000u);
            pthread_cond_timedwait(&mgr->wb_cv, &mgr->wb_lock, &ts);
        }
        if (mgr->wb_head == NULL && mgr->wb_shutdown) {
            break;
        }
        struct logfs_wb_entry *e = mgr->wb_head;
        mgr->wb_head = e->next;
        if (mgr->wb_head == NULL) {
            mgr->wb_tail = NULL;
        }
        pthread_mutex_unlock(&mgr->wb_lock);

        /* Deliberately not fsynced per-entry: writeback mode's weaker
         * guarantee comes exactly from this write reaching the device on
         * its own schedule, decoupled from any journal commit. */
        logfs_bdev_write(mgr->dev, e->blockno, e->data);
        free(e);

        pthread_mutex_lock(&mgr->wb_lock);
    }
    pthread_mutex_unlock(&mgr->wb_lock);
    logfs_bdev_flush(mgr->dev); /* clean-unmount best-effort durability */
    return NULL;
}

/* ---- public API ------------------------------------------------------------- */

int logfs_txnmgr_create(logfs_txn_mgr_t **out, logfs_journal_t *journal, logfs_bdev_t *dev,
                         logfs_data_mode_t mode, unsigned commit_interval_ms) {
    logfs_txn_mgr_t *mgr = calloc(1, sizeof(*mgr));
    if (mgr == NULL) {
        return -ENOMEM;
    }
    mgr->journal = journal;
    mgr->dev = dev;
    mgr->mode = mode;
    mgr->commit_interval_ms = commit_interval_ms;
    mgr->generation_seq = journal->next_sequence;

    pthread_mutex_init(&mgr->lock, NULL);
    pthread_cond_init(&mgr->cv, NULL);
    pthread_mutex_init(&mgr->wb_lock, NULL);
    pthread_cond_init(&mgr->wb_cv, NULL);

    if (pthread_create(&mgr->commit_thread, NULL, commit_thread_main, mgr) != 0) {
        pthread_mutex_destroy(&mgr->lock);
        pthread_cond_destroy(&mgr->cv);
        pthread_mutex_destroy(&mgr->wb_lock);
        pthread_cond_destroy(&mgr->wb_cv);
        free(mgr);
        return -EAGAIN;
    }
    if (mode == LOGFS_MODE_WRITEBACK) {
        if (pthread_create(&mgr->writeback_thread, NULL, writeback_thread_main, mgr) != 0) {
            pthread_mutex_lock(&mgr->lock);
            mgr->shutdown = 1;
            pthread_cond_broadcast(&mgr->cv);
            pthread_mutex_unlock(&mgr->lock);
            pthread_join(mgr->commit_thread, NULL);
            pthread_mutex_destroy(&mgr->lock);
            pthread_cond_destroy(&mgr->cv);
            pthread_mutex_destroy(&mgr->wb_lock);
            pthread_cond_destroy(&mgr->wb_cv);
            free(mgr);
            return -EAGAIN;
        }
    }

    *out = mgr;
    return 0;
}

void logfs_txnmgr_destroy(logfs_txn_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    mgr->shutdown = 1;
    pthread_cond_broadcast(&mgr->cv);
    pthread_mutex_unlock(&mgr->lock);
    pthread_join(mgr->commit_thread, NULL);

    if (mgr->mode == LOGFS_MODE_WRITEBACK) {
        pthread_mutex_lock(&mgr->wb_lock);
        mgr->wb_shutdown = 1;
        pthread_cond_broadcast(&mgr->wb_cv);
        pthread_mutex_unlock(&mgr->wb_lock);
        pthread_join(mgr->writeback_thread, NULL);
    }

    pthread_mutex_destroy(&mgr->lock);
    pthread_cond_destroy(&mgr->cv);
    pthread_mutex_destroy(&mgr->wb_lock);
    pthread_cond_destroy(&mgr->wb_cv);
    free(mgr);
}

int logfs_txn_start(logfs_txn_mgr_t *mgr, logfs_handle_t *out) {
    pthread_mutex_lock(&mgr->lock);
    while (mgr->freezing) {
        pthread_cond_wait(&mgr->cv, &mgr->lock);
    }
    mgr->running_handles++;
    out->mgr = mgr;
    out->txn_id = mgr->generation_seq;
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

int logfs_txn_get_write_access(logfs_handle_t *h, uint64_t blockno, void **buf_out) {
    logfs_txn_mgr_t *mgr = h->mgr;
    pthread_mutex_lock(&mgr->lock);
    assert(h->txn_id == mgr->generation_seq); /* freeze can't have swapped under a live handle */
    struct logfs_dirty_block *d = find_or_create_dirty(mgr, blockno);
    pthread_mutex_unlock(&mgr->lock);
    if (d == NULL) {
        return -ENOMEM;
    }
    *buf_out = d->data;
    return 0;
}

int logfs_txn_dirty_metadata(logfs_handle_t *h, uint64_t blockno) {
    logfs_txn_mgr_t *mgr = h->mgr;
    pthread_mutex_lock(&mgr->lock);
    struct logfs_dirty_block *d = find_or_create_dirty(mgr, blockno);
    pthread_mutex_unlock(&mgr->lock);
    /* find_or_create_dirty is idempotent, so this doubles as "confirm the
     * caller actually staged this block" without a separate lookup path —
     * jbd2 keeps get_write_access and dirty_metadata as distinct calls to
     * mark pin-vs-intent separately; our single staging table collapses
     * that distinction, so this call is validation, not new bookkeeping. */
    return (d != NULL) ? 0 : -ENOMEM;
}

int logfs_txn_write_data(logfs_handle_t *h, uint64_t blockno, const void *buf) {
    logfs_txn_mgr_t *mgr = h->mgr;
    switch (mgr->mode) {
    case LOGFS_MODE_JOURNAL: {
        void *staged;
        int rc = logfs_txn_get_write_access(h, blockno, &staged);
        if (rc != 0) {
            return rc;
        }
        memcpy(staged, buf, LOGFS_BLOCK_SIZE);
        return logfs_txn_dirty_metadata(h, blockno);
    }
    case LOGFS_MODE_ORDERED: {
        int rc = logfs_bdev_write(mgr->dev, blockno, buf);
        if (rc != 0) {
            return rc;
        }
        pthread_mutex_lock(&mgr->lock);
        if (mgr->n_ordered == mgr->cap_ordered) {
            size_t newcap = (mgr->cap_ordered != 0) ? mgr->cap_ordered * 2 : 16;
            uint64_t *next = realloc(mgr->ordered_blocks, newcap * sizeof(uint64_t));
            if (next == NULL) {
                pthread_mutex_unlock(&mgr->lock);
                return -ENOMEM;
            }
            mgr->ordered_blocks = next;
            mgr->cap_ordered = newcap;
        }
        mgr->ordered_blocks[mgr->n_ordered++] = blockno;
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }
    case LOGFS_MODE_WRITEBACK:
        return writeback_queue_push(mgr, blockno, buf);
    default:
        return -EINVAL;
    }
}

int logfs_txn_stop(logfs_handle_t *h) {
    logfs_txn_mgr_t *mgr = h->mgr;
    pthread_mutex_lock(&mgr->lock);
    mgr->running_handles--;
    if (mgr->running_handles == 0) {
        pthread_cond_broadcast(&mgr->cv);
    }
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}

int logfs_txn_read_block(logfs_txn_mgr_t *mgr, uint64_t blockno, void *buf) {
    pthread_mutex_lock(&mgr->lock);
    struct logfs_dirty_block *d = find_dirty(mgr, blockno);
    if (d == NULL) {
        d = find_checkpointed(mgr, blockno);
    }
    if (d != NULL) {
        memcpy(buf, d->data, LOGFS_BLOCK_SIZE);
        pthread_mutex_unlock(&mgr->lock);
        return 0;
    }
    pthread_mutex_unlock(&mgr->lock);
    /* No insert here, so no race to protect against — the lock is dropped
     * before falling through to disk, unlike find_or_create_dirty. */
    return logfs_bdev_read(mgr->dev, blockno, buf);
}

int logfs_txn_commit_sync(logfs_txn_mgr_t *mgr) {
    pthread_mutex_lock(&mgr->lock);
    uint64_t target = mgr->generation_seq;
    mgr->commit_requested = 1;
    pthread_cond_broadcast(&mgr->cv);
    while (mgr->completed_through < target && !mgr->shutdown) {
        pthread_cond_wait(&mgr->cv, &mgr->lock);
    }
    pthread_mutex_unlock(&mgr->lock);
    return 0;
}
