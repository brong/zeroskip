# Per-Record Checksums (Format Version 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every data record carries a trailing 4-byte checksum, verified when a record is materialized for a caller; format version 2, clean break.

**Architecture:** The checksum is the last 4 bytes of the padded record, covering `[0, len-4)` under the containing file's engine — the same last-field-covers-everything-before-it convention as the header, terminator, and trailer. Verification happens at the two yield points (lookup find, cursor step capture), in `check`, and in salvage — deliberately NOT during span replay or pointer-section load (F-24 would turn a flipped byte into a truncated file). Spec first, then the format flip with corpus regeneration, then verification behavior.

**Tech Stack:** C99, existing xxHash vendored engine, custom zstest harness, mutate.sh.

## Global Constraints

- The spec is normative; the spec commit lands before the code commit (D-14j precedent: conformance rows may cite tests that arrive in the next commit).
- Every commit must leave `make check` green EXCEPT the spec commit's conformance citations (accepted precedent).
- Corpus regeneration is part of the format-flip commit, not a separate one — corpus tests read `tests/corpus/` and a version-2 reader rejects version-1 files.
- `EXTRA_CFLAGS`, never override `CFLAGS`.
- Design doc: `docs/superpowers/specs/2026-08-10-per-record-checksums-design.md`.
- Read CLAUDE.md's "Things that look like bugs and are not" before touching replay, comparators, or engines.
- Do NOT run full `./tests/mutate.sh` — run new mutants by name and `./tests/mutate.sh --rot-only`.

---

### Task 1: Spec commit

**Files:**
- Modify: `doc/specification.md` (§4.4 record layouts ~line 340; F-7 versions; F-15 canonical; after F-30 add F-32 family; F-5a cross-ref)
- Modify: `doc/conformance.md` (new rows + counts)

**Interfaces:**
- Produces: requirement labels F-32, F-32a, F-32b, F-32c used by tests and code comments in Tasks 2–5.

- [ ] **Step 1: Update all eight record layout diagrams in §4.4.** Each gains a trailing csum line and `+ 4` in its len formula. Pattern for every form (short shown; big forms identical shape with their own fixed fields):

```
KEYVALUE (0x01)
  +0      1  type
  +1      1  keylen
  +2      2  vallen
  +4      .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(4 + keylen + 1 + vallen + 1 + 4)
```

Apply to KEYVALUE, KEYVALUE_ANC, DELETION, DELETION_ANC, BIGKEYVALUE, BIGKEYVALUE_ANC, BIGDELETION, BIGDELETION_ANC. Fixed-field offsets do not move; only the trailing field and len formulas change.

- [ ] **Step 2: Add the F-32 requirement family** after F-30:

```
- **F-32** Every data record ends in a 4-byte checksum: the last 4 bytes of
  the padded record, computed by the containing file's engine (F-5a) over
  `[0, len-4)` — type, lengths, ancestor, key, value, and padding.  This is
  the format's one checksum convention, stated once: **every checksum is the
  last field of the thing it covers, and covers everything before it**
  (header F-4, terminator F-19, trailer F-26b, records F-32).
- **F-32a** A record's checksum MUST be verified when the record is
  materialized for a caller — a lookup result or a cursor yield — unless the
  handle was opened `ZS_NOCSUM` (F-5e).  The failure is reported for that
  record alone; other records remain readable.
- **F-32b** A record's checksum MUST NOT be verified during span replay
  (F-24) or pointer-section load.  Replay completes a file at its first
  invalid record, discarding everything after it — so verifying there turns
  one flipped byte into the loss of every later record, a G-3 violation.  A
  record inside a valid span whose own checksum fails is in-place corruption,
  detected at materialization.
- **F-32c** A record copied byte-for-byte keeps a valid checksum only when
  the output file's engine matches the input's.  A writer copying records
  into a file under a different engine MUST re-encode them (D-20b already
  requires the source be verified first).
```

- [ ] **Step 3: Version bump.** In the F-7 area, state: version 2 is the first published version; `version_read = version_write = 2`; a reader MUST reject `version_read` below 2 as well as above its own (version 1 was never released — pre-release clean break). Update any `version 1` literals in spec prose.

- [ ] **Step 4: F-15 canonical encoding** gains one clause: the stored checksum MUST be correct under the file's engine; an incorrect one is corruption, not an alternative encoding.

- [ ] **Step 5: conformance.md rows.** Add after the F-30 row:

```
| `F-32` | Every data record ends in a 4-byte checksum: the last 4 bytes of | `record_byte_layout_v2`, `corpus` |
| `F-32a` | A record's checksum MUST be verified when the record is | `read_verifies_record_csum`, `read_verifies_record_csum_unordered`, `record_csum_nocsum_reads` |
| `F-32b` | A record's checksum MUST NOT be verified during span replay | `record_csum_replay_no_truncate` |
| `F-32c` | A record copied byte-for-byte keeps a valid checksum only when | `convert_reencodes_engine_mismatch` |
```

Bump the counts table: Requirements 242 → 246, With an enforcing test 233 → 237.

- [ ] **Step 6: Commit**

```bash
git add doc/specification.md doc/conformance.md
git commit -m "spec: every data record ends in its own checksum (F-32, format version 2)"
```

---

### Task 2: Format flip — encode, decode, version, builders

**Files:**
- Modify: `zeroskip.c` — `ZSI_VERSION_READ/WRITE` (~line 665), `zsi_header_decode` gate (~line 738), `struct zsi_rec` (~line 854), `zsi_rec_encoded_len` (~line 880), `zsi_rec_encode` (~line 937), `zsi_rec_decode` (~line 1017), encode call sites (~4896, ~5802)
- Modify: `zstest.c` — `sb_rec` (~1927), `ib_rec` (~2009), every `zsi_rec_encode(` call site (~25, listed by grep), every byte-literal record test (~1311–1691)

**Interfaces:**
- Produces: `zsi_rec_encode(char *buf, zs_csum *csum, const char *key, size_t keylen, const char *val, size_t vallen, bool store_ancestor, uint32_t ancestor)` — csum is the CONTAINING FILE's engine function (A-6/F-5a; the writer path already holds the right one for span checksums).
- Produces: `struct zsi_rec` gains `const char *base;` (record start, set by decode) and `uint32_t csum;` (stored trailing checksum). Task 3's `zsi_rec_verify` consumes both.

- [ ] **Step 1: Write the failing layout test** in zstest.c near the existing encode tests (~line 1300). It pins version-2 bytes against literals (the symmetric-swap mutant class from `test_header_byte_layout`):

```c
static void test_record_byte_layout_v2(void)
{
    /* F-32: the checksum is the LAST 4 bytes of the padded record, covering
     * [0, len-4).  Asserted against literals, not against the encoder's own
     * output, so a symmetric encode/decode bug cannot round-trip past it. */
    char buf[64];
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL);

    /* KEYVALUE "ab" -> "xy": fixed 4 + 2+1+2+1 + csum 4 = 14 -> len 16. */
    zsi_rec_encode(buf, cs, "ab", 2, "xy", 2, false, 0);
    ASSERT_EQU((size_t)zsi_rec_encoded_len(2, 2, false, false), 16u);
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 2);                       /* keylen */
    ASSERT_EQ(zsi_get16(buf + 2), 2);           /* vallen */
    ASSERT_MEM_EQ(buf + 4, "ab\0xy\0", 6);
    ASSERT_EQ(buf[10], 0); ASSERT_EQ(buf[11], 0);   /* pad, covered */
    ASSERT_EQU(zsi_get32(buf + 12), cs(buf, 12));   /* trailing csum */

    /* DELETION_ANC "ab", anc 5: fixed 8 + 2+1 + 4 = 15 -> len 16. */
    zsi_rec_encode(buf, cs, "ab", 2, NULL, 0, true, 5);
    ASSERT_EQ(buf[0], 0x0B);
    ASSERT_EQU(zsi_get32(buf + 4), 5u);         /* ancestor stays at +4 */
    ASSERT_EQU(zsi_get32(buf + 12), cs(buf, 12));

    /* Engine 0 writes a ZERO field. */
    zs_csum *cs0 = zsi_csum_for_id(ZSI_CSUM_NONE_ID, NULL);
    zsi_rec_encode(buf, cs0, "ab", 2, "xy", 2, false, 0);
    ASSERT_EQU(zsi_get32(buf + 12), 0u);
}
```

Check the real engine-id constant names first (`grep -n "zsi_csum_for_id" zeroskip.c`) and match the actual signature — the test must call exactly what exists. Register in the test table.

- [ ] **Step 2: Run it to make sure it fails to compile** (`zsi_rec_encode` has the old signature): `make zstest 2>&1 | head`.

- [ ] **Step 3: Implement the format flip in zeroskip.c.**

(a) Versions:

```c
#define ZSI_VERSION_READ  2
#define ZSI_VERSION_WRITE 2
```

In `zsi_header_decode`, below the existing `if (vread > ZSI_VERSION_READ) return ZS_BADFORMAT;` add:

```c
    /* Version 1 was never released (F-7): records below version 2 carry no
     * checksum, and pretending to read them would serve unverifiable data.
     * The clean break is spec'd, not an accident of this reader. */
    if (vread < 2) return ZS_BADFORMAT;
```

(b) `struct zsi_rec` gains two fields (after `len`):

```c
    const char *base;   /* record start; csum covers [base, base+len-4) */
    uint32_t    csum;   /* the stored trailing checksum (F-32) */
```

(c) `zsi_rec_encoded_len`: both `body` computations grow by 4 — delete: `zsi_add_sz(keylen, 1 + 4, &body)`; keyvalue: `zsi_add3_sz(keylen, vallen, 2 + 4, &body)`. Update the function comment: the +4 is F-32's trailing checksum, inside the roundup.

(d) `zsi_rec_encode` gains `zs_csum *csum` as its second parameter and ends with:

```c
    /* F-32: the trailing checksum, over everything before it -- fixed
     * fields, key, value, NULs, and the padding.  The engine is the
     * CONTAINING FILE's (F-5a), which is why it is a parameter: the
     * handle's engine is the same trap here as in A-6. */
    zsi_put32(buf + total - 4, csum(buf, total - 4));
```

(e) `zsi_rec_decode`: after `total` is validated, set the new fields:

```c
    out->base = buf;
    out->csum = zsi_get32(buf + total - 4);
```

and REWRITE the "Records carry no checksum of their own" comment block (~line 1010): records now carry one; it is still deliberately not verified here (F-32b — verifying at decode would put the check inside replay, where F-24 turns it into data loss).

(f) The two zeroskip.c encode call sites pass the engine they already hold: `zsi_txn_write_span` (~4896) has `cs`; `zsi_repack_merge` (~5802) has `cs` (the output engine — F-32c is satisfied there because the merge re-encodes every record it writes).

- [ ] **Step 4: Update zstest.c mechanically.** `sb_rec` and `ib_rec` pass their builder's engine: both builders already carry `engine`; resolve once in the builder (`zsi_csum_for_id`) and thread it. Then fix every direct `zsi_rec_encode(` call site — find them all:

```bash
grep -n "zsi_rec_encode(" zstest.c
```

Insert the engine argument (the test-local engine the surrounding test already uses — most use XXHASH; `TEST_EXTERNAL_CSUM` tests use that). Every byte-literal assertion in tests ~1311–1691 needs its expected `len` values +0/+8 per the new formula and any trailing-byte assertions adjusted; run the suite and fix each failure against a hand-computed expectation, never by pasting the encoder's own output (that is the round-trip trap the layout test exists to prevent).

- [ ] **Step 5: Regenerate the corpus** (the version-2 reader rejects the checked-in version-1 files, so this is part of keeping the commit green):

```bash
make && ./tests/gencorpus.sh --force && git add tests/corpus
```

- [ ] **Step 6: Full suite green:** `make check`. Corpus, tool, crash, and conformance must all pass.

- [ ] **Step 7: Commit**

```bash
git add zeroskip.c zstest.c tests/corpus
git commit -m "feat: format version 2 -- every data record ends in its own checksum (F-32)"
```

---

### Task 3: Read-path verification

**Files:**
- Modify: `zeroskip.c` — new `zsi_rec_verify` (after `zsi_rec_decode`), `zsi_cursor_step` (~line 4289, at the element-0 capture), `zsi_lookup` (~line 4050, file-arm find)
- Test: `zstest.c` — four new tests

**Interfaces:**
- Consumes: `struct zsi_rec.base/.csum` from Task 2.
- Produces: `static int zsi_rec_verify(zs_csum *csum, const struct zsi_rec *r)` returning `ZS_OK`/`ZS_BADCHECKSUM`, used again by Tasks 4–5.

- [ ] **Step 1: Write the failing tests** (register all four):

```c
static void test_read_verifies_record_csum(void)
{
    /* F-32a: a value byte flipped in place in an in-order file fails THAT
     * key at materialization; sibling keys still read; a ZS_NOCSUM handle
     * still reads the corrupt value (F-5e). */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    char got[64];
    const char *v; size_t vl;

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5, false, 0);
    ib_rec(&b, "b", 1, "other", 5, false, 0);
    ib_finish(&b);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';    /* first value byte of "a" */
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0), ZS_BADCHECKSUM);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "other", 5);
    zs_db_close(&db);

    db = open_db(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    db_get(db, "a", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "Value");
    zs_db_close(&db);
}

static void test_read_verifies_record_csum_unordered(void)
{
    /* F-32a in an unordered file: corrupt ONE record inside a span, then
     * RECOMPUTE the span checksum over the corrupted bytes so the span
     * still validates -- pure record-level corruption, invisible to replay. */
    struct zs_db *db;
    struct sb s;
    char name[ZSI_NAME_MAX];
    const char *v; size_t vl;

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "value", 5, false, 0);
    sb_rec(&s, "b", 1, "other", 5, false, 0);
    /* Corrupt "a"'s first value byte BEFORE the terminator is written, so
     * sb_term's span checksum covers the corrupt bytes -- the record csum
     * (already written by sb_rec) is now the only witness. */
    s.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    sb_term(&s, false);
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0), ZS_BADCHECKSUM);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    zs_db_close(&db);
}

static void test_record_csum_replay_no_truncate(void)
{
    /* F-32b, the G-3 half: the corrupt record above must NOT make replay
     * complete the file early -- record "b", stored AFTER "a" in the same
     * span, stays visible.  (Verifying during replay is the tempting wrong
     * version; the mutant for it is caught here.) */
    struct zs_db *db;
    struct sb s;
    char name[ZSI_NAME_MAX];
    const char *v; size_t vl;

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "value", 5, false, 0);
    sb_rec(&s, "b", 1, "other", 5, false, 0);
    s.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    sb_term(&s, false);
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "other", 5);
    zs_db_close(&db);
}

static void test_record_csum_engine0(void)
{
    /* Engine 0: the field is written zero and nothing is verified -- the
     * engine returns zero for every input, so verification passes without a
     * special case (F-5c's bargain, unchanged). */
    struct zs_db *db;
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    const char *v; size_t vl;

    clear_db();
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);
    zs_db_close(&db);
}
```

The ib/sb corruption offset (`ZSI_HEADER_LEN + 4 + 1 + 1`) is the same first-value-byte position `test_inorder_records_checksum` already uses; verify it still holds under the new layout (fixed fields did not move) by the NOCSUM read-back assertion.

- [ ] **Step 2: Run to verify they fail:** `./zstest record_csum` and `./zstest verifies_record` — the fetch of "a" returns OK (no verification yet), so the `ZS_BADCHECKSUM` assertions fail.

- [ ] **Step 3: Implement.**

(a) After `zsi_rec_decode`:

```c
/* Verify a record's trailing checksum (F-32a).
 *
 * The engine is the caller's to supply, because a record does not know its
 * file -- and it is always the CONTAINING file's engine (F-5a).  Engine 0
 * computes zero and stored zero, so it passes with no special case.
 *
 * Callers are the MATERIALIZATION points only.  Wiring this into decode or
 * replay is the data-loss bug F-32b names: replay completes a file at its
 * first invalid record (F-24), so a verifying replay converts one flipped
 * byte into the loss of every record after it. */
static int zsi_rec_verify(zs_csum *csum, const struct zsi_rec *r)
{
    if (r->len < 4) return ZS_BADFORMAT;
    if (csum(r->base, r->len - 4) != r->csum) return ZS_BADCHECKSUM;
    return ZS_OK;
}
```

(b) `zsi_cursor_step`, immediately after `struct zsi_rec rec = c->cur[0].cur;`:

```c
    /* F-32a: verify at the yield, on the record actually being consumed --
     * element 0 is the newest version and the one whose bytes the caller
     * (or the tombstone filter below) will act on.  Stale duplicates the
     * loop skips are never verified: a corrupt SHADOWED version must not
     * fail a read whose answer does not depend on it. */
    if (c->cur[0].file && !c->db->nocsum) {
        int vr = zsi_rec_verify(c->cur[0].file->csum, &rec);
        if (vr != ZS_OK) return vr;
    }
```

(c) `zsi_lookup`, in the file loop, between `zsi_fcur_find` returning ZS_OK and `zsi_fcur_fini`:

```c
        if (r == ZS_OK && !db->nocsum) {
            int vr = zsi_rec_verify(snap->files[i]->csum, out);
            if (vr != ZS_OK) { zsi_fcur_fini(&fc); return vr; }
        }
```

Check `struct zs_db`'s flag name (`db->nocsum` — confirm with grep) and `zsi_file`'s `csum` member type; if the engine call needs the id too (match how `zsi_ptrs_verify_records` at ~1859 calls it), mirror that call shape exactly.

- [ ] **Step 4: Run the new tests, then the full suite:** `./zstest record_csum && ./zstest verifies_record && make check`.

- [ ] **Step 5: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "feat: verify record checksums at materialization (F-32a, F-32b)"
```

---

### Task 4: check_consistency and salvage verify per record

**Files:**
- Modify: `zeroskip.c` — the F-28 record walk in CONSISTENCY (find via `grep -n "zsi_rec_is_canonical" zeroskip.c` — the walk that reports non-canonical records is where per-record verification belongs, same reporting style), and salvage's record emission path (SALVAGE section; find via `grep -n "ZS_SALVAGE_KEY_UNVERIFIED" zeroskip.c`)
- Test: `zstest.c`

**Interfaces:**
- Consumes: `zsi_rec_verify` from Task 3.

- [ ] **Step 1: Write the failing tests:**

```c
static void test_check_reports_record_csum(void)
{
    /* T-6 style: check reports the corruption while the database still
     * opens.  Reuses test_read_verifies_record_csum's file shape. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5, false, 0);
    ib_finish(&b);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADCHECKSUM);
    zs_db_close(&db);
}
```

For salvage: find the existing salvage tests (`grep -n "test_salvage" zstest.c`), copy the closest one's setup, corrupt one record as above, and assert the salvage report stream contains `ZS_SALVAGE_KEY_UNVERIFIED` (or the stale/unverified tally) for exactly the corrupt key and that the healthy key is recovered clean. Follow the existing salvage-test assertion style precisely.

- [ ] **Step 2: Run to verify they fail** (`./zstest check_reports_record` — check currently returns OK because only the region checksum is verified, and the region checksum in this hand-built file was computed over... NOTE: `ib_finish` computes the region checksum over the buffer BEFORE the corruption, so check already fails via F-26e. To isolate F-32: corrupt the value byte BEFORE `ib_finish` — then the region checksum covers the corrupt bytes and only the per-record checksum disagrees. Adjust the test accordingly: move the corruption line above `ib_finish(&b)`, and mirror the same order in Task 3's in-order test if its NOCSUM read-back proves the same ordering problem — the record checksum is written by `ib_rec`, so corrupting after `ib_rec` but before `ib_finish` gives: record csum stale (witness), region csum fresh (blind). Verify this reasoning against `ib_finish`'s source before running.)

- [ ] **Step 3: Implement.** In the consistency walk, at each decoded record, mirror the non-canonical reporting: call `zsi_rec_verify` with the file's engine (skip under `db->nocsum`, F-5e), report through the error callback with the file name and record offset, and make the overall return `ZS_BADCHECKSUM` (first error wins, keep walking so every problem is reported — match how the existing walk accumulates). In salvage, verify each record it is about to emit; on failure emit the existing `ZS_SALVAGE_KEY_UNVERIFIED` event for that key instead of silently accepting it — engine-0 files keep their current always-unverified reporting.

- [ ] **Step 4: Run new tests + full suite:** `make check`.

- [ ] **Step 5: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "feat: check and salvage verify per-record checksums (F-32a)"
```

---

### Task 5: Conversion re-encodes on engine mismatch (F-32c)

**Files:**
- Modify: `zeroskip.c` — `zsi_write_inorder` (~line 5302: the byte-for-byte copy loop)
- Modify: `docs/superpowers/specs/2026-08-10-per-record-checksums-design.md` — correct the repack sentence (the merge re-encodes EVERY record, so F-32c's trigger is conversion-only)
- Test: `zstest.c`

**Interfaces:**
- Consumes: `zsi_rec_encode` (Task 2 signature), `zsi_rec_verify` (Task 3).

- [ ] **Step 1: Write the failing test:**

```c
static void test_convert_reencodes_engine_mismatch(void)
{
    /* F-32c: an engine-0 file converted by an engine-1 handle.  The output
     * file's header says engine 1, so a verbatim record copy would carry a
     * ZERO checksum that fails under engine 1 for every record.  Conversion
     * must re-encode.  (The reverse mismatch would fail silently -- engine 0
     * verifies nothing -- which is why the test is this way round.) */
    struct zs_db *db;
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    const char *v; size_t vl;

    clear_db();
    /* An engine-0 database with one record... */
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    zs_db_close(&db);

    /* ...sealed by an engine-1 handle: conversion output is engine 1. */
    struct zs_open_data setup2 = ZS_OPEN_DATA_INITIALIZER;
    setup2.flags = ZS_CSUM_XXHASH;
    ASSERT_OK(zs_db_open(dbdir, &setup2, &db));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}
```

Confirm first (by reading `zsi_write_inorder` and the header it writes) which engine the conversion output actually records — if the output inherits the INPUT's engine rather than the handle's, this test passes trivially and the F-32c trigger is unreachable in conversion too; in that case assert the inherited-engine behavior instead and note F-32c is satisfied vacuously (update the design doc to say so).

- [ ] **Step 2: Run to verify** (`./zstest engine_mismatch`) — expect either the fetch/check failing (verbatim zero checksums under engine 1) or a trivial pass per the note above; investigate which before implementing.

- [ ] **Step 3: Implement** (if the mismatch is real): in `zsi_write_inorder`'s copy loop, compare the output engine id against `src->csum_id`; when equal keep the memcpy, when different re-encode:

```c
        if (out_engine_matches_input) {
            memcpy(recs + recslen, b, rec.len);     /* existing path */
        } else {
            /* F-32c: a record's checksum is under its FILE's engine, so a
             * byte-for-byte copy into a file under a different engine
             * carries a checksum that validates for nobody -- the A-6 trap
             * at one remove.  Re-encode, preserving the ancestor form
             * verbatim (F-17, D-17a). */
            zsi_rec_encode(recs + recslen, cs, rec.key, rec.keylen,
                           rec.val, rec.vallen,
                           (rec.type & ZSI_HASANCESTOR) != 0, rec.ancestor);
        }
```

Note the encoded length can differ from `rec.len` only if the source record was non-canonical (F-15); use `zsi_rec_encoded_len` for the destination offset bookkeeping, not `rec.len`, in the mismatch branch.

- [ ] **Step 4: Run new test + full suite:** `make check`.

- [ ] **Step 5: Commit**

```bash
git add zeroskip.c zstest.c docs/superpowers/specs/2026-08-10-per-record-checksums-design.md
git commit -m "feat: conversion re-encodes records under a different output engine (F-32c)"
```

---

### Task 6: Mutants, CLAUDE.md, rot check

**Files:**
- Modify: `tests/mutate.sh` (near the seal/compact mutants, ~line 940)
- Modify: `CLAUDE.md` ("Things that look like bugs and are not")

- [ ] **Step 1: Add four mutants.** Adjust each pattern to the exact final source text (they are regexes over zeroskip.c; copy the lines from the file, then escape):

```bash
# F-32a.  Skipping verification at the yield is the headline gap this whole
# format change exists to close.
mutant "record: not verified at yield" catch \
  's/    if \(c->cur\[0\]\.file && !c->db->nocsum\) \{\n        int vr = zsi_rec_verify\(c->cur\[0\]\.file->csum, &rec\);\n        if \(vr != ZS_OK\) return vr;\n    \}/    \/* F-32a removed *\//'

# F-32.  Covering [0, len) instead of [0, len-4) includes the checksum in its
# own coverage -- the off-by-pad class.
mutant "record: csum covers its own field" catch \
  's/    if \(csum\(r->base, r->len - 4\) != r->csum\) return ZS_BADCHECKSUM;/    if (csum(r->base, r->len) != r->csum) return ZS_BADCHECKSUM;/'

# F-32b.  Verifying during replay is the tempting wrong version: it turns one
# flipped byte into the loss of every record after it (F-24 + G-3).  Caught by
# test_record_csum_replay_no_truncate... as a BADFORMAT-style truncation.
mutant "record: verified during replay" catch \
  's/            struct zsi_rec r;\n            if \(zsi_rec_decode\(b, avail, f->hdr\.start, &r\) != ZS_OK\) break;/            struct zsi_rec r;\n            if (zsi_rec_decode(b, avail, f->hdr.start, \&r) != ZS_OK) break;\n            if (zsi_rec_verify(f->csum, \&r) != ZS_OK) break;/'

# F-32.  The write-side gap: a checksum that is never computed reads as engine
# 0's everywhere, so every engine-1 read fails -- unmissable, which is the
# point: it proves the READ tests actually depend on the written value.
mutant "record: checksum never written" catch \
  's/    zsi_put32\(buf \+ total - 4, csum\(buf, total - 4\)\);/    zsi_put32(buf + total - 4, 0);/'
```

The replay mutant's pattern targets pass ONE's decode in `zsi_unordered_replay` (~line 1566) — verify the exact text and that `zsi_rec_verify` is declared above replay (it is not — replay is earlier in the file; if so, target pass TWO's decode at ~1640 instead, which is below... check declaration order and pick the injection point that compiles; a forward declaration in the mutant replacement is also acceptable).

- [ ] **Step 2: Run each new mutant by name and the rot check** (NOT the full run):

```bash
./tests/mutate.sh "not verified at yield"
./tests/mutate.sh "covers its own field"
./tests/mutate.sh "verified during replay"
./tests/mutate.sh "under the handle engine"
./tests/mutate.sh --rot-only
```

All four caught; rot-only reports every pattern intact.

- [ ] **Step 3: CLAUDE.md trap entry**, in "Things that look like bugs and are not" after the non-canonical-records entry:

```markdown
- **Record checksums are not verified during replay or pointer-section load** (F-32b), only at materialization. Verifying in replay looks like defence in depth and is a data-loss bug: F-24 completes a file at its first invalid record, so a verifying replay turns one flipped value byte into the silent loss of every record after it. The span checksum guards structure and liveness (C-4f); the record checksum guards the bytes a caller is about to consume. `test_record_csum_replay_no_truncate` holds the two apart.
```

- [ ] **Step 4: Full suite one more time:** `make check`.

- [ ] **Step 5: Commit**

```bash
git add tests/mutate.sh CLAUDE.md
git commit -m "test: mutants for record checksums, and the F-32b trap documented"
```
