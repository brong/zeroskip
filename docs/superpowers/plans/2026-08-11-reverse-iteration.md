# Reverse Iteration Implementation Plan (sqlite-on-zeroskip)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Predecessor fetch (`ZS_FETCHPREV`, largest key ≤ K; with
`ZS_SKIPROOT`, strictly < K) and reverse cursors (`ZS_REVERSE`) on both
`zs_db_begin_cursor` and `zs_txn_begin_cursor`, honouring the database's
comparator, seeing the transaction's own writes, composing with
`ZS_CURSOR_PREFIX`, and supporting write-through — for SQLite's btree layer.

**Architecture:** No new resolution path (G-7). Each per-file cursor arm
(`zsi_fcur`) gains a direction; the merge's comparator flips the key sign for
reverse and keeps the generation tie-break, so D-14e's steps 2–6 run unchanged
and visibility (newest source wins, tombstones consume keys) is inherited, not
reimplemented. `ZS_FETCHPREV` is a throwaway reverse cursor, exactly as
`ZS_FETCHNEXT` is a throwaway forward one. On-disk format untouched: no
corpus, no interop-surface change.

**Explicitly out of scope** (per the requester): direction change
mid-iteration, reverse `zs_*_foreach`, `ZS_CURSOR_LIVE` composed with reverse
— the latter two are rejected with `ZS_BADUSAGE`.

## Global Constraints

- Spec-first: behaviour lands as `spec:` commits before code.
- Reverse resolves visibility by D-14 like everything else (G-7): per key,
  the newest source wins; a tombstone means absent and consumes the key.
- D-14j-a holds in reverse: positions into mutable structures are KEYS.
- A-4 pointer lifetime is identical for reverse and predecessor results.
- New tests get mutants; mutant RUNS are deferred to the end-of-session
  verification pass (user instruction) — `--rot-only` is allowed.

## Direction semantics (the mirror, stated once)

- Arm reverse seek, inclusive: position on the largest key ≤ K.
  Exclusive: largest key < K. Implemented with the existing lower-bound
  searches: `lb(K)` (first ≥ K) is the exclusive reverse position; inclusive
  is `lb + (exact ? 1 : 0)` (an upper bound). Reverse positions are encoded
  as **counts** (candidate at `pos - 1`, `0` = exhausted), so nothing
  underflows.
- Merge order reversed: exhausted last, then key DESCENDING, then generation
  descending. Equal keys stay contiguous-from-front and newest-first, so
  D-14e step 3 (skip stale duplicates) and step 4 (tombstone filter) are
  direction-blind.
- D-14j-b reversed: resume strictly BELOW the last yielded key.
- `ZS_CURSOR_PREFIX` + `ZS_REVERSE`: the start key is the prefix; the scan
  begins at the LAST key carrying it, found by seeking exclusively at the
  prefix's **byte-successor** S (increment the last non-0xFF byte, truncate
  after it; a prefix of all 0xFF has no successor and means "from the end").
  Iteration stops when a key leaves the prefix (same memcmp bound as
  forward). Sound for the default comparator, where the keys carrying a
  prefix are exactly the range [P, S); the spec says so.

---

### Task 1: Spec — reverse iteration and predecessor lookup

**Files:**
- Modify: `doc/specification.md` — §5.5 after D-14i; A-1a vicinity; the flag
  table (~line 1697); after A-4; T-8/T-11 test-suite section touch

**Interfaces:** labels **D-14k** (reverse iteration), **D-14l** (predecessor
lookup), **A-1b** (concurrent cursors + writes with cursors open), **A-12**
(`ZS_FETCHPREV`), **A-13** (`ZS_REVERSE`), consumed by Tasks 2–5.

- [ ] **Step 1: Add D-14k and D-14l after D-14i**

```markdown
- **D-14k Reverse iteration.** A cursor MAY traverse in descending key order.
  The sources, the per-source structures, and the visibility rule are exactly
  D-14's — the array of per-file cursors is instead kept sorted by **current
  key descending, then generation descending** — so steps 2 through 6 of
  D-14e apply verbatim, with "advance" meaning one step toward smaller keys
  and step 1's seek positioning each source on the **largest key ≤ the start
  key** (or the last key it holds, for an empty start). Equal keys remain
  contiguous from the front of the array with the newest source first, so
  duplicate suppression and tombstone filtering are shared with the forward
  path rather than mirrored (G-7, D-14f). D-14j applies with direction of
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
  `ZS_CURSOR_PREFIX` with `ZS_REVERSE` unless byte-successor is an upper
  bound for the prefix's keys under that order too.
- **D-14l Predecessor lookup.** The record with the largest key ≤ K (or
  strictly < K) MUST be resolved as reverse iteration's first emission from a
  seek at K — by D-14k over the same sources, not by a second search rule.
  A tombstone at the largest candidate therefore consumes that key and the
  answer moves to the next smaller live one, exactly as a forward scan past
  a tombstone does (G-7).
```

- [ ] **Step 2: Add A-1b after A-1a**

```markdown
- **A-1b** A transaction supports **any number of cursors open at once**, and
  writes through the transaction while they are open. Each cursor observes
  the write by D-14j; every key or value pointer any of them has returned
  remains valid by A-4 — a store never invalidates another read's result.
  Cursors are not thread-safe and this promises nothing across threads
  (G-5's caveats apply); it promises composition within one caller.
```

- [ ] **Step 3: Flag table rows and A-12/A-13 after A-11**

Table (after the `ZS_FETCHNEXT` row):

```markdown
| `ZS_FETCHPREV` | fetch | return the record with the largest key ≤ the given key; with `ZS_SKIPROOT`, strictly < |
| `ZS_REVERSE` | cursor | iterate toward smaller keys (D-14k); not valid on `foreach` or with `ZS_CURSOR_LIVE` |
```

After A-11:

```markdown
- **A-12** `ZS_FETCHPREV` on `zs_db_fetch` and `zs_txn_fetch` returns the
  record with the largest key ≤ the given key, resolved by D-14l; composed
  with `ZS_SKIPROOT` the bound is strict (< the given key). The transactional
  form sees the transaction's own pending writes, like every other read
  (A-1a). `ZS_FETCHNEXT` and `ZS_FETCHPREV` together are a usage error.
  A-4's lifetime applies to the result unchanged.
- **A-13** `ZS_REVERSE` on `zs_db_begin_cursor` and `zs_txn_begin_cursor`
  opens a D-14k cursor: a null or empty start key positions at the last key
  in the database; a non-empty one at the largest key ≤ it, with
  `ZS_SKIPROOT` skipping an exact match; `ZS_CURSOR_PREFIX` composes as
  D-14k describes. `zs_cursor_next` steps toward smaller keys;
  `zs_cursor_replace` and `zs_cursor_delete` work at the current position
  unchanged. `ZS_REVERSE` with `ZS_CURSOR_LIVE`, or on either `foreach`
  form, is a usage error (`ZS_BADUSAGE`): neither is needed and a rejected
  flag is cheaper than an untested promise.
```

- [ ] **Step 4: Commit**

```bash
git add doc/specification.md
git commit -m "spec: reverse iteration and predecessor lookup (D-14k, D-14l, A-12, A-13, A-1b)"
```

---

### Task 2: Arm-level reverse: index cursor, pointer array, txn arm

**Files:**
- Modify: `zeroskip.c` — PRIVATE INDEX (~2348), PER-FILE CURSOR (~3102),
  the txn-arm hooks (~4858)

**Interfaces (produced, consumed by Task 3):**
- `struct zsi_fcur` gains `bool reverse;` — when set, `pi` and `ic.bi/ic.di`
  hold COUNTS (candidate at `n-1`, 0 = exhausted) and `tkey` means "the key
  we are strictly below" once `tstarted`.
- `zsi_index_cur_seek_rev(ix, compar, key, keylen, inclusive, c)`,
  `zsi_index_cur_seek_last(ix, c)`, `zsi_index_cur_get_rev(...)`,
  `zsi_index_cur_prev(...)` — reverse twins of the existing four, same
  delta-wins-ties rule, prev on a tie steps BOTH sides.
- `zsi_fcur_seek_rev(fc, key, keylen, bool inclusive)`,
  `zsi_fcur_seek_last(fc)`; `zsi_fcur_next` and `zsi_fcur_load` become
  direction-aware internally (the merge keeps calling the same names).

- [ ] **Step 1: Reverse index-cursor twins**

```c
/* Reverse twins.  A reverse position is a COUNT: the candidate on each side
 * is [n-1], and 0 means that side is spent -- nothing underflows. */
static void zsi_index_cur_seek_last(struct zsi_index *ix,
                                    struct zsi_index_cur *c)
{
    c->bi = ix->nbase;
    c->di = ix->ndelta;
}

/* Largest key <= (inclusive) or < (exclusive) the given key: an upper or
 * lower bound respectively, which is the count of elements below the bound. */
static void zsi_index_cur_seek_rev(struct zsi_index *ix, zs_compar *compar,
                                   const char *key, size_t keylen,
                                   bool inclusive, struct zsi_index_cur *c)
{
    struct zsi_ksort ks = { ix->file, compar };

    c->bi = zsi_index_lb(ix->base, ix->nbase, &ks, key, keylen);
    if (inclusive && zsi_index_eq(ix->base, ix->nbase, c->bi, &ks, key, keylen))
        c->bi++;
    c->di = zsi_index_lb(ix->delta, ix->ndelta, &ks, key, keylen);
    if (inclusive && zsi_index_eq(ix->delta, ix->ndelta, c->di, &ks, key, keylen))
        c->di++;
}
```

`zsi_index_cur_get_rev`: candidates `base[bi-1]` / `delta[di-1]`, choose the
LARGER key, ties prefer the delta (newer). `zsi_index_cur_prev`: on a tie
decrement BOTH (mirror of next's both-advance, same D-14h reason); otherwise
decrement the side holding the larger key.

- [ ] **Step 2: fcur direction**

In `zsi_fcur_load`, the in-order case honours direction:

```c
    case ZSI_SRC_INORDER: {
        uint64_t at = fc->pi;
        if (fc->reverse) {
            if (fc->pi == 0) { fc->exhausted = true; return ZS_OK; }
            at = fc->pi - 1;
        } else if (fc->pi >= fc->file->nptrs) {
            fc->exhausted = true; return ZS_OK;
        }
        if (zsi_ptrs_rec(fc->file, at, &fc->cur) != ZS_OK) { ... }
```

the unordered case calls `zsi_index_cur_get_rev` when reverse; the txn case
stays `zsi_txn_cur_load` (Step 4 makes it direction-aware).

`zsi_fcur_seek_rev` (new): in-order — `zsi_ptrs_search` then
`fc->pi = idx + (inclusive && exact ? 1 : 0)`; unordered —
`zsi_index_cur_seek_rev`; txn — `zsi_txn_cur_seek_rev` (Step 4). Then load.
`zsi_fcur_seek_last`: `pi = nptrs`, `zsi_index_cur_seek_last`, txn tkey NULL
(reverse-NULL means "from the top"). `zsi_fcur_next` dispatches to
`pi--` / `zsi_index_cur_prev` / the same yielded-key recording for txn.

- [ ] **Step 3: txn arm reverse**

`zsi_txn_cur_index` reverse variant inside `zsi_txn_cur_load`:

```c
    if (fc->reverse) {
        size_t pos;
        if (!fc->tkey && !fc->tstarted) {          /* from the top */
            if (!txn->npend) { fc->exhausted = true; return ZS_OK; }
            ti = txn->npend - 1;
        } else {
            bool exact = (zsi_pend_search(txn, fc->tkey, fc->tkeylen, &pos)
                          == ZS_OK);
            /* Position on the largest key <= tkey after a seek (inclusive
             * unless the seek said otherwise), strictly < after a yield. */
            if (exact && !fc->tstarted && !fc->texclusive) ti = pos;
            else if (pos == 0) { fc->exhausted = true; return ZS_OK; }
            else ti = pos - 1;
        }
    }
```

(`fc->texclusive` is set by `zsi_txn_cur_seek_rev(fc, key, keylen, inclusive)`
and never after; once `tstarted`, strictly-below always applies — D-14j-a's
key-not-index property carries over verbatim, so a store below the position
mid-walk is seen and one above is already passed.)

- [ ] **Step 4: Unit-style tests over the arms**

In zstest.c, against internals, keys planted so base+delta both participate
(store, publishless small commits for delta entries, plus a fresh open for a
base-only index): `test_index_cur_reverse_walk` (full descending walk equals
the forward walk reversed, ties resolved to the delta),
`test_index_cur_reverse_seek` (inclusive lands on ≤, exclusive on <, missing
key lands on predecessor, below-first exhausts),
`test_fcur_reverse_inorder` (same over a converted file's pointer array).

- [ ] **Step 5: Build, run those tests, run the full suite**

`make && ./zstest reverse && ./zstest` — all green; nothing forward changed.

- [ ] **Step 6: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "read path: reverse traversal for the three per-source cursors (D-14k groundwork)"
```

---

### Task 3: Reverse merge cursor

**Files:**
- Modify: `zeroskip.c` — `zsi_cur_order` (~4296), `zsi_cursor_open` (~4366),
  `zsi_cursor_reseek_arm` (~4528), `zs_db_begin_cursor`/`zs_txn_begin_cursor`
  /`zs_db_foreach`/`zs_txn_foreach` wrappers; `zeroskip.h` `ZS_REVERSE = 1<<20`

**Interfaces:**
- Consumes Task 2's arm operations.
- Produces: `ZS_REVERSE` end to end; `c->reverse` on `struct zs_cursor`;
  a `zsi_cursor_seek_arm_start(c, fc)` helper owning open-time positioning,
  shared by open and by the pre-first-emit reseek.

- [ ] **Step 1: Direction-aware order**

```c
    int r = c->db->compar(a->cur.key, a->cur.keylen, b->cur.key, b->cur.keylen);
    if (r) return c->reverse ? -r : r;
```

(`zsi_cur_resort_head`'s "key only ever increases" comment becomes "moves
only in the direction of travel"; the code is untouched — it goes through
`zsi_cur_order`.)

- [ ] **Step 2: Open-time positioning and the prefix successor**

In `zsi_cursor_open`: reject `ZS_REVERSE|ZS_CURSOR_LIVE` with `ZS_BADUSAGE`;
set `c->reverse`. Extract the per-arm start seek into
`zsi_cursor_seek_arm_start`:

```c
/* Forward: lower bound of the start key, or the beginning.  Reverse: the
 * largest key <= the start key; under ZS_CURSOR_PREFIX the start key IS the
 * prefix and the scan begins at the LAST key carrying it -- an exclusive
 * seek at the prefix's byte-successor (D-14k).  A prefix of all 0xFF has no
 * successor: from the end. */
static int zsi_cursor_seek_arm_start(struct zs_cursor *c, struct zsi_fcur *fc)
{
    if (!c->reverse)
        return c->start_key ? zsi_fcur_seek(fc, c->start_key, c->start_keylen)
                            : zsi_fcur_seek_first(fc);

    if ((c->flags & ZS_CURSOR_PREFIX) && c->prefixlen) {
        char succ[ZSI_KEYLEN_MAX];      /* the successor is never longer */
        size_t n = c->prefixlen;
        memcpy(succ, c->prefix, n);
        while (n && (unsigned char)succ[n - 1] == 0xFF) n--;
        if (!n) return zsi_fcur_seek_last(fc);
        succ[n - 1] = (char)((unsigned char)succ[n - 1] + 1);
        return zsi_fcur_seek_rev(fc, succ, n, false);       /* strictly < */
    }

    return c->start_key ? zsi_fcur_seek_rev(fc, c->start_key,
                                            c->start_keylen, true)
                        : zsi_fcur_seek_last(fc);
}
```

(Check the real name/value of the key-length bound — F-14's maximum — and
use the constant that exists; if keys can reach it, a VLA-free fixed buffer
of that size is fine because the successor never exceeds the prefix length.)

- [ ] **Step 3: Reverse reseek (D-14j-b mirrored)**

In `zsi_cursor_reseek_arm`: with `last_key`, reverse-seek INCLUSIVE at it and
step once on an exact match (same shape as forward); with only a start
position, call `zsi_cursor_seek_arm_start` — which re-derives the prefix
successor rather than trusting a stale arm position.

- [ ] **Step 4: Wrappers**

`zs_db_foreach`/`zs_txn_foreach`: `if (flags & ZS_REVERSE) return ZS_BADUSAGE;`
(A-13). Both `begin_cursor` forms pass the flag through unchanged.

- [ ] **Step 5: Tests**

`test_cursor_reverse_walks_everything` (multi-generation database: small
rollover so in-order files, unordered active, and txn-pending records all
hold live keys; reverse walk == forward walk reversed, exactly once each);
`test_cursor_reverse_seek_and_skiproot` (seek at present key yields it, ≤ on
absent key, SKIPROOT gives strictly-less);
`test_cursor_reverse_tombstones` (newest version is a txn-pending delete →
key absent, next smaller yielded — D-14l/G-7);
`test_cursor_reverse_prefix` (trees `a*`,`b*`,`c*`: prefix `b` reverse yields
the `b`s descending, stops; prefix ending 0xFF; prefix whose successor
exists as a real key — plant key `c` exactly — must not yield it);
`test_cursor_reverse_own_writes` (store below the position mid-walk is
yielded later; store above is not re-yielded; replace at position works —
D-14j reversed, write-through);
`test_cursor_reverse_live_is_badusage`, `test_foreach_reverse_is_badusage`.

- [ ] **Step 6: Full suite, commit**

```bash
make && ./zstest && git add zeroskip.h zeroskip.c zstest.c
git commit -m "cursors: ZS_REVERSE, descending iteration over the same merge (D-14k, A-13)"
```

---

### Task 4: ZS_FETCHPREV

**Files:**
- Modify: `zeroskip.h` — `ZS_FETCHPREV = 1<<15`; `zeroskip.c` `zs_txn_fetch`
  (~7039)

- [ ] **Step 1: Mirror the FETCHNEXT block**

```c
    if ((flags & ZS_FETCHNEXT) && (flags & ZS_FETCHPREV)) return ZS_BADUSAGE;

    /* ZS_FETCHPREV: the largest key <= (or, with ZS_SKIPROOT, <) the given
     * key.  A REVERSE cursor seeked at the key is exactly that (D-14l), so it
     * shares the merge rather than reimplementing "previous" -- the same
     * arrangement ZS_FETCHNEXT already uses. */
    if (flags & ZS_FETCHPREV) {
        struct zs_cursor *c = NULL;
        rc = zsi_cursor_open(txn->db, txn->readonly ? NULL : txn, txn->snap,
                             key, keylen,
                             ZS_REVERSE | (uint32_t)(flags & ZS_SKIPROOT), &c);
        ...same yield/copy-out/free as the FETCHNEXT block...
    }
```

- [ ] **Step 2: Tests**

`test_fetchprev_basic` (exact hit, gap hit ≤, below-first NOTFOUND, SKIPROOT
strict), `test_fetchprev_sees_txn_writes` (pending store IS the answer;
pending delete pushes the answer lower; both through `zs_txn_fetch`),
`test_fetchprev_next_symmetry` (for every key K in a fixture:
FETCHPREV(FETCHNEXT(K)) round-trips where defined),
`test_fetchprev_both_flags_badusage`.

- [ ] **Step 3: Full suite, commit**

```bash
git commit -m "fetch: ZS_FETCHPREV, predecessor by the reverse merge (D-14l, A-12)"
```

---

### Task 5: The explicit contracts (A-1b) and the sqlite shape

**Files:**
- Modify: `zstest.c`

- [ ] **Step 1: `test_txn_many_cursors`** — one write txn, cursors on three
  key ranges ("trees") at once, forward and reverse mixed, interleaved
  stepping; each yields its own range correctly.

- [ ] **Step 2: `test_txn_insert_select_self`** — the INSERT INTO t SELECT
  FROM t shape: forward cursor over prefix `t1|`, for each row store a copy
  at a HIGHER key inside the same prefix through the same txn, cursor must
  terminate (D-14j sees the inserts landing ahead; the test bounds the walk
  and asserts the copies exist after) — and A-4: key/value pointers taken
  from earlier yields, held across the stores, still compare equal to their
  copied-out snapshots.

- [ ] **Step 3: `test_reverse_a4_lifetime`** — reverse cursor yields; hold
  the pointers; more stores through the txn; pointers still valid until
  cursor close (A-4, "reverse inherits A-4 unchanged").

- [ ] **Step 4: Full suite, commit**

```bash
git commit -m "test: transaction cursor composition and A-4 under writes (A-1b)"
```

---

### Task 6: Mutants and conformance rows

**Files:**
- Modify: `tests/mutate.sh`, `doc/conformance.md`

- [ ] **Step 1: Mutants** (patterns pinned to the exact merged source; run
  NONE of them now — verification pass at end of session):

```bash
# D-14k: the reverse order must flip ONLY the key, never the generation
# tie-break -- flipping both yields the OLDEST version of every key.
mutant "cursor: reverse flips the generation tie-break" catch \
  's/    if \(r\) return c->reverse \? -r : r;/    if (r) return c->reverse ? -r : r;\n    if (c->reverse) return a->gen > b->gen ? 1 : (a->gen < b->gen ? -1 : 0);/'

# D-14k: inclusive reverse seek off by one -- lands strictly below instead
# of on the key, so FETCHPREV of a present key returns its predecessor.
mutant "index: reverse seek never lands on the key" catch \
  's/    if \(inclusive && zsi_index_eq\(ix->base, ix->nbase, c->bi, &ks, key, keylen\)\)\n        c->bi\+\+;/    \/* inclusive dropped *\//'

# D-14h in reverse: a tie between delta and base must consume BOTH.
mutant "index: reverse tie leaves the base entry" catch \
  '<pattern for zsi_index_cur_prev tie branch, decrementing only di>'

# D-14j-a in reverse: the txn arm resuming from an index instead of a key.
mutant "cursor: reverse txn arm resumes above the last key" catch \
  '<pattern flipping tstarted strictly-below to inclusive>'

# D-14k prefix: seek at the prefix itself instead of its successor lands on
# at most the bare-prefix key and reverses an empty prefix range.
mutant "cursor: reverse prefix seeks the prefix, not its successor" catch \
  '<pattern replacing the successor computation with a plain seek_rev>'

# A-13: LIVE composed with reverse accepted silently.
mutant "cursor: reverse accepts ZS_CURSOR_LIVE" catch \
  '<pattern deleting the BADUSAGE reject>'
```

(Write the real patterns against the final source text; the placeholders
above name the bug each must be. `./tests/mutate.sh --rot-only` is the only
mutate invocation permitted in this task.)

- [ ] **Step 2: Conformance rows** for D-14k, D-14l, A-1b, A-12, A-13 naming
  the tests above; bump the requirement counts; `./tests/conformance.sh`
  passes.

- [ ] **Step 3: Commit**

```bash
git commit -m "test: mutants and conformance rows for reverse iteration"
```

---

### Task 7: Docs

- Modify: `CLAUDE.md` — read-path section note: reverse is the same merge
  with the key sign flipped (G-7), FETCHPREV is a throwaway reverse cursor;
  the prefix byte-successor trap (why seeking the prefix itself is wrong).
- Modify: `doc/overview.md` — one line under the read-path description.

```bash
git commit -m "docs: reverse iteration"
```

## Self-Review Notes

- G-7 is the spine: no second resolution path — FETCHPREV goes through the
  cursor, the cursor through the same step function; only `zsi_cur_order`
  and the arm primitives know about direction.
- The reverse-position-as-count encoding avoids unsigned underflow in all
  three arms; exhausted is `pos == 0` uniformly.
- The prefix successor lives in ONE helper used by open and reseek, so a
  refresh cannot diverge from the open (the F-32/zsi_cursor_reseek_arm
  lesson).
- SKIPROOT is handled once, at the merge's yield (existing code,
  direction-blind), not in the arms.
- `zsi_lookup` (exact fetch) is untouched; FETCHPREV takes the cursor route
  like FETCHNEXT, so the fetch family stays two shapes, not three.
