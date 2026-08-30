#define FUSE_USE_VERSION 31

#include <errno.h>
/* Included as <fuse.h>, not <fuse3/fuse.h>: `pkg-config --cflags fuse3`
 * (see the Makefile) already adds -I/usr/include/fuse3 to the search
 * path, so the subdirectory belongs in the build flags, not the
 * #include — writing <fuse3/fuse.h> here would look for a nonexistent
 * .../fuse3/fuse3/fuse.h once that flag is applied. */
#include <fuse.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "fs.h"
#include "inode.h"

/*
 * FUSE operation callbacks. Every mutating call takes the single
 * filesystem-wide g_fs_lock for its whole duration — the scope cut
 * documented in txn.h's concurrency contract: real filesystems serialize
 * concurrent operations on the same inode/directory via a per-inode lock
 * before ever reaching the journal layer, which lets unrelated inodes
 * proceed in parallel. One global lock here is simpler and still correct,
 * at the cost of that parallelism; fine-grained locking isn't this
 * project's hard part (docs/DESIGN.md §1 non-goals).
 *
 * Known gap, documented rather than fixed: operations that fail partway
 * through (e.g. logfs_dir_add() returns -ENOSPC after logfs_alloc_inode()
 * already succeeded) leave that allocation staged in the current
 * generation anyway — it will be journaled and committed on the next
 * commit cycle regardless of the FUSE call's own error return. The
 * staging table is shared per-generation, not per-handle, so there's no
 * cheap way to roll back just this handle's contribution without a larger
 * redesign. The result is a leaked (but bitmap-consistent, never
 * corrupting) inode or block on this error path — worth knowing about,
 * not worth the redesign for a project whose hard part is crash
 * consistency of committed state, not in-memory transaction abort.
 */

static pthread_mutex_t g_fs_lock = PTHREAD_MUTEX_INITIALIZER;

static void inode_to_stat(uint32_t ino, const struct logfs_inode *in, struct stat *st) {
    memset(st, 0, sizeof(*st));
    st->st_ino = ino;
    st->st_mode = in->mode;
    st->st_nlink = in->nlink;
    st->st_uid = in->uid;
    st->st_gid = in->gid;
    st->st_size = (off_t)in->size;
    st->st_blocks = (blkcnt_t)((in->size + 511) / 512);
    st->st_atime = (time_t)in->atime;
    st->st_mtime = (time_t)in->mtime;
    st->st_ctime = (time_t)in->ctime;
}

static int split_parent(const char *path, char *parent, size_t parent_cap, const char **name_out) {
    const char *slash = strrchr(path, '/');
    if (slash == NULL) {
        return -EINVAL; /* FUSE always hands us absolute paths */
    }
    size_t plen = (size_t)(slash - path);
    if (plen == 0) {
        if (parent_cap < 2) {
            return -ENAMETOOLONG;
        }
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen + 1 > parent_cap) {
            return -ENAMETOOLONG;
        }
        memcpy(parent, path, plen);
        parent[plen] = '\0';
    }
    *name_out = slash + 1;
    if (**name_out == '\0') {
        return -EINVAL; /* trailing slash */
    }
    if (strlen(*name_out) > LOGFS_NAME_MAX) {
        return -ENAMETOOLONG;
    }
    return 0;
}

/* Frees every allocated direct block whose index is >= keep_blocks. */
static int truncate_blocks(logfs_handle_t *h, logfs_fs_t *fs, struct logfs_inode *in,
                            int keep_blocks) {
    for (int b = keep_blocks; b < LOGFS_NDIR_BLOCKS; b++) {
        if (in->blocks[b] == 0) {
            continue;
        }
        int rc = logfs_free_block(h, fs, in->blocks[b]);
        if (rc != 0) {
            return rc;
        }
        in->blocks[b] = 0;
    }
    return 0;
}

static int logfs_op_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void)fi;
    logfs_fs_t *fs = fuse_get_context()->private_data;
    memset(st, 0, sizeof(*st));

    pthread_mutex_lock(&g_fs_lock);
    uint32_t ino;
    int rc = logfs_path_lookup(fs, path, &ino);
    if (rc == 0) {
        struct logfs_inode in;
        rc = logfs_inode_read(fs, ino, &in);
        if (rc == 0) {
            inode_to_stat(ino, &in, st);
        }
    }
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                             struct fuse_file_info *fi, enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    logfs_fs_t *fs = fuse_get_context()->private_data;

    pthread_mutex_lock(&g_fs_lock);
    uint32_t dir_ino;
    int rc = logfs_path_lookup(fs, path, &dir_ino);
    if (rc != 0) {
        goto out;
    }
    struct logfs_inode dir;
    rc = logfs_inode_read(fs, dir_ino, &dir);
    if (rc != 0) {
        goto out;
    }
    if (!S_ISDIR(dir.mode)) {
        rc = -ENOTDIR;
        goto out;
    }
    for (int b = 0; b < LOGFS_NDIR_BLOCKS; b++) {
        if (dir.blocks[b] == 0) {
            break;
        }
        uint8_t block[LOGFS_BLOCK_SIZE];
        rc = logfs_txn_read_block(fs->txnmgr, dir.blocks[b], block);
        if (rc != 0) {
            goto out;
        }
        struct logfs_dirent *ents = (struct logfs_dirent *)block;
        for (size_t e = 0; e < LOGFS_DIRENTS_PER_BLOCK; e++) {
            if (ents[e].ino == 0) {
                continue;
            }
            char name[LOGFS_NAME_MAX + 1];
            memcpy(name, ents[e].name, ents[e].name_len);
            name[ents[e].name_len] = '\0';
            if (filler(buf, name, NULL, 0, 0) != 0) {
                /* Caller's buffer is full; there's no offset-based resume
                 * cursor in this implementation (see the (void)offset
                 * above), so a directory that overflows a single readdir
                 * buffer would need one to list correctly — not reachable
                 * at this project's 768-entries-per-directory cap
                 * (disk_format.h), but worth stopping cleanly rather than
                 * continuing to fill past what the caller asked for. */
                goto out;
            }
        }
    }
    rc = 0;
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_mkdir(const char *path, mode_t mode) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    char parent[PATH_MAX];
    const char *name;
    int rc = split_parent(path, parent, sizeof(parent), &name);
    if (rc != 0) {
        return rc;
    }

    pthread_mutex_lock(&g_fs_lock);
    uint32_t parent_ino;
    rc = logfs_path_lookup(fs, parent, &parent_ino);
    if (rc != 0) {
        goto out;
    }

    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    uint32_t new_ino;
    rc = logfs_alloc_inode(&h, fs, &new_ino);
    if (rc != 0) {
        goto stop;
    }
    uint64_t data_block;
    rc = logfs_alloc_block(&h, fs, &data_block);
    if (rc != 0) {
        goto stop;
    }

    void *buf;
    rc = logfs_txn_get_write_access(&h, data_block, &buf);
    if (rc != 0) {
        goto stop;
    }
    memset(buf, 0, LOGFS_BLOCK_SIZE);
    struct logfs_dirent *ents = (struct logfs_dirent *)buf;
    ents[0].ino = new_ino;
    ents[0].name_len = 1;
    ents[0].file_type = LOGFS_FT_DIR;
    ents[0].name[0] = '.';
    ents[1].ino = parent_ino;
    ents[1].name_len = 2;
    ents[1].file_type = LOGFS_FT_DIR;
    ents[1].name[0] = '.';
    ents[1].name[1] = '.';
    rc = logfs_txn_dirty_metadata(&h, data_block);
    if (rc != 0) {
        goto stop;
    }

    uint64_t now = (uint64_t)time(NULL);
    struct logfs_inode newdir;
    memset(&newdir, 0, sizeof(newdir));
    newdir.mode = S_IFDIR | (mode & 0777u);
    newdir.nlink = 2;
    newdir.size = LOGFS_BLOCK_SIZE;
    newdir.blocks[0] = data_block;
    newdir.atime = newdir.mtime = newdir.ctime = now;
    rc = logfs_inode_write(&h, fs, new_ino, &newdir);
    if (rc != 0) {
        goto stop;
    }

    rc = logfs_dir_add(&h, fs, parent_ino, name, new_ino, LOGFS_FT_DIR);
    if (rc != 0) {
        goto stop;
    }

    struct logfs_inode pdir;
    rc = logfs_inode_read(fs, parent_ino, &pdir);
    if (rc != 0) {
        goto stop;
    }
    pdir.nlink += 1; /* the new subdirectory's ".." now points at parent */
    pdir.mtime = now;
    rc = logfs_inode_write(&h, fs, parent_ino, &pdir);

stop:
    logfs_txn_stop(&h);
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_rmdir(const char *path) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    char parent[PATH_MAX];
    const char *name;
    int rc = split_parent(path, parent, sizeof(parent), &name);
    if (rc != 0) {
        return rc;
    }

    pthread_mutex_lock(&g_fs_lock);
    uint32_t parent_ino, target_ino;
    uint8_t type;
    rc = logfs_path_lookup(fs, parent, &parent_ino);
    if (rc != 0) {
        goto out;
    }
    rc = logfs_dir_lookup(fs, parent_ino, name, &target_ino, &type);
    if (rc != 0) {
        goto out;
    }
    if (type != LOGFS_FT_DIR) {
        rc = -ENOTDIR;
        goto out;
    }
    rc = logfs_dir_is_empty(fs, target_ino);
    if (rc < 0) {
        goto out;
    }
    if (rc == 0) {
        rc = -ENOTEMPTY;
        goto out;
    }

    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    struct logfs_inode target;
    rc = logfs_inode_read(fs, target_ino, &target);
    if (rc != 0) {
        goto stop;
    }
    /* A directory that once held many entries can have grown past
     * blocks[0] even if it's empty now — dir_remove() only clears dirent
     * slots, it never shrinks the block list back down. Free everything
     * allocated, not just the block every directory starts with. */
    rc = truncate_blocks(&h, fs, &target, 0);
    if (rc != 0) {
        goto stop;
    }
    rc = logfs_free_inode(&h, fs, target_ino);
    if (rc != 0) {
        goto stop;
    }
    rc = logfs_dir_remove(&h, fs, parent_ino, name);
    if (rc != 0) {
        goto stop;
    }

    struct logfs_inode pdir;
    rc = logfs_inode_read(fs, parent_ino, &pdir);
    if (rc != 0) {
        goto stop;
    }
    pdir.nlink -= 1;
    pdir.mtime = (uint64_t)time(NULL);
    rc = logfs_inode_write(&h, fs, parent_ino, &pdir);

stop:
    logfs_txn_stop(&h);
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    char parent[PATH_MAX];
    const char *name;
    int rc = split_parent(path, parent, sizeof(parent), &name);
    if (rc != 0) {
        return rc;
    }

    pthread_mutex_lock(&g_fs_lock);
    uint32_t parent_ino;
    rc = logfs_path_lookup(fs, parent, &parent_ino);
    if (rc != 0) {
        goto out;
    }

    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    uint32_t new_ino;
    rc = logfs_alloc_inode(&h, fs, &new_ino);
    if (rc != 0) {
        goto stop;
    }

    uint64_t now = (uint64_t)time(NULL);
    struct logfs_inode newfile;
    memset(&newfile, 0, sizeof(newfile));
    newfile.mode = S_IFREG | (mode & 0777u);
    newfile.nlink = 1;
    newfile.atime = newfile.mtime = newfile.ctime = now;
    rc = logfs_inode_write(&h, fs, new_ino, &newfile);
    if (rc != 0) {
        goto stop;
    }

    rc = logfs_dir_add(&h, fs, parent_ino, name, new_ino, LOGFS_FT_REG);
    if (rc == 0) {
        fi->fh = new_ino;
    }

stop:
    logfs_txn_stop(&h);
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_open(const char *path, struct fuse_file_info *fi) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    pthread_mutex_lock(&g_fs_lock);
    uint32_t ino;
    int rc = logfs_path_lookup(fs, path, &ino);
    if (rc == 0) {
        fi->fh = ino;
    }
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_read(const char *path, char *buf, size_t size, off_t offset,
                          struct fuse_file_info *fi) {
    (void)path;
    logfs_fs_t *fs = fuse_get_context()->private_data;
    uint32_t ino = (uint32_t)fi->fh;

    pthread_mutex_lock(&g_fs_lock);
    struct logfs_inode in;
    int rc = logfs_inode_read(fs, ino, &in);
    if (rc != 0) {
        goto out;
    }
    if ((uint64_t)offset >= in.size) {
        rc = 0;
        goto out;
    }
    if ((uint64_t)offset + size > in.size) {
        size = (size_t)(in.size - (uint64_t)offset);
    }

    size_t done = 0;
    while (done < size) {
        uint64_t pos = (uint64_t)offset + done;
        int b = (int)(pos / LOGFS_BLOCK_SIZE);
        size_t in_block = (size_t)(pos % LOGFS_BLOCK_SIZE);
        size_t chunk = LOGFS_BLOCK_SIZE - in_block;
        if (chunk > size - done) {
            chunk = size - done;
        }
        if (in.blocks[b] == 0) {
            memset(buf + done, 0, chunk); /* sparse hole */
        } else {
            uint8_t block[LOGFS_BLOCK_SIZE];
            rc = logfs_txn_read_block(fs->txnmgr, in.blocks[b], block);
            if (rc != 0) {
                goto out;
            }
            memcpy(buf + done, block + in_block, chunk);
        }
        done += chunk;
    }
    rc = (int)size;
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_write(const char *path, const char *buf, size_t size, off_t offset,
                           struct fuse_file_info *fi) {
    (void)path;
    logfs_fs_t *fs = fuse_get_context()->private_data;
    uint32_t ino = (uint32_t)fi->fh;

    if ((uint64_t)offset + size > (uint64_t)LOGFS_NDIR_BLOCKS * LOGFS_BLOCK_SIZE) {
        return -EFBIG; /* direct-blocks-only cap, see disk_format.h */
    }

    pthread_mutex_lock(&g_fs_lock);
    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    struct logfs_inode in;
    int rc = logfs_inode_read(fs, ino, &in);
    if (rc != 0) {
        goto stop;
    }

    size_t done = 0;
    while (done < size) {
        uint64_t pos = (uint64_t)offset + done;
        int b = (int)(pos / LOGFS_BLOCK_SIZE);
        size_t in_block = (size_t)(pos % LOGFS_BLOCK_SIZE);
        size_t chunk = LOGFS_BLOCK_SIZE - in_block;
        if (chunk > size - done) {
            chunk = size - done;
        }

        if (in.blocks[b] == 0) {
            uint64_t nb;
            rc = logfs_alloc_block(&h, fs, &nb);
            if (rc != 0) {
                goto stop;
            }
            in.blocks[b] = nb;
        }

        uint8_t block[LOGFS_BLOCK_SIZE];
        if (in_block != 0 || chunk != LOGFS_BLOCK_SIZE) {
            rc = logfs_txn_read_block(fs->txnmgr, in.blocks[b], block);
            if (rc != 0) {
                goto stop;
            }
        }
        memcpy(block + in_block, buf + done, chunk);
        rc = logfs_txn_write_data(&h, in.blocks[b], block);
        if (rc != 0) {
            goto stop;
        }
        done += chunk;
    }

    if ((uint64_t)offset + size > in.size) {
        in.size = (uint64_t)offset + size;
    }
    in.mtime = (uint64_t)time(NULL);
    rc = logfs_inode_write(&h, fs, ino, &in);
    if (rc == 0) {
        rc = (int)size;
    }

stop:
    logfs_txn_stop(&h);
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_truncate(const char *path, off_t size, struct fuse_file_info *fi) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    if ((uint64_t)size > (uint64_t)LOGFS_NDIR_BLOCKS * LOGFS_BLOCK_SIZE) {
        return -EFBIG;
    }

    pthread_mutex_lock(&g_fs_lock);
    uint32_t ino;
    int rc;
    if (fi != NULL) {
        ino = (uint32_t)fi->fh;
        rc = 0;
    } else {
        rc = logfs_path_lookup(fs, path, &ino);
    }
    if (rc != 0) {
        goto out;
    }

    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    struct logfs_inode in;
    rc = logfs_inode_read(fs, ino, &in);
    if (rc != 0) {
        goto stop;
    }
    int keep_blocks = (int)(((uint64_t)size + LOGFS_BLOCK_SIZE - 1) / LOGFS_BLOCK_SIZE);
    if ((uint64_t)size < in.size) {
        rc = truncate_blocks(&h, fs, &in, keep_blocks);
        if (rc != 0) {
            goto stop;
        }
    }
    in.size = (uint64_t)size;
    in.mtime = (uint64_t)time(NULL);
    rc = logfs_inode_write(&h, fs, ino, &in);

stop:
    logfs_txn_stop(&h);
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_unlink(const char *path) {
    logfs_fs_t *fs = fuse_get_context()->private_data;
    char parent[PATH_MAX];
    const char *name;
    int rc = split_parent(path, parent, sizeof(parent), &name);
    if (rc != 0) {
        return rc;
    }

    pthread_mutex_lock(&g_fs_lock);
    uint32_t parent_ino, target_ino;
    rc = logfs_path_lookup(fs, parent, &parent_ino);
    if (rc != 0) {
        goto out;
    }
    rc = logfs_dir_lookup(fs, parent_ino, name, &target_ino, NULL);
    if (rc != 0) {
        goto out;
    }

    logfs_handle_t h;
    logfs_txn_start(fs->txnmgr, &h);

    struct logfs_inode target;
    rc = logfs_inode_read(fs, target_ino, &target);
    if (rc != 0) {
        goto stop;
    }
    rc = logfs_dir_remove(&h, fs, parent_ino, name);
    if (rc != 0) {
        goto stop;
    }

    /* No open-file-handle tracking in this toy build (see disk_format.h's
     * non-goals): a real filesystem keeps an unlinked-but-open inode alive
     * via the orphan list (docs/DESIGN.md §10) until the last fd closes.
     * Here nlink reaching 0 frees immediately. */
    target.nlink -= 1;
    if (target.nlink == 0) {
        rc = truncate_blocks(&h, fs, &target, 0);
        if (rc != 0) {
            goto stop;
        }
        rc = logfs_free_inode(&h, fs, target_ino);
    } else {
        rc = logfs_inode_write(&h, fs, target_ino, &target);
    }

stop:
    logfs_txn_stop(&h);
out:
    pthread_mutex_unlock(&g_fs_lock);
    return rc;
}

static int logfs_op_fsync(const char *path, int datasync, struct fuse_file_info *fi) {
    (void)path;
    (void)datasync;
    (void)fi;
    logfs_fs_t *fs = fuse_get_context()->private_data;
    return logfs_txn_commit_sync(fs->txnmgr);
}

static int logfs_op_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    (void)fi;
    return 0;
}

const struct fuse_operations logfs_fuse_ops = {
    .getattr = logfs_op_getattr,
    .readdir = logfs_op_readdir,
    .mkdir = logfs_op_mkdir,
    .rmdir = logfs_op_rmdir,
    .create = logfs_op_create,
    .open = logfs_op_open,
    .read = logfs_op_read,
    .write = logfs_op_write,
    .truncate = logfs_op_truncate,
    .unlink = logfs_op_unlink,
    .fsync = logfs_op_fsync,
    .release = logfs_op_release,
};
