#include "journal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "crc32.h"

/* ---- logical <-> physical translation ---------------------------------- */

static uint64_t phys_of(const logfs_journal_t *j, uint64_t logical_off) {
    return j->first + (logical_off % j->maxlen);
}

static int read_logical(logfs_journal_t *j, uint64_t off, void *buf) {
    return logfs_bdev_read(j->dev, phys_of(j, off), buf);
}

static int write_logical(logfs_journal_t *j, uint64_t off, const void *buf) {
    return logfs_bdev_write(j->dev, phys_of(j, off), buf);
}

/* ---- journal superblock (de)serialization ------------------------------ */

static void jsuper_encode(const logfs_journal_t *j, uint64_t start_off,
                           uint64_t start_seq, uint8_t block[LOGFS_BLOCK_SIZE]) {
    struct logfs_journal_super js;
    memset(&js, 0, sizeof(js));
    js.magic = LOGFS_JOURNAL_MAGIC;
    js.block_size = LOGFS_BLOCK_SIZE;
    js.maxlen = j->maxlen;
    js.first = j->first;
    js.sequence = start_seq;
    js.start = start_off;

    memset(block, 0, LOGFS_BLOCK_SIZE);
    memcpy(block, &js, sizeof(js));
}

static int jsuper_write(logfs_journal_t *j, uint64_t start_off, uint64_t start_seq) {
    uint8_t block[LOGFS_BLOCK_SIZE];
    jsuper_encode(j, start_off, start_seq, block);
    return logfs_bdev_write_fua(j->dev, LOGFS_JSUPER_BLOCKNO, block);
}

/* ---- format / open ------------------------------------------------------ */

int logfs_journal_format(logfs_bdev_t *dev, uint64_t first, uint64_t maxlen) {
    logfs_journal_t j = {.dev = dev, .first = first, .maxlen = maxlen};
    return jsuper_write(&j, LOGFS_JOURNAL_EMPTY, 0);
}

int logfs_journal_open(logfs_journal_t *j, logfs_bdev_t *dev, uint64_t first, uint64_t maxlen) {
    uint8_t block[LOGFS_BLOCK_SIZE];
    int rc = logfs_bdev_read(dev, LOGFS_JSUPER_BLOCKNO, block);
    if (rc != 0) {
        return rc;
    }
    struct logfs_journal_super js;
    memcpy(&js, block, sizeof(js));
    if (js.magic != LOGFS_JOURNAL_MAGIC) {
        return -EINVAL;
    }

    j->dev = dev;
    j->first = first;
    j->maxlen = maxlen;

    if (js.start == LOGFS_JOURNAL_EMPTY) {
        j->head = 0;
        j->tail = 0;
        j->next_sequence = 1;
    } else {
        /* head is not persisted; logfs_journal_recover() discovers the
         * true end of valid data by scanning forward from here. Until
         * recover() runs, head/tail/next_sequence are provisional. */
        j->tail = js.start;
        j->head = js.start;
        j->next_sequence = js.sequence;
    }
    return 0;
}

/* ---- space accounting ---------------------------------------------------- */

uint64_t logfs_journal_free_space(const logfs_journal_t *j) {
    return j->maxlen - (j->head - j->tail);
}

uint64_t logfs_journal_space_needed(size_t ndata, size_t nrevoke) {
    if (ndata > LOGFS_MAX_TAGS_PER_DESC || nrevoke > LOGFS_MAX_REVOKES_PER_BLOCK) {
        return 0;
    }
    return 1 + ndata + (nrevoke > 0 ? 1 : 0) + 1;
}

/* ---- checksum: whole-transaction, computed over exactly the bytes each
 * block writes to disk (descriptor block, optional revoke block, then
 * every data block), so recovery can recompute it identically from what
 * it reads back. See docs/DESIGN.md §5's checksum design note. */

static uint32_t checksum_transaction(const uint8_t desc_block[LOGFS_BLOCK_SIZE],
                                      const uint8_t *revoke_block /* nullable */,
                                      const struct logfs_pending_block *data, size_t ndata) {
    uint32_t crc = logfs_crc32_init();
    crc = logfs_crc32_update(crc, desc_block, LOGFS_BLOCK_SIZE);
    if (revoke_block != NULL) {
        crc = logfs_crc32_update(crc, revoke_block, LOGFS_BLOCK_SIZE);
    }
    for (size_t i = 0; i < ndata; i++) {
        crc = logfs_crc32_update(crc, data[i].data, LOGFS_BLOCK_SIZE);
    }
    return logfs_crc32_final(crc);
}

/* ---- commit --------------------------------------------------------------- */

int logfs_journal_commit(logfs_journal_t *j,
                          const struct logfs_pending_block *data, size_t ndata,
                          const uint64_t *revoke, size_t nrevoke,
                          uint64_t *out_sequence, uint64_t *out_desc_offset) {
    uint64_t needed = logfs_journal_space_needed(ndata, nrevoke);
    if (needed == 0) {
        return -EINVAL; /* transaction too large for a single descriptor/revoke block */
    }
    if (needed > logfs_journal_free_space(j)) {
        return -ENOSPC;
    }

    uint64_t sequence = j->next_sequence;
    uint64_t desc_offset = j->head;
    uint64_t cursor = j->head;
    int was_empty = (j->tail == j->head); /* is this commit ending the log's empty state? */

    uint8_t desc_buf[LOGFS_BLOCK_SIZE];
    {
        struct logfs_desc_block desc;
        memset(&desc, 0, sizeof(desc));
        desc.magic = LOGFS_DESC_MAGIC;
        desc.ntags = (uint32_t)ndata;
        desc.sequence = sequence;
        for (size_t i = 0; i < ndata; i++) {
            desc.tags[i].target_block = data[i].target_block;
        }
        memset(desc_buf, 0, LOGFS_BLOCK_SIZE);
        memcpy(desc_buf, &desc, sizeof(desc));
    }
    int rc = write_logical(j, cursor, desc_buf);
    if (rc != 0) {
        return rc;
    }
    cursor++;

    uint8_t revoke_buf[LOGFS_BLOCK_SIZE];
    int have_revoke = (nrevoke > 0);
    if (have_revoke) {
        struct logfs_revoke_block rv;
        memset(&rv, 0, sizeof(rv));
        rv.magic = LOGFS_REVOKE_MAGIC;
        rv.nblocks = (uint32_t)nrevoke;
        rv.sequence = sequence;
        for (size_t i = 0; i < nrevoke; i++) {
            rv.blocks[i] = revoke[i];
        }
        memset(revoke_buf, 0, LOGFS_BLOCK_SIZE);
        memcpy(revoke_buf, &rv, sizeof(rv));
        rc = write_logical(j, cursor, revoke_buf);
        if (rc != 0) {
            return rc;
        }
        cursor++;
    }

    for (size_t i = 0; i < ndata; i++) {
        rc = write_logical(j, cursor, data[i].data);
        if (rc != 0) {
            return rc;
        }
        cursor++;
    }

    /* REQ_PREFLUSH stand-in: everything above must be durable before the
     * commit block below is issued (docs/DESIGN.md §6 steps 3-4). */
    rc = logfs_bdev_flush(j->dev);
    if (rc != 0) {
        return rc;
    }

    uint8_t commit_buf[LOGFS_BLOCK_SIZE];
    {
        struct logfs_commit_block cb;
        memset(&cb, 0, sizeof(cb));
        cb.magic = LOGFS_COMMIT_MAGIC;
        cb.sequence = sequence;
        cb.checksum = checksum_transaction(desc_buf, have_revoke ? revoke_buf : NULL, data, ndata);
        memset(commit_buf, 0, LOGFS_BLOCK_SIZE);
        memcpy(commit_buf, &cb, sizeof(cb));
    }
    /* REQ_FUA stand-in: this write is the atomic commit point. */
    rc = write_logical(j, cursor, commit_buf);
    if (rc != 0) {
        return rc;
    }
    rc = logfs_bdev_flush(j->dev);
    if (rc != 0) {
        return rc;
    }
    cursor++;

    j->head = cursor;
    j->next_sequence = sequence + 1;

    if (was_empty) {
        /* This transaction is now the oldest committed-but-not-yet-
         * checkpointed one. logfs_journal_advance_tail() is the only
         * other place that moves the persisted "start" pointer, and it
         * only runs at checkpoint time — without this, a crash any time
         * before the *first* checkpoint ever happens would leave the
         * on-disk journal superblock claiming the log is empty, and
         * recovery would silently skip every transaction committed since.
         * Subsequent commits into an already-nonempty backlog don't need
         * this: recovery's pass-1 scan finds them by following the
         * descriptor sequence chain forward from wherever start points. */
        rc = jsuper_write(j, desc_offset, sequence);
        if (rc != 0) {
            return rc;
        }
    }

    if (out_sequence != NULL) {
        *out_sequence = sequence;
    }
    if (out_desc_offset != NULL) {
        *out_desc_offset = desc_offset;
    }
    return 0;
}

int logfs_journal_advance_tail(logfs_journal_t *j, uint64_t new_tail_offset,
                                uint64_t new_tail_sequence) {
    uint64_t start = (new_tail_offset == j->head) ? LOGFS_JOURNAL_EMPTY : new_tail_offset;
    int rc = jsuper_write(j, start, new_tail_sequence);
    if (rc != 0) {
        return rc;
    }
    j->tail = new_tail_offset;
    return 0;
}

/* ---- recovery -------------------------------------------------------------- */

struct recovered_txn {
    uint64_t sequence;
    size_t ndata;
    uint64_t *target_blocks; /* ndata entries */
    uint64_t *data_offsets;  /* ndata entries, logical journal offsets */
};

struct revoke_entry {
    uint64_t target_block;
    uint64_t sequence; /* invalidates any earlier-logged copy with sequence < this */
};

static void free_recovered_txns(struct recovered_txn *txns, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(txns[i].target_blocks);
        free(txns[i].data_offsets);
    }
    free(txns);
}

/* Grows *arr (an n-element array of elem_size) by one slot and returns a
 * pointer to the new slot, or NULL on allocation failure. Hand-rolled
 * rather than pulled into a generic vector type: recovery has exactly two
 * call sites and both need different element layouts, so a shared
 * container would cost more in indirection than it would save here. */
static void *grow(void **arr, size_t *n, size_t elem_size) {
    void *next = realloc(*arr, (*n + 1) * elem_size);
    if (next == NULL) {
        return NULL;
    }
    *arr = next;
    uint8_t *slot = (uint8_t *)next + (*n) * elem_size;
    memset(slot, 0, elem_size);
    (*n)++;
    return slot;
}

int logfs_journal_recover(logfs_journal_t *j, logfs_journal_install_fn install, void *ctx) {
    uint8_t block[LOGFS_BLOCK_SIZE];
    int rc = logfs_bdev_read(j->dev, LOGFS_JSUPER_BLOCKNO, block);
    if (rc != 0) {
        return rc;
    }
    struct logfs_journal_super js;
    memcpy(&js, block, sizeof(js));
    if (js.magic != LOGFS_JOURNAL_MAGIC) {
        return -EINVAL;
    }

    if (js.start == LOGFS_JOURNAL_EMPTY) {
        j->head = 0;
        j->tail = 0;
        j->next_sequence = 1;
        return 0; /* nothing to recover */
    }

    struct revoke_entry *revokes = NULL;
    size_t nrevokes = 0;
    struct recovered_txn *txns = NULL;
    size_t ntxns = 0;
    int ret = 0;

    uint64_t scan = js.start;
    uint64_t expect_seq = js.sequence;
    uint64_t last_valid_seq = js.sequence == 0 ? 0 : js.sequence - 1;

    for (;;) {
        uint8_t desc_buf[LOGFS_BLOCK_SIZE];
        if (read_logical(j, scan, desc_buf) != 0) {
            break; /* unreadable: treat as end of valid log */
        }
        struct logfs_desc_block desc;
        memcpy(&desc, desc_buf, sizeof(desc));
        if (desc.magic != LOGFS_DESC_MAGIC || desc.sequence != expect_seq ||
            desc.ntags > LOGFS_MAX_TAGS_PER_DESC) {
            break; /* not a valid next transaction: end of committed range */
        }

        uint64_t cursor = scan + 1;

        uint8_t revoke_buf[LOGFS_BLOCK_SIZE];
        int have_revoke = 0;
        {
            uint8_t peek[LOGFS_BLOCK_SIZE];
            if (read_logical(j, cursor + desc.ntags, peek) == 0) {
                uint32_t magic;
                memcpy(&magic, peek, sizeof(magic));
                if (magic == LOGFS_REVOKE_MAGIC) {
                    have_revoke = 1;
                    memcpy(revoke_buf, peek, LOGFS_BLOCK_SIZE);
                }
            }
        }

        uint64_t commit_off = cursor + desc.ntags + (have_revoke ? 1 : 0);
        uint8_t commit_buf[LOGFS_BLOCK_SIZE];
        if (read_logical(j, commit_off, commit_buf) != 0) {
            break;
        }
        struct logfs_commit_block cb;
        memcpy(&cb, commit_buf, sizeof(cb));
        if (cb.magic != LOGFS_COMMIT_MAGIC || cb.sequence != desc.sequence) {
            break; /* torn commit: this transaction never fully landed */
        }

        /* Re-read the data blocks now (rather than reusing anything from
         * the descriptor pass) so the checksum we verify is computed over
         * exactly the bytes on disk right now, not bytes we assumed are
         * still there. */
        struct logfs_pending_block *pending = NULL;
        uint8_t **data_bufs = NULL;
        if (desc.ntags > 0) {
            pending = calloc(desc.ntags, sizeof(*pending));
            data_bufs = calloc(desc.ntags, sizeof(*data_bufs));
            if (pending == NULL || data_bufs == NULL) {
                free(pending);
                free(data_bufs);
                ret = -ENOMEM;
                goto out;
            }
        }
        int data_ok = 1;
        for (uint32_t i = 0; i < desc.ntags; i++) {
            data_bufs[i] = malloc(LOGFS_BLOCK_SIZE);
            if (data_bufs[i] == NULL || read_logical(j, cursor + i, data_bufs[i]) != 0) {
                data_ok = 0;
                break;
            }
            pending[i].target_block = desc.tags[i].target_block;
            pending[i].data = data_bufs[i];
        }

        int checksum_ok = data_ok &&
            (checksum_transaction(desc_buf, have_revoke ? revoke_buf : NULL, pending, desc.ntags) ==
             cb.checksum);

        if (!checksum_ok) {
            for (uint32_t i = 0; i < desc.ntags; i++) {
                free(data_bufs[i]);
            }
            free(pending);
            free(data_bufs);
            break; /* torn/corrupt transaction: end of committed range */
        }

        struct recovered_txn *t = grow((void **)&txns, &ntxns, sizeof(*t));
        if (t == NULL) {
            for (uint32_t i = 0; i < desc.ntags; i++) {
                free(data_bufs[i]);
            }
            free(pending);
            free(data_bufs);
            ret = -ENOMEM;
            goto out;
        }
        t->sequence = desc.sequence;
        t->ndata = desc.ntags;
        t->target_blocks = calloc(desc.ntags, sizeof(uint64_t));
        t->data_offsets = calloc(desc.ntags, sizeof(uint64_t));
        if (desc.ntags > 0 && (t->target_blocks == NULL || t->data_offsets == NULL)) {
            /* t itself is already linked into txns (grow() appended it
             * above), so the out: cleanup's free_recovered_txns() will
             * free whatever of target_blocks/data_offsets did allocate —
             * just stop filling it in and bail. */
            for (uint32_t i = 0; i < desc.ntags; i++) {
                free(data_bufs[i]);
            }
            free(pending);
            free(data_bufs);
            ret = -ENOMEM;
            goto out;
        }
        for (uint32_t i = 0; i < desc.ntags; i++) {
            t->target_blocks[i] = pending[i].target_block;
            t->data_offsets[i] = cursor + i;
            free(data_bufs[i]);
        }
        free(pending);
        free(data_bufs);

        if (have_revoke) {
            struct logfs_revoke_block rv;
            memcpy(&rv, revoke_buf, sizeof(rv));
            for (uint32_t i = 0; i < rv.nblocks && i < LOGFS_MAX_REVOKES_PER_BLOCK; i++) {
                struct revoke_entry *re = grow((void **)&revokes, &nrevokes, sizeof(*re));
                if (re == NULL) {
                    ret = -ENOMEM;
                    goto out;
                }
                re->target_block = rv.blocks[i];
                re->sequence = rv.sequence;
            }
        }

        last_valid_seq = desc.sequence;
        expect_seq = desc.sequence + 1;
        scan = commit_off + 1;
    }

    /* Pass 2: replay oldest-first, honoring every revoke discovered above
     * regardless of which transaction it was logged in (docs/DESIGN.md §8
     * explains why this must be a second pass rather than replay-as-scanned). */
    for (size_t i = 0; i < ntxns; i++) {
        struct recovered_txn *t = &txns[i];
        for (size_t k = 0; k < t->ndata; k++) {
            int revoked = 0;
            for (size_t r = 0; r < nrevokes; r++) {
                if (revokes[r].target_block == t->target_blocks[k] &&
                    revokes[r].sequence > t->sequence) {
                    revoked = 1;
                    break;
                }
            }
            if (revoked) {
                continue;
            }
            uint8_t data_buf[LOGFS_BLOCK_SIZE];
            if (read_logical(j, t->data_offsets[k], data_buf) != 0) {
                ret = -EIO;
                goto out;
            }
            int irc = install(ctx, t->target_blocks[k], data_buf);
            if (irc != 0) {
                ret = irc;
                goto out;
            }
        }
    }

    j->head = scan;
    j->tail = scan;
    j->next_sequence = last_valid_seq + 1;
    ret = jsuper_write(j, LOGFS_JOURNAL_EMPTY, 0);

out:
    free(revokes);
    free_recovered_txns(txns, ntxns);
    return ret;
}
