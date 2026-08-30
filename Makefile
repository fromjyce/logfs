# Not run as part of building this repo (see README.md/docs/DESIGN.md —
# this codebase is a design-quality implementation, not a built/tested
# one). Written to build cleanly against Linux + libfuse3-dev, the
# toolchain this project targets.

CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -g -O2
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS := $(shell pkg-config --libs fuse3)

COMMON_OBJS := crc32.o blockdev.o journal.o txn.o fs.o inode.o

.PHONY: all clean test

all: logfs_mount logfs_mkfs

%.o: src/%.c
	$(CC) $(CFLAGS) -Isrc -c $< -o $@

# fs_ops.o and main.o both touch FUSE headers/types, so they need the
# fuse3 cflags; everything else in COMMON_OBJS is FUSE-agnostic on
# purpose (journal.c/txn.c/inode.c don't know FUSE exists).
fs_ops.o: src/fs_ops.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -Isrc -c $< -o $@

main.o: src/main.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -Isrc -c $< -o $@

logfs_mount: main.o fs_ops.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(FUSE_LIBS) -lpthread

logfs_mkfs: mkfs.o $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_journal: tests/test_journal.c crc32.o blockdev.o journal.o
	$(CC) $(CFLAGS) -Isrc -o $@ $^

test: test_journal
	./test_journal

clean:
	rm -f *.o logfs_mount logfs_mkfs test_journal
