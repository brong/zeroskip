# Seal, compact and salvage — design

Date: 2026-08-07
Status: all three approved. A and B ready to plan; C needs its own plan.

## Why these three together

They came out of one question — "is there a call that makes the database fully
indexed?" — and the answer is no, in three separate ways. They are **three
sub-projects**, sequenced, not one feature:

| | | depends on |
|---|---|---|
| **A** | `zs_db_seal()` — convert the active generation | — |
| **B** | `zs_db_compact()` — everything into one file | A |
| **C** | `zs_db_salvage()` — rebuild what is readable from a damaged directory | — |

A is small and bounded. B is unbounded and adds one merge entry point. C is the
largest and is different in kind: it must read structures the normal path
deliberately **refuses** to read, so it cannot share the read path and should not
try.

Land A, then B. Design C separately.

## What exists today

- `zs_db_repack()` works **only on in-order files** (D-15/D-16) and selects a
  geometric suffix: start at the newest, include the next lower while the
  accumulated size is at least as large, stop otherwise. On a well-shaped
  database it stops early by design. It is not "merge everything".
- Conversion of **non-active** unordered files happens inside any commit
  (`zsi_convert_pending`); `zstool convert` triggers it with an empty
  transaction. It explicitly skips the active file.
- Nothing rolls the active file over on demand: `zsi_writer_active` rolls over
  only when `act->size >= db->rollover_size`.

So the newest generation is always unordered and always needs replaying, which is
the cost the pointer table cache (spec section 8) works around rather than
removes.

---

# A. `zs_db_seal`

```c
int zs_db_seal(struct zs_db *db);
```

Takes the write lock and converts the active file through the existing
`zsi_convert_one`, then refreshes.

**No new file is created.** A conversion output covers the same generation range
as its input (D-5a), so afterwards the newest file is in-order,
`zsi_snapshot_active` returns NULL — a state the code already handles — and the
next write creates a fresh generation. Repeated seals therefore do not burn
generations, which a "roll over, then convert" implementation would.

Safe because the write lock is exactly what stops another writer appending to the
file being converted — the reason D-12 skips the active file does not apply to a
caller holding that lock. Readers on an older snapshot keep their mapping; the
input is removed under D-23 and the surviving set still tiles, because the output
covers the same range.

### No-ops, all returning `ZS_OK`

- no active file — already sealed
- an active file with no spans — converting it would write an empty in-order
  file and consume a generation for nothing
- an active file whose header is invalid (D-10) — nothing to read; reported
  through `db->error`

An **unclean** active file is not a no-op and is arguably the best reason to call
this: conversion reads to the complete point (F-24), so the garbage tail simply
does not survive into the output.

### Cost

Bounded by `rollover_size` — the same bound D-12d already puts on a writer's
inline conversion. Cheap enough to call routinely, including before a backup or
before handing a database to read-only consumers.

---

# B. `zs_db_compact`

```c
int zs_db_compact(struct zs_db *db);
```

1. `zs_db_seal()` — a single file cannot exclude the newest generation.
2. Convert any remaining non-active unordered files (`zsi_convert_pending`).
3. Merge **every** in-order file into one.

Step 3 is the only thing that differs from `zs_db_repack`: it bypasses D-16's
geometric selection. Implemented by giving the selection a `bool full` rather
than by calling `zsi_repack_merge` from a second place — one merge entry point,
not two.

### What it reclaims that a repack cannot

D-19: a key is removed entirely **iff** its latest version is a deletion *and*
V1's ancestor lies inside the output range. A compaction spanning generations
1..N contains every ancestor, so every fully-deleted key's tombstone is dropped.
A partial repack structurally cannot do this — it must retain tombstones because
an older file outside its input set may still hold the key (D-19a).

The existing merge already implements D-19 correctly. Full compaction gets the
reclamation for free by widening the input set; no new retention logic.

### Contract

**Best effort in action, strict in reporting.** It merges everything mergeable,
reports anything it had to leave behind through `db->error` — the same route
D-10a already uses — and only then returns `ZS_BADFORMAT` if the result is not a
single file. So the return value is a guarantee, without a damaged database
losing the reclamation it could have had.

A non-active file with an invalid header is the case that blocks it: D-10a
tolerates such a file, but it cannot be converted or merged.

### Cost

Unbounded, holding the repack lock throughout while writers continue — the same
shape `zs_db_repack` already has, and spec open item 1's unboundedness now
deliberately reachable from the API. That is recorded rather than mitigated.

### Locking

Seal takes the write lock. Compact takes the repack lock, then the write lock for
its seal step, then releases the write lock before merging. Write lock **inside**
repack lock, one consistent order (C-1h), so a compaction does not hold up
writers for its whole duration.

### Interaction with the pointer table cache

Falls out with no new code. After sealing, that generation is no longer an
unordered file, so `zsi_idx_sweep` (P-16) unlinks its table on the next snapshot.
A fully compacted database has no unordered file and therefore no tables at all —
which is the honest statement that these features overlap: if you can seal on
demand, the cache matters less for a read-mostly database, while still carrying
the active file between compactions and still giving the 10× on writes.

### Spec changes for A and B

Their own commit, before the code. New requirements in §5 beside D-12 and D-16
covering: seal's conversion of the active file under the write lock; the no-op
cases; compaction's ordering (seal, convert, merge-all); the strict return
contract; and the lock order. New `A-n` entries for both calls. A note on open
item 1 that the unbounded path is now an API entry point. Conformance rows.

### Testing for A and B

**Seal.** The active file becomes in-order with its range unchanged; no new
generation appears; a second seal is a no-op; the empty, absent and
invalid-header cases; an unclean active file seals to its complete point and the
garbage does not survive; every key reads back identically before and after; a
reader holding a snapshot across the seal is unaffected.

**Compact.** One file spanning 1..N; tombstones for fully-deleted keys are gone
and the keys stay absent; a key deleted and then rewritten survives with its new
value; `ZS_BADFORMAT` plus a report when a bad-header file blocks it, with
everything else still merged; `zs_db_should_repack` false afterwards; the whole
database reads back identically.

Plus `zstool seal` and `zstool compact` for the T-0a driver contract, a mutant
per new requirement, and a `bench_compact` row.

---

# C. Salvage

**Out-of-place rebuild.** Read the damaged directory *without opening it
normally*, scan every file for whatever validates, and write a fresh database
elsewhere through the ordinary write path. The original is never modified, so
salvage can guess, skip and improvise without risking the only copy — which is
also what R-4's "there is no in-place repair" already says about the library
itself.

### What it must recover, and why each is unrecoverable today

1. **A missing generation.** `zsi_fileset_resolve` returns `ZS_AGAIN` on a gap
   (`zeroskip.c:3393`), the snapshot retries and gives up (`:3631`). One lost
   file makes every other file inaccessible, though all of them are readable.
   Salvage must ignore the tiling requirement entirely.
2. **A bad span mid-file.** F-24 makes a file complete at its last valid span, so
   one corrupt span discards every later span in that generation — even though
   each later span is intact and independently checksummed. Salvage must resync
   past the damage and take the rest.
3. **A corrupt pointer section.** `zsi_ptrs_load` failure is fatal in
   `zsi_snapshot_take`, although the records region beside it may be perfect.
   Salvage must ignore the pointer section and re-derive order by scanning.
4. **An invalid file header.** D-10a makes such a file contribute nothing. The
   generation is recoverable from the filename; only the checksum engine is
   genuinely unknown, and there are three of them to try.

### The comparator is not a problem

This looked like an open question and dissolves. Salvage writes a **new**
database through the ordinary write path, so the output is ordered by whatever
comparator the caller supplies, and recency (D-14) is resolved by generation and
offset rather than by key order. The source's comparator therefore never affects
the correctness of the output. Salvage takes `compar` / `compar_name` exactly as
`zs_db_open` does, compares against any header that is readable, and warns on a
mismatch. Where no header is readable there is nothing to check and nothing that
needs checking.

### Resynchronisation, and why the default is fully verified

A span terminator carries `spanlen` (F-19, `zeroskip.c:1166`). That is what makes
recovery after mid-file damage sound rather than a guess:

> From the failed position, step forward in 8-byte increments (F-2). At each
> offset attempt `zsi_term_decode`. On success, compute
> `span_start = pos - spanlen`; if that lands at or after the last known good
> boundary, checksum `[span_start, pos)` together with the terminator's own bytes
> and compare against the terminator's stored checksum. A match is a **genuine,
> verified span**, and the walk resumes normally from there.

So the default recovers everything after the damage with full checksum
verification, which is stronger than merely "taking what decodes". What cannot be
recovered by verification is the damaged span's **own** records, because its
terminator is precisely what would prove them.

That reshapes the uncommitted-records question. Position is no longer the
discriminator; verifiability is:

- **Verified spans** — recovered by default, wherever they sit. This is the big
  win: today one bad span discards every later span in that generation (F-24).
- **Rolled-back spans** — never recovered, whatever the flags. F-21 and F-25 make
  them deliberately aborted, and no conforming reader has ever shown them.
- **Unverifiable spans** — a damaged span's own records, and a trailing tail with
  no valid terminator. Recovered only under `ZS_SALVAGE_UNVERIFIED`, and every
  record so recovered is reported. Note these records carry no checksum of their
  own: F-19's terminator checksum is the only thing that ever covered them.

### API

```c
enum zs_salvage_kind {
    ZS_SALVAGE_FILE_UNREADABLE,   /* could not be opened or mapped */
    ZS_SALVAGE_HEADER_INVALID,    /* generation taken from the filename */
    ZS_SALVAGE_ENGINE_GUESSED,    /* which engine the spans validated under */
    ZS_SALVAGE_GAP,               /* a generation range absent from the set */
    ZS_SALVAGE_PTRS_IGNORED,      /* pointer section unusable; order rescanned */
    ZS_SALVAGE_SPAN_LOST,         /* a span that could not be verified */
    ZS_SALVAGE_SPAN_ROLLBACK,     /* deliberately aborted; not recovered */
    ZS_SALVAGE_RESYNC,            /* a verified span found after damage */
    ZS_SALVAGE_KEY_UNVERIFIED,    /* value came from an unverifiable span */
    ZS_SALVAGE_KEY_MAYBE_STALE    /* a newer version may have been in lost bytes */
};

struct zs_salvage_event {
    int          kind;
    const char  *file;        /* data file name, or NULL */
    uint32_t     generation;
    size_t       offset;      /* byte offset within that file */
    size_t       length;      /* bytes affected */
    const char  *key;         /* per-key events only */
    size_t       keylen;
};

typedef int zs_salvage_cb(void *rock, const struct zs_salvage_event *ev);

struct zs_salvage_data {
    uint32_t        flags;         /* ZS_SALVAGE_UNVERIFIED */
    zs_compar      *compar;
    const char     *compar_name;
    zs_csum        *csum;
    zs_salvage_cb  *report;
    void           *rock;
    void          (*error)(const char *msg, const char *fmt, ...);
};

int zs_db_salvage(const char *from, const char *to,
                  struct zs_salvage_data *setup);
```

The library writes no report of its own and chooses no destination for one:
policy stays with the caller. `zstool salvage` renders the events as lines in the
T-0a format, so an operator can grep them and the interop runner can compare
them as text.

### Algorithm

1. `readdir` the source. Parse names (D-1) for uuid and generation range.
   **Neither D-5 resolution nor D-6's tiling check is applied** — a gap is
   reported and stepped over, which is the whole point.
2. Order files oldest-first: by `start` ascending, and for equal `start` the
   narrower range first, since a wider one is a repack output derived from it.
   Records are then applied oldest to newest, so the newest surviving write for
   each key naturally wins and no separate recency pass is needed.
3. Per file: map it; if the header validates take its engine, otherwise report
   `HEADER_INVALID`, take the generation from the filename, and determine the
   engine by trying 1, then 2 if a function was supplied, then 0 — reporting
   `ENGINE_GUESSED`. Engine 0 only "validates" where the stored checksum really
   is zero, so it is a genuine signal rather than a catch-all.
4. In-order files: **ignore the pointer section entirely** and walk the records
   region directly, reporting `PTRS_IGNORED` when the section was unusable. An
   in-order file has no spans (F-23), so this is a flat record walk.
5. Unordered files: walk spans, resynchronising as above.
6. Write into the destination in batched transactions through the public write
   path. Deletions are applied as deletions, so a recovered tombstone still
   removes a key and a key whose newest surviving version is a deletion ends
   absent — which is correct rather than a loss.

### Stale-value detection

Track the earliest damage point per file as `(generation, offset)`. After the
scan, a key is reported `KEY_MAYBE_STALE` if the record that won for it is older
than any damage point — because only then could something we lost have superseded
it. A key whose winning record is newer than all damage is definitely current and
is not reported, which keeps the report small enough to act on.

### Output database

A fresh UUID: it is a new database, and the source may still exist beside it.
The mapping is reported. Salvage does not enable the pointer table cache on
either side.

### What it deliberately does not do

- No in-place repair, ever (R-4). The source is opened read-only and never
  written, which is what lets salvage guess and improvise at all.
- No attempt to reconstruct a missing generation's contents. A gap is reported
  and the surrounding data recovered.
- No recovery of rolled-back spans under any flag.
