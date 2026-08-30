/*
 * Journal-layer correctness test: commit, remount-and-replay, and a
 * simulated torn-write crash (corrupt the second transaction's commit
 * block, confirm recovery stops cleanly at the first invalid one and
 * still replays everything before it). This exercises exactly the
 * property docs/DESIGN.md §8 is about — it doesn't use CrashMonkey/ACE or
 * dm-log-writes (docs/DESIGN.md §8's actual validation plan), it's a
 * hand-rolled analog scoped to what this journal module alone needs to
 * prove: recovery is idempotent-safe and stops at the right place.
 *
 * Not run as part of this build (see the repo's README/DESIGN.md for why)
 * — written to the same standard as if it were.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "blockdev.h"
#include "crc32.h"
#include "journal.h"

#define TEST_IMAGE "/tmp/logfs_test_journal.img"
#define TEST_BLOCKS 64
#define TEST_JOURNAL_LEN 32
#define TEST_JOURNAL_FIRST 3

struct home_location {
    uint8_t seen[TEST_BLOCKS][LOGFS_BLOCK_SIZE];
    int written[TEST_BLOCKS];
};

static int install_to_home(void *ctx, uint64_t target_block, const void *data) {
    struct home_location *home = ctx;
    assert(target_block < TEST_BLOCKS);
    memcpy(home->seen[target_block], data, LOGFS_BLOCK_SIZE);
    home->written[target_block] = 1;
    return 0;
}

static void fill_block(uint8_t *buf, uint8_t pattern) {
    memset(buf, pattern, LOGFS_BLOCK_SIZE);
}

static void test_commit_and_replay(void) {
    unlink(TEST_IMAGE);
    logfs_bdev_t dev;
    assert(logfs_bdev_open(&dev, TEST_IMAGE, TEST_BLOCKS, 1) == 0);
    assert(logfs_journal_format(&dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);

    logfs_journal_t j;
    assert(logfs_journal_open(&j, &dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);
    struct home_location home;
    memset(&home, 0, sizeof(home));
    assert(logfs_journal_recover(&j, install_to_home, &home) == 0); /* empty log: no-op */
    assert(j.head == 0 && j.tail == 0);

    uint8_t block_a[LOGFS_BLOCK_SIZE], block_b[LOGFS_BLOCK_SIZE];
    fill_block(block_a, 0xAA);
    fill_block(block_b, 0xBB);
    struct logfs_pending_block pend[2] = {
        {.target_block = 40, .data = block_a},
        {.target_block = 41, .data = block_b},
    };
    uint64_t seq, desc_off;
    assert(logfs_journal_commit(&j, pend, 2, NULL, 0, &seq, &desc_off) == 0);
    assert(seq == 1);

    /* "Remount": fresh in-memory journal_t bound to the same backing
     * file, exactly like logfs_fs_mount() does after a real restart. */
    logfs_journal_t j2;
    assert(logfs_journal_open(&j2, &dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);
    struct home_location home2;
    memset(&home2, 0, sizeof(home2));
    assert(logfs_journal_recover(&j2, install_to_home, &home2) == 0);

    assert(home2.written[40] && memcmp(home2.seen[40], block_a, LOGFS_BLOCK_SIZE) == 0);
    assert(home2.written[41] && memcmp(home2.seen[41], block_b, LOGFS_BLOCK_SIZE) == 0);
    assert(j2.head == j2.tail); /* fully replayed: log is empty again */
    assert(j2.next_sequence == 2);

    logfs_bdev_close(&dev);
    printf("test_commit_and_replay: PASS\n");
}

static void test_torn_commit_stops_cleanly(void) {
    unlink(TEST_IMAGE);
    logfs_bdev_t dev;
    assert(logfs_bdev_open(&dev, TEST_IMAGE, TEST_BLOCKS, 1) == 0);
    assert(logfs_journal_format(&dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);

    logfs_journal_t j;
    assert(logfs_journal_open(&j, &dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);
    struct home_location home;
    memset(&home, 0, sizeof(home));
    assert(logfs_journal_recover(&j, install_to_home, &home) == 0);

    uint8_t block_a[LOGFS_BLOCK_SIZE], block_b[LOGFS_BLOCK_SIZE];
    fill_block(block_a, 0x11);
    fill_block(block_b, 0x22);

    struct logfs_pending_block pend1[1] = {{.target_block = 10, .data = block_a}};
    uint64_t seq1, desc_off1;
    assert(logfs_journal_commit(&j, pend1, 1, NULL, 0, &seq1, &desc_off1) == 0);

    uint64_t second_txn_start = j.head;
    struct logfs_pending_block pend2[1] = {{.target_block = 11, .data = block_b}};
    uint64_t seq2, desc_off2;
    assert(logfs_journal_commit(&j, pend2, 1, NULL, 0, &seq2, &desc_off2) == 0);
    (void)desc_off1;
    (void)desc_off2;

    /* Simulate a crash that tore the second transaction's commit block:
     * corrupt it in place (as if only part of that final FUA write landed
     * before power loss). desc(1) + data(1) + commit(1) = 3 journal
     * blocks per transaction here (no revoke), so the second commit block
     * sits at second_txn_start + 2. */
    uint64_t commit_off = second_txn_start + 2;
    uint64_t commit_phys = TEST_JOURNAL_FIRST + (commit_off % TEST_JOURNAL_LEN);
    uint8_t garbage[LOGFS_BLOCK_SIZE];
    memset(garbage, 0xFF, LOGFS_BLOCK_SIZE);
    assert(logfs_bdev_write(&dev, commit_phys, garbage) == 0);
    assert(logfs_bdev_flush(&dev) == 0);

    logfs_journal_t j2;
    assert(logfs_journal_open(&j2, &dev, TEST_JOURNAL_FIRST, TEST_JOURNAL_LEN) == 0);
    struct home_location home2;
    memset(&home2, 0, sizeof(home2));
    assert(logfs_journal_recover(&j2, install_to_home, &home2) == 0);

    /* First transaction: fully valid, must be replayed. */
    assert(home2.written[10] && memcmp(home2.seen[10], block_a, LOGFS_BLOCK_SIZE) == 0);
    /* Second transaction: torn, must NOT be replayed. */
    assert(!home2.written[11]);
    /* next_sequence rolls back to the torn transaction's number, so a
     * fresh commit reuses it rather than leaving a permanent gap. */
    assert(j2.next_sequence == seq2);

    logfs_bdev_close(&dev);
    printf("test_torn_commit_stops_cleanly: PASS\n");
}

int main(void) {
    test_commit_and_replay();
    test_torn_commit_stops_cleanly();
    printf("all journal tests passed\n");
    return 0;
}
