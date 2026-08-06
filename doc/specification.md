# zeroskip: clean-room design specification

Status: draft for review
Date: 2026-08-05

This is a from-scratch specification for `libzeroskip`, an append-only
ordered key-value store. It is written from the format notes in
`doc/zeroskip.txt` and design discussion, and deliberately does not
inherit anything from the previous implementation in `src/`.

Requirements are labelled (`F-n` format, `D-n` database, `C-n`
concurrency, `R-n` recovery, `A-n` API) so the conformance suite can cite
them. **MUST** and **MUST NOT** are normative.

## 1. Purpose and scope

`libzeroskip` stores ordered key-value pairs in a directory of
append-only files. It is C11 over POSIX only (`mmap`, `fcntl`, no
external libraries), and builds on Linux, macOS and the BSDs.

The design centres on one invariant: **nothing is ever written except by
appending to a file, or by creating a new file.** The single exception is
the manifest, which is replaced atomically by `rename`.

Its sibling library `twom` is a mutable single-file skiplist. zeroskip
suits workloads that are append-heavy, want lock-free readers, and
tolerate compaction happening out of band.

## 2. Terminology

| Term | Meaning |
|---|---|
| file number | 64-bit identifier assigned to each data file, starting at 1 |
| active file | the one file the writer is currently appending to |
| in-order file | a file whose records are in key order and which has a `[Pointers]` block |
| unsorted file | a file without a `[Pointers]` block; records in append order |
| sealed file | an unsorted file that will never be appended to again |
| span | data records terminated by one commit or rollback record |
| terminator | a commit, final-commit, or rollback record |
| `append_end` | offset just past the last valid terminator in a file |
| ancestor | file number in which the record a given record supersedes was created |
| shadowed | a record superseded by a later record for the same key *in the same file* |

## 3. Guarantees

- **G-1 Append-only.** No committed byte is ever mutated. No data file is
  ever renamed or truncated.
- **G-2 Commit atomicity.** Once `zs_txn_commit` returns `ZS_OK`, the whole
  transaction is visible to new readers and, in the default sync mode,
  survives a crash. A crash exposes exactly a prefix of committed
  transactions — never a partial one.
- **G-3 Always reopens.** Any state a crash can produce MUST open
  successfully in bounded time and expose the committed prefix. Corruption
  may cost uncommitted data; it MUST NOT cost committed data, hang, crash,
  or read out of bounds.
- **G-4 Snapshot isolation, lock-free reads.** A read transaction sees a
  fixed snapshot and takes no lock. Readers never block a writer; a writer
  never blocks readers.
- **G-5 One writer.** At most one writer per database, enforced by an
  `fcntl` lock. Because the kernel releases `fcntl` locks on process death,
  a killed writer never blocks the next one. No lock state can outlive a
  process.
- **G-6 No process-lifetime state.** Correctness never depends on the
  shared-memory index, nor on any file other than the data files and the
  manifest. Deleting or corrupting the index affects performance only.
- **G-7 Read paths agree.** Point lookups and range scans resolve
  visibility through one shared rule (§6.5), so they cannot disagree about
  whether a key exists.

## 4. On-disk format

### 4.1 Conventions

- **F-1** Integers are little-endian.
- **F-2** Every record begins at an offset that is a multiple of 8 and
  occupies a whole multiple of 8 bytes. Padding bytes MUST be zero.
- **F-3** Offsets and pointers are absolute byte offsets from the start of
  the file.
- **F-4** The checksum is XXH3-64 truncated to its low 32 bits. The
  checksum field is always the **last 4 bytes** of the structure it
  protects and covers every byte from the start of the protected region up
  to (not including) the field itself. There is no field-zeroing anywhere
  in this format.
- **F-5** An implementation MUST select its checksum implementation at
  runtime, with a portable C fallback. Hardware acceleration MUST NOT be a
  build-time-only path (a library that fails to link or run on a platform
  lacking a given CPU feature does not conform).

### 4.2 File header (48 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic, ASCII `zeroskip`, no NUL |
| 8 | 4 | version, `1` |
| 12 | 16 | database UUID, binary RFC 4122 — identical in every file of a DB |
| 28 | 8 | start file number |
| 36 | 8 | end file number, or `0` |
| 44 | 4 | checksum of bytes `[0, 44)` |

- **F-6** File numbers start at 1, so `end == 0` is never legitimate and
  means "records are not in key order; this file has no `[Pointers]`
  block".
- **F-7** A file created fresh has `start == its own number` and `end == 0`.
  A file produced by merging inputs spanning numbers *i*..*j* has
  `start == i`, `end == j`.
- **F-8** The `[start, end]` ranges of the files listed in one manifest
  MUST NOT overlap. Ordering files by `start` descending is therefore total
  and ranks them newest to oldest.
- **F-9** A reader MUST reject a file whose magic, version or header
  checksum fails to validate, or whose UUID does not match the database it
  is being opened as part of.

### 4.3 Record types

The type byte is an enumeration, not a bitfield.

| Value | Type | Header size |
|---|---|---|
| `0x01` | `KEYVALUE` | 4 |
| `0x02` | `KEYVALUE_ANC` | 16 |
| `0x03` | `BIGKEYVALUE` | 24 |
| `0x04` | `BIGKEYVALUE_ANC` | 32 |
| `0x05` | `DELETION` | 16 |
| `0x06` | `BIGDELETION` | 24 |
| `0x07` | `COMMIT` | 8 |
| `0x08` | `COMMIT_LONG` | 24 |
| `0x09` | `COMMIT_LONG_2ND` | (tail of `0x08`) |
| `0x0A` | `FINAL` | 8 |
| `0x0B` | `FINAL_LONG` | 24 |
| `0x0C` | `FINAL_LONG_2ND` | (tail of `0x0B`) |
| `0x0D` | `ROLLBACK` | 8 |
| `0x0E` | `ROLLBACK_LONG` | 24 |
| `0x0F` | `ROLLBACK_LONG_2ND` | (tail of `0x0E`) |

- **F-10** Any other type byte, including `0x00`, is invalid.

### 4.4 Data records

Key and value are stored contiguously, separated by a NUL, with a further
NUL after the value, then zero padding to the next multiple of 8. Both key
and value are therefore usable in place as C strings. **F-11** Lengths are
authoritative; keys and values MAY contain NUL bytes, and the stored
lengths MUST NOT include the terminators.

```
KEYVALUE (0x01)                       -- a create; ancestor is this file
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   .  key NUL value NUL pad->8
  len = roundup8(4 + keylen + 1 + vallen + 1)

KEYVALUE_ANC (0x02)                   -- an update
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   4  pad
  +8   8  ancestor file number
  +16  .  key NUL value NUL pad->8
  len = roundup8(16 + keylen + 1 + vallen + 1)

BIGKEYVALUE (0x03) / BIGKEYVALUE_ANC (0x04)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  8  vallen
  [+24 8  ancestor file number]       -- 0x04 only
  +24/32  key NUL value NUL pad->8

DELETION (0x05)                       -- always supersedes something
  +0   1  type
  +1   1  keylen
  +2   6  pad
  +8   8  ancestor file number
  +16  .  key NUL pad->8

BIGDELETION (0x06)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  8  ancestor file number
  +24  .  key NUL pad->8
```

- **F-12** All 8-byte fields are 8-byte aligned within the file.
- **F-13** Encoding is canonical: an implementation MUST use the short form
  whenever `keylen <= 255` and `vallen <= 65535`, MUST omit the ancestor
  field exactly when the record is a create, and MUST use the short
  terminator whenever the span is `<= 0xFFFFFF` bytes. Byte-for-byte output
  is therefore determined by the logical contents.
- **F-14** A key MUST be at least 1 byte. An empty value is legal and is
  distinct from an absent key.
- **F-15** A create's ancestor is implicitly its own file number. An update
  or deletion's ancestor MUST be the file number in which the record it
  supersedes was created. This makes the per-key version chain unbroken and
  followable across files.

### 4.5 Terminators

```
short (8 bytes)                       COMMIT / FINAL / ROLLBACK
  +0   1  type
  +1   3  span length
  +4   4  checksum

long (24 bytes)                       *_LONG
  +0   1  type
  +1   7  pad
  +8   8  span length
  +16  1  type2  (the matching *_LONG_2ND value)
  +17  3  pad
  +20  4  checksum
```

- **F-16** The checksum covers the span's data bytes followed by the
  terminator's own bytes up to the checksum field.
- **F-17** `type2` exists so that the last 8 bytes of a file identify
  whether the trailing terminator is short or the tail of a long one,
  making the final terminator locatable by reading backwards.
- **F-18** A `COMMIT` makes its span's records live. A `ROLLBACK` voids
  them: a rollback is a commit that says "ignore the records in this span".
- **F-19** Because the checksum covers the span *and* the terminator, a
  terminator that reaches disk without its data fails validation and reads
  as absent. One `fsync` after the terminator is therefore sufficient; no
  intra-transaction write barrier is required.

### 4.6 The span chain

- **F-20** From offset 48 to `append_end`, a file is a flat sequence of
  spans. Each span is zero or more data records followed by exactly one
  terminator whose span length equals the span's data byte count and whose
  checksum validates. Every byte in `[48, append_end)` belongs to exactly
  one span or terminator: **no gaps and no nesting.**
- **F-21** Visibility is per span, not a watermark: a voided span may sit
  between two live ones, so a reader MUST replay spans in order and skip
  rolled-back ones.

### 4.7 Pointers

Present only in in-order files (`end != 0`), written once, immediately
before a `FINAL` terminator that covers the block.

| Off | Size | Field |
|---|---|---|
| 0 | 8 | `NumPointers` |
| 8 | 8 | `NumShadowedRecords` |
| 16 | 8 | `NumShadowedBytes` |
| 24 | 8 × N | record offsets |

- **F-22** Pointers reference every record present in a committed span
  (records in rolled-back spans are void and MUST NOT be indexed), sorted
  by key ascending, and within equal keys by offset **descending**.
  The first entry of an equal-key range is therefore the newest version,
  and walking forward through that range walks the key's history newest to
  oldest.
- **F-23** `NumShadowedRecords` and `NumShadowedBytes` count records
  superseded by a later record for the same key **within the same file**,
  and the bytes those records occupy. See §11 open items.
- **F-24** Every pointer MUST be 8-aligned and within
  `[48, pointers_offset)`.

### 4.8 Validation

- **F-25 Progress rule.** Iteration computes the next offset from the
  current record's own length fields and MUST verify that it is strictly
  greater than the current offset and within bounds. Otherwise the file is
  treated as ending at that point. Non-termination is thereby impossible by
  construction.
- **F-26** Every length, offset and pointer MUST be bounds-checked against
  the file size before any dereference.
- **F-27** Opening an in-order file is O(1): validate the header, locate
  the trailing terminator, validate it over the pointers block, and use the
  pointers. Individual records are bounds-checked on access, not on open.

## 5. Database layout

### 5.1 Directory contents

| Name | Mutability | Purpose |
|---|---|---|
| `zeroskip-<uuid>-<start>` | append-only, then sealed | unsorted data file (`end == 0`) |
| `zeroskip-<uuid>-<start>-<end>` | immutable | in-order data file |
| `zeroskip.manifest` | replaced atomically | the active-files file |
| `zeroskip.manifest.tmp.<pid>` | transient | ignorable, removable |
| `zeroskip.lock` | never replaced or unlinked | holds `fcntl` locks |
| `zeroskip.index` | pure cache | shared index for unsorted files |

- **D-1** File numbers in names are zero-padded to 10 digits so `ls` sorts
  numerically.
- **D-2** `zeroskip-*` matches data files only; `zeroskip.*` matches
  metadata. The shared literal prefix makes the set prefix-globbable and
  shell-completable.
- **D-3** `zeroskip.lock` MUST be a distinct file that is never replaced.
  `fcntl` locks attach to an inode, so locking the manifest — whose inode
  changes on every publish — would silently lose mutual exclusion.
- **D-4** A data file not referenced by the manifest MUST be ignored by
  readers. A packer MAY remove it (it is the debris of an interrupted
  merge).

### 5.2 Manifest

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic, ASCII `zsmanife` |
| 8 | 4 | version, `1` |
| 12 | 16 | database UUID |
| 28 | 8 | generation, incremented on every publish |
| 36 | 8 | active file number |
| 44 | 4 | file count |
| 48 | 4 | comparator name length |
| 52 | . | comparator name, then pad to 8 |
| . | 24 × N | `(start, end, end_offset)` per file |
| . | 4 | checksum over all preceding bytes |

- **D-5** The manifest is written to `zeroskip.manifest.tmp.<pid>` and
  `rename`d into place, so a crash yields either the old manifest or the
  new one, never a partial one.
- **D-6** `end_offset` is exact for every file except the active one: for a
  closed or sealed unsorted file it is the `append_end` recorded when the
  file stopped being written, and for an in-order file it is the file size.
  For the **active** file it is a **floor, not a truth** — its `append_end`
  as of the last publish — and a reader scans forward from it to find the
  real end. The floor only ever advances past data that has been committed,
  so a stale floor costs a short forward scan and can never cause committed
  data to be discarded.
- **D-7** The manifest is therefore rewritten only on *structural* changes
  — active-file rollover, sealing, and merge completion. A commit is append
  plus `fsync`: no rename, no directory sync.
- **D-8** Database-wide configuration lives in the manifest, not the file
  header. In v1 that is the comparator identity, which MUST be recorded
  because it determines key order and hence the meaning of `[Pointers]`.
  Opening a database with a different comparator than the one recorded is
  an error.

### 5.3 File lifecycle

```
create ──> active (unsorted, end=0) ──> closed (unsorted, end=0) ──┐
                     │                                            │
                     └── damaged ──> sealed (unsorted, read-only) ─┤
                                                                   v
                              in-order file (end=N) <── merge ─────┘
```

- **D-9** When the active file exceeds `rollover_size` (default 2 MB), the
  writer creates a new active file and publishes. The closed file remains
  unsorted; it is not rewritten at rollover, so rollover is cheap and off
  the latency path.
- **D-10** Several unsorted files MAY exist at once. Repacking is a
  separate, deferrable decision.
- **D-11** Merging *k* input files produces one in-order file covering
  their combined range, with `[Pointers]` and a `FINAL` terminator.

### 5.4 Shared index

- **D-12** `zeroskip.index` is a regular file, `mmap`'d `MAP_SHARED`,
  holding one index per unsorted file keyed by file number, each stamped
  with the database UUID, the file number, and the offset it is valid up
  to.
- **D-13** It is a pure cache and MUST NOT be authoritative. Any process
  MAY discard it and rebuild by scanning. An implementation MUST validate
  its stamps before use and MUST discard it on any inconsistency.
- **D-14** Its internal layout is **not** part of this format
  specification and may change without a version bump. Only its
  invariants are specified.

### 5.5 Lookup order

- **D-15** Resolution order is: the current write transaction's own
  uncommitted records; then all data files by `start` **descending**. Within
  a single file the newest version of a key wins — the highest offset among
  committed spans, which for an in-order file is the first entry of the
  key's equal range (F-22). The first record found by this order wins; if it
  is a deletion, the key does not exist.
- **D-16** Point lookups, cursors and range scans MUST all resolve
  visibility by D-15. Range scans are a k-way merge over the same
  per-file sources, deduplicating by key with newest-wins.

### 5.6 Merging

- **D-17** For each key in the input set, keep only the newest version, and
  set its ancestor to the **oldest merged version's** ancestor, preserving
  the chain past the merge boundary.
- **D-18** A key may be removed entirely if and only if its newest version
  is a deletion **and** the chain's create lies inside the merged range
  (`ancestor >= output start`) — that is, its whole lifespan from create
  through update to delete is contained in the merge. Otherwise the
  tombstone MUST be retained, because an older unmerged file may still hold
  the key and dropping it would resurrect the value.
- **D-19** `zs_db_should_repack` reports on accumulated reclaimable space
  and file count.

## 6. Concurrency and durability

- **C-1** `fcntl` byte-range locks on `zeroskip.lock`: byte 0 writer
  (exclusive), byte 1 packer (exclusive), byte 2 publish (exclusive, held
  briefly). Writing and merging therefore proceed concurrently.
- **C-2** Readers take **no lock**.
- **C-3** A publisher MUST take the publish lock, re-read the current
  manifest, merge its change into it, then write and rename — a
  compare-and-publish.
- **C-4 Snapshot lifetime.** A reader reads the manifest, opens and `mmap`s
  every listed file, then re-reads the manifest; if the generation changed
  it retries, bounded. Once its descriptors are open, a packer MAY `unlink`
  superseded files immediately: the kernel keeps the inodes alive until the
  last descriptor closes. There is no reference table, and nothing to clean
  up when a process dies.
- **C-5** The accepted cost of C-4 is that disk space is held until the
  last reader holding an old snapshot exits.
- **C-6** Default durability `fsync`s after each commit terminator.
  `ZS_NOSYNC` omits it, trading G-2's crash-survival for throughput while
  retaining atomicity.
- **C-7** An aborted transaction appends a `ROLLBACK` and does **not**
  `fsync`. If a crash loses it, recovery reaches the same state, so there
  is one code path to get right rather than two.

## 7. Open, recovery and repair

There is no separate recovery pass; opening is recovery.

- **R-1** Open reads the manifest, opens and maps the listed files, then
  establishes each unsorted file's true `append_end` by replaying spans
  forward from that file's recorded `end_offset` (D-6), stopping at the
  first terminator that is structurally illegal or fails its checksum. Only
  the active file can have anything to replay; the shared index normally
  means nothing is scanned at all.
- **R-2** Live data is the union of records in spans with `COMMIT`
  terminators. Rolled-back spans contribute nothing.
- **R-3** A reader MUST NOT repair. Opening a damaged database read-only
  is side-effect-free.
- **R-4 Desynchronised files are sealed, not repaired.** If a file's span
  chain does not cleanly reach EOF, the writer MUST seal it: readable up to
  its last valid terminator, never appended to again. The writer creates a
  fresh active file and continues.
- **R-5** The rationale for R-4 is that trailing garbage could in principle
  checksum as a valid terminator. Appending after a suspect boundary would
  build the chain on a possibly-false foundation and compound the error.
  Sealing bounds the damage to one file, and 64-bit file numbers make files
  effectively free.
- **R-6** `ROLLBACK` records exist for explicit aborts, where the writer
  knows exactly which span it wrote. They are not a repair mechanism.
- **R-7** A crash during a merge leaves an unreferenced file (D-4). A crash
  during publish leaves either manifest intact (D-5).
- **R-8 Missing manifest.** Data files are self-describing, so if the
  manifest is absent or fails validation the database MUST be reconstructed
  by scanning the directory: adopt every well-formed data file whose UUID
  matches, reject the set if any two ranges overlap (F-8), treat the
  unsorted file with the highest `start` as the active file, derive each
  `end_offset` by replaying spans from offset 48, and publish. A read-only
  open performs this reconstruction in memory without publishing (A-5).
  This is also how a brand-new database with `ZS_CREATE` begins.

## 8. Public API

Naming and idiom follow `twom`: `zs_db_*` / `zs_txn_*` / `zs_cursor_*`,
opaque types, one 32-bit flag space, output through pointer parameters,
`enum zs_ret` with `ZS_OK = 0`, `ZS_DONE = 1`, negatives for errors
(`ZS_NOTFOUND = -5`, `ZS_LOCKED = -4`, `ZS_BADFORMAT = -7`, …).

```c
struct zs_open_data {
    uint32_t     flags;
    zs_compar   *compar;         /* NULL = byte order */
    const char  *compar_name;    /* recorded in the manifest */
    size_t       rollover_size;  /* 0 = default 2MB */
    void       (*error)(const char *msg, const char *fmt, ...);
};

int  zs_db_open(const char *dir, struct zs_open_data *setup, struct zs_db **dbp);
int  zs_db_close(struct zs_db **dbp);

int  zs_db_fetch(struct zs_db *, const char *key, size_t keylen,
                 const char **keyp, size_t *keylenp,
                 const char **valp, size_t *vallenp, int flags);
int  zs_db_store(struct zs_db *, const char *key, size_t keylen,
                 const char *val, size_t vallen, int flags);
int  zs_db_foreach(struct zs_db *, const char *prefix, size_t prefixlen,
                   zs_cb *p, zs_cb *cb, void *rock, int flags);

int  zs_db_begin_txn(struct zs_db *, int shared, struct zs_txn **);
int  zs_txn_commit(struct zs_txn **);
int  zs_txn_abort(struct zs_txn **);
int  zs_txn_fetch/zs_txn_store/zs_txn_foreach(...);

int  zs_db_begin_cursor(struct zs_db *, const char *key, size_t keylen,
                        struct zs_cursor **, int flags);
int  zs_txn_begin_cursor(struct zs_txn *, ...);
int  zs_cursor_next(struct zs_cursor *, const char **keyp, size_t *keylenp,
                    const char **valp, size_t *vallenp);
int  zs_cursor_replace(struct zs_cursor *, const char *val, size_t vallen, int flags);
int  zs_cursor_commit(struct zs_cursor **);
int  zs_cursor_abort(struct zs_cursor **);

int  zs_db_repack(struct zs_db *);
bool zs_db_should_repack(struct zs_db *);
int  zs_db_check_consistency(struct zs_db *);
int  zs_db_dump(struct zs_db *, int detail);
int  zs_db_sync(struct zs_db *);
const char *zs_strerror(int r);
```

Flags occupy one space and are not reused across operations: `ZS_CREATE`,
`ZS_SHARED`, `ZS_NOCSUM`, `ZS_NOSYNC`, `ZS_NONBLOCKING`, `ZS_IFNOTEXIST`,
`ZS_IFEXIST`, `ZS_FETCHNEXT`, `ZS_SKIPROOT`, `ZS_CURSOR_PREFIX`,
`ZS_COMPAR_EXTERNAL`.

- **A-1** `store` with `val == NULL` writes a deletion. `store` with a
  non-NULL zero-length value stores an empty value. These are distinct
  states.
- **A-2** There is no `yield` call and no yield flags. `twom` needs them
  because its readers hold a shared lock that would block a writer; here
  readers hold no lock, so there is nothing to yield.
- **A-3** There is no MVCC flag. Snapshot isolation is the only read mode,
  because a snapshot is just a set of immutable files plus a per-file valid
  extent.
- **A-4** Returned key and value pointers remain valid for the lifetime of
  the transaction or cursor that produced them. For the non-transactional
  `zs_db_*` calls, which use an implicit single-operation transaction, they
  remain valid until the next call on that `struct zs_db`.
- **A-5** An open with `ZS_SHARED` is read-only and MUST NOT write: no
  repair, no sealing, no manifest publish, and no creation or update of the
  shared index (see R-3).

## 9. Conformance suite

`zstest`: one binary, substring filter, fresh temp directory per test,
`ASSERT_*` macros, in the style of `twomtest`. Layout stays flat:
`zeroskip.h`, `zeroskip.c`, `xxhash.h`, `zstest.c`, `zstool.c`, `Makefile`,
plus `doc/` and `tests/corpus/`.

**T-1 Golden vectors and portable corpus.** Byte-exact encode assertions
against checked-in `.zs` files, deterministic because F-13 makes encoding
canonical, with `zstool` accepting an explicit UUID so corpus generation is
reproducible without a test-only hook in the library. Decode assertions
driven by a corpus manifest. `doc/conformance.md` plus this corpus is what
an independent implementation validates against.

**T-2 Malformed input.** Every golden file truncated at *every byte offset*
— not merely record boundaries — and systematically bit-flipped. Each case
asserts an error or the committed prefix, and never a crash, hang, or
out-of-bounds read. Run under ASan and UBSan with a per-case wall-clock
timeout; the timeout is the detector for F-25.

**T-3 Behavioural.** Ordering, prefix scans, cursor replace,
`IFEXIST`/`IFNOTEXIST`/`FETCHNEXT`/`SKIPROOT`, empty-value versus absent
key (A-1), and the boundaries the format creates: 255↔256-byte keys,
65535↔65536-byte values, a 16 MB span forcing a long terminator, a record
whose length lands exactly on 8-byte alignment, and keys containing
embedded NUL bytes (F-11).

**T-4 Model-based.** Randomised operation sequences against an in-memory
reference model, checked after every step by both a point lookup and a full
scan, and cross-checked against each other — the direct test for G-7.

**T-5 Ancestor chains and merging.** For every arrangement of create,
update and delete spread across file boundaries: that chains stay unbroken
(F-15), that D-17 rewrites ancestors correctly, that D-18 drops a key only
when its whole lifespan is inside the merge, and that a key deleted in a
newer file is never resurrected by merging older ones.

**T-6 Crash injection.** A test build interposes `write`, `fsync` and
`rename`, counts calls, and aborts at call *N* for every *N* across a
scripted workload. Each case asserts: reopen terminates within a timeout,
exactly a prefix of committed transactions is visible, nothing acknowledged
is lost under default durability, and after a writer opens, it can continue
writing. Specifically targeted: crash between records and terminator; after
terminator before `fsync`; mid-publish rename; mid-merge; after
`[Pointers]` but before `FINAL`; leaving a non-8-aligned file size; and
after an invalid terminator, asserting the file is sealed per R-4 rather
than appended to. Both durability modes. Separately, the manifest deleted,
truncated and bit-flipped, asserting reconstruction by directory scan (R-8)
yields the same logical contents, and that a read-only open reconstructs
without writing (A-5).

**T-7 Multi-process.** Real forked processes. A writer plus *N* readers
asserting snapshot stability across commits and that a fresh open sees
them. Two writers, exactly one proceeding, `ZS_LOCKED` under
`ZS_NONBLOCKING`. **A writer `SIGKILL`ed while holding the lock, asserting
the next writer proceeds with no manual intervention** (G-5). A reader
holding a snapshot across a merge, asserting its data stays readable while
the inputs are unlinked (C-4). The shared index deleted, truncated and
bit-flipped mid-run, asserting identical results (G-6). Concurrent merge
and writer both proceeding with publish serialised.

**T-8 Traceability.** `doc/conformance.md` carries a checklist mapping
every normative requirement in this document to the test that enforces it.
A requirement with no test is a gap to be closed, and this mapping is what
makes the suite a conformance suite rather than a test suite.

## 10. Non-goals

Multi-writer concurrency; cross-database transactions; secondary indexes;
compression; network access; in-place value mutation; Cyrus-specific types
in the public API. A Cyrus `cyrusdb` backend is a thin separate adapter,
out of scope here.

## 11. Open items

1. **`NumShadowedRecords` / `NumShadowedBytes` are unpopulated.** Per F-23
   they count within-file supersession, but `[Pointers]` appears only in
   in-order files (F-6), and merging collapses each key to its newest
   version (D-17), so both are structurally always 0. Resolve by either
   (a) dropping the fields, (b) redefining them as reclamation statistics
   about the merge inputs, or (c) permitting `[Pointers]` on unsorted files,
   which would make them meaningful and give closed unsorted files a
   durable index — at the cost of a fourth file state.
2. **Merge input selection policy** (which files, how many at a time, what
   triggers it) is unspecified; D-19 only exposes the signal.
3. **`zs_db_repair`** is omitted pending a decision on whether sealing
   (R-4) makes an explicit repair entry point unnecessary.
