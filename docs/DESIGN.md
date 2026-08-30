# logfs — Technical Design

This is a pre-implementation technical design: architecture, on-disk format,
algorithms, and invariants for a journaling filesystem. It describes how the
system is meant to work and why each mechanism is shaped the way it is. It is
not a report of a built or tested system — nothing here has been run, and
nothing in this document should be read as a measured result. Numbers that
appear are design targets/back-of-envelope reasoning, labeled as such where
they appear.

Novel angle: the design treats the ext4-style data-mode choice
(`ordered` / `writeback` / `journal`) as a first-class, mount-time-selectable
axis, not an afterthought — §9 is written to make that comparison structurally
possible rather than bolted on.

---

## 1. Goals and non-goals

**Goal.** Group filesystem metadata mutations (and, depending on mode, data)
into transactions. A transaction either fully survives an arbitrary crash
(power loss, panic) or leaves no trace — no on-disk state is ever a partial
transaction.

**Non-goals.**
- Not a proof of correctness. This is a testable design, not a verified one
  (contrast: FSCQ, which proves crash safety in Coq). The design includes a
  validation *methodology* (§11) but that is a plan, not a result.
- Not a new on-disk data-structure family. Layout is a conventional inode +
  bitmap + fixed journal region, deliberately unoriginal, so that the
  interesting design surface is the transaction protocol, not the allocator.
- Not full data-journaling by default. `journal` mode exists as one of three
  selectable modes (§9), not the only mode.

## 2. Terminology

| Term | Meaning here |
|---|---|
| **Transaction** | A set of block mutations that must be atomic w.r.t. crash. |
| **Handle** | A live reference to the currently-open transaction, held by one in-progress filesystem operation (mirrors jbd2's `handle_t`). |
| **Commit** | The moment a transaction becomes durable — defined as the point the commit block's write is acknowledged by the device. |
| **Checkpoint** | Copying committed log data to its home location and reclaiming the log space it occupied. Distinct from commit — see §6. |
| **Revoke** | A record that a logged block must *not* be replayed on recovery, because it was freed and reused after being logged. |

## 3. Architecture overview

```
                     ┌─────────────────────────────┐
 syscalls  ────────▶ │   VFS-facing operations      │
 (mkdir,             │   super_operations            │
  write, fsync, ...) │   inode_operations            │
                     │   address_space_operations    │
                     └───────────┬───────────────────┘
                                 │ handle_get() / handle_put()
                                 ▼
                     ┌─────────────────────────────┐
                     │   Transaction manager         │
                     │   - running transaction        │
                     │   - committing transaction     │
                     │   - handle refcount / barrier   │
                     └───────────┬───────────────────┘
                                 │ commit()
                                 ▼
                     ┌─────────────────────────────┐
                     │   Journal I/O layer           │
                     │   - descriptor/commit blocks   │
                     │   - PREFLUSH/FUA ordering       │
                     └───────────┬───────────────────┘
                                 │ checkpoint()
                                 ▼
                     ┌─────────────────────────────┐
                     │   Block layer (home locations) │
                     └─────────────────────────────┘
```

Two transactions are ever live at once, matching jbd2's model: the
**running** transaction (accepting new handles, being written to the page
cache) and the **committing** transaction (being written to the journal on
disk). A third state, **checkpointing**, describes committed-but-not-yet-
installed data sitting in the log waiting to be copied to its home location.
This three-state pipeline — running / committing / checkpointing — is the
core design decision that makes commit throughput independent of checkpoint
throughput: a new transaction can start accepting handles the instant the
previous one closes for commit, without waiting for that data to reach its
home location.

## 4. On-disk layout

```
block 0        : boot block (unused, reserved)
block 1        : superblock
block 2        : journal superblock
blocks 3..J    : journal region (circular, fixed size, set at mkfs time)
blocks J+1..   : inode bitmap, block bitmap, inode table, data blocks
                 (conventional, unremarkable — not the design's focus)
```

**Superblock** fields relevant to journaling: `data_mode` (ordered /
writeback / journal, the mount-time-overridable default), `journal_inode`
or fixed journal region bounds, `s_last_orphan` (head of the in-progress
truncate/unlink orphan list — needed so an unlink that crashes mid-truncate
is finished on next mount rather than leaking space).

**Journal superblock**: `s_blocksize`, `s_maxlen` (log region size in
blocks), `s_first` (first log block), `s_sequence` (sequence number of the
oldest transaction recovery must consider — everything older is already
checkpointed and can be ignored), `s_start` (0 if the log is empty).

## 5. Journal record format

```
┌─────────────────┐
│ descriptor block │  seq, tag[] = {target_block, flags}, checksum
├─────────────────┤
│ logged block 0    │  raw copy of a dirtied block
├─────────────────┤
│ logged block 1    │
│      ...          │
├─────────────────┤
│ commit block       │  seq, checksum-of-everything-since-last-commit
└─────────────────┘
```

A **revoke block** may appear in place of a run of logged blocks: it lists
target-block numbers that must be skipped during replay even though an
earlier, still-in-the-log transaction logged them. This is required once
checkpointing is decoupled from commit (§3): block X can be logged by
transaction 5, freed and reallocated to something else by transaction 7, and
if transaction 5's copy of X is still in the log at crash time, replaying it
over transaction 7's legitimate contents would be a correctness bug, not a
recovery. The revoke list is what prevents that.

**Checksum design decision:** one checksum per commit block covering the
whole transaction (descriptor + data blocks + commit metadata), rather than
per-block checksums. Rejected alternative: per-block checksums, which detect
a *smaller* class of torn writes (a single corrupted block) but cost more
journal space and more computation per commit for a property the
whole-transaction checksum already implies — if any block in the transaction
is torn or missing, the whole-transaction checksum fails and the transaction
is correctly treated as never-committed. Per-block checksums would only earn
their cost if partial-transaction replay were ever attempted, which this
design deliberately never does (§6).

## 6. Transaction lifecycle

Mirrors the jbd2 handle protocol, in miniature:

```
handle = txn_start(nblocks_estimate)     // join running txn, or block until
                                          // one exists; reserves log space
buf = txn_get_write_access(handle, bh)   // pins buffer, takes a pre-image
                                          // copy if this is data-journal mode
                                          // and the block was already dirty
                                          // outside this handle (escrow, see below)
... caller modifies buf's contents ...
txn_dirty_metadata(handle, bh)           // marks buf as part of this handle's
                                          // commit set
txn_stop(handle)                         // drops the handle; if this was the
                                          // last live handle and a commit was
                                          // requested, wakes the commit thread
```

**Commit** (runs on a dedicated kernel thread, not inline in any syscall
path, so no single fsync() blocks other writers longer than necessary):
1. Freeze the running transaction — no new handles join it; wait for all
   outstanding handles in it to call `txn_stop`.
2. Write descriptor block(s) + copies of all dirtied buffers to the log
   region (buffered, not yet forced).
3. `REQ_PREFLUSH` — drain the device write cache, forcing everything from
   step 2 to stable storage.
4. Write the commit block with `REQ_FUA`. This is the atomic commit point:
   before this write is acknowledged, the transaction does not exist as far
   as recovery is concerned; after, it is guaranteed replayable regardless
   of what happens next.
5. The transaction moves to checkpointing state; a new running transaction
   opens immediately (this is why step 1's freeze does not stall new
   writers past the freeze instant itself).

**Checkpoint** (may run lazily, batched, or forced when the log fills):
1. For each block recorded in a committed-but-not-checkpointed transaction,
   copy it from the log to its home location.
2. Once every block in the oldest committed transaction is checkpointed,
   advance the journal superblock's `s_sequence`/`s_start` past it — this is
   what actually reclaims log space, and it too must be crash-safe (a
   superblock update torn mid-write must not make recovery think reclaimed
   space still holds valid log data — guarded by the same
   whole-block-checksum-implies-atomic-write assumption commit blocks rely
   on, since a single block write is the smallest atomic unit any block
   device offers).

**Design decision — write-ahead, not write-behind:** data/metadata is copied
into the log *before* being applied at its home location (write-ahead
logging), rather than applying at home first and logging an undo record
(write-behind / undo logging). Rejected alternative: undo logging — it
avoids the double write (log copy + home copy) common to WAL, but requires
the undo record itself to be durable *before* the home-location write
happens, which is the same ordering constraint as WAL just with the roles
reversed, and additionally requires undo records to remain valid until the
transaction is known-committed everywhere it might be read, which is more
delicate under concurrent readers than jbd2's model. WAL was chosen because
it matches the reference implementation this design is built against
(jbd2) and keeps the "committed = present in a specific place on disk"
invariant simple to state and check.

## 7. Concurrency model

- **Handle batching**: multiple concurrent syscalls (e.g. several
  `write()`s landing close together) join the *same* running transaction if
  it hasn't started committing yet — this is what makes journaling
  throughput-competitive rather than serializing every operation through its
  own commit. The batching window is bounded by a commit interval (default
  design target: 5s, matching ext4's `commit=` default reasoning — bound
  the amount of *acknowledged-but-uncommitted* work, not a throughput
  target).
- **Lock ordering**: per-inode lock → transaction handle → buffer lock, fixed
  and documented once, because a journaling filesystem is exactly the kind
  of code where an inconsistent lock order turns into a deadlock that only
  reproduces under a WAL-flush storm. No path is permitted to take a buffer
  lock and then block waiting for a new handle — new-handle acquisition must
  happen before any buffer lock in that operation is taken.
- **Escrow / pre-image buffers** (data=journal mode specifically): if a
  block already has data queued for the *committing* transaction and a new
  handle in the *running* transaction wants to modify it again before the
  commit finishes, the new write's data must not overwrite the copy the
  committing transaction is about to log. Handled by giving the committing
  transaction's copy to the I/O layer as a distinct buffer (copy-on-write at
  the buffer level) rather than sharing the live page — this is the
  mechanism jbd2 calls "shadow buffers."

## 8. Recovery algorithm

Runs once, at mount time, before the filesystem is presented to any caller:

```
1. Read journal superblock. If s_start == 0, log is empty — nothing to do.
2. PASS 1 (scan): walk the log from s_start forward, sequence number
   increasing, until a descriptor block's checksum fails or a gap in the
   sequence is found (that's the end of valid, committed log data — anything
   physically present in the log region past that point is either
   uncommitted (crash happened mid-write) or stale from a wrapped-around
   older use of the same physical blocks, and must not be trusted).
   Build the revoke set from any revoke blocks encountered in this pass.
3. PASS 2 (replay): walk the same range again; for each logged block whose
   target is in the revoke set with a revoke sequence number >= this
   transaction's sequence number, skip it. Otherwise copy the logged block
   to its home location.
4. Update the journal superblock to reflect an empty log (s_start = 0) and
   sync it before returning control to the mount path.
```

**Why two passes, not one:** a revoke record for block X can appear in a
*later* transaction than the one that logged X's now-stale contents (that's
exactly the scenario revoke exists for — X was freed and reused after being
logged). A single forward pass that replays as it goes would apply the
stale copy before ever seeing the revoke that invalidates it. Two passes
means every revoke in the recovered range is known before any replay
happens, at the cost of reading the log twice — an accepted tradeoff since
the log is small and recovery happens once at mount, not per-operation.

**Idempotency requirement:** replaying pass 2 must be safe to interrupt and
restart from scratch (i.e., crashing *during recovery* and recovering again
must converge to the same state). This falls out for free as long as replay
is expressed purely as "copy log block to home location" with no
home-location reads or conditional logic — a block copy is idempotent by
construction; the design deliberately avoids any recovery step that isn't.

## 9. Data-mode semantics (the novel-angle axis)

All three modes share the same journal and transaction machinery (§6-§8);
they differ only in **what gets a handle** and **what ordering constraint
applies to file data pages**:

| Mode | Metadata | File data | Ordering constraint |
|---|---|---|---|
| `writeback` | Journaled (has a handle) | Not journaled | None — data writeback and metadata commit are independent. Fastest; after a crash, a file's metadata can be fully consistent while its content is stale or contains blocks from a previous file (no data-content guarantee at all). |
| `ordered` (default) | Journaled | Not journaled | Data pages belonging to blocks a transaction is about to commit must reach the block device *before* that transaction's commit block is written. Enforced by walking the transaction's inode list at commit time and issuing (and waiting on) writeback for their dirty data pages before proceeding to §6 step 3. Guarantees metadata never points at garbage, without paying data's journal-space cost. |
| `journal` | Journaled | Journaled (data blocks get a handle too, via `txn_get_write_access` same as metadata) | Data is fully transactional. Slowest — ordinary writes now take the double-write cost (log copy + home copy) that metadata always paid. Strongest guarantee: after a crash, every file's content is either fully pre-write or fully post-write, never a mix. |

**Why this is a mount option and not three separate implementations:** the
transaction manager, journal format, and recovery algorithm (§3-§8) are
mode-agnostic by construction — mode only changes (a) whether
`address_space_operations->writepage` routes a data buffer through
`txn_get_write_access` or straight to the block layer, and (b) whether
commit waits on data writeback first. Keeping this a single parameterized
code path instead of three forked implementations is what makes the
mode-comparison meaningful: differences observed between modes are
attributable to the ordering/journaling choice itself, not to divergent
implementations having accumulated different bugs.

## 10. Failure and abort handling

- **Journal abort**: if a commit's I/O returns an error (not a crash — a
  live I/O error), the transaction manager marks the journal aborted,
  refuses all new handles, and the filesystem remounts read-only. This is
  deliberately conservative — a WAL that continues accepting writes after a
  provable I/O failure can no longer promise the atomicity invariant this
  whole design exists to provide, so the correct move is to stop making
  promises, not to guess.
- **Checksum mismatch during recovery** (§8 pass 1): treated identically to
  "end of valid log," not as a fatal mount error — this is precisely what
  makes an interrupted-mid-commit crash (the case this entire design exists
  to handle) recoverable rather than fatal. A checksum failure at the very
  start of the log region (before any valid transaction) is the one case
  that is fatal, since it means there is no valid recovery starting point.
- **Orphan inode list** (superblock `s_last_orphan`, §4): an unlink of a
  still-open file, or a truncate, is itself multi-step at the block-free
  level (updating the inode, freeing blocks, updating bitmaps) and is
  wrapped in the same transaction protocol — but if the *system* crashes
  between "inode unlinked" and "all its blocks freed," recovery must finish
  the truncate rather than leaking the blocks forever. The orphan list
  exists so recovery has a starting point for that cleanup independent of
  the journal replay.

## 11. Validation methodology (plan, not a result)

If this design were implemented, the properties above would need to be
checked against, specifically:
- **Recovery correctness under injected reordering**: enumerate crash points
  relative to the PREFLUSH/FUA boundary in §6 step 3-4 and assert recovery
  converges to a legal state at every one — this is the class of check
  tools like CrashMonkey/ACE (OSDI '18, black-box record/replay + bounded
  exhaustive workload generation) and dm-log-writes (block-layer write log
  + replay-to-arbitrary-point) are built for.
- **Mode-comparison measurement** (§9): the same workload run under all
  three modes, throughput/latency compared, *and* paired with a
  crash-point pass-rate check per mode — throughput alone doesn't say
  anything about whether the speed difference is buying back a real
  durability difference, and pairing the two is what makes the comparison
  meaningful. `writeback` should show the highest throughput and the lowest
  crash-point pass rate under a data-corruption check specifically (not a
  mount-succeeds check); `journal` the inverse.
- **xfstests regression** against the implemented VFS surface.

No numbers from any of the above appear in this document because none of it
has been run. Any future results belong in `docs/RESULTS.md`, generated from
committed scripts in `bench/`, never hand-written.

## 12. Alternatives considered at the whole-design level

- **Soft updates** (McKusick, FreeBSD) instead of journaling: orders
  in-place writes via per-write-type dependency tracking instead of
  write-ahead logging, avoiding the double-write WAL pays. Rejected because
  the correctness argument is distributed across every operation type's
  ordering rules rather than centralized in one commit protocol — harder to
  state a single invariant and check it, which is the actual point of this
  project.
- **Log-structured filesystem** (never overwrite in place; the log *is* the
  filesystem) instead of journaling-as-an-add-on: sidesteps the "write
  twice" cost entirely, but trades it for a much harder cleaner/garbage-
  collection design problem and a different failure-mode surface. Rejected
  as out of scope — it's a different project (and a different kind of
  hard part) from crash-consistency-via-journaling.
- **Copy-on-write with checksummed shadow trees** (btrfs/ZFS style) instead
  of journaling: also sidesteps double-writes for metadata via atomic
  superblock-pointer swaps over an immutable tree. Rejected for the same
  reason as log-structured — legitimate alternative, but its hard part is
  tree/allocator design, not the write-ordering-and-barrier reasoning this
  project is specifically about.
