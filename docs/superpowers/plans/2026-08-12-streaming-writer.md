# Streaming Writer Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every write transaction streams its records into the active file as
they are stored, so transaction memory is O(keys), not O(bytes written) — a
terabyte transaction must not eat a terabyte of RAM (Brong, 2026-08-12: "the
idea was to write everything").

**Architecture:** The pending array stops owning values and becomes a sorted
key→offset index over records already appended to the active file. A store
encodes and appends immediately (through a small flush-before-read chunk
buffer); commit appends the COMMIT terminator between the two existing
durability gates (C-7 unchanged); abort appends a ROLLBACK terminator — the
shape C-8/F-21 always described, which every reader already handles. Reads of
the transaction's own records decode from a per-transaction list of
geometrically-growing read-only mappings, which is what keeps A-4's pointers
valid without copies.

**Explicitly kept:** the two durability gates, D-25d commit-end sealing, the
D-13b incremental fold, all lock protocol, the entire read path below the txn
arm. **Removed:** the value-retire list (d8d6b5e) — replaced values now live
in the file, immutable, which is A-4 by construction.

## Global Constraints

- Spec-first; every behaviour change is a `spec:` commit before code.
- C-7's two gates per commit, exactly: flush + fdatasync, terminator +
  fdatasync. An abort adds NO sync (see Design D3).
- D-14j-a: transaction cursor positions remain KEYS, never indexes/offsets.
- Mutants for every new test; only name-filtered runs and `--rot-only`
  inline; full runs are overnight jobs.
- `make check` green at every commit.

## Design decisions (argued once, referenced by tasks)

**D1. Offsets, not values, in the pending array.**
`struct zsi_pending` becomes `{char *key; size_t keylen; size_t off; bool
is_delete;}` — the key copy stays (the sorted index and D-14j-a need it), the
value goes. A same-key overwrite appends a new record and updates `off`; the
old record becomes dead bytes in the span, dropped at the file's next
conversion exactly like any shadowed version. A read of a pending key decodes
the record at `off` from the transaction's mappings (D2).

**D2. A-4 by geometric mapping accumulation.**
The transaction keeps a list of read-only `MAP_SHARED` mappings of the active
file, created on demand when a read needs an offset past the last mapping's
extent, each new mapping covering AT LEAST DOUBLE the previous extent — so a
transaction holds at most ~log2(bytes written) mappings (≤ ~40 for a
terabyte) and total VA ≤ 2× the final size. NOTHING is unmapped until the
transaction ends, so every pointer any read returned stays valid (A-4). The
file is append-only under our held write lock, so an old mapping never goes
stale. The chunk buffer is flushed before any read that needs unflushed
bytes: `if (need > flushed) flush()`.

**D3. Abort writes a ROLLBACK terminator and no sync.**
F-21's reasoning: without the ROLLBACK, a later commit's span would enclose
the aborted records. The ROLLBACK must therefore be ON DISK before the next
COMMIT terminator — and the next commit's gate 1 (fdatasync before its own
terminator) guarantees exactly that ordering. A crash before the ROLLBACK
lands leaves a torn span, which F-22/F-24 already discard. An abort that
stored nothing writes nothing, as today.

**D4. The active file is chosen at the first store, not at commit.**
The write lock is held from begin, so D-9's rules apply identically — only
the moment moves. `zsi_writer_active`'s rollover check runs there; D-25d's
commit-end seal is unchanged. An fd and the span base offset live on the txn.

**D5. Ancestors are computed at store time, against the transaction's
snapshot.** The record is encoded when stored, so its ancestor field must be
too. Mid-transaction the file set can change only by repack/conversion
(C-1a), which never changes WHICH generation range holds a key's previous
version — an output covers its inputs' ranges (D-5a), and ancestors resolve
by range. Needs a spec note (F-16/D-19a vicinity), not a spec change.

**D6. The D-13b fold takes its offsets from the pending array.**
`zsi_txn_write_span` dies; commit already knows every surviving record's
offset (`pend[i].off` — the final version per key, which is exactly what
D-13a wants folded).

**D7. `zs_cursor_replace` and IFEXIST/IFNOTEXIST** go through the same
store path; `zsi_lookup`'s txn source decodes at `off` via D2. The txn arm's
`zsi_txn_cur_load` does the same. Values yielded to callers point into D2
mappings or at pend key copies — both transaction-lifetime stable.

**D8. The terminator checksum is computed by re-reading the streamed span
through the mapping.** C-4f's checksum covers the span's BYTES plus the
terminator, and the engine API (`zs_csum`, one-shot — and engine 2 is
caller-supplied, so no incremental variant is possible) needs them
contiguous: at commit AND at abort, flush, take `zsi_txn_at(txn, span_base,
wsize - span_base)` (one mapping covers the whole span by construction), and
hand that to `zsi_term_encode`. O(span) read at terminator time, O(1)
memory; the buffered writer paid the same pass over RAM. A ROLLBACK's
checksum matters as much as a COMMIT's — an invalid rollback span completes
the file early (F-24) and costs everything after it.

**D9. `zsi_txn_write_span` already has `rollback_only` plumbing** (the
streaming writer was always intended); the terminator-encode path and its
A-6 file-engine discipline are reused, the span-buffer accumulation is what
dies.

---

### Task 1: Spec — the streaming writer

**Files:** `doc/specification.md` (C-8 vicinity, F-21 note, F-16/D-19a note,
A-4 note), `CLAUDE.md` ("never produces a ROLLBACK span" bullet flips).

- [ ] Amend the C-8 discussion: this implementation now streams; the
  buffered writer remains conforming for peers; state D3's no-sync-on-abort
  argument and D5's ancestor timing note in the spec's voice.
- [ ] Flip the CLAUDE.md bullet: we now WRITE ROLLBACK spans; the read-side
  handling is no longer only for peers. Note the O(keys) memory promise.
- [ ] Commit: `spec: the writer streams, and aborts write ROLLBACK (C-8)`.

### Task 2: The transaction mapping list (D2)

**Files:** `zeroskip.c` WRITE PATH.

- [ ] `struct zs_txn` gains `{int wfd; size_t span_base; size_t wsize;
  size_t flushed; char *chunk; size_t chunklen, chunkcap; struct zsi_txnmap
  *maps; size_t nmaps;}` where `zsi_txnmap` is `{const char *base; size_t
  len;}`.
- [ ] `zsi_txn_at(txn, off, len)` — return a pointer to bytes [off, off+len)
  of the active file: flush the chunk if needed, find/extend the mapping list
  (geometric rule), return `maps[last].base + off`. Failure returns NULL and
  the caller reports `ZS_IOERROR`.
- [ ] `zsi_txn_flush(txn)` — write the chunk buffer, update `flushed`.
- [ ] All released in `zsi_txn_free`.
- [ ] Tests: `test_txn_mapping_growth` (internals: many stores, assert nmaps
  stays ≤ log2 bound while every earlier returned pointer still reads back).
- [ ] Commit.

### Task 3: Streaming store, commit, abort

**Files:** `zeroskip.c` WRITE PATH; `zstest.c`; `tests/mutate.sh`.

- [ ] First store: `zsi_writer_active` runs (D4), header/engine chosen as at
  today's commit (A-6: the FILE's engine), `span_base` recorded.
- [ ] `zsi_pend_set` becomes: encode record (ancestor per D5, F-32 checksum),
  append via chunk buffer, update/insert `{key, off}` (D1). The retire list
  and its mutant are REMOVED (values no longer owned); the A-4 test
  `test_txn_fetch_survives_overwrite` MUST keep passing unchanged — it now
  passes because the old record is in the map, which was the user's original
  intuition.
- [ ] Commit: flush, gate 1, COMMIT terminator, gate 2; D-13b fold from
  `pend[i].off` (D6); D-25d seal unchanged.
- [ ] Abort: flush, ROLLBACK terminator, no sync (D3); nothing stored ⇒
  nothing written.
- [ ] Read paths (D7): `zsi_fcur_find` txn case, `zsi_txn_cur_load`,
  IFEXIST/IFNOTEXIST decode at `off`.
- [ ] Tests: abort-mid-file leaves later commit readable and aborted keys
  absent after reopen (the F-21 case, now writer-side); a 100MB transaction
  with small `getrusage`-checked heap growth (the actual point — assert
  RSS-ish bound loosely or count pend bytes); dump shows our own ROLLBACK
  (`test_dump_shows_rollback` gains a natively-written sibling);
  crash between stores (fork/kill) opens clean minus the torn span.
- [ ] Mutants: abort writes COMMIT instead of ROLLBACK; ROLLBACK omitted
  entirely (later commit encloses aborted records — the F-21 disaster);
  gate 1 skipped before terminator; ancestor computed against a NULL
  snapshot.
- [ ] Commit.

### Task 4: Corpus and crash suite

- [ ] A corpus case written by the streaming writer whose transaction was
  aborted: data file with a ROLLBACK span produced natively (spec T-0);
  `zstool` may need an `abort` op in the driver contract.
- [ ] `zstest-crash` cases: kill mid-stream, kill between flush and
  terminator, kill after ROLLBACK terminator before anything else.
- [ ] Commit.

### Task 5: Docs, conformance rows, verification

- [ ] Conformance rows for the amended C-8/F-21 behaviour; CLAUDE.md and
  overview updates ("a transaction must fit in memory" dies).
- [ ] `make check`, `make asan`, `make clean && make leaks` (the mapping
  list is prime leak territory), new mutants by name, `--rot-only`.
  Full mutate is an overnight job — schedule, don't block.
- [ ] Commit.

## Self-Review Notes

- The one-write-path property is the point: no buffered/streamed seam.
- G-6 is untouched: mappings accumulate per-txn and are private; nothing a
  reader holds is ever unmapped or mutated.
- NOSYNC composes: it drops the two gates, as today; D3's abort never had one.
- The bench "publish threshold" numbers should not move (same fold, same
  publishes); the single-store-txn bench may improve slightly (no value
  memcpy at commit).
