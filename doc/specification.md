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
| unordered file | holds exactly **one** generation; records in append order; **no** pointer section; `end == 0` |
| in-order file | holds a **range** of generations; records in key order; **has** a pointer section; `end != 0` |
| active file | the highest-generation unordered file — the only file a writer appends to |
| span | zero or more data records followed by one terminator |
| terminator | a commit or rollback record, ending a span |
| complete | an unordered file whose content ends at its last valid span |
| clean | an active file that is complete *and* has nothing after that last valid span |
| ancestor | the absolute generation at which the shadow cast by a record hits the previous record for that key |
| shadowed | a record superseded by a later record for the same key, anywhere |

The two file kinds are exhaustive and distinguishable from the header alone:
**`end == 0` means unordered with no pointers; `end != 0` means in-order with
pointers.** A reader therefore always knows, before reading anything else,
whether a pointer section must be present.

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
| `89` | high bit set, so no text file can be mistaken for a database and a transfer that strips the eighth bit is detected; also **invalid UTF-8** (F-6a) |
| `zeroskip` | human-readable in a hex dump and to `file(1)` |
| `1` | major format version *in the magic*, so an incompatible future format is distinguishable without parsing |
| `0D 0A` | CR-LF trap: newline translation in either direction alters it |
| `1A` | DOS end-of-file, so accidentally `type`-ing a file stops early |
| `0A` | bare LF, catching the inverse newline translation |
| `00 00` | NUL-terminates the printable part and pads to 16 |

- **F-6** A reader MUST validate all 16 bytes, not a prefix.
- **F-6a** The magic is **not valid UTF-8**: `0x89` lies in the continuation-byte
  range `0x80`–`0xBF`, and a continuation byte cannot begin a sequence. Anything
  that validates the file as text fails at the first byte rather than part-way
  through, and anything that sanitises invalid UTF-8 by substitution replaces
  `0x89` with U+FFFD, destroying the magic detectably instead of silently
  corrupting the body. This is the modern counterpart of the eighth-bit and
  newline traps, and it costs nothing: every byte after the first is ASCII, so
  byte 0 alone carries the property.

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
  of the pointer section, so storing it per file is what makes the manifest
  reconstructible (D-4). Opening a database whose files disagree, or whose
  comparator differs from the caller's, is an error.

### 4.4 Record types

The type byte is a bitfield of six independent properties:

| Bit | Name | Meaning |
|---|---|---|
| `0x01` | `HasKey` | a data record: carries a key |
| `0x02` | `IsDelete` | negation — of a key, or of a span |
| `0x04` | `IsBig` | wide length fields |
| `0x08` | `HasAncestor` | an ancestor generation is stored |
| `0x10` | `SpanTerminator` | ends a span |
| `0x20` | `Pointers` | begins a pointer section |

Every legal combination, and no others:

| Value | Type | Header | Bits |
|---|---|---|---|
| `0x01` | `KEYVALUE` | 4 | `HasKey` |
| `0x09` | `KEYVALUE_ANC` | 8 | `HasKey HasAncestor` |
| `0x05` | `BIGKEYVALUE` | 24 | `HasKey IsBig` |
| `0x0D` | `BIGKEYVALUE_ANC` | 24 | `HasKey IsBig HasAncestor` |
| `0x03` | `DELETION` | 4 | `HasKey IsDelete` |
| `0x0B` | `DELETION_ANC` | 8 | `HasKey IsDelete HasAncestor` |
| `0x07` | `BIGDELETION` | 16 | `HasKey IsDelete IsBig` |
| `0x0F` | `BIGDELETION_ANC` | 16 | `HasKey IsDelete IsBig HasAncestor` |
| `0x10` | `COMMIT` | 8 | `SpanTerminator` |
| `0x14` | `COMMIT_LONG` | 24 | `SpanTerminator IsBig` |
| `0x12` | `ROLLBACK` | 8 | `SpanTerminator IsDelete` |
| `0x16` | `ROLLBACK_LONG` | 24 | `SpanTerminator IsDelete IsBig` |
| `0x20` | `PTRS32` | 8 | `Pointers` |
| `0x24` | `PTRS64` | 16 | `Pointers IsBig` |

- **F-12** The table above is normative: any byte not in it is invalid,
  including `0x00`. It is structured rather than arbitrary — exactly one of
  `HasKey`, `SpanTerminator` and `Pointers` is set, since they select the
  family; `HasAncestor` appears only with `HasKey`; `IsDelete` appears with
  `HasKey` or `SpanTerminator` but never with `Pointers`; `IsBig` may appear
  with any family; and bits `0x40` and `0x80` are reserved and always zero.
- **F-12a** The bits are meaningful in isolation, which is the point of
  encoding them this way. `type & IsBig` selects the wide layout in all three
  families, `type & HasAncestor` says whether the ancestor field is present, and
  `type & IsDelete` means negation whether the subject is a key or a span. A
  decoder reads the shape from the bits rather than from a lookup table.
- **F-12b** Each data shape has exactly two forms, one storing an ancestor and
  one omitting it. Nothing distinguishes a "create" at the record level, because
  nothing needs to (F-17).
- **F-12c** A 32-bit ancestor fits inside the padding the big forms already
  carry, so `HasAncestor` costs **nothing** there; in the short forms it adds 4
  bytes.

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

KEYVALUE_ANC (0x09)
  +0   1  type
  +1   1  keylen
  +2   2  vallen
  +4   4  ancestor generation
  +8   .  key NUL value NUL pad->8
  len = roundup8(8 + keylen + 1 + vallen + 1)

DELETION (0x03)
  +0   1  type
  +1   1  keylen
  +2   2  pad
  +4   .  key NUL pad->8
  len = roundup8(4 + keylen + 1)

DELETION_ANC (0x0B)
  +0   1  type
  +1   1  keylen
  +2   2  pad
  +4   4  ancestor generation
  +8   .  key NUL pad->8
  len = roundup8(8 + keylen + 1)

BIGKEYVALUE (0x05)
  +0   1  type
  +1   7  pad
  +8   8  keylen
  +16  8  vallen
  +24  .  key NUL value NUL pad->8

BIGKEYVALUE_ANC (0x0D)
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

BIGDELETION_ANC (0x0F)
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
short (8 bytes)                       COMMIT / ROLLBACK
  +0   1  type
  +1   3  span length
  +4   4  checksum

long (24 bytes)                       COMMIT_LONG / ROLLBACK_LONG
  +0   1  type
  +1   7  pad
  +8   8  span length
  +16  4  pad
  +20  4  checksum
```

- **F-19** The checksum covers the span's data bytes followed by the
  terminator's own bytes up to the checksum field.
- **F-20** Terminators are only ever found by scanning **forward** from the
  header. Nothing reads them backwards, because the pointer section is located
  by its own trailer (§4.9), so a long terminator needs no marker in its second
  half.
- **F-21** A `COMMIT` makes its span's records live. A `ROLLBACK` is a commit
  that says "ignore the records in this span", voiding them. An aborted
  transaction appends a `ROLLBACK`; without one, a later commit's span would
  enclose the aborted records and make them live.
- **F-22** Because the checksum covers the span **and** the terminator, a
  terminator reaching disk without its data fails validation and reads as
  absent. One `fsync` after the terminator is therefore sufficient, and no
  intra-transaction write barrier is needed.

### 4.8 The span chain

Spans exist only in **unordered** files. An in-order file has none (§4.9).

- **F-23** From the end of an unordered file's header onwards, the file is a flat
  sequence of spans. Each span is zero or more data records followed by exactly
  one terminator whose span length equals the span's data byte count and whose
  checksum validates. Every byte belongs to exactly one span or terminator: no
  gaps, no nesting.
- **F-24** An unordered file is **complete** at its last valid span, whether
  that is the end of the file or the point where a record or terminator fails to
  validate. Content beyond it is not part of the database.
- **F-24a** An in-order file has no equivalent notion, because it is written
  whole under a temporary name and renamed only once finished (D-21). A partial
  one is never referenced, so it is debris rather than a file that is complete
  early.
- **F-25** Visibility is per span, not a watermark: a rolled-back span may sit
  between two live ones, so a reader MUST replay spans in order and skip
  rolled-back ones.

### 4.9 The pointer section

An in-order file always ends with a pointer section followed by a 16-byte
trailer; an unordered file never has either. So the whole layout is:

```
in-order   [header][records][pointer section][trailer]
unordered  [header](span)*
```

An in-order file has no spans and no terminators. Every record in it is live by
construction, and it is written whole under a temporary name and renamed only
once finished (D-21), so a commit record would assert nothing that is not
already guaranteed.

The section is self-describing — its own type states whether it is narrow or
wide, and the count matches that width:

```
PTRS32 (0x20)                         narrow
  +0    1      type
  +1    3      pad
  +4    4      count (uint32)
  +8    4×N    record offsets (uint32)
        .      pad with zeroes to a multiple of 8

PTRS64 (0x24)                         wide
  +0    1      type
  +1    7      pad
  +8    8      count (uint64)
  +16   8×N    record offsets (uint64)
```

The trailer is a **fixed 16 bytes**, always, so it can be read without knowing
anything else about the file:

```
filesize-16   8   offset of the start of the pointer section
filesize-8    4   checksum of the records region
filesize-4    4   checksum of the pointer section
```

- **F-26** Pointers reference every record in the file, sorted by key
  ascending. Because a repack emits exactly one record per key (D-17), keys in
  an in-order file are unique and the array is a strict ordering.
- **F-26a** The trailer's back pointer is what locates the section, so nothing
  needs to be found by scanning backwards through records. The trailer's size is
  fixed and its back pointer is always 8 bytes wide even in a narrow file,
  because a variable-size trailer could not be read without first knowing which
  size it was.
- **F-26b** The pointer-section checksum covers everything from the start of the
  section up to the checksum field itself — the section, its padding, the back
  pointer, and the records checksum — following F-4 with no special case. The
  back pointer is read as plain data first, so there is no circularity.
- **F-26e** The records checksum covers the region from the end of the header to
  the start of the pointer section. A commit record would have carried this, and
  the commit record itself is redundant, but the coverage is not: without it a
  record body corrupted in place would go undetected, whereas the equivalent
  region of an unordered file is covered by its span terminator. Placing it in
  the trailer's otherwise-unused four bytes costs nothing, and F-26b means it is
  itself protected.
- **F-26f** The records checksum is verified lazily — by
  `zs_db_check_consistency`, or by a caller that chooses to — never on open,
  which stays O(1) (F-31). The pointer-section checksum *is* verified on open,
  because everything the file's structure depends on lives inside it.
- **F-26c** Encoding is canonical: `PTRS32` MUST be used when every record
  offset fits in 32 bits, and `PTRS64` otherwise. Since all records precede the
  section, that is equivalent to the section's own offset fitting in 32 bits.
- **F-26d** The narrow section is padded with zeroes to a multiple of 8 so the
  trailer begins 8-aligned (F-2). The pad is 0 or 4 bytes and the checksum
  covers it.
- **F-26g** `count` MAY be **zero**. An in-order file with no records is legal
  and expected — a repack that drops every key produces one (D-22) — and its
  layout is simply `[header][pointer section][trailer]` with an empty records
  region. Specifically:

  - the smallest valid in-order file is **96 bytes**: a 72-byte header, an
    8-byte `PTRS32` section with `count == 0`, and the 16-byte trailer. A file
    shorter than that cannot be a valid in-order file;
  - the width is `PTRS32`, since F-26c's condition holds vacuously when there
    are no offsets. An empty file is therefore byte-identical every time it is
    produced;
  - the records checksum covers zero bytes, so it takes the engine's value for
    empty input, not zero. Engine 0 writes zeros as always.
- **F-26h** An **unordered** file may equally hold no records: an active file
  that is only a header, or one whose every span was rolled back. Neither is an
  error.
- **F-27** Every pointer MUST be 8-aligned and lie between the header and the
  pointer section. With `count == 0` there are no pointers and the requirement is
  vacuous.
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
- **F-31** Opening an in-order file is O(1): validate the header, read the
  16-byte trailer, verify the pointer-section checksum, use the pointers.
  Records are bounds-checked on access, and their checksum is verified only on
  demand (F-26f).

## 5. Database layout

### 5.1 Directory contents

| Name | Mutability | Purpose |
|---|---|---|
| `zeroskip-<uuid>-<gen>` | append-only | unordered file, one generation |
| `zeroskip-<uuid>-<start>-<end>` | immutable | in-order file |
| `zeroskip.manifest` | replaced atomically | published file set |
| `zeroskip.tmp.<pid>.<n>` | transient | repack output and manifest staging |
| `zeroskip.lock` | never replaced or unlinked | holds `fcntl` locks |
| `zeroskip.index` | pure cache | key order for files without a pointer section; location not normative (D-13b1) |

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
- **D-11** The writer never appends a pointer section to an unordered file. When it
  moves on, the previous file simply stays unordered until a repack rewrites
  it. Key order for such a file lives in the shared index until then.
- **D-12** A writer MAY repack, so it may rewrite the file it has just left as
  its single-generation in-order form before exiting.

### 5.4 Shared index

- **D-13** `zeroskip.index` is `mmap`'d `MAP_SHARED`, holding
  key order for files that lack a pointer section — the active file and any
  unordered file not yet repacked — keyed by generation and stamped with the
  database UUID, the generation, and the offset it is valid to.
- **D-13a** It is a pure cache and MUST NOT be authoritative. Any process MAY
  discard it and rebuild by scanning. An implementation MUST validate its
  stamps before use and discard it on any inconsistency.
- **D-13b** Its internal layout is not part of this specification and may
  change without a format version bump. Only its invariants are specified.
- **D-13b1** Nor is its **location** normative. A regular file in the database
  directory is the portable default: it works identically on Linux, macOS and
  the BSDs, needs no configuration, occupies no global namespace, and is removed
  along with the database. Since it is never `fsync`ed (C-6b), its pages are
  written back lazily or not at all.
- **D-13b2** An implementation MAY place it on a **tmpfs** or in a POSIX shared
  memory object instead, which suits a pure cache — RAM-backed, no writeback,
  cleared by a reboot. Neither can be the *default*: there is no portable tmpfs
  path, macOS having no `/dev/shm`.
- **D-13b2a** For `shm_open` the database UUID supplies the name, and macOS's
  31-character `PSHMNAMLEN` is not an obstacle provided the UUID is encoded
  compactly: `/zs-` followed by base64url of the 16 raw bytes is 26 characters,
  and base32 is 30. Only the 36-character textual form fails to fit. The UUID also
  makes the global namespace collision-free. What remains is cleanup — an
  `shm_unlink` is needed when a database is destroyed, and an orphaned segment
  survives until the next reboot.
- **D-13b2b** Wherever the index lives, three things MUST hold:

  1. the staging file for a rebuild is on the **same filesystem**, or `rename`
     cannot switch it atomically (D-13i);
  2. the stamps of D-13a are sufficient to reject a **foreign or stale** index,
     since a location outside the database directory can outlive the database or
     be shared by unrelated ones;
  3. an index on a filesystem local to one host is **per-host**, so a database on
     shared storage may have several independent caches. That is harmless
     precisely because it is a cache, and it is the stamps that keep it so.

- **D-13b3** Anonymous shared memory is **not** an option for the shared index:
  `MAP_ANONYMOUS|MAP_SHARED` is shareable only by descendants of the process that
  created it, and the processes using one database are typically unrelated. It is
  the right choice for the **private** copy D-13g requires.
- **D-13c** Those invariants are that, for one file, the index supports all
  three of:

  1. **point lookup** — given a key, the offset of its newest committed record,
     or absent;
  2. **lower-bound seek** — given a key, a position from which ordered
     traversal begins at the first key greater than or equal to it;
  3. **ordered traversal** — successive keys in comparator order, each with the
     offset of its newest committed record.

  A hash table satisfies only the first, so the index MUST be an ordered
  structure. Requirement 3 is what makes an unordered file usable as a merge
  source (D-14e).
- **D-13d** The index reflects **committed spans only**, and for each key only
  its newest committed record. Building it therefore means replaying spans and
  skipping rolled-back ones (F-25), not simply walking every record in the file.
  An index built by naively scanning records would resurrect aborted writes.
- **D-13e** A reader's view of the index MUST be **stable for the lifetime of
  its transaction**, exactly as its view of the data files is (C-4). Correctness
  MUST NOT depend on timing: it is not acceptable for a writer's update to
  perturb a binary search or an ordered traversal already in progress.
- **D-13f** Only the **active file's** entry is mutable. Every other unordered
  file has stopped being written, so its index data never changes again and can
  be read in shared memory with no copying and no coordination. The stability
  problem is therefore bounded to one file, whose size is bounded by
  `rollover_size`.
- **D-13g** An implementation SHOULD satisfy D-13e by **copying the active
  file's array into private memory** when a transaction takes its snapshot,
  through the sequence counter of C-4d. That costs one bounded copy, requires no
  cooperation from the writer beyond maintaining the counter, and adds no shared
  mutable state that a reader must modify. The alternative — having the writer publish a fresh copy
  elsewhere in the index and leave the old one for readers — needs a scheme for
  reclaiming superseded copies, which is precisely the class of problem avoided
  for data files by relying on POSIX `unlink` semantics (C-4). There is no
  equivalent trick inside a shared mapping.
- **D-13h** An implementation MAY instead replace the whole index file by
  `rename`, in which case readers holding the old one mapped are unaffected for
  the same reason they are for data files. That handles growth and rebuilds but
  not incremental updates to the active file, so it does not remove the need for
  D-13g.

**Reclaiming entries for repacked files (D-13i).** When a repack turns unordered
files into an in-order one, their index entries become garbage. The protocol is:

1. The repack publishes a manifest that no longer lists those generations, and
   removes the files (D-23). **It does not touch the index.**
2. Their entries are now **dead**. A reader MUST ignore any index entry whose
   generation is not in its own manifest snapshot, so dead entries are inert —
   they waste space and nothing else.
3. The **writer**, holding the write lock, rebuilds the index when dead entries
   or total size pass a threshold: write a fresh index containing entries only
   for generations in the current manifest, then `rename` it into place. No
   `fsync` and no directory sync (C-6b) — the rename is for atomic visibility,
   not durability.
4. Readers holding the old index mapped are unaffected (D-13h), and the kernel
   reclaims the old inode once the last mapping closes.

- **D-13j** The repack cannot perform step 3 itself, even though it creates the
  garbage. Rebuilding means copying the active file's array, which is being
  mutated concurrently, and the repacker is forbidden from taking the write lock
  (C-1b). Deferring to the writer is not a stylistic choice: the writer is the
  only party that can safely read that array.
- **D-13k** Only a writer ever writes the index file, since a read-only open must
  have no side effects (R-3). Two consequences worth stating: rebuilds are
  serialised for free by the write lock, needing no additional lock; and a
  database that is only ever read never has its index populated, so every reader
  scans privately. That is the correct trade — a reader that wrote to shared
  state would no longer be read-only — and it costs nothing for a database with
  any write traffic at all.
- **D-13l** No step above requires reference counting or liveness detection. Dead
  entries are recognised by comparing against the manifest, and superseded index
  files are reclaimed by the kernel, so nothing has to be cleaned up when a
  process dies.

The layout is not normative (D-13b), but the shape that works best is worth
recording. Begin the file with a map from generation to an
`(offset, IsBig, count)` triple, then store each file's offsets **in exactly the
format an in-order file uses for its pointer section** (§4.9). One
implementation of "binary search a pointer array" and "walk a pointer array in
order" then serves both cases, differing only in the base address it is handed —
a pointer into the index rather than into a file's mapped pointer section.

### 5.5 Reading

Every read draws on the same set of **sources**, ordered newest to oldest:

| Priority | Source | Search primitive |
|---|---|---|
| highest | the current write transaction's uncommitted records | its private in-memory map |
| then | data files by `start` **descending** | see D-14b |
| lowest | the oldest file | |

- **D-14** Within a file the newest version of a key wins — the highest offset
  among committed spans. Across sources, the first record found in the order
  above wins. If that record is a deletion, the key does not exist.
- **D-14a** Point lookups, cursors and range scans MUST all resolve visibility
  by D-14, which is what makes it impossible for them to disagree (G-7).
- **D-14b** Searching one file for a key:

  | File kind | Method | Cost |
  |---|---|---|
  | in-order | binary search the pointer array, comparing the key at each probe | O(log n) comparisons |
  | unordered | point lookup in the index (D-13c) | as the index provides |

  Both MUST report "absent" for an empty source rather than misbehaving: a
  binary search over a zero-length array, and an index for a file with no
  committed records, are both ordinary cases (F-26g, F-26h).

- **D-14c** Ancestors are **not** consulted by any read. They exist solely for
  repacking (F-16), so a lookup never follows a chain.

**Point lookup (D-14d).** Walk the sources in priority order; in each, search
for the key by D-14b; stop at the first source that has it. That record decides
the answer — its value, or absence if it is a deletion. A source that does not
have the key is skipped, and no source may be skipped for any other reason.

An implementation MAY probe the first and last pointers before the rest, which
rejects an out-of-range key in two comparisons rather than the log₂n a plain
binary search would take to walk to an end. This is a search *strategy*, not a
way of avoiding the search: those two pointers still have to be dereferenced and
their keys compared, so it is the same kind of work, just less of it. It needs no
cached metadata and cannot change the answer.

The cost is therefore proportional to the **number of files**, which is why
keeping that count low is the point of the repack policy (D-16).

**Iteration (D-14e).** A cursor holds one **per-file cursor** per source, each
traversable in comparator order — for an in-order file by walking its pointer
array, for an unordered file by ordered traversal of its index (D-13c), and for
the write transaction by its own ordered map.

The per-file cursors are held in an array kept sorted by:

> **current key ascending, then generation descending.**

1. **Seek.** Seek every per-file cursor to the start point, so its current
   record is the first with key `>=` the start key — or mark it exhausted, which
   is immediately the case for a source holding no records (F-26g, F-26h). A
   binary search yields either an exact match or the preceding position, in
   which case one step forward gives this. A full scan seeks to the beginning.
   Then sort the array.
2. **Take.** The next record to emit is always **element 0**: O(1), with no
   comparison needed to find it.
3. **Skip stale duplicates.** Because equal keys sort by generation descending,
   every cursor positioned on the emitted key is **contiguous from the front**
   of the array, and element 0 is the newest. Advance elements 1, 2, … for as
   long as their current key equals the emitted one.
4. **Filter.** If element 0's record is a deletion, emit nothing. The key is
   consumed either way.
5. **Advance and re-sort.** Advance element 0, then move it down the array to
   its new position — its key only ever increases, so this is a single insertion
   into an already-sorted array, and it stops as soon as it is no longer greater
   than its neighbour.
6. **Bound.** For a prefix scan, stop when the emitted key leaves the prefix.

- **D-14f** Folding the tie-break into the sort is what makes step 3 safe.
  Advancing only the winning cursor would leave the same key at the head of
  another, which then emits an older version of a record already returned — the
  same class of bug as a point lookup and a scan disagreeing. Because equal keys
  are adjacent and newest-first, "advance while the next cursor's key matches"
  is both the complete fix and cheap.
- **D-14g** The write transaction's own records sort as though they had a
  generation above every file's, giving them highest priority for equal keys
  without a special case in the comparator.
- **D-14h** A per-file cursor never yields the same key twice: an in-order file
  holds one record per key (D-17), and an unordered file's index exposes only
  the newest committed record per key (D-13d). Duplicates therefore arise only
  *across* sources, which is exactly what step 3 handles.
- **D-14i** Picking the next record is O(1) and re-sorting one cursor is O(k)
  worst case, where *k* is the number of sources. A sorted array rather than a
  heap is the right shape because *k* is small by design (D-16) and usually the
  advanced cursor stays at or near the front, so the insertion terminates
  immediately. The sources are fixed for the cursor's lifetime, so no file can
  appear or vanish mid-scan (C-4).

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
- **D-20** Inputs are iterated in key order: from the pointer section where present,
  otherwise from the same index any reader of a pointerless file must build.
  There is nothing repack-specific about this.
- **D-21** The output is written to `zeroskip.tmp.<pid>.<n>` and `rename`d to
  `zeroskip-<uuid>-<start>-<end>` covering the entire range of every input,
  only once complete.
- **D-22** The output may legitimately contain **zero records**, in the form
  F-26g specifies — for instance
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
**C-4 Taking a snapshot.** The protocol is:

1. Read the manifest, noting its publish sequence *S*.
2. `open` and `mmap` every file it lists.
3. Re-read the manifest. If the sequence is no longer *S*, or if any `open` in
   step 2 failed with `ENOENT`, discard everything and restart from 1.
4. For each unordered file, obtain a stable view of its index entry by C-4d.
5. Set the active file's snapshot boundary per C-4e.

Everything the snapshot will read is immutable from here on, for the reasons
below. No lock is taken at any point.

- **C-4a Completeness.** Step 3 is a verify-after-read, and it is sound because
  a file is only ever removed under the packer lock *after* a publish (D-23), so
  a removal always advances the sequence. Observing an unchanged sequence across
  step 2 therefore proves no file was removed during it, and the opened set is
  exactly the published one. Conversely an `ENOENT` proves a publish happened, so
  the retry will see a higher sequence and a different set.
- **C-4b Manifest atomicity.** Reading the manifest needs no lock because it is
  replaced by `rename` (D-5): an `open` binds one inode, and the reader reads
  that version whole even if it is replaced meanwhile.
- **C-4c Immutability of what was opened.** In-order files are never modified.
  An unordered file that is not the active one is never appended to again. The
  active file *is* appended to, but only ever appended to (G-1), so every byte
  below the snapshot boundary is immutable — a prefix of an append-only file is
  stable by construction. Growth beyond the boundary is invisible: the mapping
  covers the prefix and the reader never looks past it.
- **C-4d Index entry stability.** An index entry for the active file changes
  under the reader (D-13f), so it MUST be read through a **sequence counter**:
  read the entry's sequence, read `valid_to` and copy the array, then re-read the
  sequence and retry if it changed. The writer increments the counter before and
  after mutating. This is the same verify-after-read shape as step 3, and like it
  requires no lock — only that the reader be willing to retry.
- **C-4e The boundary, and why the index cannot simply be clamped.** The index
  holds only the *newest* offset per key (D-13d). If a reader chose a boundary
  earlier than the index's `valid_to` and then ignored entries pointing past it,
  a key updated after that boundary would appear **absent** — its newer offset
  discarded and its older version unreachable, because the index never recorded
  it. Clamping is therefore wrong. Instead the reader **adopts** the index's
  `valid_to` as its boundary and scans forward from there to the last valid
  terminator, merging anything newer into its private view. Both are commit
  boundaries, so adopting the later one is a consistent snapshot; it is simply a
  slightly newer one than the reader might have taken.
- **C-4f Concurrent visibility.** A reader scanning the active file may meet a
  span the writer is still writing. The terminator's checksum covers the span's
  data (F-19), so a terminator whose data is not yet fully visible fails
  validation and reads as absent — the reader stops there, exactly as it would
  after a crash. This is what makes lock-free reading of a live file safe, not
  merely crash recovery: the checksum supplies the ordering guarantee that no
  memory barrier is available to provide across independent processes sharing a
  mapping.
- **C-4g Lifetime.** Once its descriptors are open a packer may (subject to
  D-23) `unlink` superseded files immediately: the kernel keeps each inode alive
  until the last descriptor *and mapping* is gone. There is no reference table
  and nothing to clean up when a process dies.
- **C-4h Termination.** Steps 1–3 retry only when a publish intervenes, and step
  4 only when the writer touches that entry. Both are structural events, not
  per-operation ones, so retries are rare; each costs only the `open` calls or
  one array copy. An implementation SHOULD bound the retry count and report
  `ZS_AGAIN` rather than spin indefinitely, so a pathological publish rate
  surfaces as an error instead of a livelock.
- **C-5** The accepted cost of C-4g is that disk space is held until the last
  reader holding an old snapshot exits.
- **C-6 Directory durability.** After creating a **data file** (a new active
  file, or a repack output) and after renaming a repack output or the manifest,
  the implementation MUST `fdatasync` the **directory**, otherwise the name may
  be absent after a crash even though the file's contents are durable.
- **C-6a** A directory sync is **not** required after `unlink`. If a removed
  name reappears after a crash the file is unreferenced debris, which readers
  ignore (D-7) and a later repack removes again (D-23).
- **C-6b** The **index** is never `fsync`ed and its rename needs no directory
  sync, because it is a pure cache (D-13a): a crash that loses it, or leaves it
  torn or absent, costs a rebuild and nothing else. Syncing it would buy nothing
  — there is no state in it worth preserving that cannot be recomputed from the
  data files.
- **C-6c** What replaces that durability is **validation**. Because a torn index
  is now a state the design deliberately permits, an implementation MUST be able
  to detect one: the stamps of D-13a are not merely a version check but the
  mechanism that makes skipping `fsync` safe, and they MUST be strong enough to
  reject a partially written or partially visible index rather than read it as
  valid. `rename` remains necessary for a different reason — it gives readers an
  atomic switch between whole index files — but atomicity here is about
  visibility, not durability.
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
(`ZS_NOTFOUND = -5`, `ZS_LOCKED = -4`, `ZS_BADFORMAT = -7`, `ZS_FULL`,
`ZS_AGAIN`, …).

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
`0D 0A`, and byte 0 replaced by the UTF-8 substitution character's encoding
`EF BF BD` (F-6a) — each rejected. Also that the 16 bytes fail a UTF-8 validity
check, so the property is asserted rather than merely believed. Read and write versions above the library's rejected
appropriately, and a file readable-but-not-writable accepted read-only (F-7).

**T-2a The trailer.** Opening an in-order file depends entirely on it, so: the
16-byte trailer read without prior knowledge of the file, the back pointer
locating the section, and the checksum verified over section-through-back-pointer
(F-26b). Negatively — a back pointer past the end of the file, before the
header, not 8-aligned, or pointing at a byte that is not a `PTRS32`/`PTRS64`
type; a file shorter than header plus trailer; and a corrupted pad byte — each
rejected rather than read. The records checksum verified on demand and asserted
to catch a record body corrupted in place (F-26e), which nothing else would
detect in an in-order file.

**T-2b Type byte validity.** All 256 byte values fed as a record type, asserting
exactly the 14 in F-12's table are accepted and the other 242 rejected. A
bitfield admits far more values than it defines, so the near-misses matter most:
two family bits set at once, `HasAncestor` without `HasKey`, `IsDelete` with
`Pointers`, and either reserved bit set. Each is a plausible result of a single
flipped bit in a valid type, and each MUST be rejected rather than
half-interpreted.

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
scan, cross-checked against each other — the direct test for G-7 and for D-14f.
The generator MUST produce the shapes that stress the merge: the same key live in
several files at once, a key deleted in a newer file and present in older ones, a
key whose only version is in the oldest file, and keys adjacent in comparator
order but split across files. Runs are repeated with the file set arranged so
sources hold overlapping key ranges, since a merge that mishandles ties only
fails when ties occur.

**T-5b Cursor mechanics.** The sorted-array invariant of D-14e asserted after
every step: that the array is ordered by key ascending then generation
descending, that cursors on equal keys are contiguous from the front with the
newest first, and that element 0 is always the correct next record. Then the
failure D-14f describes, constructed directly — the same key present in three
files at once, asserting it is emitted once from the newest and that advancing
only element 0 would have emitted it three times. Plus exhaustion handling: a
source exhausted at seek, one exhausted mid-scan, and every source exhausted; a
cursor seeked past every key; and a start key that exactly matches a record in
some sources but not others (D-14e step 1).

**T-5a Read paths under every file arrangement.** The same assertions driven
against a database deliberately arranged as: one unordered file only; several
unordered files; one in-order file only; a mixture; after a repack that collapsed
some but not all files; **and with an empty in-order file among populated ones**,
since a zero-pointer source is where a binary search or a cursor seek is most
likely to go wrong (F-26g). Each arrangement exercises a different
combination of D-14b search primitives, and the answers MUST be identical
throughout. If the first-and-last probe of D-14d is implemented, results are
compared with it disabled to confirm it changes nothing — including for keys
below the first and above the last.

**T-6 File states and encoding.** That `end == 0` and `end != 0` files are
recognised solely from the header and that a pointers block is present exactly
when `end != 0`; that an in-order file's pointers are strictly increasing by key
(F-28); that `PTRS32` and `PTRS64` are each honoured for the width they state,
including a hand-constructed `PTRS64` file so the wide form is covered without
writing 4GB of real data, and that a file whose offsets all fit is written as
`PTRS32` (F-26c), plus the 0-and-4-byte padding cases (F-26d); that a zero-record
in-order file round-trips as exactly 96 bytes, is written as `PTRS32`, and
carries the engine's empty-input checksum for its records region (F-26g); that a
database consisting only of such a file reads as empty and iterates to zero
records; and that every row of F-17's table round-trips — in particular that
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
mid-repack; after the pointer section but before the trailer; leaving a non-8-aligned file
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
writer both proceeding, with publish serialised. Index reclamation (D-13i): after
a repack, dead entries asserted inert and ignored; a rebuild asserted to drop
exactly them; a reader mapping the pre-rebuild index asserted unaffected; and,
since the index is never synced (C-6b), an index truncated or garbled at a random
offset asserted to be rejected and rebuilt rather than read — the case that would
otherwise have justified an `fsync`.

**T-10b The snapshot protocol.** Each step of C-4 attacked directly, since these
fail only under concurrency. A reader interrupted between reading the manifest
and opening files, with a repack completing in the gap, asserting the retry
observes a higher sequence and succeeds (C-4a). A file unlinked between steps 2
and 3, asserting `ENOENT` triggers a retry rather than a partial snapshot. A
reader holding a snapshot while the writer commits repeatedly, asserting bytes
below its boundary never change and growth above it is invisible (C-4c). A writer
committing while a reader copies an index entry, asserting the sequence counter
forces a retry and the copy is never torn (C-4d). A reader whose index `valid_to`
is ahead of where it would otherwise have stopped, asserting it adopts the later
boundary and that a key updated past an earlier boundary is **not** reported
absent — the clamping bug C-4e describes, constructed to fail if clamping is ever
implemented. And a writer killed mid-span while a reader scans, asserting the
reader stops at the last valid terminator (C-4f).

**T-10a Index stability under a writing writer.** A reader mid-scan while a
writer commits repeatedly to the active file, asserting the reader's results are
exactly its snapshot throughout — the direct test for D-13e, and one that fails
only under concurrency, so it runs with the writer committing in a tight loop for
a bounded time rather than a fixed number of operations. Repeated with the reader
holding a cursor across many commits, and with the writer rolling over to a new
generation mid-scan. Also asserts that a non-active unordered file's index region
is never written after the file stops being active (D-13f), by checkpointing
those bytes and comparing.

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
2. **Index rebuild threshold.** D-13i specifies how the index is reclaimed but
   not when: the dead-entry or size threshold that triggers a writer-side
   rebuild is a tuning constant, and picking it wants a measurement rather than
   an argument.
