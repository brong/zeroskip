# zeroskip: design specification

Status: draft for review
Date: 2026-08-06

`libzeroskip` is an append-only ordered key-value store: a directory of
immutable and append-only files, with lock-free readers and a single writer.

Requirements are labelled (`F-n` format, `D-n` database, `C-n` concurrency,
`R-n` recovery, `A-n` API, `T-n` tests) so the conformance suite can cite
them. **MUST** and **MUST NOT** are normative.

## 1. Purpose and scope

This document specifies zeroskip so that **independent implementations, in
different languages, interoperate on the same database concurrently**. Keys are
ordered by a comparator, byte order by default.

**What is normative for every implementation:** the on-disk format (§4), the
database layout and its algorithms (§5), the concurrency and durability
protocol (§6), and open and recovery (§7). Two implementations that agree on
these can share a directory, read each other's files, and lock against each
other.

Salvage (§9) is likewise optional, and normative only for an implementation
that offers one — what such a tool must not do matters more than whether it
exists.

The pointer table cache (§8) is normative *when present* — implementations that
use one must agree on its bytes, or they will reject each other's work — but it
is optional and never load-bearing. A conforming implementation MUST produce
identical results with it absent.

**What is a binding, not a contract:** §10 gives a C API. Its *semantics* are
normative — what `store` with no value means, what a transaction makes visible,
what each flag does — but its spelling is not. An implementation in another
language SHOULD express the same semantics idiomatically.

- **G-0** Nothing in the format depends on `mmap`, on pointer-sized integers,
  or on any CPU feature. An implementation MAY read files with ordinary reads
  and copy data out; `mmap` is an optimisation the format permits, not one it
  requires. Only §10's zero-copy pointer lifetimes (A-4) assume it, and that is a
  binding-level promise a copying implementation simply makes differently.
- **G-0a** Every integer in every structure is little-endian (F-1), including
  lengths, counts, generations and offsets. A header checksum will not catch a
  wrong byte order, since it is computed over whatever was written.
- **G-0b** Any arithmetic on a length, count or offset **read from a file** MUST
  be overflow-checked before use. `keylen + vallen + 2` and
  `offset + record_length` are attacker- and corruption-controlled; wrapping them
  turns a bounds check into a bypass. Languages differ here — C is undefined,
  Rust wraps in release and panics in debug, Go wraps silently — so the
  requirement is on the implementation, not on the language.

The design rests on one invariant, without exception: **nothing is ever written
except by appending to a file or by creating a new file.** No file is ever
modified in place or truncated, and there is no mutable object of any kind — no
manifest, no shared cache. Files are created, appended to, and eventually
removed once something else holds their data.

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
| complete at *n* | the logical end of an unordered file: the offset after its last valid span, which may be short of the file's physical end |
| clean | an unordered file whose complete point *is* its physical end — nothing follows the last valid span |
| ancestor | the absolute generation at which the shadow cast by a record hits the previous record for that key |
| shadowed | a record superseded by a later record for the same key, anywhere |
| tombstone | a deletion record — data that hides every older version of its key (D-14), not mere absence |
| file set | the data files present in the directory, derived from their names alone (D-2) |
| resolved set | the file set after overlap resolution (D-5): the files a reader actually consults |
| tiling | the property that the resolved set covers generation 1 through the newest contiguously, each generation exactly once (D-6) |
| staging name | the dot-prefixed name a new file is built under, invisible to the fileset scan, until rename publishes it (C-3) |
| publish | make a completed structure visible in one atomic `rename` from its staging name; nothing is ever visible half-written |
| replay | walking an unordered file's span chain from the header (or a known span boundary) forward, verifying each terminator, to find its records and its complete point (F-20–F-24) |
| snapshot | the fixed set of files, with their indexes, that one read observes — taken lock-free by the C-4 protocol, private to its holder |
| handle | one process's open database object; not thread-safe, and never two writers in one process (G-5) |
| rollover | a writer moving to a new active file once the current one exceeds `rollover_size` (D-9a) |
| conversion | rewriting one non-active unordered file as an in-order file covering the same generation (D-12) |
| repack | merging adjacent in-order files into one file covering their combined range (D-16–D-23) |
| seal | converting the active file in place, so every file in the database is in-order (D-25) |
| compaction | repacking every maximal run of adjacent in-order files, aiming for a single file (D-26) |
| salvage | rebuilding whatever is readable from a damaged directory into a new database, never writing the source (§9) |
| resync | salvage scanning forward after damage for the next span it can verify (S-7) |
| private index | the in-memory ordered index a snapshot builds over an unordered file's committed records (D-13); private to the process that built it |
| pointer section | the sorted array of record offsets, plus trailer, that an in-order file carries (F-26) |
| pointer table | an optional cached private index for an unordered file, persisted outside the database (§8) |
| pending array | a write transaction's uncommitted records, held sorted in memory until commit (A-1a); the highest-priority read source (D-14) |
| source | anything a read draws records from: the pending array, then each data file newest to oldest (D-14) |
| merge | the traversal over all of a cursor's sources that implements D-14e's six steps |
| arm | one per-source cursor inside a merge; the sorted array of arms is what D-14e's steps operate on |
| exhausted | an arm with no record at or after its position; exhausted arms sort last (D-14e step 1) |
| yield | hand one record to the caller; a cursor's unit of progress (D-14j) |
| stale duplicate | the emitted key surfacing again at an older arm's head, consumed without being yielded (D-14e step 3, D-14f) |
| liveness | which writes made during a traversal a cursor observes; depends on how it was opened (D-14j) |
| handle-live | a cursor from the non-transactional forms, which observes commits made through its own handle as it goes (D-14j) |
| resume point | the key a refreshed cursor resumes strictly after: the last key it yielded, or before anything has been yielded, the key it was opened at (D-14j-b) |
| refresh | a cursor noticing a change it is allowed to observe and re-seeking its arms to the resume point (D-14j) |

The two file kinds are exhaustive and distinguishable from the header alone:
**`end == 0` means unordered with no pointers; `end != 0` means in-order with
pointers.** A reader therefore always knows, before reading anything else,
whether a pointer section must be present.

## 3. Guarantees

- **G-1 Append-only.** No committed byte is ever mutated and no file is ever
  truncated. A file is renamed exactly once, from its staging name to its final
  name, which is the instant it joins the file set (C-3); once named, it is never
  renamed again.
- **G-2 Commit atomicity.** Once `zs_txn_commit` returns `ZS_OK`, the whole
  transaction is visible to new readers and, under default durability,
  survives a crash. A crash exposes exactly a prefix of committed
  transactions — never a partial one.
- **G-3 Always reopens.** Any state a crash can produce MUST open in bounded
  time and expose the committed data. Corruption may cost uncommitted data;
  it MUST NOT cost committed data, hang, crash, or read out of bounds.
- **G-4 Snapshot isolation, lock-free reads.** An **explicit** read transaction
  sees a fixed snapshot and takes no lock. Readers never block a writer; a writer
  never blocks readers.

  The non-transactional forms are deliberately not fixed in the same way: they
  observe writes committed through their own handle as they go (D-14j), because
  a traversal whose callback modifies the database is an ordinary pattern and a
  fixed view makes it silently miss its own work. Nothing about that costs a
  lock or a syscall — see D-14j for why.
- **G-5 One writer.** At most one writer per database, enforced by an `fcntl`
  byte-range lock. Because the kernel releases `fcntl` locks on process death, a
  killed writer never blocks the next one; no lock state can outlive a process.
  A handle is not thread-safe. Two write handles on one database within a
  process are excluded from each other by C-1j, so the second blocks or reports
  `ZS_LOCKED` exactly as a second process would — but that is exclusion between
  *handles*, not thread safety: two threads sharing one handle remain the
  caller's problem, and no lock in this specification helps them.
- **G-6 No shared mutable state.** Nothing a reader may be reading is ever
  rewritten beneath it: files are append-only, a new file is published by
  `rename`, and every index is private to the process that built it. There is no
  manifest and no shared cache, so correctness cannot depend on one, and nothing
  needs cleaning up when a process dies. The optional pointer table cache (§8)
  does not weaken this: it lives outside the database, its tables are published
  by `rename` and never modified, and results MUST be identical with it absent.
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
- **F-5b** Engine 1 is **`XXH3_64bits` with the default seed of 0**, and the
  stored checksum is the **low 32 bits** of the 64-bit result — that is,
  `(uint32_t)(h & 0xFFFFFFFF)` — written little-endian like every other integer
  (F-1). Both the seed and which half is kept must be pinned or two
  implementations would produce different bytes from the same input.
- **F-5b1** xxHash is used through its vendored reference implementation, whose
  scalar path is portable C; any SIMD acceleration within it is an internal
  optimisation of the same function, not a separate code path that could be
  absent on a given platform. The resulting value MUST be bit-identical
  everywhere, which the golden corpus pins (T-1).
- **F-5c** Engine 0 weakens G-2 and G-3, because F-22's property — that a
  terminator reaching disk without its data fails validation — depends on a
  real checksum. With engine 0 a torn tail is undetectable. It exists for
  testing and for callers with durability guarantees elsewhere.
- **F-5d** Engine 2 makes a file readable only by a caller supplying the same
  function, so the conformance corpus covers engines 0 and 1 only.
- **F-5e** `ZS_NOCSUM` is distinct from engine 0: it skips verification of
  **record** checksums at materialization (F-32a) — checksums that are
  nonetheless written — and nothing else. It is a **read-path** flag only: an
  operation that writes a new file from existing records MUST still verify its
  inputs (D-20b). Span and terminator checksums are outside its reach:
  **verification rides indexing**. A span's checksum MUST be verified by
  whoever adds that span to an index — the replay of C-4 step 4 — in every
  mode, because the replay is what decides which records exist, and a
  post-crash reopen under relaxed durability (C-7c) can meet a terminator
  whose data never landed; accepting it on the strength of its length field
  surfaces garbage as committed records, which no read-time flag was ever
  meant to permit. Conversely a span already indexed is not re-verified: a
  pointer table carries its builder's verification (P-11's flags bit 4), and
  a writer's own fold (D-13b) indexes spans whose checksums it computed
  itself.

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
- **F-7a** Version 2 is the first published version: a conforming writer
  writes `version_read = version_write = 2`, and a reader MUST refuse
  `version_read` below 2 as well as above its own. Version 1 was never
  released, so this is a pre-release clean break — not a compatibility path
  dropped for a format that shipped — made necessary by the per-record
  checksum (F-32): a version-1 record has no trailing checksum field to read.
- **F-8** Reserved fields MUST be written as zero and MUST be ignored on read.
  Compatibility decisions belong to the version fields, not to reserved bytes.
- **F-9** Generations start at 1, so `end == 0` is never a legitimate
  generation and unambiguously marks an unordered file.
- **F-10** An unordered file holds **exactly one** generation: `start` is that
  generation and `end == 0`. An in-order file produced from inputs spanning
  generations *i*..*j* has `start == i` and `end == j`.
- **F-11** Every file of a database MUST carry the same UUID and the same
  comparator name. The comparator determines key order and hence the meaning
  of the pointer section, so it must be recorded per file — there is no manifest
  to hold it (§5.2). Opening a database whose files disagree, or whose comparator
  differs from the caller's, is an error.
- **F-11a The default comparator.** Compare `min(alen, blen)` bytes as
  **unsigned** octets; if they differ, that decides. If they are equal, the
  **shorter key sorts first**. Equal length and equal bytes means equal keys.
  Nothing here may be left to the C library: `memcmp` is only guaranteed to
  return a sign, and `char` signedness varies by platform, so a comparator that
  compares `char` directly orders keys differently on ARM than on x86 and
  produces incompatible pointer sections.
- **F-11b** The default comparator's recorded name is the ASCII string
  `memcmp`, NUL-padded to 16 bytes. A caller supplying its own comparator MUST
  supply a name; names are compared byte-for-byte, and an empty name is invalid.
  Two implementations must agree on this string or neither will open the other's
  database.

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
- **F-12a** Each bit is meaningful in isolation: `type & IsBig` selects the wide
  layout in all three families, `type & HasAncestor` says whether the ancestor
  field is present, and `type & IsDelete` means negation, whether of a key or of a
  span. A decoder reads a record's shape from the bits.
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
  +0      1  type
  +1      1  keylen
  +2      2  vallen
  +4      .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(4 + keylen + 1 + vallen + 1 + 4)

KEYVALUE_ANC (0x09)
  +0      1  type
  +1      1  keylen
  +2      2  vallen
  +4      4  ancestor generation
  +8      .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(8 + keylen + 1 + vallen + 1 + 4)

DELETION (0x03)
  +0      1  type
  +1      1  keylen
  +2      2  pad
  +4      .  key NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(4 + keylen + 1 + 4)

DELETION_ANC (0x0B)
  +0      1  type
  +1      1  keylen
  +2      2  pad
  +4      4  ancestor generation
  +8      .  key NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(8 + keylen + 1 + 4)

BIGKEYVALUE (0x05)
  +0      1  type
  +1      7  pad
  +8      8  keylen
  +16     8  vallen
  +24     .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(24 + keylen + 1 + vallen + 1 + 4)

BIGKEYVALUE_ANC (0x0D)
  +0      1  type
  +1      3  pad
  +4      4  ancestor generation
  +8      8  keylen
  +16     8  vallen
  +24     .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(24 + keylen + 1 + vallen + 1 + 4)

BIGDELETION (0x07)
  +0      1  type
  +1      7  pad
  +8      8  keylen
  +16     .  key NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(16 + keylen + 1 + 4)

BIGDELETION_ANC (0x0F)
  +0      1  type
  +1      3  pad
  +4      4  ancestor generation
  +8      8  keylen
  +16     .  key NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(16 + keylen + 1 + 4)
```

- **F-15** Encoding is canonical: an implementation MUST use the short form
  whenever `keylen <= 255` and `vallen <= 65535`; MUST use the short terminator
  whenever the span is `<= 0xFFFFFF` bytes; and MUST select between the
  ancestor-storing and ancestor-omitting forms exactly as F-17 requires; and MUST
  choose the pointer width by F-26c; and the stored checksum (F-32) MUST be
  correct under the containing file's engine — a checksum that does not match
  is corruption, never an alternative encoding of the same record. Output
  bytes are therefore determined by the logical contents together with what
  the file already holds. The big form is chosen by key or value length only,
  never by the ancestor, which is 4 bytes whenever it is present.

### 4.6 Ancestors

- **F-16** Every record that casts a shadow MUST know the generation at which
  that shadow hits the previous record for its key — either its own generation
  or an earlier one. An update or deletion stores it; a create stores nothing,
  its ancestor being implicitly its own generation.
- **F-16a** The stored value is the **`start` of the range of the file holding
  the superseded record** when the shadow was cast — not its `end`. Since
  `start <= end`, D-19's containment test then errs toward retaining a tombstone
  rather than dropping one, so an imprecise ancestor costs disk space and not
  correctness.
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
  terminator that reaches disk without its data fails validation and reads as
  absent. A torn tail is therefore always detectable, which recovery (F-24) and
  concurrent reading (C-4f) both depend on. Durability is separate, and requires
  the two gates of C-7.

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
- **F-26c** Encoding is canonical: `PTRS32` MUST be used when every record
  offset fits in 32 bits, and `PTRS64` otherwise. Since all records precede the
  section, that is equivalent to the section's own offset fitting in 32 bits.
- **F-26d** The narrow section is padded with zeroes to a multiple of 8 so the
  trailer begins 8-aligned (F-2). The pad is 0 or 4 bytes and the checksum
  covers it.
- **F-26e** The records checksum covers the region from the end of the header to
  the start of the pointer section, so a record body corrupted in place is
  detectable. F-26b covers the field itself.
- **F-26f** The records checksum is verified lazily — by
  `zs_db_check_consistency`, or by a caller that chooses to — never on open,
  which stays O(1) (F-31). The pointer-section checksum *is* verified on open,
  because everything the file's structure depends on lives inside it.
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
- **F-32** Every data record ends in a 4-byte checksum: the last 4 bytes of
  the padded record, computed by the containing file's engine (F-5a) over
  `[0, len-4)` — type, lengths, ancestor, key, value, and padding. This is
  the format's one checksum convention, stated once: **every checksum is the
  last field of the thing it covers, and covers everything before it**
  (header F-4, terminator F-19, trailer F-26b, records F-32).
- **F-32a** A record's checksum MUST be verified when the record is
  materialized for a caller — a lookup result or a cursor yield — unless the
  handle was opened `ZS_NOCSUM` (F-5e). The failure is reported for that
  record alone; other records remain readable. A cursor verifies the record
  it takes from the head of the merge even when a bound (such as a prefix)
  then ends the scan instead of yielding it: converting a corrupt record into
  a silent early end of traversal would hide exactly what this exists to
  surface.
- **F-32b** A record's checksum MUST NOT be verified during span replay
  (F-24) or pointer-section load. Replay completes a file at its first
  invalid record, discarding everything after it — so verifying there turns
  one flipped byte into the loss of every later record, a G-3 violation. A
  record inside a valid span whose own checksum fails is in-place corruption,
  detected at materialization.
- **F-32c** A record copied byte-for-byte keeps a valid checksum only when
  the output file's engine matches the input's. A writer copying records
  into a file under a different engine MUST re-encode them (D-20b already
  requires the source be verified first).

## 5. Database layout

### 5.1 Directory contents

| Name | Mutability | Purpose |
|---|---|---|
| `zeroskip-<uuid>-<gen>` | append-only | unordered file, one generation |
| `zeroskip-<uuid>-<start>-<end>` | immutable | in-order file |
| `zeroskip.tmp.<pid>.<n>` | transient | staging for a repack or conversion output |
| `zeroskip.lock` | never replaced or unlinked | holds `fcntl` locks |
| `zeroskip.cache/` | directory | opt-in pointer table cache (§8, P-2b) |

- **D-0** The `<uuid>` in a filename is the **36-character lowercase hyphenated
  RFC 4122 form** of the header's 16-byte UUID, for example
  `4941da54-9406-4faa-a457-c4b65beae3eb`. Lowercase, against the uppercase
  generations of D-1.
- **D-1** Generations in filenames are **uppercase hexadecimal, zero-padded to
  8 digits**, so a file holding the first ten generations is
  `zeroskip-<uuid>-00000001-0000000A`. Eight hex digits is exactly the range of
  a 32-bit generation, so every representable generation has a name and the
  width never needs to change. Fixed width also keeps lexical and numeric order
  identical, and hexadecimal keeps names short.
- **D-1a** Data files carry **no extension**. An unordered file's name is
  therefore a strict **prefix** of the in-order name covering the same generation —
  `...-00000005` against `...-00000005-00000005` — and so sorts before it, which
  D-5's rule requires. Any suffix sorting after `-` would reverse that pair.
- **D-2** `zeroskip-*` matches data files only and `zeroskip.*` matches
  metadata, so both sets are prefix-globbable and shell-completable. Staging
  names begin `zeroskip.` and therefore never match the data-file pattern.
- **D-3** `zeroskip.lock` MUST be a distinct file that is never replaced.
  `fcntl` locks attach to an inode, so locking a file that is replaced by
  `rename` would silently lose mutual exclusion.
- **D-3a** It is created with the database (D-8a), and is created on open with
  `O_CREAT` if absent so an existing database is never unopenable for want of it.
  Concurrent creation is harmless: `O_CREAT` on one path yields one inode, so every
  process ends up locking the same object.
- **D-3b** It MUST NOT be unlinked — not by the library, and not by anything
  else. Unlinking it while processes hold locks is the one way to break mutual
  exclusion from outside: the holders keep locking the removed inode while a new
  process creates a fresh one and locks that, so **two writers each believe they
  hold the write lock** and append to the same file. This is the same failure mode
  as mixing `flock` with `fcntl` (C-1e) and equally silent. It is worth saying
  plainly because an empty file named `*.lock` is exactly what cleanup scripts
  delete.
- **D-3c** The lock file is empty and its contents are never read. Nothing about
  the database is stored in it, so it carries no version, no pid and no state to
  become stale.

### 5.2 The file set

There is no manifest. **The directory is the file set.** Filenames carry each
file's generation range (D-1), so one `readdir` yields the set and every range
without opening a single file.

- **D-4** A file participates if its name matches `zeroskip-<uuid>-*` for this
  database's UUID. Staging files and other databases' files are ignored by
  construction.
- **D-4a** On first open the UUID is not yet known, so it is **discovered**: take
  the names matching `zeroskip-*`, parse the UUID from each, and require they all
  agree. Disagreement means two databases' files have been mixed into one
  directory and MUST be an error rather than a choice of majority — silently
  adopting one would read half a database and call it whole. A directory with no
  data files has no UUID, which is the empty case D-8a handles.
- **D-5 Resolution by scan.** An output is renamed into place before its inputs
  are removed, so a scan legitimately sees overlapping files. **An overlap is
  never an error — it is resolved, not rejected**, by a single sweep over the
  sorted names:

  > Start at the lowest generation present. Repeatedly take the **last** file
  > whose `start` equals the current generation, then set the current generation
  > to that file's `end + 1` (or `start + 1` for an unordered file). Stop when no
  > file starts at the current generation.

  The files taken are the resolved set; every other file is superseded, and is
  ignored for reading and removable under D-23.
- **D-5a** One rule handles every overlap that can occur, because the naming
  scheme arranges for the correct file to sort last in both cases:

  | Situation | Files sharing a `start` | Last, and correct |
  |---|---|---|
  | repack output `[1-4]` present with inputs `[1-1]`…`[4-4]` | `00000001-00000001`, `00000001-00000004` | `[1-4]` — fixed-width hex makes lexical order numeric, so the widest `end` sorts last |
  | conversion output present with its input | `00000005`, `00000005-00000005` | the in-order file — the unordered name is a prefix, so it sorts first |
  | both at once: unordered *N*, `N-N`, and a wider `N-M` | `00000005`, `00000005-00000005`, `00000005-00000009` | `[5-9]`, the widest |

- **D-5b** The rule requires an unordered file's name to sort before the in-order
  name for the same generation, which D-1a's naming provides.
- **D-5c** A **partial** overlap, where neither range contains the other, cannot
  arise from any legal sequence and MUST be reported as corruption rather than
  resolved.
- **D-6 Completeness.** A set is complete if and only if the scan of D-5 consumes
  every generation from the lowest present through the active file's, leaving no
  gap. The resolved ranges therefore **tile a contiguous interval**, from the oldest surviving generation through the active
  file's. This is the whole test. No sequence number, timestamp or publication
  record is required, because tiling means every generation is accounted for, and
  therefore nothing committed is missing.
- **D-7** `readdir` is not atomic, so a scan may miss an entry and produce a set
  that does not tile. That MUST be retried. A set that is *stale* but tiles is
  not an error — it is simply an older snapshot, and every snapshot is an older
  snapshot of something.
- **D-8a Creating a database.** With `ZS_CREATE` and no existing directory, or a
  directory holding no data files, an implementation creates the directory, the
  lock file (D-3a), generates a UUID, and creates generation 1 as the active file — a 72-byte header
  and no spans, which F-26h makes a legal empty file. Without `ZS_CREATE` this is
  `ZS_NOTFOUND`. The UUID's value is arbitrary and opaque; only its 16-byte
  encoding (F-11) and its textual form in filenames (D-0) are fixed.
- **D-8** Creating a file **is** publishing it, since the directory is the truth.
  There is no window in which a generation has been allocated but is invisible, so
  the highest generation present cannot regress (D-9b).

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
- **D-9b** The next generation is one above the highest present in the directory.
  No counter is stored, and none is needed: a file is visible the moment it is
  created (D-8), and is only removed once an enclosing file covers its
  generation (D-23), so the highest generation present never regresses and a
  generation can never be reissued.
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
- **D-10a** A **non-active** file with an invalid header MUST be **reported**
  through the error callback, and its generation treated as holding no records.
  It MUST NOT be fatal. Discarding the file still requires an explicit tool
  action.
- **D-10b** An earlier version of D-10a made this fatal, which contradicts D-10
  and breaks G-3. D-10 directs a writer to move on from an unclean active file;
  the instant it does, that file becomes non-active, so a fatal rule turns the
  **first write after an ordinary crash** into a permanently unopenable database.
  Tolerating it costs nothing that was not already lost, because an invalid
  header means no record in that file is recoverable either way — whereas
  refusing to open costs every other file too. "Silently" is the hazard the rule
  guards against, and reporting addresses it.
- **D-11** The writer never appends a pointer section to an unordered file. When
  it moves on, the previous file simply stays unordered until it is converted.
- **D-12 Immediate conversion.** A writer that finds a **non-active unordered
  file** MUST convert it to its single-generation in-order form —
  `<uuid>-N` becomes `<uuid>-N-N` — before it finishes, oldest first, and MUST
  NOT go further: it does not merge in-order files, which is the repacker's job
  (D-16).
- **D-12a** This is what keeps the steady state at **exactly one unordered file,
  the active one**, so a snapshot normally replays only that file and nothing
  else (D-13d). A non-active unordered file exists only transiently — between a
  rollover and the next writer's conversion, or after a crash left an unclean
  file behind (D-10). With D-25d in play the rollover case is rarer still — a
  commit normally seals the file it overgrew — so a non-active unordered file
  is chiefly a crash artefact. Several can accumulate only across several
  crashes, and each is converted in turn.
- **D-12b** A writer MUST convert **oldest first**. That keeps the generation
  range split into a prefix of in-order files followed by a suffix of unordered
  ones, the last of which is the active file. Converting out of order would leave
  an unordered file stranded between in-order ones, and since only adjacent files
  may be merged (D-19), that hole would block the repacker's cascade until it was
  filled.
- **D-12c** Conversion never takes the repack lock. It renames its output in
  without any lock, and takes the remove lock only momentarily to retire the
  input, so a writer never waits on a repack.
- **D-12d** Each conversion is bounded by `rollover_size` — sort the keys, write
  the records in order, append the pointer section and trailer — so a writer's
  extra cost is bounded and predictable rather than proportional to the database.
  The bound assumes files grown by transactions smaller than `rollover_size`;
  a single larger transaction produces a proportionally larger unordered
  file, which is why D-25d converts it in the commit that created it rather
  than leaving it for a later writer.

### 5.4 Indexing unordered files

An unordered file has no pointer section, so key order for it must be derived by
replaying its spans. **There is no shared index file.** Every process builds its
own index, in private memory, for each unordered file in its snapshot.

- **D-13** The private index MUST support point lookup, lower-bound seek and
  ordered traversal. The third is what makes an unordered file usable as a merge
  source (D-14e), and a hash table provides only the first, so it MUST be an
  ordered structure.
- **D-13a** It reflects **committed spans only**, and for each key only its
  newest committed record. Building it means replaying spans and skipping
  rolled-back ones (F-25), never simply walking every record, which would
  resurrect aborted writes.
- **D-13b** A **writer is a reader that also maintains the active file's index
  incrementally.** It already knows every record it appends, so it folds them in
  at commit and discards them on rollback, and never rescans a file it is
  writing.
- **D-13c** No shared state is mutated in place anywhere in the design: files are
  append-only, a new file is published by `rename`, and superseded files are
  reclaimed by the kernel when the last reader closes.
- **D-13d** The cost is that each snapshot replays the unordered files it
  includes. That is bounded twice over: each such file is at most
  `rollover_size`, and D-12 keeps their number at one in the steady state.

### 5.5 Reading

Every read draws on the same set of **sources**, ordered newest to oldest:

| Priority | Source | Search primitive |
|---|---|---|
| highest | the current write transaction's uncommitted records | its private in-memory map |
| then, newest to oldest | each data file, by `start` **descending** | see D-14b |

- **D-14** Within a file the newest version of a key wins — the highest offset
  among committed spans. Across sources, the first record found in the order
  above wins. If that record is a deletion, the key does not exist.
- **D-14a** Point lookups, cursors and range scans MUST all resolve visibility
  by D-14, which is what makes it impossible for them to disagree (G-7).
- **D-14b** Searching one file for a key:

  | File kind | Method | Cost |
  |---|---|---|
  | in-order | binary search the pointer array, comparing the key at each probe | O(log n) comparisons |
  | unordered | point lookup in the private index (D-13) | as that structure provides |

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
array, for an unordered file by ordered traversal of its private index, and for
the write transaction by its own ordered map.

The per-file cursors are held in an array kept sorted by:

> **current key ascending, then generation descending.**

- **D-14j Liveness.** What a cursor observes of writes made *while it runs*
  depends on how it was opened, and the three cases are deliberately different:

  - **Inside an explicit transaction**, the file set is fixed for the cursor's
    lifetime (G-4). A write on that same transaction is still visible, because
    the transaction's own records are a source (D-14) — and that includes one
    written after the cursor was opened, at a key not yet reached (A-1a).
  - **From a database handle** (the non-transactional forms), the cursor
    additionally observes anything **committed through that handle** while it
    runs. A traversal whose callback writes is an ordinary pattern, and a fixed
    view makes it silently skip its own work.
  - **With `ZS_CURSOR_LIVE`**, it additionally observes writes committed by *other
    processes*. This is the only one that costs: readers take no lock (C-2) and
    have nothing to be notified by, so detecting an external write means looking
    for it. An implementation MAY re-scan per record, and the flag exists so
    that nobody pays for it who has not asked.
- **D-14j-a** A source's records MUST NOT be yielded twice because of a write
  made during the traversal. A cursor position expressed as an *index* into a
  mutable structure is the trap here: inserting ahead of it shifts the element
  under the index and re-yields a record. Positions into anything a write can
  modify MUST therefore be expressed as keys, not offsets.
- **D-14j-b** After observing a change, a cursor resumes at the first key
  strictly after the last one it yielded. It does not re-yield, and it does not
  skip keys that were already present.

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

- **D-14f** Because the tie-break is part of the sort, cursors on the emitted key
  are adjacent and newest-first, so advancing while the next cursor's key matches
  is a complete treatment of step 3. Advancing only element 0 would leave the same
  key at another cursor's head, to be emitted again from an older version.
- **D-14g** The write transaction's own records sort as though they had a
  generation above every file's, giving them highest priority for equal keys
  without a special case in the comparator.
- **D-14h** A per-file cursor never yields the same key twice: an in-order file
  holds one record per key (D-17), and an unordered file's private index exposes
  only the newest committed record per key (D-13a). Duplicates therefore arise only
  *across* sources, which is exactly what step 3 handles.
- **D-14i** Picking the next record is O(1) and re-sorting one cursor is O(k)
  worst case, where *k* is the number of sources. A sorted array rather than a
  heap is the right shape because *k* is small by design (D-16) and usually the
  advanced cursor stays at or near the front, so the insertion terminates
  immediately. The sources are fixed for the cursor's lifetime, so no file can
  appear or vanish mid-scan (C-4).
- **D-14k Reverse iteration.** A cursor MAY traverse in descending key order.
  The sources, the per-source structures, and the visibility rule are exactly
  D-14's — the array of per-file cursors is instead kept sorted by **current
  key descending, then generation descending** — so steps 2 through 6 of
  D-14e apply verbatim, with "advance" meaning one step toward smaller keys
  and step 1's seek positioning each source on the **largest key ≤ the start
  key** (or the last key it holds, for an empty start). Equal keys remain
  contiguous from the front of the array with the newest source first, so
  duplicate suppression and tombstone filtering are shared with the forward
  path rather than mirrored (G-7, D-14f). D-14j applies with the direction of
  travel reversed: a resume is at the first key strictly BELOW the last one
  yielded (D-14j-b), and positions are still keys, never indexes (D-14j-a).
  Direction is fixed at open; an implementation need not support turning.

  With a prefix bound (`ZS_CURSOR_PREFIX`), the scan begins at the last key
  carrying the prefix and stops when a key leaves it. The last such key is
  found by an exclusive seek at the prefix's byte-successor — the prefix with
  its last non-`0xFF` byte incremented and everything after it discarded; a
  prefix of all `0xFF` bytes has no successor, meaning "from the end". That
  derivation bounds the prefix range only under the default comparator
  (F-11a): a caller supplying its own comparator MUST NOT combine
  `ZS_CURSOR_PREFIX` with reverse iteration unless byte-successor is an upper
  bound for the prefix's keys under that order too.
- **D-14l Predecessor lookup.** The record with the largest key ≤ K (or
  strictly < K) MUST be resolved as reverse iteration's first emission from a
  seek at K — by D-14k over the same sources, not by a second search rule. A
  tombstone at the largest candidate therefore consumes that key and the
  answer moves to the next smaller live one, exactly as a forward scan past a
  tombstone does (G-7).

### 5.6 Repacking

- **D-15** The repacker **never touches the active file**, and never touches an
  unordered file at all (D-16). It runs periodically, or whenever D-24 reports
  work.
- **D-16** The repacker works **only on in-order files**; converting unordered
  files is the writer's job (D-12). Input selection:
  1. Start from the newest in-order file.
  2. If the result would be **at least as large as** the next lowest in-order
     file, include that file too and repeat.
  3. Stop when every file is included or the next lowest in-order file is
     strictly larger.

  This yields geometrically sized in-order files and amortised O(log n)
  rewrites per record.
- **D-16d** Step 2's comparison MUST include equality. Rollover produces files of
  near-identical size, so equal sizes are the common case rather than a boundary:
  with a strict comparison neither step 2 nor step 3 fires, no merge ever happens,
  and the file count grows without bound — which defeats the policy whose whole
  purpose is keeping that count low. Merging equals is also what produces the
  geometric progression, since two files of size *s* become one of size 2*s*.
- **D-16a** The two jobs divide by whether a file has an `end`, which is what
  makes them independent (C-1a): a writer's conversion is bounded by
  `rollover_size` and runs inline (D-12d), the repacker's cascade is unbounded and
  runs out of band.
- **D-16b** A cascade writes one output for the whole selected set, not one per
  pair. A single invocation is therefore unbounded in duration (see open items).
- **D-16c** Because D-12b keeps in-order files as a contiguous prefix, the
  repacker's inputs are always adjacent (D-19) and the cascade is never blocked
  by an unordered file sitting between two candidates.
- **D-17** The output holds **exactly one record per key**, built from the live
  records of all inputs, skipping rolled-back spans. Where the inputs hold
  versions V1, V2, V3 of a key from oldest to newest, the emitted record
  carries **V3's value** — possibly a deletion — and **V1's ancestor**.
- **D-17a** Ancestors are copied verbatim; nothing is renumbered and no
  ancestor is recalculated (F-16c).
- **D-17b** A repack MUST consider the versions of a key in a **total order**,
  oldest to newest:

  1. across files, by increasing `start` generation — the tiling invariant
     (D-6) means ranges never overlap, so this is total;
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
  shadows the key: being shadowed does not permit dropping a record. Only D-19
  does. The retained record carries the chain's reach, which no other file
  records. T-7 constructs the resurrection that follows from dropping it.
- **D-20** Inputs are iterated in key order: from the pointer section where present,
  otherwise from the same private index any reader of a pointerless file builds.
  There is nothing repack-specific about this.
- **D-20a** A staging file MUST be created with `O_CREAT|O_EXCL`, advancing `<n>`
  until it succeeds. A process identifier is not unique on shared storage — two
  hosts readily have the same pid — and two processes writing one staging file
  would produce an interleaved output that is then renamed into place as though
  complete. `O_EXCL` costs nothing and removes the case.
- **D-20b** Before writing the output, the writer MUST verify the checksums
  covering every input record it will copy: the records-region checksum
  (F-26e) of each in-order input, and the span checksums (F-22) of an
  unordered input's chain. This applies to conversion, sealing, repack and
  compaction alike, and it applies **regardless of `ZS_NOCSUM`** (F-5e), which
  affects only reads. The output is written under fresh checksums, so copying
  unverified input would launder corruption into a file that validates
  perfectly — and D-23 then removes the failing input, the only evidence a
  later check could have caught. A verification failure MUST abort the
  operation with every input left in place: the database stays readable, and
  salvage (section 9) stays possible.
- **D-21** The output is written to `zeroskip.tmp.<pid>.<n>` and `rename`d to
  `zeroskip-<uuid>-<start>-<end>` covering the entire range of every input,
  only once complete.
- **D-22** The output may legitimately contain **zero records**, in the form
  F-26g specifies — for instance
  if every record in the database was deleted and all files were then
  repacked, or if generation *X* created one record and *X+1* deleted it and
  those two were repacked together. The file MUST still be written, so the
  generation range stays tiled (D-6). It is cheap and short-lived: an empty
  file violates D-16's size relation maximally, so the next repack absorbs it.
- **D-23** Removing a data file — a converted unordered file, repack inputs, or
  the contained files left by an interrupted repack — MUST be done **holding the
  remove lock**, and only after verifying
  that a complete set of files exists without it (D-6). Verification and
  removal MUST happen under one unbroken hold of the lock, so the set cannot
  change in between. If verification fails the file MUST be left alone:
  leaking a file costs disk space, removing a needed one costs the database.
- **D-23a** Tiling alone is **not** a sufficient test, because D-6 measures
  completeness from the oldest *surviving* generation: deleting the oldest file
  merely raises that floor, so the remainder tiles perfectly while its data is
  gone. With `{[1-2], 3}`, removing `[1-2]` leaves `{3}`, which tiles. The
  candidate MUST therefore also be **superseded**: the set without it MUST still
  span the **same generation interval**, from the same lowest generation through
  the same highest. A set covering less is not a complete set of the same
  database.
- **D-24** `zs_db_should_repack` reports whether D-16 currently has work.
- **D-25 Sealing.** A writer MAY convert the **active** file on demand, holding
  the write lock. D-12 skips the active file because another writer may be
  appending to it; a caller holding the write lock has excluded exactly that, so
  the exception does not apply. Sealing leaves every file in the database with a
  pointer section, so no reader has to replay a span chain.
- **D-25a** Sealing MUST NOT create a replacement active file. A conversion
  output covers its input's range (D-5a), so afterwards the newest file is
  in-order and the set simply has no active file — a state a reader already
  handles — and the next write creates a new generation (D-9b). An
  implementation that rolled over first and converted second would reach the
  same layout while consuming a generation per seal, and generations are finite
  (D-9c).
- **D-25b** Sealing is a no-op, and NOT an error, when there is no active file,
  when the active file holds no valid spans, or when its header does not
  validate (D-10). The last MUST be reported.
- **D-25c** An unclean active file (D-9) MAY be sealed. The conversion reads to
  the complete point (F-24), so content past it does not survive into the
  output — the same outcome R-4 already produces, reached sooner.
- **D-25d** A writer SHOULD seal at the end of any commit that leaves the
  active file at or above `rollover_size`, while still holding the write
  lock, and after D-12's pending conversions so D-12b's oldest-first order is
  preserved. This is the one case D-12d's bound cannot cover: a single
  transaction larger than `rollover_size` grows the active file past the
  threshold in one append, and deferring the conversion hands its full,
  unbounded cost to the next writer's commit — a writer that did nothing to
  incur it. Sealing at commit end assigns the cost to the transaction that
  caused it, and a bulk load ends with an in-order file rather than an
  oversized unordered one. A failure to seal MUST NOT fail the commit: the
  records are already durable, and D-9a's rollover recovers the layout at the
  next commit. A writer that relies on D-9a alone remains conforming.
- **D-25e** A writer sealing under D-25d SHOULD NOT publish a pointer table
  for that file in the same commit (P-13). A table covers only unordered
  files (P-1), so it would be created already stale and removed at the next
  sweep (P-16). A table published by a concurrent reader that took its
  snapshot before the seal is harmless for the same reason.
- **D-26 Compaction.** An implementation MAY merge the **entire** database into
  one file. The order is normative: seal (D-25), then convert every remaining
  unordered file (D-12), then merge in-order files until no two adjacent ones
  remain.
- **D-26a** D-16's geometric selection does NOT apply to compaction. That rule
  exists to keep a repack amortised, and compaction is explicitly the
  unamortised case. Every other repack rule applies unchanged, D-17 through
  D-23 — **including adjacency** (D-19).
- **D-26b** Adjacency is why compaction merges every maximal **run** of adjacent
  in-order files rather than the in-order prefix. A file that can be neither
  converted nor merged (D-28) splits the set into runs, and each must be merged
  separately. Taking only the prefix would merge nothing at all when such a file
  is second in the set — which is precisely the damaged database where D-28's
  "everything mergeable" has to mean something.
- **D-27** Because a compaction output spans the whole generation interval,
  D-19's containment test succeeds for every key, so every tombstone whose
  lifespan it contains is dropped. This is the **only** merge that can reclaim
  them: a partial repack MUST retain a tombstone, because a file outside its
  input set may still hold the key (D-19a).
- **D-28** Compaction is **best effort in action and strict in reporting**: it
  merges everything mergeable (D-26b) and reports what it could not, and reports
  failure only if the result is not a single file. A non-active file with an
  invalid header is the case that blocks it — D-10a tolerates such a file, but
  it can be neither converted nor merged.
- **D-29** Compaction is unbounded: it rewrites the whole database in one
  invocation while writers continue. That is open item 1's unboundedness made a
  deliberate entry point rather than an emergent property of D-16's cascade, and
  the mitigations sketched there apply to it equally.

## 6. Concurrency and durability

- **C-1** Three byte-range locks on `zeroskip.lock`:

  | Byte | Lock | Held for | Covers |
  |---|---|---|---|
  | 0 | write | a write transaction | appending, creating a new active file, converting an unordered file |
  | 1 | repack | a whole repack, possibly long | merging in-order files |
  | 2 | remove | momentarily | verify completeness, then unlink |

  The mechanism is **exactly** `fcntl` record locking: `F_SETLK` or `F_SETLKW`
  with `l_type = F_WRLCK`, `l_whence = SEEK_SET`, `l_start` the byte above, and
  `l_len = 1`. This is part of the interoperability surface, not an
  implementation choice — see C-1e.
- **C-1e** The primitive and the byte offsets are normative, because
  implementations in different languages must exclude each other. An
  implementation MUST NOT use `flock`, which occupies a separate lock space from
  `fcntl` on Linux and does not exclude it, and is a no-op over some network
  filesystems.
- **C-1f** `fcntl` locks are per-process, not per-thread: two threads of one
  process both acquire the same lock successfully, and two handles in one process
  do not exclude each other. The `fcntl` lock alone therefore does not deliver
  G-5 within a process; C-1j is what closes that, and a binding SHOULD still
  document its handles as not thread-safe, which is a different property.
- **C-1j Same-process exclusion.** An implementation SHOULD exclude two handles
  on one database within a single process, so that the second blocks or reports
  `ZS_LOCKED` exactly as a second process would. This is a **binding property,
  not interoperability surface**: it changes nothing on disk and nothing another
  implementation can observe, so a peer that omits it still interoperates.
  Two mechanisms, in order of preference:

  1. `F_OFD_SETLK` (C-1i) where the platform has it, which is scoped to an open
     file description and so already excludes two handles with no extra state;
  2. otherwise a **process-global registry** keyed by the lock file's identity
     — its `st_dev` and `st_ino`, not its path, since two paths can reach one
     inode — recording which of C-1's three locks are held within this process.
     It MUST be consulted before the `fcntl` lock and released after it, so the
     two agree on C-1d's ordering.

  A **mutex is not one of the mechanisms**, and the distinction is the whole
  point: a per-handle mutex is two different objects and excludes only threads
  sharing one handle, which is not what G-5 promises. The registry excludes by
  the *database's* identity, which is.

  An implementation MUST NOT let the registry substitute for the `fcntl` lock.
  The registry is invisible outside the process; the `fcntl` lock is what a peer
  implementation sees (C-1e). Both are taken, always.
- **C-1g** `fcntl` locks are released by closing **any** descriptor for the file
  in that process. An implementation MUST hold exactly one descriptor for
  `zeroskip.lock` for the handle's lifetime, and MUST NOT open a second.

- **C-1a** The write and repack locks never contend, because the two jobs
  consume **disjoint sets of files**: a writer only ever converts files with
  `end == 0`, and the repacker only ever merges files with `end != 0` (D-16). A
  file becomes visible to the repacker precisely when the writer has finished
  with it, so a writer never waits on a repack.
- **C-1b** Publishing a new file needs **no lock at all**: `rename` into the
  directory is atomic, and a repack's output `[a..b]` and a conversion's output
  `[c..c]` with *c* > *b* are disjoint, so they cannot interfere in either order,
  and a repack stays valid when a new in-order file appears above it midway
  through.
- **C-1c** The **remove** lock makes verifying completeness and unlinking one
  step (D-23).
- **C-1d Lock ordering.** The locks form one total order: **repack → write →
  remove**. A holder MAY acquire any lock later in that order and MUST NOT
  acquire one earlier, so no cycle exists.

  Most operations use a sub-chain of it: a writer takes write → remove, a
  repacker takes repack → remove, and the two never contend (C-1a). Only
  compaction (D-26) holds both repack and write, which is why the order is
  stated as a chain rather than as two disjoint pairs. A caller taking both
  MUST release write before the merge, so an unbounded compaction does not
  block writers for its whole duration.
- **C-1h Locks across databases.** C-1d orders the locks within one database.
  The library cannot see across two, so a caller that holds locks on several
  while writing MUST impose its own consistent order.
- **C-1i** An implementation MAY use `F_OFD_SETLK` instead of `F_SETLK`, but only
  where it has verified that the two conflict with each other on that platform.
  `F_OFD_SETLK` is scoped to an open file description rather than a process, so
  C-1g does not apply to it — and unlike `F_SETLK` it *does* exclude two handles
  in one process, which is the one thing C-1f gives up. An implementation that
  needs that exclusion should reach for it rather than for a mutex.
- **C-2** Readers take **no lock**.
- **C-3** A file is published by writing it under a staging name, then
  `rename`ing it to its final name. Readers see it only once complete, and the
  rename is the instant it joins the file set (D-8).

**C-4 Taking a snapshot.** The protocol is:

1. `readdir` the database directory, keeping names matching `zeroskip-<uuid>-*`
   and parsing each generation range from its name.
2. Run D-5's scan over the sorted names. If it leaves a gap the set is
   incomplete (D-6) — either a torn `readdir` or corruption (D-5c) — so restart
   from 1.
3. `open` and `mmap` every file in the resolved set. If any `open` fails with
   `ENOENT`, restart from 1.
4. Build a private index for each unordered file by replaying its spans, taking
   its snapshot boundary to be the end of its last valid span.

Everything the snapshot will read is immutable from here on. No lock is taken at
any point.

- **C-4a Completeness.** Step 2's tiling check *is* the completeness proof: every
  generation in the interval is covered by exactly one file, so no committed data
  is missing. Nothing needs to be compared against a published record, and a set
  that is stale rather than current is simply an older snapshot.
- **C-4b Why a retry suffices.** `readdir` may miss entries, and a file may be
  removed between steps 2 and 3. Both show up as a failure — a set that does not
  tile, or an `ENOENT` — never as a set that looks complete but is not, because
  removal is only ever permitted when the remaining files still tile (D-23).
  Retrying re-scans and converges, and each attempt costs only a directory read
  and a few opens.
- **C-4c Immutability of what was opened.** In-order files are never modified.
  An unordered file that is not the active one is never appended to again. The
  active file *is* appended to, but only ever appended to (G-1), so every byte
  below the snapshot boundary is immutable — a prefix of an append-only file is
  stable by construction. Growth beyond the boundary is invisible: the mapping
  covers the prefix and the reader never looks past it.
- **C-4d** Every index is private (D-13c), so a snapshot needs no synchronisation
  against a writer beyond what C-4c already provides.
- **C-4f Concurrent visibility.** A reader scanning the active file may meet a
  span the writer is still writing. The terminator's checksum covers the span's
  data (F-19), so a terminator whose data is not yet fully visible fails
  validation and reads as absent — the reader stops there, exactly as it would
  after a crash. In the running system the kernel already orders most of this
  — a walk is bounded by a size read whose synchronization makes the bytes
  below it visible — but the checksum makes the guarantee unconditional: it
  holds for the crash-written state, where the *disk* reorders freely and a
  terminator can be durable while its data never landed (the state C-7's gate
  1 exists to prevent and C-7c permits), and for any reader whose walk is not
  so bounded. That is what permits reading a live file without a lock, and
  why the check runs in every mode (F-5e).
- **C-4g Lifetime.** Once its descriptors are open a packer may (subject to
  D-23) `unlink` superseded files immediately: the kernel keeps each inode alive
  until the last descriptor *and mapping* is gone. There is no reference table
  and nothing to clean up when a process dies.
- **C-4h Termination.** A retry happens only when the file set changed during
  the scan, which is a structural event rather than a per-operation one, so
  retries are rare and each costs one directory read plus a few opens. An
  implementation SHOULD bound the retry count and report `ZS_AGAIN` rather than
  spin indefinitely, so a pathological rate of structural change surfaces as an
  error instead of a livelock.
- **C-4i Freshness.** Beginning a transaction — shared or exclusive — MUST
  yield a snapshot that observes every transaction whose commit completed
  before the begin, in any process. A handle that holds a database open for
  hours reads as current as one opened for the call; snapshot isolation (G-4)
  is a property of a transaction's lifetime, never of a handle's. A cached
  snapshot MAY be reused only after an inspection that would have detected
  any such commit. Inspecting the directory's **name set** and the active
  file's **size** is sufficient and exact — not a heuristic — because every
  commit either appends to the active file or publishes a file by `rename`
  (C-3, D-21), and file timestamps, whose granularity is a filesystem
  property, play no part. `ZS_CURSOR_LIVE`'s per-step check (D-14j) MAY use
  the same inspection rather than rebuilding a snapshot each step.
- **C-5** The accepted cost of C-4g is that disk space is held until the last
  reader holding an old snapshot exits.
- **C-6 Directory durability.** After creating a **data file** (a new active
  file) and after renaming a repack or conversion output into place, the
  implementation MUST `fdatasync` the **directory**, otherwise the name may be
  absent after a crash even though the file's contents are durable.
- **C-6a** A directory sync is **not** required after `unlink`. If a removed name
  reappears after a crash it is a file an enclosing range already supersedes,
  which readers ignore (D-5) and a later pass removes again (D-23).
- **C-6b Output durability.** A conversion or repack output MUST be
  `fdatasync`ed before the rename that publishes it, and a new active file's
  header before its creation returns. These syncs — and C-6's directory
  syncs — are **integrity**, not durability, and hold in every durability
  mode: no flag relaxes them. Publication asserts completeness (D-21), so a
  name that exists pointing at a partial file is corruption *created by* a
  rename — and the inputs it entitles a later pass to retire (D-23) were the
  data's only complete copy. An output-sync failure fails the conversion or
  repack; a directory-sync failure is reported and tolerated (C-6), because a
  lost name merely un-happens a publication the inputs still cover.
- **C-7 Two gates per commit.** Under default durability a commit is:

  1. append the span's data records, then **`fdatasync`**;
  2. append the terminator, then **`fdatasync`** again.

  `fdatasync` rather than `fsync` because appending changes only the metadata
  needed to read the data back, which `fdatasync` is required to flush.
- **C-7a** Together the gates make "a valid terminator implies its data is
  durable" a guarantee of the filesystem. Handling for each failure:

  | Failure | State | Required behaviour |
  |---|---|---|
  | gate 1 fails | no terminator was written | report the error; the transaction did not happen |
  | gate 2 fails | terminator may or may not be durable | either outcome is correct, so report the error and do nothing else |

  An implementation MUST NOT retry a failed sync and treat success as evidence the
  data survived: a second call can succeed after the dirty pages were discarded.
- **C-7b** The cost is two syncs per commit rather than one. It is paid per
  *transaction*, not per record, so a caller that batches many operations into one
  transaction amortises it — which is the reason `zs_txn_*` exists alongside the
  single-operation `zs_db_*` calls.
- **C-7c** `ZS_NOSYNC` omits **both** gates — and nothing else. The structural
  syncs stay in every mode: C-6's directory syncs and C-6b's output syncs are
  integrity, and only the two per-commit gates were ever durability. Atomicity
  survives, because a torn tail is still detectable (F-22); C-7a's ordering
  guarantee does not — a valid terminator no longer implies durable data — and
  per-commit durability becomes the caller's affair (`zs_db_sync` is the
  on-demand gate). What a crash leaves is a valid **prefix** of the active
  generation, possibly empty, on top of every generation a conversion or
  repack already published: the loss bound is the active file's unconverted
  tail, at most `rollover_size` — not the database. An implementation that
  also skipped the structural syncs would have no bound at all: a rename could
  publish a torn in-order file and entitle the retirement of the inputs that
  were its records' only complete copy (C-6b), so a crash would cost converted
  generations, which no caller asking to skip *commit* syncs agreed to.
- **C-8** An aborted transaction appends a `ROLLBACK` and syncs **neither** gate.
  If a crash loses it, the active file is simply no longer clean, so the next
  writer moves to a new file (D-9) and reaches the same state. Nothing is being
  promised to a caller, so there is nothing to make durable.
- **C-8a** The unsynced `ROLLBACK` is still **ordered** ahead of any later
  commit, without costing a sync of its own: F-21 needs the `ROLLBACK` on disk
  before the next `COMMIT` terminator, and the next commit's first gate — the
  fdatasync before its own terminator (C-7) — flushes everything before it,
  the `ROLLBACK` included. A writer that streams records as they are stored
  therefore aborts for free, and a crash that loses the tail leaves a torn
  span F-22 already discards.

## 7. Open and recovery

Opening is recovery; there is no separate pass.

- **R-1** Open is C-4: scan the directory, resolve enclosures, check the tiling,
  map the files, then replay each unordered file's spans from its start —
  building the private index as it goes (D-13a) and stopping at the first record
  or terminator that fails to validate, which establishes that file's end.
- **R-2** Live data is the union of records in spans with `COMMIT` terminators;
  rolled-back spans contribute nothing.
- **R-3** A reader MUST NOT write **to the database**. Opening a damaged
  database read-only is side-effect-free: no conversion, no repack, no new
  active file, no removal. There is no shared index inside the database for it
  to update (D-13c); `zeroskip.cache` (P-2b), when present, is outside the
  file set — nothing in it parses as a data file (D-2, P-3). Publishing a
  pointer table (§8) is therefore not a write to the database, and a
  read-only handle MAY publish into an existing cache directory — but MUST
  NOT create `zeroskip.cache`, because creating a directory inside the
  database is a visible side effect.
- **R-4** There is no in-place repair. A file that is not clean is simply
  complete at its last valid span (F-24), and the writer moves to a new
  generation. Nothing is ever appended past a boundary that failed to
  validate, so a spurious terminator in trailing garbage — which a checksum
  can never wholly exclude — cannot become the foundation of a later chain.
  Generations are cheap.
- **R-5** A crash during a repack or conversion leaves either a staging file,
  which is ignored because its name does not match (D-4), or an output already
  renamed in — whose range either encloses its inputs (D-5) or, for a conversion,
  equals its input's (D-5a). Both resolve in favour of the output. Either way the surviving set tiles and the contained files may be
  removed (D-23). There is no separate reconstruction path, because discovery by
  directory scan is the only path.

---

## 8. Pointer table cache

An unordered file has no pointer section, so key order for it is derived by
replaying its spans (D-13a). That replay is bounded by `rollover_size` but paid
on every open. A **pointer table** is one process's replay result, published so
another can load it and replay only the suffix beyond it.

It is **optional and never load-bearing**. A conforming implementation MUST
produce identical results with the cache directory empty, absent, unwritable, or
full of tables it rejects. Nothing here may turn a readable database into an
unreadable one.

- **P-1** A pointer table covers exactly one **unordered** file, identified by
  the database uuid and that file's generation. An in-order file has a pointer
  section (§4.9) and MUST NOT have a table.
- **P-2** Tables live in a **cache root** named by the caller, or — when the
  caller opts in (A-8a) — in the reserved directory `zeroskip.cache` inside
  the database directory. A configured root MUST NOT be the database
  directory itself, and an implementation MUST reject that configuration.
  Beyond those two locations an implementation MUST NOT choose one on the
  caller's behalf — a planted table yields wrong records, and a
  world-writable default such as `/tmp` would make planting one trivial. The
  in-database directory is not such a default: it inherits the database
  directory's ownership and permissions, so anyone able to plant a table
  there could already rewrite the data files.
- **P-2a** Under a configured root, the tables for a database live in the
  subdirectory named by the database's uuid in D-0's form —
  `<root>/<uuid>/` — and the staging names of P-4 live there too. An
  implementation creates that subdirectory as needed; any handle MAY,
  including a read-only one, because it is outside the database and R-3 is
  untouched. The root itself is never created (A-8). Scoping by database
  keeps P-16's sweep proportional to one database's tables rather than to
  every database sharing the root, and makes "not ours to remove"
  structural rather than a filename filter.
- **P-2b** With A-8a's flag, the cache directory is `zeroskip.cache` inside
  the database directory, holding tables directly — it serves exactly one
  database, so P-2a's uuid level would be redundant. Only a writable handle
  creates it; a read-only handle uses it if present and is otherwise simply
  without a cache, because creating a directory inside the database is a
  visible side effect R-3 forbids. The cache then shares the database's
  lifetime: deleting the database directory deletes its tables, which a
  shared root cannot offer — P-16's sweep runs only for databases that get
  opened, so a deleted database's tables under a shared root are nobody's to
  remove. In this directory, and only here, a table whose uuid is not the
  database's is garbage by construction, and a process MAY remove it — a
  relaxation of P-16's uuid rule that is safe precisely because the
  directory belongs to one database.
- **P-3** A published table is named `zeroskip.index-<uuid>-<GEN8hex>`, using
  D-0's uuid form and D-1's generation form. The `zeroskip.` prefix puts it in
  the metadata namespace (D-2), so it can never be parsed as a data file.
- **P-4** A table is published by writing a complete file under a staging name
  `zeroskip.tmp.<pid>.<hex digits>` in the cache directory and `rename`-ing it
  over the published name. A table is never modified in place, never appended
  to, and never truncated. G-6 therefore holds for the cache directory as well
  as for the database.
- **P-5** A table is a 96-byte header, then `nptrs` 8-byte little-endian record
  offsets, then a 4-byte checksum over those offsets. Its size is exactly
  `96 + 8 × nptrs + 4`.

  | offset | size | field |
  |---|---|---|
  | 0 | 16 | magic (P-6) |
  | 16 | 1 | version_read |
  | 17 | 1 | version_write |
  | 18 | 2 | flags — low 4 bits the checksum engine, bit 4 "checksums verified" |
  | 20 | 4 | reserved, written zero, ignored on read |
  | 24 | 16 | uuid |
  | 40 | 4 | start — the covered file's generation |
  | 44 | 4 | reserved, written zero, ignored on read |
  | 48 | 16 | comparator name |
  | 64 | 8 | valid_upto |
  | 72 | 8 | term_off |
  | 80 | 8 | nptrs |
  | 88 | 4 | term_csum |
  | 92 | 4 | checksum, covering [0, 92) |

- **P-6** The magic is the 16 bytes

      89 7A 73 69 6E 64 65 78 31 0D 0A 1A 0A 00 00 00
      \x89  z  s  i  n  d  e  x  1 \r \n ^Z \n \0 \0 \0

  built on the same principles as F-6's and deliberately **different** from it,
  so a table and a data file are distinguishable by content as well as by name.
  A reader MUST validate all 16 bytes, not a prefix.
- **P-7** A table's checksums use **the engine named by the covered data file's
  header**, not the engine the reading or writing handle would choose for a new
  file. This is F-5a applied to the table: any peer able to read the data file
  can validate its table. A table checksummed under the handle's engine instead
  validates for nobody, so every reader silently rejects it and the cache does
  nothing while appearing to work.
- **P-8** `valid_upto` is the data-file offset the table covers. It MUST be a
  span boundary: the offset immediately after a valid span's terminator, or the
  data file's header length when the file has no valid spans.
- **P-9** The offsets are the record offsets of every distinct key committed
  below `valid_upto`, each being that key's newest such record (D-14), sorted
  ascending by key under the named comparator. Rolled-back spans contribute
  nothing (F-25).
- **P-10** `term_off` is the offset of the terminator immediately below
  `valid_upto`, and `term_csum` is the checksum that terminator carries. When
  the file has no valid spans, `valid_upto` and `term_off` both equal the data
  file's header length, `term_csum` is 0 and `nptrs` is 0.

  `term_off` is recorded rather than derived because terminators are located by
  walking spans **forward**: a reader given only `valid_upto` could not find the
  terminator below it without performing the very replay the table exists to
  avoid.
- **P-11** A reader MUST use a table only if **all** of the following hold, and
  MUST otherwise ignore it and build the index by replay. A rejected table is
  never an error and MUST NOT be reported as corruption — it lives outside the
  database, and reporting it would let a file the database does not depend on
  make a readable database look unreadable.
  - the magic matches, the header checksum validates, and the checksum over the
    offset array validates;
  - `version_read` does not exceed the reader's;
  - the file size is exactly `96 + 8 × nptrs + 4`;
  - the recorded engine is the one the data file's header names (P-7);
  - uuid and `start` match the data file;
  - the comparator name matches both the data file's field and the reader's own;
  - flags bit 4 is set. A conforming builder always sets it, since span
    verification rides indexing in every mode (F-5e); a table without it
    was built by an implementation that indexed spans nobody verified, and
    accepting it would seed an index with them. (Bit 4 was reader-exempted
    under `ZS_NOCSUM` until 2026-08-14, when F-5e narrowed to record
    checksums.);
  - `H ≤ valid_upto ≤` the data file's size, where `H` is the data file's header
    length;
  - every offset lies in `[H, valid_upto)`;
  - either P-10's no-spans case holds exactly, or `H ≤ term_off < valid_upto`,
    the bytes at `term_off` decode as a terminator, `term_off + term_len =
    valid_upto`, and that terminator's checksum equals `term_csum`.
- **P-12** Having accepted a table, a reader takes its offsets as the index's
  ordered base and replays spans from `valid_upto` onwards, folding the results
  in (D-13b). Beginning a replay at a span boundary is sound because a span is
  self-delimiting and self-validating (F-23, F-19).
- **P-13** After building or extending an index over an unordered file, a
  process SHOULD publish a table covering that file's complete point when the
  distance from the loaded table's `valid_upto` — or from `H`, if no table was
  loaded — reaches an implementation-defined threshold. Readers and writers
  apply the same rule: whoever builds the pointers publishes them.

  The threshold is required rather than advisory, and **both ends of it
  cost** — paid by different parties.

  The replay from the last published `valid_upto` is paid per snapshot
  **rebuild**: at open, and at any begin whose C-4i inspection detected
  another process's commit. A sole writer's steady state rebuilds nothing —
  its begins reuse the snapshot (C-4i) and its commits fold incrementally
  (D-13b) — but writers alternating one-store transactions across processes
  rebuild at every begin, and a threshold that publishes rarely leaves each
  of those replays unbounded, making that load quadratic in the active file.
  Too low costs whoever publishes an O(records) merge and a whole-table
  rewrite per commit, which is real but bounded, because a table is never
  synced (P-14).

  Measured on 16000 single-store transactions over a 2 MB active file, with a
  rebuild forced at every begin — the alternating shape: no cache 13.2 s,
  threshold 1 byte 5.7 s, 4 KB 2.0 s, 32 KB 2.0 s, 256 KB 3.5 s, 1 MB 8.8 s.
  The same load as a sole writer whose begins reuse: no cache 1.2 s, 1 byte
  5.0 s, 4 KB 1.4 s, 32 KB 1.3 s, 256 KB 1.2 s, 1 MB 1.3 s. The low end
  keeps its cost in both shapes; the high end costs only whoever rebuilds —
  and the knee a default must sit in is the rebuilding shape's, because that
  is the shape a threshold exists to bound. An implementation SHOULD choose
  its default from measurements like these, and the figure is an absolute
  byte count rather than a fraction of `rollover_size`: the knee is set by
  how much data a replay walks, which has nothing to do with how large a
  caller lets a generation grow.
- **P-14** A table MUST NOT be `fsync`ed before publication. It is rebuildable,
  and a torn or zero-filled file after a crash is rejected by P-11. Syncing it
  would add a sync to the commit path, which C-7 defines as exactly two.
- **P-15** A failure to publish MUST NOT fail the operation that triggered it.
  The data is already durable, and a cache is not something a caller can act on.
- **P-16** A process MAY unlink tables in its per-database cache directory
  (P-2a, P-2b) whose uuid matches its own database and whose generation is not
  present as an unordered file in its snapshot. Tables carrying another uuid
  are not its to remove — except inside `zeroskip.cache`, where P-2b relaxes
  this. Unlinking is safe against a concurrent reader: a descriptor already
  open survives it, and a reader that misses a table replays instead.
- **P-17** P-10's binding detects a data file whose covered prefix has changed.
  Within the format that cannot happen — files are append-only and generations
  are never reissued (D-9b) — so the check exists for out-of-band events, of
  which restoring a database directory from backup under a surviving cache
  directory is the realistic one. It examines one span, so it cannot detect
  divergence confined to an earlier one. A cache directory MUST therefore be
  scoped to the lifetime of the database instance, and a caller that restores a
  database directory out of band MUST discard its tables. An in-database cache
  directory (P-2b) travels with its database: a consistent copy restores tables
  P-11 accepts, and the hazard here arises only when a cache outlives or
  predates the data files beside it.

Checksumming the whole covered prefix would close P-17 completely, and is
deliberately not required: the cache's largest benefit on a cold page cache is
not touching the data file at all, and hashing the prefix would force reading
every byte of it.

---

## 9. Salvage

Recovery of what is readable from a **damaged** directory. Everything in this
section is OPTIONAL: an implementation that offers no salvage is fully
conforming. What is normative is what an implementation that *does* offer it
must and must not do — because a salvage tool that silently invents data, or
silently resurrects a transaction that never committed, is worse than none.

- **S-1** Salvage reads a source directory and writes a **new** database
  elsewhere. It MUST NOT write to the source, take any lock on it, or unlink
  anything in it. That is precisely what lets it read structures §5 and §7
  refuse, and it is why R-4's "there is no in-place repair" needs no exception.
- **S-2** Salvage MUST NOT apply D-5's overlap resolution or D-6's tiling check.
  A gap is reported and stepped over. One missing generation makes a database
  unopenable under §7 while every surviving file remains perfectly readable, and
  recovering those files is the point.
- **S-3** Files are processed **oldest first**: by `start` ascending, and for
  equal `start` the narrower range first, since a wider one is a repack output
  derived from it. Records are applied in that order, so the newest surviving
  version of each key wins by construction and no separate recency pass is
  needed (D-14).
- **S-4** The source's comparator does not affect the output. The output is
  ordered by the comparator the caller supplies, and recency is resolved by
  generation and offset rather than by key order. Where a source header is
  readable, its comparator name MUST be compared against the caller's and a
  mismatch reported; where no header is readable there is nothing to check and
  nothing that needs checking.
- **S-5** A file whose header does not validate is still processed. Its
  generation comes from its filename (D-1). Its checksum engine is determined by
  trying each in turn and keeping the one under which a span validates — engine
  0 matches only where a stored checksum really is zero, so it is a genuine
  signal rather than a catch-all. Both the invalid header and the engine
  determination MUST be reported.
- **S-6** For an in-order file the pointer section MUST be ignored and the
  records region walked directly. A pointer section that fails to load makes the
  file unreadable under §7 while its records may be intact, and an in-order file
  has no spans (F-23), so the walk is a flat record walk.
- **S-7 Resynchronisation.** On reaching a span that does not validate, salvage
  MUST attempt to resynchronise rather than stopping where F-24 requires a
  reader to stop. From the failed position, step forward in 8-byte increments
  (F-2) and attempt to decode a terminator at each. On success compute
  `span_start = position - spanlen`; if that lands at or after the last known
  good boundary, checksum `[span_start, position)` together with the
  terminator's own bytes and compare against the terminator's stored checksum.
  A match is a **verified** span, and the walk resumes from it.

  This is what makes the default sound rather than a guess: everything recovered
  after damage is checksum-verified, not merely decodable. Under F-24 one bad
  span discards every later span in its generation, and those later spans are
  exactly what this recovers.
- **S-8** The span that failed cannot be verified — its terminator is what would
  prove it — and neither can a trailing region with no valid terminator. Their
  records MUST NOT be recovered unless explicitly requested, and every record so
  recovered MUST be reported. A record's own checksum (F-32) does not change
  this: it proves the record's **bytes**, while the terminator is what proves
  the transaction was **committed** — a torn tail with pristine record
  checksums was still never acknowledged to anyone, so salvage MUST NOT treat
  byte-proof as commitment-proof in a span walk.
- **S-8a** An in-order file has no commitment question: it was written whole
  and published by a single `rename` (D-21), so every record in it belongs to
  committed history. Salvage of an in-order file therefore verifies **per
  record** against F-32, and reports as unverified exactly the records that
  cannot be proved: every record of an engine-0 file, and any record whose
  checksum fails.
- **S-9** A **rolled-back** span MUST NOT be recovered under any option. F-21
  and F-25 make it deliberately aborted, and no conforming reader has ever shown
  its records; recovering them would resurrect a transaction that did not
  happen.
- **S-10** Salvage MUST report, per key, where the record it recovered may be
  **stale**: where that record is older than some point at which data was lost,
  a newer version may have been among the lost bytes. A key whose winning record
  is newer than every damage point MUST NOT be reported, so the report stays
  small enough to act on.

  The rule is deliberately conservative rather than exact. Determining which
  keys were actually superseded would require the key set of the lost bytes,
  which is precisely what has been lost.
- **S-11** Reporting MUST be structured — a kind, a location, and a key where
  one applies — rather than prose. S-10's report is the mitigation for emitting
  a possibly stale value, so it has to be machine-readable rather than something
  an operator parses back out of a message.
- **S-12** Salvage MUST NOT reconstruct a missing generation's contents, invent
  records, or repair the source.

---

## 10. C binding

The semantics below are normative; the spelling is a C binding (§1).
Opaque types, one 32-bit flag space, output through pointer parameters, and
`enum zs_ret` with `ZS_OK = 0`, `ZS_DONE = 1`, negatives for errors
(`ZS_NOTFOUND = -5`, `ZS_LOCKED = -4`, `ZS_BADFORMAT = -7`, `ZS_FULL`,
`ZS_AGAIN`, …).

```c
typedef int      zs_cb(void *rock, const char *key, size_t keylen,
                       const char *val, size_t vallen);
typedef int      zs_compar(const char *a, size_t alen,
                           const char *b, size_t blen);
typedef uint32_t zs_csum(const char *buf, size_t len);

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
int  zs_db_foreach(struct zs_db *, const char *start, size_t startlen,
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
int  zs_txn_foreach(struct zs_txn *txn, const char *start, size_t startlen,
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
int  zs_db_seal(struct zs_db *);
int  zs_db_compact(struct zs_db *);
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
| `ZS_NOCSUM` | open | skip record-checksum verification at materialization (F-5e); span checksums are still verified at indexing |
| `ZS_NOSYNC` | open | omit both durability gates on commit, and nothing else (C-7c, C-6b) |
| `ZS_NONBLOCKING` | open, txn | fail with `ZS_LOCKED` rather than wait for a lock |
| `ZS_IFNOTEXIST` | store | store only if the key is absent, else `ZS_EXISTS` |
| `ZS_IFEXIST` | store | store only if the key is present, else `ZS_NOTFOUND` |
| `ZS_FETCHNEXT` | fetch | return the record with the smallest key ≥ the given key; with `ZS_SKIPROOT`, strictly > (A-12) |
| `ZS_FETCHPREV` | fetch | return the record with the largest key ≤ the given key; with `ZS_SKIPROOT`, strictly < (A-12) |
| `ZS_REVERSE` | cursor | iterate toward smaller keys (D-14k); not valid on `foreach` or with `ZS_CURSOR_LIVE` (A-13) |
| `ZS_SKIPROOT` | foreach, cursor | skip the first record if it matches the start key exactly |
| `ZS_CURSOR_PREFIX` | foreach, cursor | treat the start key as a prefix and stop when a key leaves it |
| `ZS_CURSOR_LIVE` | foreach, cursor | also observe writes by other processes (D-14j); costs a re-scan per record |
| `ZS_CSUM_NONE` | open | write engine 0 into files this handle creates |
| `ZS_CSUM_XXHASH` | open | engine 1, the default if no `ZS_CSUM_*` is given |
| `ZS_CSUM_EXTERNAL` | open | engine 2; `zs_open_data.csum` MUST be supplied |

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

  **Including a traversal already in progress**: a record written from inside a
  `zs_txn_foreach` callback, at a key the traversal has not yet reached, MUST be
  visible to the rest of that traversal (D-14j).
- **A-1c** A transaction supports **any number of cursors open at once**, and
  writes through the transaction while they are open. Each cursor observes
  the write by D-14j; every key or value pointer any of them has returned
  remains valid by A-4 — a store never invalidates another read's result.
  Cursors are not thread-safe and this promises nothing across threads (G-5's
  caveats apply); it promises composition within one caller, which is what a
  layered consumer — one query touching several key ranges inside one write
  transaction — depends on.
- **A-2** There is no `yield` call and no yield flags: readers hold no lock, so
  there is nothing to yield.
- **A-3** There is no MVCC flag. Snapshot isolation is the only read mode,
  because a snapshot is a set of immutable files plus a per-file valid extent.
- **A-4** Returned key and value pointers remain valid for the lifetime of the
  transaction or cursor that produced them; for the non-transactional
  `zs_db_*` calls, until the next call on that `struct zs_db`.
- **A-4a** A-4 binds across a **snapshot swap**. A transaction or cursor may
  move to a newer snapshot while it is alive — a write transaction resolves its
  active file at its first store (D-9), and a `ZS_CURSOR_LIVE` cursor follows
  the handle's file set (D-14j) — and the snapshot it leaves behind owns the
  mappings every pointer it already returned points into. Releasing that
  snapshot at the swap is therefore a **use-after-unmap in the caller**, not a
  cleanup: the borrowed bytes MUST outlive the swap and MAY only be released
  when the borrowing transaction or cursor ends.

  A snapshot is the wrong granularity to retain, because it also holds a
  descriptor per file and a borrower that swaps repeatedly would exhaust the
  process's descriptor table. An implementation SHOULD retain only the
  **mappings**, closing descriptors and freeing indexes at the swap as usual:
  the bytes are page-cache-backed and cost address space alone.

  This is not a fresh guarantee, only the reading of A-4 that a caller depends
  on. It is called out because both of the swaps above are invisible from the
  API — nothing the caller did says "the file set moved" — so an implementation
  passes every single-snapshot test and still hands out dangling pointers the
  moment a transaction's first store starts a new generation.
- **A-5** `ZS_SHARED` is read-only and MUST NOT write (R-3).
- **A-6** A `ZS_CSUM_*` flag chooses the engine for files this handle **creates**;
  it never overrides what an existing file records, since each file's engine comes
  from its own header (F-5a). Opening a database whose files use engine 2 without
  supplying `csum` is an error, as those files cannot be verified.
- **A-7** `zs_compar` returns negative, zero or positive like `memcmp`, but MUST
  implement F-11a's total order rather than delegating to `memcmp` alone, which
  says nothing about keys of differing length.
- **A-8** `index_dir` names the pointer table cache **root** (§8); tables live
  under `<index_dir>/<uuid>/` (P-2a). A null or absent value disables the
  cache unless A-8a's flag is set, and disabled is the default: an
  implementation MUST NOT choose a location itself (P-2). Naming the database
  directory is a usage error (`ZS_BADUSAGE`). The library creates the
  per-database subdirectory but never the root; a root that is missing or
  unwritable disables the cache for that handle rather than failing the open
  (P-15).
- **A-8a** `ZS_INDEX_LOCAL` selects P-2b's in-database cache directory,
  `zeroskip.cache`. Setting it together with a non-null `index_dir` is a
  usage error (`ZS_BADUSAGE`): the two name different locations for the same
  tables.
- **A-9** `index_threshold` is P-13's threshold in bytes. Zero selects an
  implementation-defined default, so a caller that sets `index_dir` and nothing
  else still gets bounded publishing. That default SHOULD be capped below
  `rollover_size` so a caller using small generations still publishes within
  one rather than never.
- **A-10** `zs_db_seal` performs D-25. It returns `ZS_OK` for each of D-25b's
  no-op cases, and `ZS_READONLY` on a read-only handle (R-3).
- **A-11** `zs_db_compact` performs D-26, returning `ZS_OK` only when the
  database is a single file and `ZS_BADFORMAT` otherwise, having merged whatever
  it could first (D-28).
- **A-12** The point-lookup forms on `zs_db_fetch` and `zs_txn_fetch`:
  `ZS_FETCHPREV` returns the record with the largest key ≤ the given key,
  and `ZS_FETCHNEXT` the record with the smallest key ≥ it — each resolved
  by the same merge as iteration in its direction (D-14l, G-7), so a point
  lookup structurally cannot disagree with a walk. Composed with
  `ZS_SKIPROOT` either bound is strict, exactly as a cursor's seek treats
  it: the family is two directions by two bounds, with one spelling each.
  The transactional form sees the transaction's own pending writes, like
  every other read (A-1a). `ZS_FETCHNEXT` and `ZS_FETCHPREV` together are a
  usage error (`ZS_BADUSAGE`). A-4's lifetime applies to the result
  unchanged. (History: bare `ZS_FETCHNEXT` was the strict bound until
  2026-08-13, when the strictness moved to the modifier for symmetry with
  `ZS_FETCHPREV`; every known consumer was updated with the change.)
- **A-13** `ZS_REVERSE` on `zs_db_begin_cursor` and `zs_txn_begin_cursor`
  opens a D-14k cursor: a null or empty start key positions at the last key
  in the database; a non-empty one at the largest key ≤ it, with
  `ZS_SKIPROOT` skipping an exact match; `ZS_CURSOR_PREFIX` composes as
  D-14k describes. `zs_cursor_next` steps toward smaller keys;
  `zs_cursor_replace` and `zs_cursor_delete` work at the current position
  unchanged, and A-4 applies to everything yielded. `ZS_REVERSE` with
  `ZS_CURSOR_LIVE`, or on either `foreach` form, is a usage error
  (`ZS_BADUSAGE`): neither is needed by any known consumer, and a rejected
  flag is cheaper than an untested promise.

## 11. Conformance suite

The suite has two halves. **T-1 to T-11 are per-implementation tests**, run
inside one implementation in whatever harness suits it — for the C
implementation, `zstest`: one binary, substring filter, fresh temp directory per
test, `ASSERT_*` macros, alongside `zeroskip.h`, `zeroskip.c`, `xxhash.h`,
`zstool.c` and a `Makefile`. **T-12 to T-14 are cross-implementation tests**,
run by a shared language-neutral runner over every implementation.

### 9.1 The interop harness

- **T-0 The corpus is language-neutral.** `tests/corpus/` holds data files
  alongside a description of each in a portable text format, not in any
  implementation's source. Every implementation validates against the same
  bytes, and any implementation may generate the corpus — a corpus that only one
  can produce proves nothing.
- **T-0b Corpus workloads avoid writer-choice-dependent bytes.** A buffered
  writer coalesces a transaction to one record per key; a streaming writer
  records its history, later records shadowing earlier by offset (D-17b).
  Both are conforming, so a corpus operation sequence MUST NOT store or
  delete the same key twice within one transaction — the resulting bytes
  would depend on which conforming writer generated them, and T-0's "any
  implementation may generate" would silently stop being true.
- **T-0a Driver contract.** Each implementation MUST provide a small executable
  exposing a fixed set of subcommands over a database directory, so one runner
  can drive all of them:

  | Subcommand | Behaviour |
  |---|---|
  | `create --uuid U` | create a database with a given UUID, so output is reproducible |
  | `store K V` / `delete K` | one transaction, one operation |
  | `batch < script` | a sequence of operations in one transaction, so multi-record spans are testable; a trailing `abort` line aborts it instead of committing, so a natively written `ROLLBACK` span (C-8) is testable too |
  | `get K` | print the value, or a defined not-found marker |
  | `scan [--prefix P]` | print every visible pair in comparator order |
  | `dump` | print structure — files, generations, spans, record types, offsets |
  | `check` | run the consistency checks (F-28, F-26f) and report |
  | `convert` / `repack` | force one conversion or one repack |
  | `hold-write --for MS` | take the write lock and hold it, for lock-contention tests |

  Output is a defined line format so the runner compares text, not internals.
  Without this each language reimplements the suite and they drift apart, which
  is the failure mode the whole exercise exists to prevent.

**T-1 Golden vectors.** Byte-exact encode assertions against checked-in files,
deterministic because F-15 makes encoding canonical, with `zstool` accepting an
explicit UUID so corpus generation needs no test hook in the library. Decode
assertions from the corpus description (T-0). Generated for engines 0 and 1 (F-5d), which
also pins that a file's engine comes from its own header rather than the
reader's configuration. `doc/conformance.md` plus this corpus is what an
independent implementation validates against.

**T-2 Magic and versions.** All 16 magic bytes required (F-6); each
single-byte mutation rejected; the specific corruptions the magic is designed
to catch — eighth bit stripped, `0D 0A` collapsed to `0A`, `0A` expanded to
`0D 0A`, and byte 0 replaced by the UTF-8 substitution character's encoding
`EF BF BD` (F-6a) — each rejected. Also that the 16 bytes fail a UTF-8 validity
check, so the property is asserted rather than merely believed. Read and write
versions above the library's own rejected appropriately, and a file that is
readable but not writable accepted read-only (F-7).

**T-2a The trailer.** Opening an in-order file depends entirely on it, so: the
16-byte trailer read without prior knowledge of the file, the back pointer
locating the section, and the checksum verified over section-through-back-pointer
(F-26b). Negatively — a back pointer past the end of the file, before the
header, not 8-aligned, or pointing at a byte that is not a `PTRS32`/`PTRS64`
type; a file shorter than header plus trailer; and a corrupted pad byte — each
rejected rather than read. The records checksum verified on demand and asserted
to catch a record body corrupted in place (F-26e), which nothing else would
detect in an in-order file.

**T-2c Interoperability constants.** The values two implementations must agree on
bit-for-bit, each asserted against a literal rather than against the
implementation's own computation: `XXH3_64bits` with seed 0 over known inputs,
truncated low-32 and little-endian (F-5b), including the empty input that F-26g
needs; the default comparator's total order over a table that includes a key and
its own prefix, keys differing only above `0x7F` — which a signed-`char` compare
gets wrong — and the empty-versus-one-byte case (F-11a); the exact bytes of the
`memcmp` comparator name field (F-11b); and a generated filename for a known UUID
and generation range, character for character (D-0, D-1).

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
only in-order files as inputs (D-16), and the cascade reaching the
geometric size relation after many rollovers — and that an empty output is
still written (D-22).

**T-8 Crash injection.** A test build interposes `write`, `fdatasync`, `rename`
and `unlink`, counts calls, and aborts at call *N* for every *N* over a scripted
workload. Both durability gates are therefore crash points in their own right
(C-7), including the window between them — the state a single-gate design could
not distinguish. Each case asserts reopen terminates within a timeout, exactly a
prefix of committed transactions is visible, nothing acknowledged is lost under
default durability, and a writer can then continue. Targeted: crash between the
records and the first gate; between the two gates; after the terminator but
before the second gate; mid-publish rename;
mid-repack; after the pointer section but before the trailer; leaving a non-8-aligned file
length; and after an invalid terminator, asserting the writer moves to a new
generation rather than appending (R-4, D-9). Both durability modes. Separately,
with directory syncs suppressed, that a crash can lose a *name* — the test that
justifies C-6.

**T-8a Sync failure.** The case C-7a exists for, which no crash test reaches:
`fdatasync` made to fail. On gate 1 failing, assert no terminator was written and
the error reached the caller, so the transaction plainly did not happen. On gate 2
failing, assert the database is correct whichever way the terminator landed —
either the commit is visible with durable data, or the span reads as absent — and
in particular that the implementation does **not** retry the sync and treat
success as proof the data survived, since a second `fdatasync` can succeed after
the dirty pages have already been discarded.

**T-9 File set discovery.** That the set and every range are derived from
filenames alone, without opening a file. Overlap resolution (D-5) driven by a
directory seeded as an interrupted repack — output present alongside its inputs —
asserting the output wins and the contained files are ignored for reading. D-5's scan
over each overlap shape in D-5a's table: a repack output with its inputs, asserting
the widest wins; the same with some inputs already unlinked, asserting the set
still tiles; a directory seeded mid-conversion, asserting the in-order file wins,
the set is judged complete, and leaving it that way indefinitely — as a writer
death would — does not make readers retry forever; and all three files
sharing a `start` at once. Taking the *first* file instead asserted to fail, so
D-5b's error is caught rather than rediscovered. A partial overlap reported rather
than worked around (D-5c). And the prefix property D-1a depends on, asserted
directly on generated names, so adding an extension later breaks a test rather
than the database (D-5c). Sets
that do **not** tile rejected and retried (D-7): a missing middle generation, a
gap at the bottom, two files claiming overlapping ranges that are not nested.
Files disagreeing on UUID rejected rather than resolved by majority (D-4a), and
files disagreeing on comparator rejected (F-11). A database with the lock file
absent opened successfully, recreating it (D-3a). A staging name already taken,
asserting `O_EXCL` advances rather than overwriting (D-20a). Foreign names and
staging files ignored (D-4). And that the next generation is one above the
highest present, including after files have been removed (D-9b).

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
lock, asserting refusal (D-23). Concurrent repack and writer both proceeding,
with publish serialised — and a writer whose conversion finds the packer lock held
by a repack, asserting it skips rather than waits and that the next writer
performs it (D-12b).

**T-10b The snapshot protocol.** Each step of C-4 attacked directly, since these
fail only under concurrency. A reader interrupted between scanning the directory
and opening files, with a repack completing in the gap, asserting the retry
converges on a tiling set (C-4a, C-4b). A file unlinked between steps 2 and 3,
asserting `ENOENT` triggers a retry rather than a partial snapshot. A
reader holding a snapshot while the writer commits repeatedly, asserting bytes
below its boundary never change and growth above it is invisible (C-4c). And a
writer killed mid-span while a reader scans, asserting the reader stops at the
last valid terminator (C-4f) — the case that shows the terminator checksum, not a
lock, is what makes reading a live file safe.

**T-10a Steady state.** That the number of unordered files returns to **at
most** one after each commit — exactly one while the active file remains below
`rollover_size`, zero immediately after a commit that crossed it and sealed
(D-25d) — by driving many rollovers through a writer and asserting after each
commit that no more than one unordered file remains, that any unordered file
is the newest, and that the rest are in-order (D-12a). Then a backlog: several
crashes each leaving an unclean active file, asserting successive writers
convert them oldest first and the count drains.

**T-12 Write-here-read-there.** For every ordered pair of implementations *(A,
B)*: *A* creates a database and performs a workload; *B* opens it and its `scan`
output MUST match *A*'s byte for byte. Then *B* writes further and *A* reads
back. Repeated with the workload arranged to exercise each structural state —
records still in the active file, a converted single-generation in-order file, a
merged multi-generation file, a rolled-back span, a tombstone whose chain leaves
the file, an empty in-order file, keys spanning the short/big encoding boundary,
and keys containing NUL bytes.

**T-12a Byte-for-byte agreement.** Given the same UUID and the same operation
sequence, every implementation MUST produce **identical files**, since encoding
is canonical (F-15, F-26c) and no timestamps or nondeterminism enter the format.
The runner diffs the directories. This is a much sharper test than reading each
other's output, because it catches divergence in padding, in ancestor omission,
in the choice of short versus big form, and in checksum seeding — before that
divergence has a chance to become a compatibility rule nobody meant to make.

**T-13 Cross-implementation concurrency.** The tests most likely to find real
bugs, since they exercise the locking interop surface (C-1e):

- *A* holds the write lock via `hold-write` while *B* attempts a write:
  *B* MUST block, or return `ZS_LOCKED` under its non-blocking option. **A pass
  here where either side used `flock` would be a false pass on Linux**, so the
  runner also asserts the lock is visible to a third observer using `fcntl`
  directly.
- *A* writes continuously while *B* scans repeatedly: every *B* snapshot MUST be
  a consistent prefix of *A*'s commits, never a mixture.
- *A* repacks while *B* writes, and the reverse, asserting both complete and the
  resulting set tiles (D-6) — the concrete test of C-1a's disjointness claim.
- *A* is `SIGKILL`ed holding the write lock; *B* MUST proceed with no manual
  intervention (G-5).
- *A* and *B* both attempt to remove files concurrently, asserting the surviving
  set still tiles (D-23).
- the lock file unlinked while *A* holds the write lock, then *B* started:
  asserting the implementations still exclude each other, or — if they cannot,
  which is the honest outcome — that the test documents D-3b's hazard rather than
  appearing to pass.

**T-14 Two handles in one process.** Two write handles on one database from a
single process, asserting that the second is excluded (C-1j): it blocks, or
reports `ZS_LOCKED` under `ZS_NONBLOCKING`. Where the implementation has both
of C-1j's mechanisms, the test MUST run against **each** — the registry path is
otherwise dead code on every platform that has `F_OFD_SETLK`, which is where
development happens, so it would rot unexercised and be discovered by the one
platform that depends on it.

The test began as the opposite assertion — that an implementation does *not*
claim to exclude them — because a per-handle mutex excludes only threads
sharing a handle and would have been a guarantee the format cannot enforce.
That hazard is unchanged; what changed is that C-1j names two mechanisms which
key on the *database's* identity rather than the handle's, and so deliver the
property the mutex only appeared to. An implementation that excludes them by a
mutex is still wrong, and this test cannot tell the two apart — only reading
the code can.

**T-11 Traceability.** `doc/conformance.md` maps every normative requirement
here to the test enforcing it. A requirement with no test is a gap to close. Each
implementation records which requirements it passes, so partial conformance is
visible.

## 12. Non-goals

Multi-writer concurrency; thread-safe handles; cross-database transactions;
secondary indexes; compression; network access; in-place value mutation. A Cyrus `cyrusdb`
backend is a thin separate adapter, out of scope.

## 13. Open items

1. **Repack duration is unbounded.** D-16 can cascade into rewriting the whole
   database while the writer continues appending. The packer lock permits one
   repack at a time, but nothing bounds how long one runs or lets it be
   interrupted and resumed. This is a deliberate trade for lower total I/O and
   a smaller file count (D-16b), and writing continues throughout regardless.
   If it ever needs bounding, two mitigations preserve the same steady state:
   merge pairwise, one step per invocation, or cap the cascade at a projected
   output size and resume next time.

   Compaction (D-26) makes this unboundedness a **deliberate API entry point**
   rather than an emergent property of D-16's cascade — a caller asks for the
   whole database to be rewritten and gets exactly that. Both mitigations apply
   to it unchanged, and neither is implemented.
**Resolved.** *Whether a shared index is worth reintroducing.* Measurement said
yes — roughly 1.5 ms per open at the 2 MB `rollover_size` ceiling, flat per
record, which is the dominant cost for a process that opens a database per
request. §8 is the answer, and it is a different shape from the one sketched
here. That sketch was an append-only `(key, offset)` log per file inside the
database directory, published by a single aligned atomic, with readers sorting
privately: it traded a scan for a sort. §8 publishes the **sorted** table
instead, which removes the sort as well as the scan, and puts it outside the
database directory, which leaves D-1, D-2, D-4 and the file-set scan untouched.
The cost of that choice is P-17: a table outlives an out-of-band restore of the
database directory, so a cache directory must be scoped to the database
instance.
