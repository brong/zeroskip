# Pointer table cache — design

Date: 2026-08-07
Status: approved, not yet implemented

## Problem

Every snapshot replays the active unordered file's spans and merge-sorts the
resulting record offsets to build a private index (`zsi_index_build`,
`zeroskip.c:2120`, reached from `zsi_snapshot_take`). Readers pay this at
`zs_db_open`; writers pay it again on the refresh paths.

`doc/benchmarking.md` already measures it: roughly **1.5 ms per open** at the
2 MB default `rollover_size`, with a flat per-record cost. For a long-lived
handle that is irrelevant. For a process that opens a database per request it is
the dominant cost.

This is spec **open item 2**, which proposes sharing the work through an
append-only `(key, offset)` log inside the database directory, published by a
single aligned atomic, with readers still sorting privately.

## Chosen shape

A **pointer table cache**: a separately configured directory in which any process
that builds a sorted pointer table over an unordered file publishes it by
`rename`, and from which any process may load one and replay only the suffix
beyond it.

This differs from open item 2 deliberately, and better:

- it removes the **sort** as well as the scan, because what is published is
  already in key order;
- it keeps the database directory pure — nothing new is written there, so
  D-1/D-2/D-4 and the file-set scan are untouched;
- publication is `rename`, which is the same publication mechanism the rest of
  the format already uses, rather than a new aligned-atomic protocol.

The cost is that the cache is a second artefact with its own lifetime, and one
that a restore-from-backup can outlive. Section 4 addresses that.

## 1. Configuration

Two new fields in `struct zs_open_data`:

```c
const char *index_dir;        /* NULL = no pointer table cache (default) */
size_t      index_threshold;  /* 0 = default rollover_size/8 */
```

`index_dir` NULL leaves every current behaviour, test and golden corpus case
untouched. The feature is opt-in, and the library never chooses a directory
itself: a planted or corrupt table produces **wrong answers** (offsets are
bounds-checked at `zeroskip.c:1256`, so it is not memory-unsafe, but it can point
at the wrong record), and a world-writable default such as `/tmp` would make
planting one trivial for any local user. The caller names a directory it
controls.

`index_dir` is **rejected with `ZS_BADUSAGE` if it names the database
directory**. Otherwise a read-only handle would write into the database, and R-3
would be broken in substance rather than only in wording.

The library does not create `index_dir`. If it is missing or unwritable, that is
reported once through `db->error` and the handle runs without a cache. One
directory serves any number of databases, because table names carry the uuid.

## 2. What is cached

One table per **unordered file**, not only the active one. It is the same code
path, and it also serves a read-only handle that finds a stranded unconverted
file it has no right to convert. In-order files already carry a pointer section
on disk and need nothing.

A table holds the private index's merged base+delta as one sorted array of record
offsets, and nothing else.

Storing an inline key prefix alongside each offset — which would make binary
search cache-friendly instead of chasing pointers into the mapped data file — is
a real further win and is **explicitly deferred**. The version field leaves room
to add it.

## 3. File format

This is interoperability surface: a table published by one implementation is
usable by another.

Published name, in `index_dir`:

    zeroskip.index-<uuid>-<GEN8hex>

`<uuid>` is the lowercase hyphenated form used by D-0; `<GEN8hex>` is the
uppercase 8-digit hex generation used by D-1. The `zeroskip.` prefix is the
existing metadata namespace (D-2), so a table can never be parsed as a data file
even if the two directories were somehow the same.

Staging name, in `index_dir`:

    zeroskip.tmp.<pid>.<8 random hex digits>

then `rename` into the published name. This is the staging convention repack and
conversion already use, plus random digits so that two processes sharing an
`index_dir` across a network filesystem cannot collide on a temp name.

### Layout

```
 0  16  magic  89 7A 73 69 6E 64 65 78 31 0D 0A 1A 0A 00 00 00
               "\x89zsindex1\r\n\x1a\n\0\0\0"
16   1  version_read   (1)
17   1  version_write  (1)
18   2  flags   low 4 bits: checksum engine
                bit 4: built with checksum verification
                       (clear means built under ZS_NOCSUM)
20   4  reserved (zero)
24  16  uuid
40   4  start          generation of the unordered file this covers
44   4  reserved (zero)
48  16  compar         comparator name, byte-identical to the data file's field
64   8  valid_upto     data-file offset covered; always a span boundary
72   8  nptrs
80   4  term_csum      checksum carried by the terminator immediately below
                       valid_upto; 0 when the file has no spans
84   4  header checksum over [0, 84)
88      nptrs x 8-byte little-endian record offsets, sorted by key ascending
        under compar, one entry per distinct key, each being the newest
        committed record for that key below valid_upto
        4-byte checksum over the pointer array
```

Total size is `88 + 8 * nptrs + 4`.

The magic follows the same construction as the data-file magic and for the same
reasons: high bit set so no text file is mistaken for one and a transfer that
strips the eighth bit is detected, invalid UTF-8 at byte 0, a CR-LF trap, a DOS
end-of-file, a bare LF, and NUL padding. It is a **different** 16 bytes from the
data-file magic, so the two artefacts are never confused by content either.

The checksum engine is **the data file's**, not the handle's — the same rule that
already governs appending (A-6, F-5a). A peer able to read the data file can
therefore always validate its table. Using the handle's engine instead would
produce tables that a conforming peer must reject, which is the same silent
data-path failure the appending rule exists to prevent.

## 4. Acceptance

A reader uses a table only if **all** of the following hold. Otherwise it ignores
the table and builds from scratch. A rejected table is never an error and is
never reported as corruption: it is a cache.

- magic, header checksum and pointer-array checksum all validate
- `version_read` is not greater than ours
- uuid and `start` match the data file
- `compar` matches both the data file's comparator name field and our own
- `H <= valid_upto <= data file size`, and every offset lies in `[H, valid_upto)`,
  where `H` is the **data file's** header length, 72 — not the table's, which is 88
- flags bit 4 is set, unless we are ourselves running under `ZS_NOCSUM` — an
  index built without checksum verification may contain records a verifying
  reader would reject, so it must not be handed to one
- the terminator immediately below `valid_upto` carries checksum `term_csum`, or
  `valid_upto == H` (the file has no spans) and `term_csum == 0`

### Why `term_csum`, and what it does not cover

A table is bound to a data file by (uuid, generation, `valid_upto`). That is
sound while files are append-only and generations are never reissued, which the
format guarantees for every in-spec sequence of operations.

It is **not** sound across an out-of-band event: restoring a database directory
from backup while the cache directory survives yields the same uuid and the same
generation numbers over different bytes, and a stale table would then return
wrong records.

Checksumming the whole covered prefix would close this completely, and is
rejected: the cache's largest benefit on a cold page cache is *not touching* the
data file at all, and hashing the prefix forces reading all of it.

`term_csum` is the O(1) compromise. The terminator immediately below `valid_upto`
already carries a checksum over its span and itself, so comparing it costs one
page. It catches the restore case with high probability but cannot detect
divergence confined to an earlier span.

**Operational consequence, which must be documented in the spec:** an
`index_dir` must be scoped to the lifetime of the database instance. A per-boot
temporary directory satisfies this naturally. A caller that restores a database
directory out of band MUST discard the corresponding tables.

## 5. Read path

`zsi_index_build` gains a front end:

1. If `index_dir` is set, try to load a table for this file's (uuid,
   generation). On acceptance, take its array as the index's `base`, leave the
   delta empty, and set `f->complete = valid_upto`.
2. Replay spans starting at `valid_upto` rather than at the header length,
   folding each committed record in through `zsi_index_insert`, which already
   handles the delta and its bounded merge. This advances `f->complete` to the
   true end.
3. With no usable table, perform today's full replay from the header length.

The only change to existing machinery is parameterising `zsi_unordered_replay`
with a start offset. Beginning at any span boundary is already sound, because a
span is self-delimiting and self-validating; `valid_upto` is always such a
boundary by construction.

## 6. Publication

One rule, applied by readers and writers alike. After building or extending an
index over an unordered file, if

    f->complete - table_valid_upto >= index_threshold

(taking `table_valid_upto` as the header length when no table was loaded), then
publish a table covering `f->complete`.

That single rule covers the writer, after the incremental `zsi_index_insert` at
commit (`zeroskip.c:3973`), and the reader, after a snapshot build. Publication
is safe whoever performs it, because it is a write to a fresh temp file followed
by an atomic rename.

The threshold is what keeps this from being quadratic. Dumping on every commit
would rewrite the whole table each time — 128 KB for a 2 MB active file of
100-byte values, so roughly 1 GB over one rollover in the one-record-per-
transaction case, plus an O(n) base+delta merge per commit. The threshold bounds
both the publisher's write amplification and the next reader's catch-up replay
to the same figure.

Further properties:

- **No fsync.** The table is rebuildable, and a torn or zero-filled file after a
  crash is rejected by its checksums. Syncing it would put an fsync back onto the
  commit path, which is the cost this work exists to reduce.
- A publish failure never fails a commit and never propagates to the caller. It
  is reported once per handle through `db->error`, then suppressed.
- Concurrent publishers are fine: distinct temp names, atomic rename, last writer
  wins, and both contents are individually valid.

## 7. Cleanup

When a handle publishes a table for generation G, it unlinks tables in
`index_dir` that carry the same uuid and a generation not present as an unordered
file in its snapshot. The handle has the file set in hand, so this is nearly
free, and it is self-maintaining: whoever publishes tidies up.

Unlinking is safe against a concurrent reader. A descriptor already open survives
the unlink, and a reader that misses the table simply rebuilds.

Generations that are still present as unordered files are kept, so a stranded
unconverted file does not lose its table.

## 8. Code layering

A new section **POINTER TABLE CACHE** in `zeroskip.c`, between `PRIVATE INDEX`
and `PER-FILE CURSOR`. It may call downwards into `PRIVATE INDEX` and
`FILE OBJECT` only. `SNAPSHOT` and `WRITE PATH` call into it.

## 9. Spec changes

These land in their own commit, before any code, per the project's rule that the
spec wins or the spec changes deliberately.

- New top-level section **8. Pointer table cache**, inserted after §7 Open and
  recovery, with requirements prefixed **`P-n`** — F, D, C, R, A, G and T are
  already taken. Sections 8 to 11 renumber to 9 to 12; the four existing §8
  references (three in the preamble, one at the head of the C binding) and
  `doc/implementation-plan.md`'s section map are updated to match. It belongs at
  top level rather than under §5 because it spans format, layout and
  concurrency; the preamble's "what is normative" list gains it.
- **R-3** amended: a reader MUST NOT write *to the database*. Publishing a
  pointer table into a separately configured cache directory is not a database
  write.
- **G-6** amended: the cache directory holds no mutable state either — tables are
  published by rename and never modified — and a conforming implementation MUST
  produce identical results with the cache absent.
- New **A-n** entries for the two `zs_open_data` fields.
- **Open item 2 closed**, recording that the chosen shape differs from the one
  sketched there, and why.
- `doc/conformance.md` gains a row per `P-n`.

The following become interoperability surface and cannot change without a further
spec commit: the 16 magic bytes, the 88-byte header layout, the pointer array
encoding and its trailing checksum, the published and staging name formats, and
the acceptance rules in section 4.

## 10. Testing

`zstest`:

- a cached build and a from-scratch build agree, key for key
- suffix replay across a span that begins below `valid_upto` and ends above it
- each acceptance rule rejected individually: bad magic, bad header checksum,
  bad array checksum, wrong uuid, wrong generation, wrong comparator name, a
  `nocsum`-built table offered to a verifying reader, `valid_upto` past
  end-of-file, `valid_upto` below the header length, an offset outside
  `[72, valid_upto)`
- `term_csum` mismatch rejected
- threshold behaviour: no publication below it, publication at it
- publication while another handle holds a snapshot over the same file
- superseded generations unlinked, live unordered generations retained
- `index_dir` deleted mid-run: reads still correct
- `index_dir` naming the database directory rejected with `ZS_BADUSAGE`

`tests/mutate.sh` — one mutant per new requirement, since a test that cannot fail
reads as coverage while providing none:

- drop the comparator-name check
- drop the `term_csum` check
- accept a `nocsum`-built table under a verifying reader
- publish by writing the published name in place instead of renaming
- publish a `valid_upto` that is not a span boundary
- checksum the table with the handle's engine instead of the data file's

`tests/corpus/` gains a golden pointer table with its portable text description,
because the format is now interop surface.

`zstool` gains a subcommand to dump and validate a table, so the T-0a driver
contract and the interop runner can compare it as text.

`zsbench` gains an `open (cached)` row beside the existing `snapshot open`, and
`doc/benchmarking.md` carries the before-and-after.

## 11. Deliberately out of scope

- Inline key prefixes in the table (section 2).
- Caching anything for in-order files, which already have a pointer section.
- Any table for the transaction's own uncommitted records.
- Automatic creation of `index_dir` by the library.
- Sharing tables between databases with different comparators, which acceptance
  forbids outright.

## 12. Open choices

Both are guesses to be settled during implementation rather than assumptions
baked in:

- The **88-byte header layout**. It could be padded or rearranged to mirror the
  72-byte data header's shape more closely.
- The **`rollover_size / 8` threshold default**. `zsbench` should choose it.
