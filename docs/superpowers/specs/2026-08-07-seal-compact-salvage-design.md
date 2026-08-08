# Seal, compact and salvage — design

Date: 2026-08-07
Status: A and B approved; C's shape approved, its detail needs its own brainstorm

## Why these three together

They came out of one question — "is there a call that makes the database fully
indexed?" — and the answer is no, in three separate ways. They are **three
sub-projects**, sequenced, not one feature:

| | | depends on |
|---|---|---|
| **A** | `zs_db_seal()` — convert the active generation | — |
| **B** | `zs_db_compact()` — everything into one file | A |
| **C** | Salvage — rebuild what is readable from a damaged directory | — |

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

# C. Salvage — shape only

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

### Open questions for its own brainstorm

- **Key order when the comparator name is unreadable.** The output database needs
  a comparator; a damaged header may not say which. Default and warn, or refuse?
- **Recency across recovered records.** D-14 resolves visibility by file
  generation and within-file offset. With gaps and skipped spans, some of that
  ordering evidence is missing. What does salvage do when it cannot tell which of
  two records for one key is newer?
- **What to do with records inside spans that never committed.** They are on
  disk and readable. Recovering them would resurrect writes that a conforming
  reader has always treated as absent.
- **Reporting.** An operator needs to know what was lost, not only what was
  saved.

These are not implementation details; each changes what the tool produces.
