#include "blockdev.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static int full_pwrite(int fd, const void *buf, size_t len, off_t off) {
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pwrite(fd, p + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (n == 0) {
            return -EIO; /* backing file can't take any more writes */
        }
        done += (size_t)n;
    }
    return 0;
}

static int full_pread(int fd, void *buf, size_t len, off_t off) {
    uint8_t *p = buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, p + done, len - done, off + (off_t)done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -errno;
        }
        if (n == 0) {
            /* short file: treat unread tail as zeros, matching a freshly
             * mkfs'd block device where blocks past the last write read
             * back as zero */
            memset(p + done, 0, len - done);
            return 0;
        }
        done += (size_t)n;
    }
    return 0;
}

int logfs_bdev_open(logfs_bdev_t *dev, const char *path, uint64_t nblocks, int create) {
    int flags = O_RDWR | (create ? O_CREAT : 0);
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        return -errno;
    }
    if (create) {
        if (nblocks == 0) {
            close(fd);
            return -EINVAL;
        }
        off_t size = (off_t)(nblocks * LOGFS_BLOCK_SIZE);
        if (ftruncate(fd, size) != 0) {
            int e = -errno;
            close(fd);
            return e;
        }
        dev->nblocks = nblocks;
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0) {
            int e = -errno;
            close(fd);
            return e;
        }
        dev->nblocks = (uint64_t)st.st_size / LOGFS_BLOCK_SIZE;
    }
    dev->fd = fd;
    return 0;
}

void logfs_bdev_close(logfs_bdev_t *dev) {
    if (dev->fd >= 0) {
        close(dev->fd);
        dev->fd = -1;
    }
}

int logfs_bdev_read(logfs_bdev_t *dev, uint64_t blockno, void *buf) {
    if (blockno >= dev->nblocks) {
        return -EINVAL;
    }
    return full_pread(dev->fd, buf, LOGFS_BLOCK_SIZE, (off_t)(blockno * LOGFS_BLOCK_SIZE));
}

int logfs_bdev_write(logfs_bdev_t *dev, uint64_t blockno, const void *buf) {
    if (blockno >= dev->nblocks) {
        return -EINVAL;
    }
    return full_pwrite(dev->fd, buf, LOGFS_BLOCK_SIZE, (off_t)(blockno * LOGFS_BLOCK_SIZE));
}

int logfs_bdev_flush(logfs_bdev_t *dev) {
    if (fsync(dev->fd) != 0) {
        return -errno;
    }
    return 0;
}

int logfs_bdev_write_fua(logfs_bdev_t *dev, uint64_t blockno, const void *buf) {
    int rc = logfs_bdev_write(dev, blockno, buf);
    if (rc != 0) {
        return rc;
    }
    return logfs_bdev_flush(dev);
}
