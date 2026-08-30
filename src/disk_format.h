#ifndef LOGFS_DISK_FORMAT_H
#define LOGFS_DISK_FORMAT_H

#include <stdint.h>

/*
 * On-disk layout (see docs/DESIGN.md §4-5):
 *
 *   block 0        boot block, reserved, never written by logfs itself
 *   block 1        struct logfs_super
 *   block 2        struct logfs_journal_super
 *   [journal_start, journal_start+journal_len)   circular journal region
 *   [inode_bitmap_start)                          1 block, bit per inode
 *   [block_bitmap_start)                          1 block, bit per data block
 *   [inode_table_start, +inode_table_len)         struct logfs_inode array
 *   [data_start, total_blocks)                    data blocks
 *
 * Every on-disk struct is read/written as a whole LOGFS_BLOCK_SIZE buffer
 * (see blockdev.h) and then reinterpreted through these struct defs, so
 * struct size only needs to fit inside a block, not equal it exactly.
 */

#define LOGFS_BLOCK_SIZE   4096u

#define LOGFS_SUPER_MAGIC     0x4C4F4746u /* "LOGF" */
#define LOGFS_JOURNAL_MAGIC   0x4C4A524Eu /* "LJRN" */
#define LOGFS_DESC_MAGIC      0x4C444352u /* "LDCR" */
#define LOGFS_COMMIT_MAGIC    0x4C434D54u /* "LCMT" */
#define LOGFS_REVOKE_MAGIC    0x4C52564Bu /* "LRVK" */

#define LOGFS_SUPER_BLOCKNO    1u
#define LOGFS_JSUPER_BLOCKNO   2u

#define LOGFS_ROOT_INO         1u

#define LOGFS_FORMAT_VERSION   1u

typedef enum {
    LOGFS_MODE_WRITEBACK = 0, /* metadata journaled, data unordered  */
    LOGFS_MODE_ORDERED   = 1, /* metadata journaled, data forced out before commit */
    LOGFS_MODE_JOURNAL   = 2, /* metadata and data both journaled */
} logfs_data_mode_t;

struct logfs_super {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t data_mode;         /* default logfs_data_mode_t, overridable at mount */

    uint64_t total_blocks;
    uint64_t journal_start;
    uint64_t journal_len;
    uint64_t inode_bitmap_start;
    uint64_t block_bitmap_start;
    uint64_t inode_table_start;
    uint64_t inode_table_len;
    uint64_t data_start;
    uint64_t total_inodes;

    uint32_t root_ino;
    uint32_t last_orphan;       /* head of unlinked-but-open orphan list, 0 = none */
    uint32_t checksum;          /* crc32 over the struct with this field zeroed */
    uint32_t _reserved;
};

struct logfs_journal_super {
    uint32_t magic;
    uint32_t block_size;
    uint64_t maxlen;    /* journal region length in blocks, excludes this superblock */
    uint64_t first;     /* physical block number of the journal region's first block */
    uint64_t sequence;  /* oldest transaction sequence recovery must still consider */
    uint64_t start;     /* logical offset (mod maxlen) of that transaction's descriptor,
                          * UINT64_MAX means the log is empty */
};

#define LOGFS_JOURNAL_EMPTY UINT64_MAX

struct logfs_journal_tag {
    uint64_t target_block;
};

/* sized so a descriptor block fits in one LOGFS_BLOCK_SIZE buffer */
#define LOGFS_MAX_TAGS_PER_DESC \
    ((LOGFS_BLOCK_SIZE - 16u) / sizeof(struct logfs_journal_tag))

struct logfs_desc_block {
    uint32_t magic;
    uint32_t ntags;
    uint64_t sequence;
    struct logfs_journal_tag tags[LOGFS_MAX_TAGS_PER_DESC];
};

struct logfs_commit_block {
    uint32_t magic;
    uint32_t _reserved;
    uint64_t sequence;
    uint32_t checksum;    /* crc32 over descriptor block + every logged data block
                            * in this transaction, computed over their on-disk bytes */
};

#define LOGFS_MAX_REVOKES_PER_BLOCK \
    ((LOGFS_BLOCK_SIZE - 16u) / sizeof(uint64_t))

struct logfs_revoke_block {
    uint32_t magic;
    uint32_t nblocks;
    uint64_t sequence;   /* revokes any earlier-logged copy of these blocks belonging
                           * to a transaction with sequence < this one */
    uint64_t blocks[LOGFS_MAX_REVOKES_PER_BLOCK];
};

#define LOGFS_NDIR_BLOCKS 12

struct logfs_inode {
    uint32_t mode;   /* POSIX mode_t: type bits + permission bits */
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint64_t blocks[LOGFS_NDIR_BLOCKS];
    /* Direct blocks only: 12 * LOGFS_BLOCK_SIZE = 48KiB max file size. No
     * indirect blocks — the allocator isn't this project's hard part (see
     * docs/DESIGN.md §1 non-goals); this cap is a deliberate scope cut. */
};

#define LOGFS_NAME_MAX 55

struct logfs_dirent {
    uint32_t ino;        /* 0 = free slot */
    uint8_t  name_len;
    uint8_t  file_type;  /* LOGFS_FT_* */
    uint16_t _pad;
    char     name[LOGFS_NAME_MAX + 1];
}; /* fixed 64-byte slot: LOGFS_BLOCK_SIZE / 64 == 64 dirents per block */

#define LOGFS_FT_REG  1
#define LOGFS_FT_DIR  2

#define LOGFS_DIRENTS_PER_BLOCK (LOGFS_BLOCK_SIZE / sizeof(struct logfs_dirent))

_Static_assert(sizeof(struct logfs_super) <= LOGFS_BLOCK_SIZE, "super too big");
_Static_assert(sizeof(struct logfs_journal_super) <= LOGFS_BLOCK_SIZE, "jsuper too big");
_Static_assert(sizeof(struct logfs_desc_block) <= LOGFS_BLOCK_SIZE, "desc too big");
_Static_assert(sizeof(struct logfs_commit_block) <= LOGFS_BLOCK_SIZE, "commit too big");
_Static_assert(sizeof(struct logfs_revoke_block) <= LOGFS_BLOCK_SIZE, "revoke too big");
_Static_assert(sizeof(struct logfs_dirent) == 64, "dirent slot must be 64 bytes");
_Static_assert(LOGFS_BLOCK_SIZE % sizeof(struct logfs_dirent) == 0,
               "dirents must tile a block evenly");

#endif /* LOGFS_DISK_FORMAT_H */
