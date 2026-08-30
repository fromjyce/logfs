#include "inode.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static void inode_location(const logfs_fs_t *fs, uint32_t ino, uint64_t *block, uint64_t *offset) {
    uint64_t idx = ino - 1; /* inode 1 (root) lives at index 0 of the table */
    uint64_t per_block = LOGFS_BLOCK_SIZE / sizeof(struct logfs_inode);
    *block = fs->sb.inode_table_start + idx / per_block;
    *offset = (idx % per_block) * sizeof(struct logfs_inode);
}

int logfs_inode_read(logfs_fs_t *fs, uint32_t ino, struct logfs_inode *out) {
    if (ino == 0 || ino > fs->sb.total_inodes) {
        return -EINVAL;
    }
    uint64_t block, offset;
    inode_location(fs, ino, &block, &offset);
    uint8_t buf[LOGFS_BLOCK_SIZE];
    int rc = logfs_txn_read_block(fs->txnmgr, block, buf);
    if (rc != 0) {
        return rc;
    }
    memcpy(out, buf + offset, sizeof(*out));
    return 0;
}

int logfs_inode_write(logfs_handle_t *h, logfs_fs_t *fs, uint32_t ino, const struct logfs_inode *in) {
    if (ino == 0 || ino > fs->sb.total_inodes) {
        return -EINVAL;
    }
    uint64_t block, offset;
    inode_location(fs, ino, &block, &offset);
    void *buf;
    int rc = logfs_txn_get_write_access(h, block, &buf);
    if (rc != 0) {
        return rc;
    }
    memcpy((uint8_t *)buf + offset, in, sizeof(*in));
    return logfs_txn_dirty_metadata(h, block);
}

int logfs_alloc_inode(logfs_handle_t *h, logfs_fs_t *fs, uint32_t *ino_out) {
    void *buf;
    int rc = logfs_txn_get_write_access(h, fs->sb.inode_bitmap_start, &buf);
    if (rc != 0) {
        return rc;
    }
    uint8_t *bitmap = buf;
    for (uint64_t i = 1; i <= fs->sb.total_inodes; i++) {
        if ((bitmap[i / 8] & (1u << (i % 8))) == 0) {
            bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
            rc = logfs_txn_dirty_metadata(h, fs->sb.inode_bitmap_start);
            if (rc != 0) {
                return rc;
            }
            *ino_out = (uint32_t)i;
            return 0;
        }
    }
    return -ENOSPC;
}

int logfs_free_inode(logfs_handle_t *h, logfs_fs_t *fs, uint32_t ino) {
    if (ino == 0 || ino > fs->sb.total_inodes) {
        return -EINVAL;
    }
    void *buf;
    int rc = logfs_txn_get_write_access(h, fs->sb.inode_bitmap_start, &buf);
    if (rc != 0) {
        return rc;
    }
    uint8_t *bitmap = buf;
    bitmap[ino / 8] &= (uint8_t)~(1u << (ino % 8));
    return logfs_txn_dirty_metadata(h, fs->sb.inode_bitmap_start);
}

int logfs_alloc_block(logfs_handle_t *h, logfs_fs_t *fs, uint64_t *blockno_out) {
    void *buf;
    int rc = logfs_txn_get_write_access(h, fs->sb.block_bitmap_start, &buf);
    if (rc != 0) {
        return rc;
    }
    uint8_t *bitmap = buf;
    uint64_t total_data_blocks = fs->sb.total_blocks - fs->sb.data_start;
    for (uint64_t i = 0; i < total_data_blocks; i++) {
        if ((bitmap[i / 8] & (1u << (i % 8))) == 0) {
            bitmap[i / 8] |= (uint8_t)(1u << (i % 8));
            rc = logfs_txn_dirty_metadata(h, fs->sb.block_bitmap_start);
            if (rc != 0) {
                return rc;
            }
            *blockno_out = fs->sb.data_start + i;
            return 0;
        }
    }
    return -ENOSPC;
}

int logfs_free_block(logfs_handle_t *h, logfs_fs_t *fs, uint64_t blockno) {
    if (blockno < fs->sb.data_start || blockno >= fs->sb.total_blocks) {
        return -EINVAL;
    }
    uint64_t i = blockno - fs->sb.data_start;
    void *buf;
    int rc = logfs_txn_get_write_access(h, fs->sb.block_bitmap_start, &buf);
    if (rc != 0) {
        return rc;
    }
    uint8_t *bitmap = buf;
    bitmap[i / 8] &= (uint8_t)~(1u << (i % 8));
    return logfs_txn_dirty_metadata(h, fs->sb.block_bitmap_start);
}

/* ---- directories ------------------------------------------------------------
 * Flat, unsorted, fixed-size-slot directories (disk_format.h's struct
 * logfs_dirent doc explains the tradeoff). LOGFS_NDIR_BLOCKS blocks *
 * LOGFS_DIRENTS_PER_BLOCK entries/block caps a directory at 768 entries —
 * another deliberate scope cut, not the interesting part of this project. */

int logfs_dir_lookup(logfs_fs_t *fs, uint32_t dir_ino, const char *name,
                      uint32_t *ino_out, uint8_t *type_out) {
    struct logfs_inode dir;
    int rc = logfs_inode_read(fs, dir_ino, &dir);
    if (rc != 0) {
        return rc;
    }
    if (!S_ISDIR(dir.mode)) {
        return -ENOTDIR;
    }
    size_t name_len = strnlen(name, LOGFS_NAME_MAX + 1);
    if (name_len == 0 || name_len > LOGFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    for (int b = 0; b < LOGFS_NDIR_BLOCKS; b++) {
        if (dir.blocks[b] == 0) {
            break;
        }
        uint8_t buf[LOGFS_BLOCK_SIZE];
        rc = logfs_txn_read_block(fs->txnmgr, dir.blocks[b], buf);
        if (rc != 0) {
            return rc;
        }
        struct logfs_dirent *ents = (struct logfs_dirent *)buf;
        for (size_t e = 0; e < LOGFS_DIRENTS_PER_BLOCK; e++) {
            if (ents[e].ino != 0 && ents[e].name_len == name_len &&
                memcmp(ents[e].name, name, name_len) == 0) {
                *ino_out = ents[e].ino;
                if (type_out != NULL) {
                    *type_out = ents[e].file_type;
                }
                return 0;
            }
        }
    }
    return -ENOENT;
}

int logfs_dir_add(logfs_handle_t *h, logfs_fs_t *fs, uint32_t dir_ino,
                   const char *name, uint32_t ino, uint8_t type) {
    struct logfs_inode dir;
    int rc = logfs_inode_read(fs, dir_ino, &dir);
    if (rc != 0) {
        return rc;
    }
    if (!S_ISDIR(dir.mode)) {
        return -ENOTDIR;
    }
    size_t name_len = strlen(name);
    if (name_len == 0 || name_len > LOGFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }

    uint32_t existing_ino;
    if (logfs_dir_lookup(fs, dir_ino, name, &existing_ino, NULL) == 0) {
        return -EEXIST;
    }

    for (int b = 0; b < LOGFS_NDIR_BLOCKS; b++) {
        uint64_t blockno = dir.blocks[b];

        if (blockno == 0) {
            /* No more existing blocks — allocate one; it's guaranteed to
             * have a free slot, so this is always where the entry lands. */
            rc = logfs_alloc_block(h, fs, &blockno);
            if (rc != 0) {
                return rc;
            }
            dir.blocks[b] = blockno;
            dir.size += LOGFS_BLOCK_SIZE;

            void *buf;
            rc = logfs_txn_get_write_access(h, blockno, &buf);
            if (rc != 0) {
                return rc;
            }
            /* Freshly allocated: may hold stale dirents from a directory
             * that freed this block earlier (free_block doesn't zero on
             * free — see disk_format.h). Must clear before reusing. */
            memset(buf, 0, LOGFS_BLOCK_SIZE);
            struct logfs_dirent *ents = (struct logfs_dirent *)buf;
            ents[0].ino = ino;
            ents[0].name_len = (uint8_t)name_len;
            ents[0].file_type = type;
            memcpy(ents[0].name, name, name_len);
            rc = logfs_txn_dirty_metadata(h, blockno);
            if (rc != 0) {
                return rc;
            }
            return logfs_inode_write(h, fs, dir_ino, &dir);
        }

        uint8_t rbuf[LOGFS_BLOCK_SIZE];
        rc = logfs_txn_read_block(fs->txnmgr, blockno, rbuf);
        if (rc != 0) {
            return rc;
        }
        struct logfs_dirent *rents = (struct logfs_dirent *)rbuf;
        int free_slot = -1;
        for (size_t e = 0; e < LOGFS_DIRENTS_PER_BLOCK; e++) {
            if (rents[e].ino == 0) {
                free_slot = (int)e;
                break;
            }
        }
        if (free_slot < 0) {
            continue; /* block full, try the next one */
        }

        void *wbuf;
        rc = logfs_txn_get_write_access(h, blockno, &wbuf);
        if (rc != 0) {
            return rc;
        }
        struct logfs_dirent *wents = (struct logfs_dirent *)wbuf;
        wents[free_slot].ino = ino;
        wents[free_slot].name_len = (uint8_t)name_len;
        wents[free_slot].file_type = type;
        memset(wents[free_slot].name, 0, sizeof(wents[free_slot].name));
        memcpy(wents[free_slot].name, name, name_len);
        return logfs_txn_dirty_metadata(h, blockno);
    }
    return -ENOSPC; /* directory full: all LOGFS_NDIR_BLOCKS blocks allocated and full */
}

int logfs_dir_remove(logfs_handle_t *h, logfs_fs_t *fs, uint32_t dir_ino, const char *name) {
    struct logfs_inode dir;
    int rc = logfs_inode_read(fs, dir_ino, &dir);
    if (rc != 0) {
        return rc;
    }
    if (!S_ISDIR(dir.mode)) {
        return -ENOTDIR;
    }
    size_t name_len = strlen(name);

    for (int b = 0; b < LOGFS_NDIR_BLOCKS; b++) {
        if (dir.blocks[b] == 0) {
            break;
        }
        uint8_t rbuf[LOGFS_BLOCK_SIZE];
        rc = logfs_txn_read_block(fs->txnmgr, dir.blocks[b], rbuf);
        if (rc != 0) {
            return rc;
        }
        struct logfs_dirent *rents = (struct logfs_dirent *)rbuf;
        for (size_t e = 0; e < LOGFS_DIRENTS_PER_BLOCK; e++) {
            if (rents[e].ino != 0 && rents[e].name_len == name_len &&
                memcmp(rents[e].name, name, name_len) == 0) {
                void *wbuf;
                rc = logfs_txn_get_write_access(h, dir.blocks[b], &wbuf);
                if (rc != 0) {
                    return rc;
                }
                struct logfs_dirent *wents = (struct logfs_dirent *)wbuf;
                wents[e].ino = 0;
                return logfs_txn_dirty_metadata(h, dir.blocks[b]);
            }
        }
    }
    return -ENOENT;
}

int logfs_dir_is_empty(logfs_fs_t *fs, uint32_t dir_ino) {
    struct logfs_inode dir;
    int rc = logfs_inode_read(fs, dir_ino, &dir);
    if (rc != 0) {
        return rc;
    }
    if (!S_ISDIR(dir.mode)) {
        return -ENOTDIR;
    }
    for (int b = 0; b < LOGFS_NDIR_BLOCKS; b++) {
        if (dir.blocks[b] == 0) {
            break;
        }
        uint8_t buf[LOGFS_BLOCK_SIZE];
        rc = logfs_txn_read_block(fs->txnmgr, dir.blocks[b], buf);
        if (rc != 0) {
            return rc;
        }
        struct logfs_dirent *ents = (struct logfs_dirent *)buf;
        for (size_t e = 0; e < LOGFS_DIRENTS_PER_BLOCK; e++) {
            if (ents[e].ino == 0) {
                continue;
            }
            int is_dot = (ents[e].name_len == 1 && ents[e].name[0] == '.');
            int is_dotdot = (ents[e].name_len == 2 && ents[e].name[0] == '.' && ents[e].name[1] == '.');
            if (!is_dot && !is_dotdot) {
                return 0;
            }
        }
    }
    return 1;
}

int logfs_path_lookup(logfs_fs_t *fs, const char *path, uint32_t *ino_out) {
    if (path == NULL || path[0] != '/') {
        return -EINVAL;
    }
    uint32_t cur = fs->sb.root_ino;
    size_t len = strlen(path);
    size_t i = 1;
    while (i < len) {
        while (i < len && path[i] == '/') {
            i++;
        }
        if (i >= len) {
            break;
        }
        size_t start = i;
        while (i < len && path[i] != '/') {
            i++;
        }
        size_t comp_len = i - start;
        if (comp_len > LOGFS_NAME_MAX) {
            return -ENAMETOOLONG;
        }
        char comp[LOGFS_NAME_MAX + 1];
        memcpy(comp, path + start, comp_len);
        comp[comp_len] = '\0';

        uint32_t next_ino;
        int rc = logfs_dir_lookup(fs, cur, comp, &next_ino, NULL);
        if (rc != 0) {
            return rc;
        }
        cur = next_ino;
    }
    *ino_out = cur;
    return 0;
}
