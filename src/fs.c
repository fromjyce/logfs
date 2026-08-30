#include "fs.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "crc32.h"

#define LOGFS_JOURNAL_REGION_START 3u /* blocks 0=boot,1=super,2=journal-super */

static uint32_t super_checksum(const struct logfs_super *sb) {
    struct logfs_super tmp = *sb;
    tmp.checksum = 0;
    uint32_t crc = logfs_crc32_init();
    crc = logfs_crc32_update(crc, &tmp, sizeof(tmp));
    return logfs_crc32_final(crc);
}

static int write_super(logfs_bdev_t *dev, const struct logfs_super *sb) {
    uint8_t block[LOGFS_BLOCK_SIZE];
    memset(block, 0, LOGFS_BLOCK_SIZE);
    memcpy(block, sb, sizeof(*sb));
    return logfs_bdev_write_fua(dev, LOGFS_SUPER_BLOCKNO, block);
}

static void set_bit(uint8_t *bitmap, uint32_t index) {
    bitmap[index / 8] |= (uint8_t)(1u << (index % 8));
}

int logfs_fs_format(const char *path, uint64_t total_blocks, uint64_t journal_len,
                     logfs_data_mode_t default_mode) {
    if (total_blocks < LOGFS_JOURNAL_REGION_START + journal_len + 4) {
        return -EINVAL; /* not enough room for journal + bitmaps + at least one inode/data block */
    }

    logfs_bdev_t dev;
    int rc = logfs_bdev_open(&dev, path, total_blocks, 1);
    if (rc != 0) {
        return rc;
    }

    uint64_t journal_start = LOGFS_JOURNAL_REGION_START;
    uint64_t inode_bitmap_start = journal_start + journal_len;
    uint64_t block_bitmap_start = inode_bitmap_start + 1;
    uint64_t inode_table_start = block_bitmap_start + 1;

    /* Inode numbers map directly to bitmap bit indices (ino N == bit N),
     * and bit 0 is reserved as the "never allocated" sentinel — so the
     * highest usable inode number is LOGFS_FS_MAX_BITMAP_UNITS - 1, not
     * LOGFS_FS_MAX_BITMAP_UNITS (that would need bit index
     * LOGFS_FS_MAX_BITMAP_UNITS, one past the bitmap's last valid bit). */
    uint64_t total_inodes = total_blocks / 4;
    if (total_inodes > LOGFS_FS_MAX_BITMAP_UNITS - 1) {
        total_inodes = LOGFS_FS_MAX_BITMAP_UNITS - 1;
    }
    if (total_inodes < 16) {
        total_inodes = 16;
    }
    uint64_t inode_table_len =
        (total_inodes * sizeof(struct logfs_inode) + LOGFS_BLOCK_SIZE - 1) / LOGFS_BLOCK_SIZE;
    uint64_t data_start = inode_table_start + inode_table_len;

    if (data_start >= total_blocks) {
        logfs_bdev_close(&dev);
        return -ENOSPC; /* image too small for the requested journal_len/inode count */
    }
    uint64_t total_data_blocks = total_blocks - data_start;
    if (total_data_blocks > LOGFS_FS_MAX_BITMAP_UNITS) {
        logfs_bdev_close(&dev);
        return -EFBIG; /* exceeds the single-block data bitmap's range, see fs.h */
    }

    rc = logfs_journal_format(&dev, journal_start, journal_len);
    if (rc != 0) {
        logfs_bdev_close(&dev);
        return rc;
    }

    uint8_t inode_bitmap[LOGFS_BLOCK_SIZE];
    memset(inode_bitmap, 0, LOGFS_BLOCK_SIZE);
    set_bit(inode_bitmap, 0);            /* inode number 0 is never allocated (0 == "free") */
    set_bit(inode_bitmap, LOGFS_ROOT_INO); /* root */
    rc = logfs_bdev_write(&dev, inode_bitmap_start, inode_bitmap);
    if (rc != 0) {
        logfs_bdev_close(&dev);
        return rc;
    }

    uint8_t block_bitmap[LOGFS_BLOCK_SIZE];
    memset(block_bitmap, 0, LOGFS_BLOCK_SIZE);
    set_bit(block_bitmap, 0); /* data block 0 (relative) holds the root directory */
    rc = logfs_bdev_write(&dev, block_bitmap_start, block_bitmap);
    if (rc != 0) {
        logfs_bdev_close(&dev);
        return rc;
    }

    uint8_t zero_block[LOGFS_BLOCK_SIZE];
    memset(zero_block, 0, LOGFS_BLOCK_SIZE);
    for (uint64_t i = 0; i < inode_table_len; i++) {
        rc = logfs_bdev_write(&dev, inode_table_start + i, zero_block);
        if (rc != 0) {
            logfs_bdev_close(&dev);
            return rc;
        }
    }

    uint64_t now = (uint64_t)time(NULL);
    struct logfs_inode root;
    memset(&root, 0, sizeof(root));
    root.mode = S_IFDIR | 0755;
    root.nlink = 2;
    root.size = LOGFS_BLOCK_SIZE;
    root.atime = root.mtime = root.ctime = now;
    root.blocks[0] = data_start; /* first (and only, initially) data block */

    uint8_t inode_block[LOGFS_BLOCK_SIZE];
    memset(inode_block, 0, LOGFS_BLOCK_SIZE);
    memcpy(inode_block, &root, sizeof(root)); /* root is inode 1: index 0 in the table */
    rc = logfs_bdev_write(&dev, inode_table_start, inode_block);
    if (rc != 0) {
        logfs_bdev_close(&dev);
        return rc;
    }

    uint8_t dir_block[LOGFS_BLOCK_SIZE];
    memset(dir_block, 0, LOGFS_BLOCK_SIZE);
    struct logfs_dirent *ents = (struct logfs_dirent *)dir_block;
    ents[0].ino = LOGFS_ROOT_INO;
    ents[0].name_len = 1;
    ents[0].file_type = LOGFS_FT_DIR;
    memcpy(ents[0].name, ".", 1);
    ents[1].ino = LOGFS_ROOT_INO;
    ents[1].name_len = 2;
    ents[1].file_type = LOGFS_FT_DIR;
    memcpy(ents[1].name, "..", 2);
    rc = logfs_bdev_write(&dev, data_start, dir_block);
    if (rc != 0) {
        logfs_bdev_close(&dev);
        return rc;
    }

    struct logfs_super sb;
    memset(&sb, 0, sizeof(sb));
    sb.magic = LOGFS_SUPER_MAGIC;
    sb.version = LOGFS_FORMAT_VERSION;
    sb.block_size = LOGFS_BLOCK_SIZE;
    sb.data_mode = (uint32_t)default_mode;
    sb.total_blocks = total_blocks;
    sb.journal_start = journal_start;
    sb.journal_len = journal_len;
    sb.inode_bitmap_start = inode_bitmap_start;
    sb.block_bitmap_start = block_bitmap_start;
    sb.inode_table_start = inode_table_start;
    sb.inode_table_len = inode_table_len;
    sb.data_start = data_start;
    sb.total_inodes = total_inodes;
    sb.root_ino = LOGFS_ROOT_INO;
    sb.last_orphan = 0;
    sb.checksum = super_checksum(&sb);

    rc = write_super(&dev, &sb); /* last write of mkfs, on purpose */
    logfs_bdev_close(&dev);
    return rc;
}

static int recover_install(void *ctx, uint64_t target_block, const void *data) {
    logfs_bdev_t *dev = ctx;
    return logfs_bdev_write(dev, target_block, data);
}

int logfs_fs_mount(logfs_fs_t *fs, const char *path, int mode_override) {
    memset(fs, 0, sizeof(*fs));

    int rc = logfs_bdev_open(&fs->dev, path, 0, 0);
    if (rc != 0) {
        return rc;
    }

    uint8_t block[LOGFS_BLOCK_SIZE];
    rc = logfs_bdev_read(&fs->dev, LOGFS_SUPER_BLOCKNO, block);
    if (rc != 0) {
        logfs_bdev_close(&fs->dev);
        return rc;
    }
    memcpy(&fs->sb, block, sizeof(fs->sb));
    if (fs->sb.magic != LOGFS_SUPER_MAGIC) {
        logfs_bdev_close(&fs->dev);
        return -EINVAL;
    }
    if (fs->sb.checksum != super_checksum(&fs->sb)) {
        /* EUCLEAN ("structure needs cleaning") is Linux-specific — the
         * same errno ext2/3/4 use for exactly this case. Fine here since
         * this project targets Linux/FUSE only (see docs/DESIGN.md §4);
         * flagged because it wouldn't build against a strict-POSIX libc. */
        logfs_bdev_close(&fs->dev);
        return -EUCLEAN;
    }
    if (fs->sb.total_blocks != fs->dev.nblocks) {
        /* Backing file size disagrees with what mkfs recorded — either a
         * truncated image or the wrong file. Refuse rather than guess. */
        logfs_bdev_close(&fs->dev);
        return -EINVAL;
    }

    rc = logfs_journal_open(&fs->journal, &fs->dev, fs->sb.journal_start, fs->sb.journal_len);
    if (rc != 0) {
        logfs_bdev_close(&fs->dev);
        return rc;
    }
    rc = logfs_journal_recover(&fs->journal, recover_install, &fs->dev);
    if (rc != 0) {
        logfs_bdev_close(&fs->dev);
        return rc;
    }
    rc = logfs_bdev_flush(&fs->dev); /* durably land whatever recovery just replayed */
    if (rc != 0) {
        logfs_bdev_close(&fs->dev);
        return rc;
    }

    logfs_data_mode_t mode =
        (mode_override < 0) ? (logfs_data_mode_t)fs->sb.data_mode : (logfs_data_mode_t)mode_override;
    rc = logfs_txnmgr_create(&fs->txnmgr, &fs->journal, &fs->dev, mode, 5000);
    if (rc != 0) {
        logfs_bdev_close(&fs->dev);
        return rc;
    }
    return 0;
}

void logfs_fs_unmount(logfs_fs_t *fs) {
    logfs_txnmgr_destroy(fs->txnmgr);
    logfs_bdev_close(&fs->dev);
}
