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
| generation | 64-bit counter, starting at 1, incremented for each new data file. A file covers a **range** of generations `[start, end]`: one generation when freshly created, several once it has absorbed others by merging |
| active file | the one file the writer is currently appending to |
| in-order file | a file whose records are in key order and which has a `[Pointers]` block |
| unsorted file | a file without a `[Pointers]` block; records in append order |
| sealed file | an unsorted file that will never be appended to again |
| span | data records terminated by one commit or rollback record |
| terminator | a commit, final-commit, or rollback record |
| `append_end` | offset just past the last valid terminator in a file |
| ancestor | the absolute generation at which the shadow cast by a record hits the previous record for that key — either the record's own generation, or an earlier one (F-15) |
| shadowed | a record superseded by a later record for the same key, anywhere — later in the same file, or in any newer file |

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
- **F-4** The checksum field is always the **last 4 bytes** of the structure
  it protects, and covers every byte from the start of the protected region
  up to (not including) the field itself. There is no field-zeroing anywhere
  in this format.
- **F-5** Exactly three checksum engines are permitted:

  | Id | Engine | Behaviour |
  |---|---|---|
  | 0 | none | write zeros, never verify |
  | 1 | xxHash | XXH3-64 truncated to its low 32 bits (default) |
  | 2 | external | a function supplied by the caller at open time |

- **F-5a** The engine id is recorded in the **file header flags**, so every
  file is self-describing. The flags field is plain data, read before any
  checksum is verified, so there is no bootstrapping problem: a reader learns
  the engine and then validates the header with it. Files written under
  different engines may therefore coexist in one database.
- **F-5b** An implementation MUST select its checksum implementation at
  runtime, with a portable C fallback. Hardware acceleration MUST NOT be a
  build-time-only path — a library that fails to link or run on a platform
  lacking a given CPU feature does not conform.
- **F-5c** Engine 0 weakens G-2 and G-3: F-19's property, that a terminator
  reaching disk without its data fails validation, depends on a real
  checksum. With engine 0 a torn tail cannot be detected, so atomicity is no
  longer enforced by the format. Engine 0 is for testing and for callers who
  have durability guarantees elsewhere.
- **F-5d** Engine 2 makes a file readable only by a caller supplying the same
  function. The conformance corpus therefore covers engines 0 and 1 only.
- **F-5e** `ZS_NOCSUM` is distinct from engine 0: it is a runtime open flag
  that skips verification of checksums that are nonetheless present and
  written.

### 4.2 File header (56 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic, ASCII `zeroskip`, no NUL |
| 8 | 4 | version, `1` |
| 12 | 4 | flags — bits 0..3 checksum engine id (F-5), rest reserved and MUST be zero |
| 16 | 16 | database UUID, binary RFC 4122 — identical in every file of a DB |
| 32 | 8 | start generation |
| 40 | 8 | end generation, or `0` |
| 48 | 4 | padding, MUST be zero |
| 52 | 4 | checksum of bytes `[0, 52)` |

- **F-6** Generations start at 1, so `end == 0` is never legitimate and
  means "records are not in key order".
- **F-6a** Whether a file carries a `[Pointers]` block is a **separate,
  independently self-describing** property: a file has pointers if and only
  if its trailing terminator is a `FINAL` variant, since `FINAL` by
  definition marks the commit following `[Pointers]` (F-17 makes that
  terminator locatable by reading the last 8 bytes). A file may therefore
  have a durable index without being in key order.
- **F-7** A freshly created file occupies a single generation: `start` is that
  generation and `end == 0`. A file produced by merging inputs spanning
  generations *i*..*j* has `start == i` and `end == j`.
- **F-8** The `[start, end]` ranges of the files listed in one manifest
  MUST NOT overlap. Ordering files by `start` descending is therefore total
  and ranks them newest to oldest.
- **F-9** A reader MUST reject a file whose magic, version or header
  checksum fails to validate, or whose UUID does not match the database it
  is being opened as part of.

### 4.3 Record types

The type byte is an enumeration, not a bitfield.

High nibble `0` is a data record, `1` a terminator.

| Value | Type | Header | Ancestor |
|---|---|---|---|
| `0x01` | `KEYVALUE` | 4 | implicit — a create |
| `0x02` | `KEYVALUE_PRIOR` | 4 | on an earlier record in this same file |
| `0x03` | `KEYVALUE_ANC` | 16 | explicit |
| `0x04` | `BIGKEYVALUE` | 24 | implicit — a create |
| `0x05` | `BIGKEYVALUE_PRIOR` | 24 | on an earlier record in this same file |
| `0x06` | `BIGKEYVALUE_ANC` | 32 | explicit |
| `0x07` | `DELETION_PRIOR` | 4 | on an earlier record in this same file |
| `0x08` | `DELETION_ANC` | 16 | explicit |
| `0x09` | `BIGDELETION_PRIOR` | 16 | on an earlier record in this same file |
| `0x0A` | `BIGDELETION_ANC` | 24 | explicit |
| `0x10` | `COMMIT` | 8 | |
| `0x11` | `COMMIT_LONG` | 24 | |
| `0x12` | `COMMIT_LONG_2ND` | (tail of `0x11`) | |
| `0x13` | `FINAL` | 8 | |
| `0x14` | `FINAL_LONG` | 24 | |
| `0x15` | `FINAL_LONG_2ND` | (tail of `0x14`) | |
| `0x16` | `ROLLBACK` | 8 | |
| `0x17` | `ROLLBACK_LONG` | 24 | |
| `0x18` | `ROLLBACK_LONG_2ND` | (tail of `0x17`) | |

- **F-10** Any other type byte, including `0x00`, is invalid.
- **F-10a** There is no create form of a deletion: a deletion always
  supersedes something, so it is only ever `_PRIOR` or `_ANC`.

### 4.4 Data records

Key and value are stored contiguously, separated by a NUL, with a further
NUL after the value, then zero padding to the next multiple of 8. Both key
and value are therefore usable in place as C strings. **F-11** Lengths are
authoritative; keys and values MAY contain NUL bytes, and the stored
lengths MUST NOT include the terminators.

The ancestor is stored as an **absolute 64-bit generation**, never as a delta
or any other value relative to the containing file. Only the **first
occurrence of a key within a file** stores one: later occurrences use a
`_PRIOR` form and omit it, because nothing ever reads their ancestor (F-15d).

```
KEYVALUE (0x01) / KEYVALUE_PRIOR (0x02)
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   .  key NUL value NUL pad->8
  len = roundup8(4 + keylen + 1 + vallen + 1)

KEYVALUE_ANC (0x03)
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   4  pad
  +8   8  ancestor generation
  +16  .  key NUL value NUL pad->8
  len = roundup8(16 + keylen + 1 + vallen + 1)

DELETION_PRIOR (0x07)
  +0   1  type
  +1   1  keylen
  +2   2  pad
  +4   .  key NUL pad->8
  len = roundup8(4 + keylen + 1)

DELETION_ANC (0x08)
  +0   1  type
  +1   1  keylen
  +2   6  pad
  +8   8  ancestor generation
  +16  .  key NUL pad->8
  len = roundup8(16 + keylen + 1)

BIGKEYVALUE (0x04) / BIGKEYVALUE_PRIOR (0x05) / BIGKEYVALUE_ANC (0x06)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  8  vallen
  [+24 8  ancestor generation]        -- 0x06 only
  +24/32  key NUL value NUL pad->8

BIGDELETION_PRIOR (0x09) / BIGDELETION_ANC (0x0A)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  [+16 8  ancestor generation]        -- 0x0A only
  +16/24  key NUL pad->8
```

- **F-12** All 8-byte fields are 8-byte aligned within the file.
- **F-13** Encoding is canonical: an implementation MUST use the short form
  whenever `keylen <= 255` and `vallen <= 65535`; MUST use the short
  terminator whenever the span is `<= 0xFFFFFF` bytes; and MUST select
  between the create, `_PRIOR` and `_ANC` forms exactly as F-15d and F-15e
  require. Byte-for-byte output is therefore determined by the logical
  contents together with what the file already holds. The big form is
  selected by key or value length only — never by the ancestor, which is
  always 8 bytes when present.
- **F-14** A key MUST be at least 1 byte. An empty value is legal and is
  distinct from an absent key.
- **F-15** Every record that casts a shadow MUST know the generation at
  which that shadow hits the previous record for its key. That generation is
  either the record's own, or an earlier one. An update or deletion stores it
  explicitly; a create stores nothing, its ancestor being implicitly its own
  generation. Together these make the per-key version chain unbroken and
  followable.
- **F-15a** The stored value is the **`start` of the range of the file that
  held the superseded record at the time the shadow was cast**. Referencing
  `start` rather than `end` is deliberately conservative: since
  `start <= end`, it points at or further back than strictly necessary, so
  D-18's containment test errs toward "the create lies outside this range" —
  that is, toward **retaining** a tombstone. A wrong answer costs disk space,
  never correctness, and the rule stays sound if file boundaries shift under
  a partial or abandoned repack.
- **F-15b** There is **no guarantee the ancestor is numerically close** to
  the generation of the record holding it: a key untouched for a long time
  and then updated casts its shadow far back. A record in generation 20 may
  legitimately reference generation 5.
- **F-15c** The ancestor is stored **absolutely**, not relative to the
  containing file, precisely so that it never has to be recalculated. A
  merge copies ancestors through unchanged, and a generation the ancestor
  names may since have been absorbed into a file covering a range of
  generations — which is harmless, because D-18 compares the absolute
  ancestor against the merge output's generation range.
- **F-15d** Only the **first occurrence of a key within a file** stores an
  ancestor. Where a file holds versions V1, V2, V3 of a key, V1 carries the
  ancestor and V2 and V3 use a `_PRIOR` form, because nothing ever reads
  their ancestor: a lookup wants only the newest version, and a merge takes
  V1's ancestor with V3's value (D-17). This makes a repeated update within
  one file cost a 4-byte header instead of 16.
- **F-15e** A `_PRIOR` record therefore asserts "an earlier record for this
  key exists in this same file". An implementation MUST NOT write a plain
  create form for a record that supersedes something, and MUST NOT write a
  `_PRIOR` form where no earlier occurrence exists in the file. Keeping the
  two distinct is what stops a merge mistaking a mid-file update for a
  create, concluding the chain starts there, and dropping a tombstone that
  should have been retained — the resurrection of D-17c. A `_PRIOR` record
  with no earlier occurrence in its file is a detectable inconsistency
  rather than a silent wrong answer.

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

- **F-20** From offset 56 to `append_end`, a file is a flat sequence of
  spans. Each span is zero or more data records followed by exactly one
  terminator whose span length equals the span's data byte count and whose
  checksum validates. Every byte in `[56, append_end)` belongs to exactly
  one span or terminator: **no gaps and no nesting.**
- **F-21** Visibility is per span, not a watermark: a voided span may sit
  between two live ones, so a reader MUST replay spans in order and skip
  rolled-back ones.

### 4.7 Pointers

Written once, immediately before a `FINAL` terminator that covers the block.
Present in any file that has been closed — both in-order files and closed
unsorted files (F-6a).

| Off | Size | Field |
|---|---|---|
| 0 | 8 | `NumPointers` |
| 8 | 8 × N | record offsets |

- **F-22** Pointers reference every record present in a committed span
  (records in rolled-back spans are void and MUST NOT be indexed), sorted
  by key ascending, and within equal keys by offset **descending**.
  The first entry of an equal-key range is therefore the newest version,
  and walking forward through that range walks the key's history newest to
  oldest.
- **F-23** A record is **shadowed** when a later record for the same key
  supersedes it — whether that later record is further on in the same file
  or in any newer file. Shadowing is a property of the database as a whole,
  not of a file: a subsequent write elsewhere can shadow a record here.
- **F-23a** For that reason the format stores **no** shadowed-record counts.
  Any value written into a file would be a snapshot that decays into a lower
  bound as soon as the next write lands, and nothing consumes it: the merge
  policy triggers on file size (D-20). `zs_db_check_consistency` and
  `zs_db_dump` MAY compute shadowing live by walking the current file set,
  which is both accurate and more useful than a stale stored count.
- **F-24** Every pointer MUST be 8-aligned and within
  `[56, pointers_offset)`.

### 4.8 Validation

- **F-25 Progress rule.** Iteration computes the next offset from the
  current record's own length fields and MUST verify that it is strictly
  greater than the current offset and within bounds. Otherwise the file is
  treated as ending at that point. Non-termination is thereby impossible by
  construction.
- **F-26** Every length, offset and pointer MUST be bounds-checked against
  the file size before any dereference.
- **F-27** Opening any file with a `[Pointers]` block — closed or in-order
  (F-6a) — is O(1): validate the header, locate the trailing terminator,
  validate it over the pointers block, and use the pointers. Individual
  records are bounds-checked on access, not on open.

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
| 36 | 8 | active file generation |
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

Four states, each distinguishable from the file itself plus the manifest:

| State | `end` | trailing `FINAL` | records | searched via |
|---|---|---|---|---|
| active | 0 | no | append order | shared index |
| closed | 0 | yes | append order | own `[Pointers]` |
| sealed | 0 | no | append order | shared index or scan |
| in-order | N | yes | key order | own `[Pointers]` |

The active file is the one the manifest names; a non-active `end == 0` file
without a trailing `FINAL` is therefore sealed.

- **D-9** When the active file exceeds `rollover_size` (default 2 MB), the
  writer **closes** it — appending `[Pointers]` and a `FINAL` terminator —
  then creates a new active file and publishes. Closing does not reorder
  records, so it costs one sort of keys the writer already has in memory
  plus one `8 × NumPointers` append, keeping rollover off the latency path.
- **D-9a** Closing gives every rolled-over file a durable index, so the
  shared index is only ever needed for the active file and for sealed
  files, and losing it can never force a rescan of a closed file.
- **D-9b** A **sealed** file cannot be closed, because appending
  `[Pointers]` would mean appending past an untrustworthy terminator
  boundary (R-4). Sealed files are searched via the shared index or a scan
  until a merge absorbs them. This is the only case where index loss costs
  a rescan.
- **D-10** Several unsorted files MAY exist at once; putting records into
  key order is a separate, deferrable decision made by merging.
- **D-11** Merging *k* input files produces one in-order file covering their
  combined range, with `[Pointers]` and a `FINAL` terminator.

### 5.4 Shared index

- **D-12** `zeroskip.index` is a regular file, `mmap`'d `MAP_SHARED`,
  holding one index per file that lacks a `[Pointers]` block — that is, the
  active file and any sealed files (D-9a) — keyed by generation and each
  stamped with the database UUID, the generation, and the offset it is
  valid up to.
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

- **D-17** A merge emits **exactly one record per key**. Where the inputs hold
  versions V1, V2, V3 of a key from oldest to newest, the emitted record
  carries **V3's value** — which may be a deletion — and **V1's ancestor**,
  that being the earliest parent generation among the merged versions.
  Intermediate versions disappear, so no within-file shadowing survives a
  merge, and consequently an in-order file never contains a `_PRIOR` record.
- **D-17d** Finding V1's ancestor means finding the **first occurrence** of
  the key, since later occurrences within a file omit theirs (F-15d). In an
  unsorted file that is the lowest offset for the key; in an in-order file
  there is only one record per key, which carries it. A merge scanning its
  inputs MUST therefore resolve each key's earliest ancestor before deciding
  D-17b, and MUST NOT read the ancestor of a `_PRIOR` record, which has none.
- **D-17a** Ancestors are copied through **verbatim**. Nothing is ever
  renumbered and no ancestor is ever recalculated: generations are absolute
  (F-15c), and a merge only changes which file a generation lives in, never
  the generation itself. That is the whole reason the value is absolute.
- **D-17b** Whether the emitted record is written as a create or as an
  update follows from where the earliest ancestor falls, giving one decision
  table per key:

  | earliest ancestor | latest version | emit |
  |---|---|---|
  | `>= output start` | deletion | **nothing** — drop the key (D-18) |
  | `>= output start` | value | a **create**, implicit ancestor |
  | `< output start` | either | an **update or deletion**, explicit ancestor = earliest |

- **D-17c** The emitted record MUST be written even when a newer, unmerged
  file already shadows the key (F-23). Being shadowed is **not** a licence to
  drop a record; only D-18 permits removal. Concretely: key K is created in
  generation 3, updated in generation 7, and updated again in generation 9.
  Merging `[5, 7]` emits one record for K carrying ancestor 3. Drop that
  record and a later merge of `[5, 9]` sees only generation 9's record, whose
  ancestor points at the `[5, 7]` file and so reads as 5; since `5 >= 5` it
  concludes the whole lifespan is contained and drops K — resurrecting it,
  because the create was really in generation 3. The retained record carrying
  ancestor 3 is the only surviving evidence of how far back the chain
  reaches.
- **D-18** A key is removed entirely if and only if its latest version is a
  deletion **and** its earliest ancestor lies inside the merged range
  (`earliest ancestor >= output start`) — that is, its whole lifespan from
  create through update to delete is contained in the merge. Otherwise the
  tombstone MUST be retained, because an older unmerged file may still hold
  the key and dropping it would resurrect the value.
- **D-19** Only **adjacent** files in the `start` ordering may be merged.
  Merging non-adjacent files would produce an output range enclosing a file
  that was not merged, violating the non-overlap invariant (F-8). Adjacency
  also makes D-18 sound: because ranges are contiguous and non-overlapping,
  an output range `[S, E]` accounts for every generation it spans, so
  `ancestor >= S` really does prove the create was merged.

**Merge policy (D-20).** Order the closed files oldest to newest — oldest
first, and by construction largest first. The target invariant is that each
file is at least as big as its next-newer neighbour, giving geometrically
decreasing sizes:

> If a file is as big as its parent, merge it into its parent. Repeat as
> needed.

Concretely, after a rollover adds a new newest closed file, walk from newest
towards oldest: while `size(file) >= size(parent)`, merge the two and
re-test the result against *its* parent, cascading until the invariant holds
or the oldest file is reached. "Size" is bytes on disk, so the trigger is
evaluable from `stat` alone and needs no knowledge of how much of a file is
dead. Reclaiming shadowed bytes is a consequence of merging rather than its
trigger (F-23a).

- **D-21** This yields O(log(total ÷ `rollover_size`)) files with
  geometrically increasing sizes, and amortised O(log n) rewrites per
  record. A lookup miss therefore touches a logarithmic number of files.
- **D-22** The active file never participates; it becomes a merge candidate
  only once closed. Sealed files participate normally.
- **D-23** `zs_db_should_repack` reports whether D-20 currently has any
  merge to do.

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
  Sealing bounds the damage to one file, and 64-bit generations make files
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
  `end_offset` by replaying spans from offset 56, and publish. A read-only
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
an independent implementation validates against. The corpus is generated for
engines 0 and 1 (F-5d), which also pins that a file's engine is honoured from
its own header flags rather than from the reader's configuration, and that
files written under different engines coexist in one database.

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
newer file is never resurrected by merging older ones. Also that only
adjacent files are ever merged (D-19), and that the D-20 cascade reaches the
geometric size invariant — driven by appending enough data to force many
rollovers and asserting the resulting file count and size distribution.

**T-5b Shadowed record retention.** The specific resurrection D-17c guards
against, constructed directly as the generations 3/7/9 case in that
requirement: assert the key stays absent after both merges, and assert that
dropping the retained record instead *does* produce the resurrection — so the
test fails if the retention rule is ever removed as a space optimisation.
Repeated for update-instead-of-delete, for chains longer than three links, and
for chains whose create lies outside every merge performed. Also asserts the
D-17b decision table directly: that V1's ancestor and V3's value survive a
merge, that exactly one record per key is emitted, and that the create/update
form of the emitted record follows from where the earliest ancestor falls.

**T-5c `_PRIOR` encoding.** That repeated writes to one key within a file
produce one ancestor-bearing record followed by `_PRIOR` records (F-15d), that
byte-exact output matches the golden corpus for that shape, and that an
in-order file never contains a `_PRIOR` record (D-17). Negatively: a
hand-built file whose first occurrence of a key is `_PRIOR`, and one where a
mid-file update is written as a create, are both reported by
`zs_db_check_consistency` rather than silently mis-merged (F-15e) — and a
merge of the latter is asserted *not* to drop the tombstone.

**T-5a File states.** All four states of §5.3 round-trip and are correctly
identified from the file plus manifest: that a closed file is searched via
its own `[Pointers]` and an in-order file likewise, that both are recognised
by their trailing `FINAL` (F-6a) independently of `end`, and that a sealed
file is never closed or appended to (D-9b). Also that a live shadowing
computation over the current file set (F-23a) agrees with the reference
model about which records are dead.

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

## 11. Divergences from the original format notes

For anyone comparing this against the notes in `doc/zeroskip.txt`:

| Change | Reason |
|---|---|
| Header 56 bytes, not 40 | generations are 64-bit, and a checksum-engine field is needed so a file is self-describing (F-5a) |
| Type byte is an enum, not a bitfield | bit-packing was premature optimisation; 255 values is ample |
| Selectable checksum engine — none, xxHash, or external — not a fixed CRC32 | consistency with `twom`; the engine id lives in the header flags so each file is self-describing (F-5a) |
| Checksum always the last field, covering everything before it | one rule everywhere, and no field-zeroing dance |
| `keylen` 8 bits in **both** data records | notes had 8 for KeyValue and 24 for Deletion, so a key could be deleted but not stored; Big variants of both cover the rest |
| `key NUL value NUL`, then pad | both usable in place as C strings; lengths remain authoritative (F-11) |
| `ROLLBACK` record added | aborts must void their span, or a later commit would fold the aborted records in and make them live (F-18) |
| Absolute ancestor generation on data records | gives unbroken per-key chains, so a tombstone is droppable exactly when its whole lifespan is inside a merge (D-18); absolute so it never needs recalculating on repack (F-15c) |
| `NumShadowedRecords`/`NumShadowedBytes` removed | any stored value decays into a lower bound and nothing consumes it (F-23a) |
| Little-endian | notes were silent; the older document said network order, `twom` uses little-endian |
| `[Pointers]` also in closed unsorted files | gives a durable index without reordering records, so rollover stays off the latency path (D-9) |

## 12. Open items

1. **`zs_db_repair`** is omitted pending a decision on whether sealing
   (R-4) makes an explicit repair entry point unnecessary. A plausible
   remaining use is forcing a merge that absorbs sealed files, since that is
   the only way to recover their space and give their records a durable
   index (D-9b).
2. **Merge concurrency limit.** D-20 can cascade into a large merge (the
   oldest file is the biggest) while the writer continues appending. The
   packer lock permits exactly one merge at a time, but nothing bounds how
   long one may run or lets it be interrupted and resumed. Deferred until
   there is a measurement to argue from.
3. **Shared index sizing.** D-14 fixes the invariants but not the growth
   policy for `zeroskip.index`, which must hold an index for the active file
   and any sealed files without unbounded growth.
