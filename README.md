# logfs

A journaling filesystem, implemented as a FUSE filesystem in C: metadata
(and, depending on mount mode, data) mutations are grouped into
transactions that either fully survive an arbitrary crash or leave no
trace at all — the same crash-consistency contract ext4's jbd2 layer
provides, reimplemented from first principles at a scope small enough to
reason about end to end.

## Features

- **Write-Ahead Journaling**: jbd2-style handle protocol with commit
  decoupled from checkpoint (running → committing → checkpointing as
  separate pipeline stages, not one synchronous step)
- **Selectable Data-Consistency Modes**: `ordered` (default), `writeback`,
  `journal` — chosen at mount time, implemented as one parameterized code
  path so a measured difference between them is attributable to the
  ordering/journaling choice itself, not divergent implementations
- **Crash-Safe Recovery**: two-pass scan-then-replay recovery, with
  revoke-record support for blocks freed and reused after being logged
- **Whole-Transaction Checksums**: CRC-32 over the descriptor, revoke, and
  data blocks of each transaction, verified at recovery time
- **Concurrent Transaction Batching**: background commit thread with
  handle batching, a checkpoint-lag window, and an independent async
  writeback path for `writeback` mode's weaker durability guarantee
- **POSIX Filesystem Surface**: mkdir/create/read/write/unlink/rmdir/
  truncate/fsync over a conventional inode + bitmap + flat-directory layout
- **Toolchain-Realistic**: targets Linux 6.6+ and libfuse3, with an
  explicit dev-environment plan for building it from a non-Linux host

## Architecture

```
FUSE syscalls (mkdir, write, fsync, ...)
        ↓
FUSE operation callbacks (src/fs_ops.c)
        ↓
Inode table / allocator / directories (src/inode.c)
        ↓
Transaction manager: handles, running/committing/
checkpointing pipeline, mode-dependent data path (src/txn.c)
        ↓
Journal I/O: commit sequencing, two-pass recovery (src/journal.c)
        ↓
Block device: backing-file I/O, FLUSH/FUA stand-ins (src/blockdev.c)
```

Full design rationale — on-disk format, the write-ordering/PREFLUSH/FUA
reasoning behind why any of this is hard, and the alternatives considered
at each layer — is in [`docs/DESIGN.md`](docs/DESIGN.md).

## Installation

```bash
git clone https://github.com/fromjyce/logfs.git
cd logfs
make
```

Requires Linux 6.6+ and `libfuse3-dev` (`pkg-config fuse3` must resolve).
See `docs/DESIGN.md` §4 for the dev-VM path if building from a non-Linux
host.

## Usage

### Format an image

```bash
./logfs_mkfs --mode=ordered image.bin 16384      # 64MiB image, ordered mode
./logfs_mkfs --journal-blocks=2048 image.bin 32768
```

### Mount

```bash
./logfs_mount image.bin /mnt/logfs               # uses the image's default mode
./logfs_mount --mode=journal image.bin /mnt/logfs -f   # override mode, foreground
```

### Run the journal test

```bash
make test
```

## Supported Filesystem Operations

- `getattr`, `readdir`, `mkdir`, `rmdir`
- `create`, `open`, `read`, `write`, `truncate`, `unlink`
- `fsync` (forces and blocks on a journal commit)

## Tech Stack

- **Language**: C11
- **Filesystem interface**: FUSE3 (`libfuse3`)
- **Concurrency**: POSIX threads (background commit + writeback threads,
  mutex/condvar-coordinated handle batching)
- **Journal integrity**: self-contained CRC-32, no external dependency
- **Storage**: plain backing-file block I/O — no external database

## Contact

If you come across any issues, have suggestions for improvement, or want
to discuss further enhancements, feel free to contact me at
[jaya2004kra@gmail.com](mailto:jaya2004kra@gmail.com). Your feedback is
greatly appreciated.

## License

All the code and resources in this repository are licensed under the GNU
General Public License. You are free to use, modify, and distribute the
code under the terms of this license. However, I do not take
responsibility for the accuracy or reliability of the programs.
