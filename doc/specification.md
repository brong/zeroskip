# zeroskip: design specification

Status: draft for review
Date: 2026-08-06

`libzeroskip` is an append-only ordered key-value store: a directory of
immutable and append-only files, with lock-free readers and a single writer.

Requirements are labelled (`F-n` format, `D-n` database, `C-n` concurrency,
`R-n` recovery, `A-n` API, `T-n` tests) so the conformance suite can cite
them. **MUST** and **MUST NOT** are normative.

## 1. Purpose and scope

C11 over POSIX only — `mmap`, `fcntl`, no external libraries — building on
Linux, macOS and the BSDs. Keys are ordered by a comparator, byte order by
default.

No code path may depend on a CPU feature: the library MUST build and run on
any conforming POSIX platform, and every on-disk value MUST be bit-identical
across them.

The design rests on one invariant: **nothing is ever written except by
appending to a file or by creating a new file.** No data file is ever
modified in place, renamed, or truncated. The manifest is the one mutable
object, and it is replaced atomically by `rename`.

Its sibling library `twom` is a mutable single-file skiplist. zeroskip suits
workloads that are append-heavy, want readers that never take a lock, and
tolerate compaction happening out of band.

## 2. Terminology

| Term | Meaning |
|---|---|
| generation | 32-bit counter, starting at 1, incremented for each new data file |
| unordered file | holds exactly **one** generation; records in append order; **no** `[Pointers]`; `end == 0` |
| in-order file | holds a **range** of generations; records in key order; **has** `[Pointers]`; `end != 0` |
| active file | the highest-generation unordered file — the only file a writer appends to |
| span | zero or more data records followed by one terminator |
| terminator | a commit, final-commit, or rollback record |
| complete | a file whose content ends at its last valid span |
| clean | an active file that is complete *and* has nothing after that last valid span |
| ancestor | the absolute generation at which the shadow cast by a record hits the previous record for that key |
| shadowed | a record superseded by a later record for the same key, anywhere |

The two file kinds are exhaustive and distinguishable from the header alone:
**`end == 0` means unordered with no pointers; `end != 0` means in-order with
pointers.** A reader therefore always knows, before reading anything else,
whether a terminating pointers block must be present.

## 3. Guarantees

- **G-1 Append-only.** No committed byte is ever mutated; no data file is ever
  renamed or truncated.
- **G-2 Commit atomicity.** Once `zs_txn_commit` returns `ZS_OK`, the whole
  transaction is visible to new readers and, under default durability,
  survives a crash. A crash exposes exactly a prefix of committed
  transactions — never a partial one.
- **G-3 Always reopens.** Any state a crash can produce MUST open in bounded
  time and expose the committed data. Corruption may cost uncommitted data;
  it MUST NOT cost committed data, hang, crash, or read out of bounds.
- **G-4 Snapshot isolation, lock-free reads.** A read transaction sees a fixed
  snapshot and takes no lock. Readers never block a writer; a writer never
  blocks readers.
- **G-5 One writer.** At most one writer per database, enforced by an `fcntl`
  lock. Because the kernel releases `fcntl` locks on process death, a killed
  writer never blocks the next one; no lock state can outlive a process.
- **G-6 No process-lifetime state.** Correctness never depends on the shared
  index or on the manifest, both of which are reconstructible. Deleting or
  corrupting either costs performance, not data.
- **G-7 Read paths agree.** Point lookups and range scans resolve visibility
  through one shared rule (D-14), so they cannot disagree about whether a key
  exists.

## 4. On-disk format

### 4.1 Conventions

- **F-1** Integers are little-endian.
- **F-2** Every record begins at an offset that is a multiple of 8 and
  occupies a whole multiple of 8 bytes. Padding bytes MUST be zero. Fields are
  naturally aligned: 8-byte fields on 8-byte boundaries, 4-byte fields on
  4-byte boundaries.
- **F-3** Offsets and pointers are absolute byte offsets from the file start.
- **F-4** The checksum field is always the **last 4 bytes** of the structure
  it protects, covering every byte from the start of the protected region up
  to (not including) the field. There is no field-zeroing anywhere.
- **F-5** Exactly three checksum engines exist:

  | Id | Engine | Behaviour |
  |---|---|---|
  | 0 | none | write zeros, never verify |
  | 1 | xxHash | XXH3-64 truncated to its low 32 bits (default) |
  | 2 | external | a function supplied by the caller at open time |

- **F-5a** The engine id is recorded in each file header, so every file is
  self-describing. The field is plain data read before any verification, so
  there is no bootstrapping problem, and files written under different engines
  may coexist.
- **F-5b** xxHash is used through its vendored reference implementation, whose
  scalar path is portable C; any SIMD acceleration within it is an internal
  optimisation of the same function, not a separate code path that could be
  absent on a given platform. The resulting value MUST be bit-identical
  everywhere, which the golden corpus pins (T-1).
- **F-5c** Engine 0 weakens G-2 and G-3, because F-16's property — that a
  terminator reaching disk without its data fails validation — depends on a
  real checksum. With engine 0 a torn tail is undetectable. It exists for
  testing and for callers with durability guarantees elsewhere.
- **F-5d** Engine 2 makes a file readable only by a caller supplying the same
  function, so the conformance corpus covers engines 0 and 1 only.
- **F-5e** `ZS_NOCSUM` is distinct from engine 0: it skips verification of
  checksums that are nonetheless written.

### 4.2 Magic

Every file begins with the same 16 bytes:

```
89 7A 65 72 6F 73 6B 69 70 31 0D 0A 1A 0A 00 00
\x89  z  e  r  o  s  k  i  p  1 \r \n ^Z \n \0 \0
```

Each part earns its place, following the reasoning behind the PNG signature:

| Bytes | Purpose |
|---|---|
| `89` | high bit set, so no text file can be mistaken for a database and a transfer that strips the eighth bit is detected |
| `zeroskip` | human-readable in a hex dump and to `file(1)` |
| `1` | major format version *in the magic*, so an incompatible future format is distinguishable without parsing |
| `0D 0A` | CR-LF trap: newline translation in either direction alters it |
| `1A` | DOS end-of-file, so accidentally `type`-ing a file stops early |
| `0A` | bare LF, catching the inverse newline translation |
| `00 00` | NUL-terminates the printable part and pads to 16 |

- **F-6** A reader MUST validate all 16 bytes, not a prefix.

### 4.3 File header (72 bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 16 | magic (§4.2) |
| 16 | 1 | read version — the lowest library version able to read this file |
| 17 | 1 | write version — the lowest library version able to write it |
| 18 | 2 | flags; low 4 bits are the checksum engine id (F-5) |
| 20 | 4 | reserved, written as zero |
| 24 | 16 | database UUID, binary RFC 4122 |
| 40 | 4 | start generation |
| 44 | 4 | end generation, or `0` for an unordered file |
| 48 | 16 | comparator name, NUL-padded |
| 64 | 4 | reserved, written as zero |
| 68 | 4 | checksum of bytes `[0, 68)` |

- **F-7** Split read and write versions let an older library determine that it
  may still read a newer file even when it must not write to it. A reader MUST
  refuse to read above its read version and MUST refuse to write above its
  write version.
- **F-8** Reserved fields MUST be written as zero and MUST be ignored on read.
  Compatibility decisions belong to the version fields, not to reserved bytes.
- **F-9** Generations start at 1, so `end == 0` is never a legitimate
  generation and unambiguously marks an unordered file.
- **F-10** An unordered file holds **exactly one** generation: `start` is that
  generation and `end == 0`. An in-order file produced from inputs spanning
  generations *i*..*j* has `start == i` and `end == j`.
- **F-11** Every file of a database MUST carry the same UUID and the same
  comparator name. The comparator determines key order and hence the meaning
  of `[Pointers]`, so storing it per file is what makes the manifest
  reconstructible (D-4). Opening a database whose files disagree, or whose
  comparator differs from the caller's, is an error.

### 4.4 Record types

The type byte is an enumeration. High nibble `0` is a data record, `1` a
terminator.

| Value | Type | Header | Ancestor |
|---|---|---|---|
| `0x01` | `KEYVALUE` | 4 | omitted — the file's `start` |
| `0x02` | `KEYVALUE_ANC` | 8 | explicit |
| `0x03` | `BIGKEYVALUE` | 24 | omitted — the file's `start` |
| `0x04` | `BIGKEYVALUE_ANC` | 24 | explicit |
| `0x05` | `DELETION` | 4 | omitted — the file's `start` |
| `0x06` | `DELETION_ANC` | 8 | explicit |
| `0x07` | `BIGDELETION` | 16 | omitted — the file's `start` |
| `0x08` | `BIGDELETION_ANC` | 16 | explicit |

A 32-bit ancestor fits inside the padding the big forms already carry, so
`_ANC` costs **nothing** there, and the short `_ANC` forms are 8 bytes rather
than 16.
| `0x10` | `COMMIT` | 8 | |
| `0x11` | `COMMIT_LONG` | 24 | |
| `0x12` | `COMMIT_LONG_2ND` | (tail of `0x11`) | |
| `0x13` | `FINAL` | 8 | |
| `0x14` | `FINAL_LONG` | 24 | |
| `0x15` | `FINAL_LONG_2ND` | (tail of `0x14`) | |
| `0x16` | `ROLLBACK` | 8 | |
| `0x17` | `ROLLBACK_LONG` | 24 | |
| `0x18` | `ROLLBACK_LONG_2ND` | (tail of `0x17`) | |

- **F-12** Any other type byte, including `0x00`, is invalid.
- **F-12a** Each record shape has exactly two forms: one storing an ancestor
  and one omitting it. Nothing distinguishes a "create" at the record level,
  because nothing needs to (F-17).

### 4.5 Data records

Key and value are contiguous, separated by a NUL, with a further NUL after the
value, then zero padding to the next multiple of 8. Both are therefore usable
in place as C strings.

- **F-13** Lengths are authoritative; keys and values MAY contain NUL bytes,
  and stored lengths MUST NOT include the terminators.
- **F-14** A key MUST be at least 1 byte. An empty value is legal and distinct
  from an absent key.

The ancestor is an **absolute 32-bit generation**, never relative to the
containing file, and is stored only when it differs from the containing file's
`start` (F-17).

```
KEYVALUE (0x01)
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   .  key NUL value NUL pad->8
  len = roundup8(4 + keylen + 1 + vallen + 1)

KEYVALUE_ANC (0x02)
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   4  ancestor generation
  +8   .  key NUL value NUL pad->8
  len = roundup8(8 + keylen + 1 + vallen + 1)

DELETION (0x05)
  +0   1  type
  +1   1  keylen
  +2   2  pad
  +4   .  key NUL pad->8
  len = roundup8(4 + keylen + 1)

DELETION_ANC (0x06)
  +0   1  type
  +1   1  keylen
  +2   2  pad
  +4   4  ancestor generation
  +8   .  key NUL pad->8
  len = roundup8(8 + keylen + 1)

BIGKEYVALUE (0x03)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  8  vallen
  +24  .  key NUL value NUL pad->8

BIGKEYVALUE_ANC (0x04)
  +0   1  type
  +1   3  pad
  +4   4  ancestor generation
  +8   8  keylen
  +16  8  vallen
  +24  .  key NUL value NUL pad->8

BIGDELETION (0x07)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  .  key NUL pad->8

BIGDELETION_ANC (0x08)
  +0   1  type
  +1   3  pad
  +4   4  ancestor generation
  +8   8  keylen
  +16  .  key NUL pad->8
```

- **F-15** Encoding is canonical: an implementation MUST use the short form
  whenever `keylen <= 255` and `vallen <= 65535`; MUST use the short terminator
  whenever the span is `<= 0xFFFFFF` bytes; and MUST select between the
  ancestor-storing and ancestor-omitting forms exactly as F-17 requires; and
  MUST choose the pointer width by F-26a. Output
  bytes are therefore determined by the logical contents together with what
  the file already holds. The big form is chosen by key or value length only,
  never by the ancestor, which is always 8 bytes when present.

### 4.6 Ancestors

- **F-16** Every record that casts a shadow MUST know the generation at which
  that shadow hits the previous record for its key — either its own generation
  or an earlier one. An update or deletion stores it; a create stores nothing,
  its ancestor being implicitly its own generation.
- **F-16a** The stored value is the **`start` of the range of the file holding
  the superseded record** when the shadow was cast. Referencing `start` rather
  than `end` is deliberately conservative: since `start <= end` it points at or
  further back than strictly necessary, so D-19's containment test errs toward
  "the create lies outside this range" and therefore toward **retaining** a
  tombstone. Wrong answers cost disk space, never correctness.
- **F-16b** There is **no guarantee the ancestor is numerically close**: a key
  untouched for a long time and then updated casts its shadow far back. A
  record in generation 20 may legitimately reference generation 5.
- **F-16c** The ancestor is absolute precisely so it never needs
  recalculating. A repack copies ancestors through verbatim, and the generation
  named may since have been absorbed into a file covering a range — harmless,
  because D-19 compares the absolute ancestor against a generation range.
- **F-17** The ancestor is **omitted exactly when it equals the containing
  file's `start` generation**, and a record with no stored ancestor is read as
  having that value. This one rule covers every case:

  | Situation | Ancestor | Encoding |
  |---|---|---|
  | new key written into the active file | its own generation = the file's `start` | omitted |
  | key updated again later in the same file | the file holding what it superseded is this one | omitted |
  | key last written in an older file | that file's `start`, which is lower | stored |
  | repack output, chain begins inside the output range | the output's `start` | omitted |
  | repack output, chain begins earlier | V1's ancestor, which is lower | stored |

- **F-17a** The two conditions "this is a later occurrence in the file" and
  "the ancestor equals this file's `start`" coincide, because a later
  occurrence by definition supersedes a record in this same file. Decoding
  therefore never needs to establish whether a record is the first occurrence
  of its key. A repeated update within one file costs a 4-byte header rather
  than 8.

### 4.7 Terminators

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

- **F-19** The checksum covers the span's data bytes followed by the
  terminator's own bytes up to the checksum field.
- **F-20** `type2` exists so the last 8 bytes of a file reveal whether the
  trailing terminator is short or the tail of a long one, making it locatable
  by reading backwards.
- **F-21** A `COMMIT` makes its span's records live. A `ROLLBACK` is a commit
  that says "ignore the records in this span", voiding them. An aborted
  transaction appends a `ROLLBACK`; without one, a later commit's span would
  enclose the aborted records and make them live.
- **F-22** Because the checksum covers the span **and** the terminator, a
  terminator reaching disk without its data fails validation and reads as
  absent. One `fsync` after the terminator is therefore sufficient, and no
  intra-transaction write barrier is needed.

### 4.8 The span chain

- **F-23** From the end of the header onwards, a file is a flat sequence of
  spans. Each span is zero or more data records followed by exactly one
  terminator whose span length equals the span's data byte count and whose
  checksum validates. Every byte belongs to exactly one span or terminator:
  no gaps, no nesting.
- **F-24** A file is **complete** at its last valid span, whether that is the
  end of the file or the point where a record or terminator fails to validate.
  Content beyond it is not part of the database.
- **F-25** Visibility is per span, not a watermark: a rolled-back span may sit
  between two live ones, so a reader MUST replay spans in order and skip
  rolled-back ones.

### 4.9 Pointers

Present in in-order files only, written once, immediately before a `FINAL`
terminator covering the block.

Pointers are 32-bit in a file of `0xFFFFFFFF` bytes or fewer, and 64-bit
otherwise:

```
32-bit form
  +0    8    NumPointers
  +8    4×N  record offsets (uint32)
  pad to a multiple of 8

64-bit form
  +0    8    NumPointers
  +8    8×N  record offsets (uint64)
```

- **F-26** Pointers reference every record in the file, sorted by key
  ascending. Because a repack emits exactly one record per key (D-17), keys in
  an in-order file are unique and the array is a strict ordering.
- **F-26a** The pointer width is **derived from the file size**: 32-bit if the
  file is `0xFFFFFFFF` bytes or fewer, 64-bit otherwise. Nothing records it.
  A header flag could not: the header is written before the file's eventual
  size is known, so setting one correctly would require rewriting the header
  afterwards, which G-1 forbids. The `FINAL` terminator is written last and
  could have carried it, but derivation needs no state at all, and the size is
  known as soon as the file is mapped.
- **F-26b** Because every record lies before the pointers block, a file within
  `0xFFFFFFFF` bytes has every record offset inside 32 bits, so the narrow form
  is always sufficient when selected. The rule is canonical: a file over the
  bound uses 64-bit pointers even if every offset would have fitted.
- **F-26c** The block is padded with zeroes to a multiple of 8 so the `FINAL`
  terminator begins 8-aligned (F-2). The pad is 0 or 4 bytes and is covered by
  the terminator's checksum like any other span byte.
- **F-27** Every pointer MUST be 8-aligned and lie between the header and the
  pointers block.
- **F-28** `zs_db_check_consistency` MUST verify that an in-order file's
  pointer array is strictly increasing by key, which both confirms the sort and
  catches a repack that emitted a key twice (D-17).

### 4.10 Validation

- **F-29 Progress rule.** Iteration computes the next offset from the current
  record's own length fields and MUST verify it is strictly greater than the
  current offset and within bounds. Otherwise the file is complete at that
  point (F-24). Non-termination is impossible by construction.
- **F-30** Every length, offset and pointer MUST be bounds-checked against the
  file size before any dereference.
- **F-31** Opening an in-order file is O(1): validate the header, locate the
  trailing terminator, validate it over the pointers block, use the pointers.
  Records are bounds-checked on access, not on open.

## 5. Database layout

### 5.1 Directory contents

| Name | Mutability | Purpose |
|---|---|---|
| `zeroskip-<uuid>-<gen>` | append-only | unordered file, one generation |
| `zeroskip-<uuid>-<start>-<end>` | immutable | in-order file |
| `zeroskip.manifest` | replaced atomically | published file set |
| `zeroskip.tmp.<pid>.<n>` | transient | repack output and manifest staging |
| `zeroskip.lock` | never replaced or unlinked | holds `fcntl` locks |
| `zeroskip.index` | pure cache | key order for files without pointers |

- **D-1** Generations in filenames are **uppercase hexadecimal, zero-padded to
  8 digits**, so a file holding the first ten generations is
  `zeroskip-<uuid>-00000001-0000000A`. Eight hex digits is exactly the range of
  a 32-bit generation, so every representable generation has a name and the
  width never needs to change. Fixed width also keeps lexical and numeric order
  identical, and hexadecimal keeps names short.
- **D-2** `zeroskip-*` matches data files only and `zeroskip.*` matches
  metadata, so both sets are prefix-globbable and shell-completable.
- **D-3** `zeroskip.lock` MUST be a distinct file that is never replaced.
  `fcntl` locks attach to an inode, so locking the manifest — whose inode
  changes on every publish — would silently lose mutual exclusion.

### 5.2 Manifest

The manifest publishes a consistent file set so a reader need not race a
directory scan against a repack.

| Off | Size | Field |
|---|---|---|
| 0 | 16 | magic (§4.2) |
| 16 | 16 | database UUID |
| 32 | 8 | publish sequence, incremented on every publish |
| 40 | 8 | active file's `append_end` as of this publish |
| 48 | 4 | generation counter — highest generation ever allocated |
| 52 | 4 | file count |
| 56 | 16 × N | `(start, end, size)` per file |
| . | 4 | checksum over all preceding bytes |

- **D-3a** The manifest's checksum always uses xxHash, whatever engine the data
  files use. It is not part of the data format, and a failure merely triggers
  reconstruction (R-5), so it costs nothing to protect unconditionally.
- **D-4** The manifest MUST be fully **reconstructible** from the directory,
  because every fact in it is derivable: the file set from the listing, each
  file's range from its own header, the active file as the highest-generation
  unordered file, and the comparator and UUID from any header (F-11). It is a
  cache and a publication point, never a source of truth.
- **D-5** It is written to `zeroskip.tmp.<pid>.<n>` and `rename`d into place,
  so a crash yields either the old manifest or the new one, never a partial
  one.
- **D-6** The recorded `append_end` is a **floor, not a truth**: a reader scans
  the active file forward from it. The floor only advances past committed data,
  so a stale floor costs a short scan and can never discard committed data.
  The manifest is therefore rewritten only on structural change — a new active
  file or a completed repack — never per commit.
- **D-7** A data file not referenced by the manifest MUST be ignored by
  readers.
- **D-8** Reconstruction MUST reject a set whose ranges do not **tile a
  contiguous interval of generations** — no overlaps, no gaps. One exception is
  expected and resolvable: a repack interrupted after renaming its output but
  before removing its inputs leaves an in-order file whose range *encloses*
  other files. The enclosing file supersedes those it contains, which are
  treated as unreferenced debris (D-7, D-11).

### 5.3 Writing

- **D-9** An active file is **clean** if it has a valid header and zero or more
  valid spans with nothing after the last. While holding the write lock, a
  writer MUST either append spans to a clean active file, or create a new
  unordered file whose generation is exactly one higher than the current active
  file, write a valid header, and append spans to that — making it the new
  active file.
- **D-9a** A writer moves to a new file when the active file is not clean, or
  when it exceeds `rollover_size` (default 2MB). Rollover is cheap: a new
  header and nothing else, since no pointers are written (D-11).
- **D-9b** The manifest's generation counter is a high-water mark of every
  generation ever allocated, so reconstruction after files have been removed
  cannot reissue one. A writer allocates `active + 1` and MUST advance the
  counter to at least that.
- **D-9c** Generations are never reused and never reset, not even by a repack
  that collapses the whole database into one file — the next new file is
  `end + 1`. Allocating past `0xFFFFFFFF` MUST fail with `ZS_FULL` rather than
  wrap. At the 2MB default that bound is around 8PB of cumulative writes, and
  the remedy is to dump and reload into a fresh database.
- **D-10** An active file with a corrupt header or zero length is treated as a
  **complete file with zero spans**. It is not an error and its generation is
  taken from its filename. Because it is not clean, a writer moves to a new
  file rather than appending, so no chain is ever built on an untrustworthy
  boundary.
- **D-10a** A **non-active** file with an invalid header is an error
  (`ZS_BADFORMAT`): its records cannot be recovered and silently skipping the
  generation would lose committed data. Discarding it requires an explicit
  tool action.
- **D-11** The writer never appends `[Pointers]` to an unordered file. When it
  moves on, the previous file simply stays unordered until a repack rewrites
  it. Key order for such a file lives in the shared index until then.
- **D-12** A writer MAY repack, so it may rewrite the file it has just left as
  its single-generation in-order form before exiting.

### 5.4 Shared index

- **D-13** `zeroskip.index` is a regular file `mmap`'d `MAP_SHARED`, holding
  key order for files that lack `[Pointers]` — the active file and any
  unordered file not yet repacked — keyed by generation and stamped with the
  database UUID, the generation, and the offset it is valid to.
- **D-13a** It is a pure cache and MUST NOT be authoritative. Any process MAY
  discard it and rebuild by scanning. An implementation MUST validate its
  stamps before use and discard it on any inconsistency.
- **D-13b** Its internal layout is not part of this specification and may
  change without a format version bump. Only its invariants are specified.

### 5.5 Lookup order

- **D-14** Resolution order is: the current write transaction's own
  uncommitted records; then all data files by `start` **descending**. Within a
  file the newest version of a key wins — the highest offset among committed
  spans. The first record found wins; if it is a deletion, the key does not
  exist.
- **D-14a** Point lookups, cursors and range scans MUST all resolve visibility
  by D-14. Range scans are a k-way merge over the same per-file sources,
  deduplicating by key with newest-wins.

### 5.6 Repacking

- **D-15** The repacker **never touches the active file**. It runs
  periodically, or whenever a non-active unordered file exists.
- **D-16** Input selection:
  1. Take **all** non-active unordered files, which collapse together into a
     **single** ordered file rather than one ordered file each.
  2. If the resulting file would be larger than the next lowest in-order file,
     include that file too and repeat.
  3. Stop when every file is included or the next lowest in-order file is
     larger.

  This yields geometrically sized in-order files and amortised O(log n)
  rewrites per record.
- **D-16a** Step 1 collapses all unordered files at once because the cost of a
  read scales with the number of files that must be consulted, so minimising
  the resulting file count is the point. Where only one non-active unordered
  file exists — the common case, since a writer converts the file it has just
  left (D-12) — this is the same thing as converting it alone.
- **D-16b** Steps 2 and 3 cascade rather than merging two files per invocation.
  Both converge to the same steady state, since a cascade is simply several
  pairwise steps run back to back, but the cascade does strictly less total
  I/O: merging *A*, *B*, *C* in one pass writes `a+b+c`, whereas *A*+*B* then
  (*A*+*B*)+*C* writes `2a+2b+c`. Pairwise also leaves a higher file count
  between invocations, which reads pay for. The cost of the cascade is that a
  single invocation is unbounded in duration; that is accepted (see open
  items).
- **D-17** The output holds **exactly one record per key**, built from the live
  records of all inputs, skipping rolled-back spans. Where the inputs hold
  versions V1, V2, V3 of a key from oldest to newest, the emitted record
  carries **V3's value** — possibly a deletion — and **V1's ancestor**.
- **D-17a** Ancestors are copied verbatim; nothing is renumbered and no
  ancestor is recalculated (F-16c).
- **D-17b** A repack MUST consider the versions of a key in a **total order**,
  oldest to newest:

  1. across files, by increasing `start` generation — the tiling invariant
     (D-8) means ranges never overlap, so this is total;
  2. within one unordered file, by increasing offset among committed spans;
  3. an in-order file holds one record per key, so there is nothing to order.

  V1 is the first version in that order and V3 the last. The emitted record
  takes **V3's value** and **V1's ancestor** — from those records specifically,
  and by no other route.
- **D-18** Per key:

  | V1's ancestor | V3's value | emit |
  |---|---|---|
  | `>= output start` | a deletion | **nothing** — drop the key |
  | `>= output start` | a value | the **ancestor-omitting** form |
  | `< output start` | either | the **ancestor-storing** form, ancestor = V1's |

- **D-19** A key is removed entirely if and only if its latest version is a
  deletion **and** V1's ancestor lies inside the output range — its
  whole lifespan from create through update to delete is contained. Otherwise
  the tombstone MUST be retained, because an older file may still hold the key
  and dropping it would resurrect the value.
- **D-19a** The emitted record MUST be written even when a newer file already
  shadows the key. Being shadowed is **not** a licence to drop a record.
  Concretely: K is created in generation 3, updated in 7, updated again in 9.
  Repacking `[5, 7]` emits one record for K carrying ancestor 3. Drop it, and a
  later repack of `[5, 9]` sees only generation 9's record, whose ancestor
  points at the `[5, 7]` file and so reads as 5; since `5 >= 5` it concludes
  the lifespan is contained and drops K — resurrecting it, because the create
  was really in generation 3. The retained record is the only surviving
  evidence of how far back the chain reaches.
- **D-20** Inputs are iterated in key order: from `[Pointers]` where present,
  otherwise from the same index any reader of a pointerless file must build.
  There is nothing repack-specific about this.
- **D-21** The output is written to `zeroskip.tmp.<pid>.<n>` and `rename`d to
  `zeroskip-<uuid>-<start>-<end>` covering the entire range of every input,
  only once complete.
- **D-22** The output may legitimately contain **zero records** — for instance
  if every record in the database was deleted and all files were then
  repacked, or if generation *X* created one record and *X+1* deleted it and
  those two were repacked together. The file MUST still be written, so the
  generation range stays tiled (D-8). It is cheap and short-lived: an empty
  file violates D-16's size relation maximally, so the next repack absorbs it.
- **D-23** Removing a data file — repack inputs, or debris from an interrupted
  repack — MUST be done **holding the packer lock**, and only after verifying
  that a complete set of files exists without it (D-8). Verification and
  removal MUST happen under one unbroken hold of the lock, so the set cannot
  change in between. If verification fails the file MUST be left alone:
  leaking a file costs disk space, removing a needed one costs the database.
- **D-24** `zs_db_should_repack` reports whether D-16 currently has work.

## 6. Concurrency and durability

- **C-1** Two `fcntl` byte-range locks on `zeroskip.lock`:

  | Byte | Lock | Covers |
  |---|---|---|
  | 0 | write | appending to the active file, creating a new active file |
  | 1 | packer | repacking, rewriting the manifest, removing files |

  Appending needs only the write lock, so writing and repacking proceed
  concurrently.
- **C-1a** Grouping publish and removal under the packer lock is what makes
  both safe: the file set cannot change between reading it and acting on it.
- **C-1b Lock ordering.** A writer holding the write lock MAY acquire the
  packer lock, which it needs briefly to publish. A packer MUST NOT acquire
  the write lock. Acquisition is always write → packer, so the two cannot
  deadlock.
- **C-2** Readers take **no lock**.
- **C-3** A publisher MUST hold the packer lock, re-read the current manifest,
  merge its change into it, then write and rename — a compare-and-publish.
- **C-4 Snapshot lifetime.** A reader reads the manifest, opens and `mmap`s
  every listed file, then re-reads the manifest; if the publish sequence
  changed it retries, bounded. Once its descriptors are open a packer may
  (subject to D-23) `unlink` superseded files immediately: the kernel keeps
  the inodes alive until the last descriptor closes. There is no reference
  table and nothing to clean up when a process dies.
- **C-5** The accepted cost of C-4 is that disk space is held until the last
  reader holding an old snapshot exits.
- **C-6 Directory durability.** After creating a file (a new active file, or a
  repack output) and after any `rename`, the implementation MUST `fdatasync`
  the **directory**, otherwise the name may be absent after a crash even
  though the file's contents are durable.
- **C-6a** A directory sync is **not** required after `unlink`. If a removed
  name reappears after a crash the file is unreferenced debris, which readers
  ignore (D-7) and a later repack removes again (D-23).
- **C-7** Default durability `fsync`s after each commit terminator.
  `ZS_NOSYNC` omits it, trading crash survival for throughput while retaining
  atomicity.
- **C-8** An aborted transaction appends a `ROLLBACK` and does **not** `fsync`.
  If a crash loses it, the active file is simply no longer clean, so the next
  writer moves to a new file (D-9) and reaches the same state.

## 7. Open and recovery

Opening is recovery; there is no separate pass.

- **R-1** Open reads the manifest, opens and maps the listed files, then
  establishes the active file's true end by replaying spans forward from the
  recorded floor (D-6), stopping at the first record or terminator that fails
  to validate. The shared index normally means nothing is scanned.
- **R-2** Live data is the union of records in spans with `COMMIT` terminators;
  rolled-back spans contribute nothing.
- **R-3** A reader MUST NOT write. Opening a damaged database read-only is
  side-effect-free: no repack, no new active file, no manifest publish, no
  index update.
- **R-4** There is no in-place repair. A file that is not clean is simply
  complete at its last valid span (F-24), and the writer moves to a new
  generation. Nothing is ever appended past a boundary that failed to
  validate, so a spurious terminator in trailing garbage — which a checksum
  can never wholly exclude — cannot become the foundation of a later chain.
  Generations are cheap.
- **R-5** If the manifest is missing or fails validation it MUST be
  reconstructed by scanning the directory (D-4, D-8). A read-only open does so
  in memory without publishing. This is also how a new database begins.
- **R-6** A crash during a repack leaves an unreferenced or enclosing file,
  resolved by D-8 and removable under D-23. A crash during publish leaves
  either manifest intact (D-5).

## 8. Public API

Opaque types, one 32-bit flag space, output through pointer parameters, and
`enum zs_ret` with `ZS_OK = 0`, `ZS_DONE = 1`, negatives for errors
(`ZS_NOTFOUND = -5`, `ZS_LOCKED = -4`, `ZS_BADFORMAT = -7`, `ZS_FULL`, …).

```c
struct zs_open_data {
    uint32_t     flags;
    zs_compar   *compar;         /* NULL = byte order */
    const char  *compar_name;    /* stored in every file header */
    zs_csum     *csum;           /* required for engine 2 */
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

/* transactions */
int  zs_db_begin_txn(struct zs_db *db, int shared, struct zs_txn **txnp);
int  zs_txn_commit(struct zs_txn **txnp);
int  zs_txn_abort(struct zs_txn **txnp);

int  zs_txn_fetch(struct zs_txn *txn, const char *key, size_t keylen,
                  const char **keyp, size_t *keylenp,
                  const char **valp, size_t *vallenp, int flags);
int  zs_txn_store(struct zs_txn *txn, const char *key, size_t keylen,
                  const char *val, size_t vallen, int flags);
int  zs_txn_foreach(struct zs_txn *txn, const char *prefix, size_t prefixlen,
                    zs_cb *p, zs_cb *cb, void *rock, int flags);

/* cursors, from a db (implicit transaction) or inside one */
int  zs_db_begin_cursor(struct zs_db *db, const char *key, size_t keylen,
                        struct zs_cursor **curp, int flags);
int  zs_txn_begin_cursor(struct zs_txn *txn, const char *key, size_t keylen,
                         struct zs_cursor **curp, int flags);
int  zs_cursor_next(struct zs_cursor *cur,
                    const char **keyp, size_t *keylenp,
                    const char **valp, size_t *vallenp);
int  zs_cursor_replace(struct zs_cursor *cur,
                       const char *val, size_t vallen, int flags);
int  zs_cursor_commit(struct zs_cursor **curp);
int  zs_cursor_abort(struct zs_cursor **curp);
void zs_cursor_fini(struct zs_cursor **curp);

/* deletion is a store of a NULL value; these are macros, not functions */
#define zs_db_delete(db, key, keylen, flags) \
        zs_db_store((db), (key), (keylen), NULL, 0, (flags))
#define zs_txn_delete(txn, key, keylen, flags) \
        zs_txn_store((txn), (key), (keylen), NULL, 0, (flags))
#define zs_cursor_delete(cur, flags) \
        zs_cursor_replace((cur), NULL, 0, (flags))

int  zs_db_repack(struct zs_db *);
bool zs_db_should_repack(struct zs_db *);
int  zs_db_check_consistency(struct zs_db *);
int  zs_db_dump(struct zs_db *, int detail);
int  zs_db_sync(struct zs_db *);
const char *zs_strerror(int r);
```

Flags occupy one 32-bit space and are never reused for different meanings in
different calls, though not every flag is meaningful everywhere:

| Flag | Where | Meaning |
|---|---|---|
| `ZS_CREATE` | open | create the database if absent |
| `ZS_SHARED` | open, txn | read-only (A-5) |
| `ZS_NOCSUM` | open | do not verify checksums on read (F-5e) |
| `ZS_NOSYNC` | open | do not `fsync` on commit (C-7) |
| `ZS_NONBLOCKING` | open, txn | fail with `ZS_LOCKED` rather than wait for a lock |
| `ZS_IFNOTEXIST` | store | store only if the key is absent, else `ZS_EXISTS` |
| `ZS_IFEXIST` | store | store only if the key is present, else `ZS_NOTFOUND` |
| `ZS_FETCHNEXT` | fetch | return the record *after* the given key |
| `ZS_SKIPROOT` | foreach, cursor | skip the first record if it matches the start key exactly |
| `ZS_CURSOR_PREFIX` | foreach, cursor | stop when the key leaves the prefix |

- **A-0** Every read and write entry point exists in three forms — on the
  database, on a transaction, and via a cursor — and all three take `flags`.
  The `zs_db_*` forms are convenience wrappers that open an implicit
  single-operation transaction, so there is no operation reachable one way but
  not another.

- **A-1** `store` with `val == NULL` writes a deletion; with a non-NULL
  zero-length value it stores an empty value. These are distinct states. This
  matches the convention of the other Cyrus database backends.
- **A-1b** The `*_delete` forms are **macros** over `store` and
  `cursor_replace`, not separate entry points, so there is exactly one write
  path to implement and test. `ZS_IFEXIST` composes with them naturally to mean
  "delete only if present".
- **A-1a** A write inside a transaction is visible to subsequent reads on that
  same transaction, and to nothing else until commit. `zs_txn_fetch` and
  `zs_txn_foreach` therefore consult the transaction's own uncommitted records
  first, per D-14.
- **A-2** There is no `yield` call and no yield flags: readers hold no lock, so
  there is nothing to yield.
- **A-3** There is no MVCC flag. Snapshot isolation is the only read mode,
  because a snapshot is a set of immutable files plus a per-file valid extent.
- **A-4** Returned key and value pointers remain valid for the lifetime of the
  transaction or cursor that produced them; for the non-transactional
  `zs_db_*` calls, until the next call on that `struct zs_db`.
- **A-5** `ZS_SHARED` is read-only and MUST NOT write (R-3).

## 9. Conformance suite

`zstest`: one binary, substring filter, fresh temp directory per test,
`ASSERT_*` macros. Flat layout: `zeroskip.h`, `zeroskip.c`, `xxhash.h`,
`zstest.c`, `zstool.c`, `Makefile`, plus `doc/` and `tests/corpus/`.

**T-1 Golden vectors.** Byte-exact encode assertions against checked-in files,
deterministic because F-15 makes encoding canonical, with `zstool` accepting an
explicit UUID so corpus generation needs no test hook in the library. Decode
assertions from a corpus manifest. Generated for engines 0 and 1 (F-5d), which
also pins that a file's engine comes from its own header rather than the
reader's configuration. `doc/conformance.md` plus this corpus is what an
independent implementation validates against.

**T-2 Magic and versions.** All 16 magic bytes required (F-6); each
single-byte mutation rejected; the specific corruptions the magic is designed
to catch — eighth bit stripped, `0D 0A` collapsed to `0A`, `0A` expanded to
`0D 0A` — each rejected. Read and write versions above the library's rejected
appropriately, and a file readable-but-not-writable accepted read-only (F-7).

**T-3 Malformed input.** Every golden file truncated at *every byte offset*,
not merely record boundaries, and systematically bit-flipped. Each case asserts
an error or the committed prefix, never a crash, hang, or out-of-bounds read.
Under ASan and UBSan with a per-case wall-clock timeout, the timeout being the
detector for F-29.

**T-4 Behavioural.** Ordering, prefix scans, cursor replace,
`IFEXIST`/`IFNOTEXIST`/`FETCHNEXT`/`SKIPROOT`/`CURSOR_PREFIX`, every flag
exercised through all three entry points — database, transaction and cursor —
and asserted to behave identically (A-0), read-your-own-writes inside a
transaction and invisibility outside it (A-1a), empty-value versus absent key
(A-1), and the encoding boundaries: 255↔256-byte keys, 65535↔65536-byte
values, a 16MB span forcing a long terminator, a record landing exactly on
8-byte alignment, and keys containing embedded NULs (F-13).

**T-5 Model-based.** Randomised operation sequences against an in-memory
reference model, checked after every step by both a point lookup and a full
scan, cross-checked against each other — the direct test for G-7.

**T-6 File states and encoding.** That `end == 0` and `end != 0` files are
recognised solely from the header and that a pointers block is present exactly
when `end != 0`; that an in-order file's pointers are strictly increasing by key
(F-28); that the pointer width follows the file size, including a
hand-constructed file just over `0xFFFFFFFF` bytes so the 64-bit form is covered
without writing 4GB of real data, and one just under it (F-26a), plus the
0-and-4-byte padding cases (F-26c); and that every row of F-17's table
round-trips — in particular that
repeated writes to one key in a file store the ancestor at most once, and that a
record with no stored ancestor decodes to its file's `start`. Negatively, a
hand-built file storing an ancestor equal to its own `start` (which F-15 forbids
as non-canonical) is reported by `zs_db_check_consistency`.

**T-7 Ancestors and repacking.** For every arrangement of create, update and
delete spread across generations: chains stay unbroken (F-16), D-17 preserves
V1's ancestor with V3's value, exactly one record per key is emitted, D-18's
table is followed, and D-19 drops a key only when its whole lifespan is inside
the output. Version ordering (D-17b) is tested directly, since getting it wrong
silently emits the wrong value or the wrong ancestor: several versions of one
key at increasing offsets within an unordered file, several across files with
different `start` generations, and both at once — asserting the emitted value
comes from V3 and the emitted ancestor from V1 under that total order, not from
whichever record the merge's internal iteration happened to touch first or last.
Then D-19a's resurrection is constructed directly, asserting both that the key
stays absent and that dropping the retained record *does* produce the bug.
Coverage continues with
the output. The D-19a resurrection is constructed directly, asserting both that
the key stays absent and that dropping the retained record *does* produce the
bug — so the test fails if the rule is ever removed as an optimisation. Also
that D-16 selects inputs correctly — several unordered files collapsing into
one ordered file rather than one each (D-16a), and the cascade reaching the
geometric size relation after many rollovers — and that an empty output is
still written (D-22).

**T-8 Crash injection.** A test build interposes `write`, `fsync`, `rename` and
`unlink`, counts calls, and aborts at call *N* for every *N* over a scripted
workload. Each case asserts reopen terminates within a timeout, exactly a
prefix of committed transactions is visible, nothing acknowledged is lost under
default durability, and a writer can then continue. Targeted: crash between
records and terminator; after terminator before `fsync`; mid-publish rename;
mid-repack; after `[Pointers]` but before `FINAL`; leaving a non-8-aligned file
length; and after an invalid terminator, asserting the writer moves to a new
generation rather than appending (R-4, D-9). Both durability modes. Separately,
with directory syncs suppressed, that a crash can lose a *name* — the test that
justifies C-6.

**T-9 Manifest reconstruction.** The manifest deleted, truncated and
bit-flipped, asserting reconstruction from the directory yields identical
logical contents (R-5), and that a read-only open reconstructs without writing
(A-5). A directory seeded with an interrupted repack — an in-order file
enclosing files it superseded — asserting D-8 resolves it. Files disagreeing on
UUID or comparator rejected (F-11).

**T-10 Multi-process.** Real forked processes. A writer plus *N* readers
asserting snapshot stability across commits and that a fresh open sees them.
Two writers, exactly one proceeding, `ZS_LOCKED` under `ZS_NONBLOCKING`. **A
writer `SIGKILL`ed holding the lock, asserting the next writer proceeds with no
manual intervention** (G-5). A reader holding a snapshot across a repack,
asserting its data stays readable while inputs are unlinked (C-4). Two
processes racing to remove debris, asserting the surviving set still tiles and
no needed file is removed; a directory seeded with the debris of two
half-finished repacks over overlapping ranges, where naive independent cleanup
would remove both and lose a generation; removal attempted without the packer
lock, asserting refusal (D-23). The shared index deleted, truncated and
bit-flipped mid-run, asserting identical results (G-6). Concurrent repack and
writer both proceeding, with publish serialised.

**T-11 Traceability.** `doc/conformance.md` maps every normative requirement
here to the test enforcing it. A requirement with no test is a gap to close;
this mapping is what makes the suite a conformance suite rather than a test
suite.

## 10. Non-goals

Multi-writer concurrency; cross-database transactions; secondary indexes;
compression; network access; in-place value mutation. A Cyrus `cyrusdb`
backend is a thin separate adapter, out of scope.

## 11. Open items

1. **Repack duration is unbounded.** D-16 can cascade into rewriting the whole
   database while the writer continues appending. The packer lock permits one
   repack at a time, but nothing bounds how long one runs or lets it be
   interrupted and resumed. This is a deliberate trade for lower total I/O and
   a smaller file count (D-16b), and writing continues throughout regardless.
   If it ever needs bounding, two mitigations preserve the same steady state:
   merge pairwise, one step per invocation, or cap the cascade at a projected
   output size and resume next time.
2. **Shared index growth.** D-13 fixes the invariants but not the growth policy
   for `zeroskip.index`, which must cover the active file and any unordered
   files awaiting repack without unbounded growth.
