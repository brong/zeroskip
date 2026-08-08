# Seal and Compact Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `zs_db_seal()` converts the active generation so a database is fully indexed; `zs_db_compact()` merges everything into a single file, reclaiming the tombstones a partial repack structurally cannot.

**Architecture:** Both reuse existing machinery. Seal calls `zsi_convert_one` on the active file under the write lock — a conversion output covers its input's range (D-5a), so no new file and no wasted generation. Compact seals, converts stragglers, then merges every in-order file by giving `zsi_repack_select` a `full` flag rather than adding a second merge entry point.

**Tech Stack:** C99, POSIX, no external libraries.

**Design doc:** `docs/superpowers/specs/2026-08-07-seal-compact-salvage-design.md`, sections A and B. Section C (salvage) is **out of scope** for this plan.

## Global Constraints

- **The spec is normative.** Spec changes land in their own commit, *before* the code (Task 1).
- Append to `CFLAGS` with `EXTRA_CFLAGS=...`; never override it.
- Every function returns `enum zs_ret`. Internals are `static` with a `zsi_` prefix.
- Sections in `zeroskip.c` may only call **upwards**. Seal and compact live in `REPACK` / `PUBLIC API`, both below `CONVERSION`, so both may call `zsi_convert_one` and `zsi_convert_pending`.
- **Lock order is repack-then-write**, established here. Nothing currently takes the write lock and then the repack lock — D-12c is explicit that conversion never takes the repack lock — so this order is free, and the spec must state it (C-1h).
- `make asan` and `make leaks` clean first; a bare `make check` straight after `make asan` runs the *sanitizer* binary.
- **Nothing may touch `zeroskip.c` while `tests/mutate.sh` runs**, and two runs must never overlap.
- Commit messages end with `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`.

---

## File Structure

No new source files; both features are a few hundred lines in existing sections.

| File | Change |
|---|---|
| `doc/specification.md` | `D-24`–`D-28` for seal and compaction; `A-10`/`A-11`; a note on open item 1; C-1h's lock order |
| `doc/conformance.md` | a row per new requirement |
| `zeroskip.h` | `zs_db_seal`, `zs_db_compact` |
| `zeroskip.c` | `zsi_seal` in `CONVERSION`; `full` flag in `zsi_repack_select`; `zsi_repack_run` extracted; `zsi_compact` in `REPACK` |
| `zstool.c` | `seal` and `compact` subcommands |
| `zstest.c` | tests |
| `tests/mutate.sh` | a mutant per requirement |
| `zsbench.c` | a `compact` row |
| `doc/benchmarking.md`, `doc/overview.md`, `CLAUDE.md` | prose |

---

## Task 1: Spec

**Files:** `doc/specification.md`, `doc/conformance.md`

No code, no tests. The spec changes deliberately, in its own commit, first.

- [ ] **Step 1: Add the requirements**

In §5, after D-23, add:

```markdown
- **D-24 Sealing.** A writer MAY convert the **active** file on demand, holding
  the write lock. D-12 skips the active file because another writer may be
  appending to it; a caller holding the write lock has excluded exactly that, so
  the exception does not apply.
- **D-24a** Sealing MUST NOT create a replacement active file. A conversion
  output covers its input's range (D-5a), so afterwards the newest file is
  in-order and the file set has no active file — a state a reader already
  handles, and which the next write resolves by creating a new generation
  (D-9b). An implementation that instead rolled over and then converted would
  consume a generation per seal.
- **D-24b** Sealing is a no-op, and not an error, when there is no active file,
  when the active file holds no valid spans, or when its header does not
  validate (D-10). The last MUST be reported.
- **D-24c** An unclean active file (D-9) MAY be sealed. The conversion reads to
  the complete point (F-24), so content past it does not survive into the
  output — which is the same outcome R-4 already produces, reached sooner.
- **D-25 Compaction.** An implementation MAY merge the **entire** database into
  one file. The order is normative: seal (D-24), then convert every remaining
  unordered file (D-12), then merge every in-order file as one input set,
  bypassing D-16's selection.
- **D-25a** D-16's geometric selection exists to keep a repack amortised.
  Compaction is explicitly the unamortised case, so it does not apply. Every
  other repack rule does, D-17 through D-23 unchanged.
- **D-26** Because a compaction output spans the whole generation interval,
  D-19's containment test succeeds for every key, so every tombstone whose
  lifespan is contained is dropped. This is the only merge that can reclaim
  them: a partial repack MUST retain a tombstone because a file outside its
  input set may still hold the key (D-19a).
- **D-27** Compaction is **best effort in action and strict in reporting**. It
  merges everything mergeable and reports what it could not, and reports failure
  only if the result is not a single file. A non-active file with an invalid
  header is the case that blocks it: D-10a tolerates such a file but it can be
  neither converted nor merged.
- **D-28** A caller taking both locks MUST take **repack before write**.
  Compaction needs the write lock for its seal step and the repack lock for the
  merge; taking write first would invert the order against a conforming peer.
  Nothing in this specification takes the write lock and then the repack lock —
  D-12c makes conversion lock-free with respect to repack — so the order costs
  nothing to adopt. Compaction MUST release the write lock before merging, so a
  long compaction does not block writers throughout.
```

- [ ] **Step 2: Add the API requirements**

In the C binding section, after A-9:

```markdown
- **A-10** `zs_db_seal` performs D-24. It returns `ZS_OK` for the no-op cases of
  D-24b, and `ZS_READONLY` on a read-only handle.
- **A-11** `zs_db_compact` performs D-25, returning `ZS_OK` only when the
  database is a single file and `ZS_BADFORMAT` otherwise, having merged whatever
  it could first (D-27).
```

- [ ] **Step 3: Note it on open item 1**

Open item 1 says repack duration is unbounded. Append: compaction makes that
unboundedness a deliberate, documented API entry point rather than an emergent
property of D-16's cascade, and the mitigations sketched there apply to it
equally if it ever needs bounding.

- [ ] **Step 4: Conformance rows**

Add a row per new requirement, citing the tests Tasks 2–4 create:

| Req | Test |
|---|---|
| D-24 | `seal_converts_the_active_file` |
| D-24a | `seal_creates_no_new_generation` |
| D-24b | `seal_noop_cases` |
| D-24c | `seal_unclean_active_file` |
| D-25 | `compact_to_one_file` |
| D-25a | `compact_ignores_geometric_selection` |
| D-26 | `compact_drops_tombstones` |
| D-27 | `compact_reports_and_fails_on_bad_file` |
| D-28 | `compact_lock_order` |
| A-10 | `seal_noop_cases`, `seal_readonly` |
| A-11 | `compact_to_one_file`, `compact_reports_and_fails_on_bad_file` |

- [ ] **Step 5: Verify and commit**

Run: `./tests/conformance.sh`
Expected: every new label present in both files. The cited-test check will fail
until Tasks 2–4 land — that is by construction, since the spec changes first.

```bash
git add doc/specification.md doc/conformance.md
git commit -m "spec: seal and compact"
```

---

## Task 2: `zs_db_seal`

**Files:**
- Modify: `zeroskip.h` (declaration), `zeroskip.c` (`CONVERSION` section, `PUBLIC API`)
- Test: `zstest.c`

**Interfaces:**
- Produces: `int zs_db_seal(struct zs_db *db);` and `static int zsi_seal(struct zs_db *db);`

- [ ] **Step 1: Write the failing tests**

```c
/* D-24, D-24a: the active generation becomes an in-order file covering the same
 * range, and no new generation appears.  A "roll over, then convert"
 * implementation would pass the first assertion and fail the second. */
static void test_seal_converts_the_active_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    uint32_t gen;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "value", 5, 0));
    }

    gen = zsi_snapshot_active(db->snap)->hdr.start;
    ASSERT_OK(zs_db_seal(db));

    /* No active file, one in-order file, same range, and no new generation. */
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, gen);
    ASSERT_EQU(db->snap->files[0]->hdr.end, gen);

    /* Every key still reads back. */
    for (int i = 0; i < 20; i++) {
        char k[32];
        const char *v = NULL;
        size_t vl = 0;
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 5u);
    }

    /* And the next write starts a fresh generation rather than reusing one. */
    ASSERT_OK(zs_db_store(db, "after", 5, "x", 1, 0));
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(zsi_snapshot_active(db->snap)->hdr.start, gen + 1);

    ASSERT_OK(zs_db_close(&db));
}

/* D-24a: sealing repeatedly must not consume generations.  This is the
 * assertion that separates "convert in place" from "roll over then convert". */
static void test_seal_creates_no_new_generation(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    uint32_t gen;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = zsi_snapshot_active(db->snap)->hdr.start;

    for (int i = 0; i < 5; i++) ASSERT_OK(zs_db_seal(db));

    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_EQU(db->snap->files[0]->hdr.end, gen);
    ASSERT_OK(zs_db_close(&db));
}

/* D-24b: the three no-op cases, each ZS_OK and each leaving the set alone. */
static void test_seal_noop_cases(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* A brand-new database: an active file with no spans. */
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_seal(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));   /* still unordered */

    /* Already sealed: no active file at all. */
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_EQU(db->snap->nfiles, 1u);

    ASSERT_OK(zs_db_close(&db));
}

/* A-10: a read-only handle must not seal (R-3). */
static void test_seal_readonly(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(zs_db_seal(db), ZS_READONLY);
    ASSERT_OK(zs_db_close(&db));
}

/* D-24c: an unclean active file seals to its complete point, and the garbage
 * does not survive.  Reaching this state needs a raw append, which is what
 * put_garbage_on_newest does elsewhere in this suite. */
static void test_seal_unclean_active_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX], path[PATH_MAX];
    size_t clean_size;
    int fd;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    clean_size = zsi_snapshot_active(db->snap)->size;
    zsi_name_format(name, db->uuid, zsi_snapshot_active(db->snap)->hdr.start, 0);
    ASSERT_OK(zs_db_close(&db));

    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
    fd = open(path, O_WRONLY | O_APPEND);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "\336\255\276\357\336\255\276\357", 8), 8);
    close(fd);

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQU(zsi_snapshot_active(db->snap)->complete, clean_size);
    ASSERT_OK(zs_db_seal(db));

    ASSERT_NULL(zsi_snapshot_active(db->snap));
    {
        const char *v = NULL;
        size_t vl = 0;
        ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}
```

Register all five in the `tests[]` table.

- [ ] **Step 2: Run to verify they fail**

Run: `make zstest && ./zstest seal`
Expected: build failure — `zs_db_seal` undeclared.

- [ ] **Step 3: Implement**

`zeroskip.h`, beside `zs_db_repack`:

```c
/* Convert the active generation, so every file in the database has a pointer
 * section and no reader has to replay a span chain.  Bounded by rollover_size.
 * A no-op when there is nothing to seal (D-24b). */
int  zs_db_seal(struct zs_db *db);
```

`zeroskip.c`, in `CONVERSION` after `zsi_convert_pending`:

```c
/* D-24: convert the ACTIVE file, on demand, under the write lock.
 *
 * D-12 skips the active file because another writer may be appending to it. A
 * caller holding the write lock has excluded exactly that, so the exception does
 * not apply -- which is why this is safe and why it must not be reachable
 * without the lock.
 *
 * NO replacement active file is created (D-24a).  A conversion output covers its
 * input's range (D-5a), so afterwards the newest file is in-order,
 * zsi_snapshot_active returns NULL -- a state every reader already handles -- and
 * the next write creates a new generation (D-9b).  Rolling over first and
 * converting second would reach the same place while consuming a generation per
 * seal, and generations are finite (D-9c). */
static int zsi_seal(struct zs_db *db)
{
    struct zsi_file *act;
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    if (db->write_txn) return ZS_BADUSAGE;      /* one writer at a time */

    r = zsi_lock_take(&db->locks, ZSI_LOCK_WRITE,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    /* Refresh under the lock: another writer may have rolled over while we
     * waited, and sealing a stale view would convert a file already superseded. */
    r = zsi_db_refresh(db);
    if (r != ZS_OK) goto out;

    act = zsi_snapshot_active(db->snap);

    /* D-24b's three no-ops, none of them errors. */
    if (!act) goto out;                                     /* already sealed */
    if (!act->hdr_valid) {                                  /* D-10 */
        db->error("active file has an invalid header; nothing to seal",
                  "file=<%s>", act->fname);
        goto out;
    }
    if (act->complete <= ZSI_HEADER_LEN) goto out;          /* no valid spans */

    r = zsi_convert_one(db, act);
    if (r != ZS_OK) goto out;

    r = zsi_db_refresh(db);

    /* A seal is a natural moment to catch up any stranded unordered file, and
     * costs nothing when there is none (D-12). */
    if (r == ZS_OK) (void)zsi_convert_pending(db);

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
    return r;
}
```

`PUBLIC API`, beside `zs_db_repack`:

```c
int zs_db_seal(struct zs_db *db)
{
    if (!db) return ZS_BADUSAGE;
    return zsi_seal(db);
}
```

- [ ] **Step 4: Run to verify they pass**

Run: `make zstest && ./zstest seal`
Expected: all five `ok`.

- [ ] **Step 5: Whole suite**

Run: `make check`
Expected: all pass except `conformance.sh`, still red for Task 3's tests.

- [ ] **Step 6: Commit**

```bash
git add zeroskip.h zeroskip.c zstest.c
git commit -m "feat: zs_db_seal"
```

---

## Task 3: `zs_db_compact`

**Files:**
- Modify: `zeroskip.h`, `zeroskip.c` (`REPACK`, `PUBLIC API`)
- Test: `zstest.c`

**Interfaces:**
- Consumes: `zsi_seal` from Task 2.
- Produces:
  - `zsi_repack_select(struct zsi_snapshot *snap, bool full, size_t *first)` — one new parameter, second position. `zsi_should_repack` passes `false`.
  - `static int zsi_repack_run(struct zs_db *db, size_t first, size_t count);` — the merge-and-retire body extracted from `zsi_repack`, so there is one merge entry point.
  - `int zs_db_compact(struct zs_db *db);`

- [ ] **Step 1: Write the failing tests**

```c
/* D-25, A-11: everything becomes one in-order file spanning the whole range. */
static void test_compact_to_one_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;          /* force several generations */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT(db->snap->nfiles > 1);

    ASSERT_OK(zs_db_compact(db));

    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, 1u);
    ASSERT(!zs_db_should_repack(db));

    for (int i = 0; i < 80; i++) {
        char k[32];
        const char *v = NULL;
        size_t vl = 0;
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 10u);
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-26: a compaction spanning 1..N drops tombstones, which a repack cannot.
 * Asserted by SIZE as well as by behaviour -- "the key is absent" holds either
 * way, so only the file getting smaller shows the tombstone actually went. */
static void test_compact_drops_tombstones(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    size_t before, after;
    const char *v = NULL;
    size_t vl = 0;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 60; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    /* Delete most of them, which writes 40 tombstones. */
    for (int i = 0; i < 40; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_delete(db, k, strlen(k), 0));
    }
    ASSERT_OK(zs_db_seal(db));
    before = 0;
    for (size_t i = 0; i < db->snap->nfiles; i++) before += db->snap->files[i]->size;

    ASSERT_OK(zs_db_compact(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    after = db->snap->files[0]->size;

    ASSERT(after < before);

    /* Deleted keys stay deleted; survivors survive. */
    for (int i = 0; i < 40; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_EQ(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0),
                  ZS_NOTFOUND);
    }
    for (int i = 40; i < 60; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
    }

    /* A key deleted and then rewritten must survive with its NEW value -- the
     * case where dropping the tombstone would be wrong if done carelessly. */
    ASSERT_OK(zs_db_store(db, "key000", 6, "again", 5, 0));
    ASSERT_OK(zs_db_compact(db));
    ASSERT_OK(zs_db_fetch(db, "key000", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 5u);
    ASSERT_MEM_EQ(v, "again", 5);

    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-25a: compaction merges files a repack deliberately leaves alone.  Built so
 * that zs_db_should_repack is FALSE beforehand -- otherwise a repack would have
 * done the same thing and the test would prove nothing. */
static void test_compact_ignores_geometric_selection(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT_OK(zs_db_seal(db));
    while (zs_db_should_repack(db)) ASSERT_OK(zs_db_repack(db));

    /* Repacking is finished by D-16's rule, and more than one file remains. */
    ASSERT(!zs_db_should_repack(db));
    ASSERT(db->snap->nfiles > 1);

    ASSERT_OK(zs_db_compact(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_close(&db));
}

/* D-27, A-11: best effort in action, strict in reporting. */
static void test_compact_reports_and_fails_on_bad_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX], path[PATH_MAX];
    uint32_t victim;
    int fd;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    setup.error = counting_error;
    report_count = 0;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT(db->snap->nfiles > 2);
    victim = db->snap->files[1]->hdr.start;
    zsi_name_format(name, db->uuid, db->snap->files[1]->hdr.start,
                    db->snap->files[1]->hdr.end);
    ASSERT_OK(zs_db_close(&db));

    /* Destroy one file's header, which D-10a tolerates and nothing can merge. */
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
    fd = open(path, O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "not a zeroskip header at all", 28), 28);
    close(fd);

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    report_count = 0;
    ASSERT_EQ(zs_db_compact(db), ZS_BADFORMAT);

    /* It reported, and it still did the work it could. */
    ASSERT(report_count > 0);
    ASSERT(db->snap->nfiles > 1);
    (void)victim;
    ASSERT_OK(zs_db_close(&db));
}
```

`counting_error` and `report_count` already exist in `zstest.c`; confirm with
`grep -n 'counting_error\|report_count' zstest.c` and reuse rather than
redefining.

- [ ] **Step 2: Run to verify they fail**

Run: `make zstest && ./zstest compact`
Expected: build failure — `zs_db_compact` undeclared.

- [ ] **Step 3: Add the `full` flag and extract the merge body**

In `REPACK`, change the selector's signature and add the early return:

```c
static size_t zsi_repack_select(struct zsi_snapshot *snap, bool full,
                                size_t *first)
{
    /* The in-order prefix: files [0, nio). */
    size_t nio = 0;
    while (nio < snap->nfiles && !zsi_file_is_unordered(snap->files[nio])) nio++;

    /* D-25a: compaction is explicitly the unamortised case, so D-16's geometric
     * selection does not apply to it.  Every other repack rule still does. */
    if (full) { *first = 0; return nio; }

    if (nio < 2) { *first = 0; return 0; }      /* nothing to merge */
    ...unchanged...
}
```

`zsi_should_repack` passes `false`.

Extract the body of `zsi_repack` from the name-capture down to the final
refresh into:

```c
/* Merge snap->files[first .. first+count) into one, retire the inputs, refresh.
 *
 * The single merge entry point.  Both zsi_repack and zsi_compact reach the merge
 * through here, so D-17 to D-23 are implemented once -- a second call site for
 * zsi_repack_merge is exactly how the retention rules would drift apart. */
static int zsi_repack_run(struct zs_db *db, size_t first, size_t count)
```

`zsi_repack` becomes: take the repack lock, refresh, `select(snap, false, &first)`,
return `ZS_OK` if `count < 2`, else `zsi_repack_run`, release.

- [ ] **Step 4: Implement compaction**

```c
/* D-25: the whole database into one file.
 *
 * Unbounded by design, and holding the repack lock throughout while writers
 * continue -- the same shape zs_db_repack already has, and spec open item 1's
 * unboundedness now deliberately reachable from the API.
 *
 * Lock order is REPACK then WRITE (D-28), and the write lock is released before
 * the merge, so a long compaction does not block writers throughout.  Nothing in
 * the format takes write-then-repack -- D-12c makes conversion lock-free with
 * respect to repack -- so this order costs nothing. */
static int zsi_compact(struct zs_db *db)
{
    size_t first, count;
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    r = zsi_lock_take(&db->locks, ZSI_LOCK_REPACK,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    /* Step 1 and 2: seal the active generation and convert any straggler.
     * zsi_seal takes and releases the write lock itself. */
    r = zsi_seal(db);
    if (r != ZS_OK) goto out;

    r = zsi_db_refresh(db);
    if (r != ZS_OK) goto out;

    /* Step 3: merge every in-order file as one input set. */
    count = zsi_repack_select(db->snap, true, &first);
    if (count >= 2) {
        r = zsi_repack_run(db, first, count);
        if (r != ZS_OK) goto out;
    }

    /* D-27: strict in reporting, having already done what it could.  The file
     * that blocks this is a non-active one with an invalid header -- D-10a
     * tolerates it, and nothing can convert or merge it. */
    if (db->snap->nfiles != 1) {
        for (size_t i = 0; i < db->snap->nfiles; i++)
            if (!db->snap->files[i]->hdr_valid)
                db->error("file cannot be merged; compaction left it in place",
                          "file=<%s>", db->snap->files[i]->fname);
        r = ZS_BADFORMAT;
    }

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_REPACK);
    return r;
}
```

Public entry point beside `zs_db_repack`, and the declaration in `zeroskip.h`:

```c
/* Merge the entire database into a single file, reclaiming the tombstones a
 * partial repack cannot (D-26).  Unbounded: it rewrites everything.  Returns
 * ZS_BADFORMAT if a file could not be merged, having merged the rest. */
int  zs_db_compact(struct zs_db *db);
```

- [ ] **Step 5: Run to verify they pass**

Run: `make zstest && ./zstest compact`
Expected: all four `ok`.

- [ ] **Step 6: Whole suite and sanitizers**

Run: `make check`, then `make asan`, then `make leaks`.
Expected: all pass. `conformance.sh` still needs Task 4's lock-order test.

- [ ] **Step 7: Commit**

```bash
git add zeroskip.h zeroskip.c zstest.c
git commit -m "feat: zs_db_compact"
```

---

## Task 4: Lock order, and the tool

**Files:** `zstest.c`, `zstool.c`

- [ ] **Step 1: The lock-order test**

D-28 is the requirement most likely to be broken silently, because a wrong order
only deadlocks against a *peer* holding the other lock. Assert it against a real
second process, using the existing `hold-write` machinery.

```c
/* D-28: compaction takes REPACK then WRITE, and releases WRITE before merging.
 *
 * Asserted with a second process holding the write lock: compaction must reach
 * the merge anyway once that process lets go, and must not deadlock.  A
 * nonblocking handle gives the assertion a bound -- without it a wrong order
 * would hang the suite rather than fail it, which is a worse failure. */
static void test_compact_lock_order(void)
{
    SKIP_IF_NO_FORK();
    /* Follow test_mp_* in this file: fork a child that opens the database and
     * holds the write lock for ~200ms, while the parent calls zs_db_compact and
     * asserts it completes with one file and within a timeout. */
}
```

Write it following whichever `test_mp_*` case already forks and holds a lock —
`grep -n 'test_mp_repack_and_writer_concurrent' -A 40 zstest.c` is the closest
shape.

- [ ] **Step 2: `zstool` subcommands**

Add to the usage text and the dispatch chain, beside `repack`:

```c
    } else if (!strcmp(cmd, "seal")) {
        r = zs_db_seal(db);
        if (r != ZS_OK) oops("seal", r);

    } else if (!strcmp(cmd, "compact")) {
        r = zs_db_compact(db);
        if (r != ZS_OK) oops("compact", r);
```

- [ ] **Step 3: Verify by hand**

```bash
make zstool
rm -rf /tmp/zsc && ./zstool /tmp/zsc create
./zstool /tmp/zsc store 6b31 7631
./zstool /tmp/zsc seal
./zstool /tmp/zsc dump | head
./zstool /tmp/zsc compact
ls /tmp/zsc
```
Expected: after `seal`, `dump` shows one `inorder` file; after `compact`, one
data file remains.

- [ ] **Step 4: Conformance and commit**

Run: `./tests/conformance.sh`
Expected: green — every cited test now exists.

```bash
git add zstest.c zstool.c
git commit -m "test: compaction's lock order, and zstool seal/compact"
```

---

## Task 5: Mutants

**Files:** `tests/mutate.sh`

**Nothing may edit `zeroskip.c` while this runs, and two runs must not overlap.**

- [ ] **Step 1: Add the mutants**

```bash
echo
echo "seal and compact (D-24..D-28)"

# D-24a.  Rolling over first reaches the same file layout while consuming a
# generation per seal, and generations are finite (D-9c).
mutant "seal: rolls over instead of converting" catch \
  's/    r = zsi_convert_one\(db, act\);/    r = zsi_writer_rollover_for_test(db);/'

# D-24b.  Sealing an active file with no spans writes an empty in-order file and
# burns a generation for nothing.
mutant "seal: seals a file with no spans" catch \
  's/    if \(act->complete <= ZSI_HEADER_LEN\) goto out;          \/\* no valid spans \*\//    \/* D-24b removed *\//'

# The write lock is what makes converting the ACTIVE file safe at all: without it
# another writer may be appending to the file being converted.
mutant "seal: converts without the write lock" catch \
  's/    r = zsi_lock_take\(&db->locks, ZSI_LOCK_WRITE,\n                      db->nonblocking \? ZS_NONBLOCKING : 0\);\n    if \(r != ZS_OK\) return r;\n\n    \/\* Refresh under the lock/    \/* lock removed *\/\n\n    \/* Refresh under the lock/'

# D-25a.  Falling back to the geometric rule leaves more than one file.
mutant "compact: honours geometric selection" catch \
  's/    count = zsi_repack_select\(db->snap, true, &first\);/    count = zsi_repack_select(db->snap, false, \&first);/'

# D-25.  Skipping the seal leaves the active generation out of the result.
mutant "compact: skips the seal" catch \
  's/    r = zsi_seal\(db\);\n    if \(r != ZS_OK\) goto out;/    \/* seal skipped *\//'

# D-27.  Returning OK regardless makes the return value meaningless exactly
# where a caller most needs it.
mutant "compact: reports success regardless" catch \
  's/        r = ZS_BADFORMAT;/        r = ZS_OK;/'

# D-28.  Taking WRITE outside REPACK inverts the order against a peer.
mutant "compact: takes the write lock outermost" catch \
  's/    r = zsi_lock_take\(&db->locks, ZSI_LOCK_REPACK,/    r = zsi_lock_take(\&db->locks, ZSI_LOCK_WRITE,/'
```

The `seal: rolls over instead of converting` pattern needs a rollover helper to
exist. If none is reachable, replace that mutant with one that adds a
`zsi_writer_active` call before the conversion — the observable effect is the
same: a generation is consumed, which
`test_seal_creates_no_new_generation` catches.

- [ ] **Step 2: Run them**

Run: `./tests/mutate.sh seal` then `./tests/mutate.sh compact`
Expected: every one caught. `PATTERN ROTTED` means the pattern does not match the
source as written — fix the pattern, do not delete the mutant. `NOT CAUGHT`
means a test that cannot fail; check first whether it is genuinely subsumed by a
sibling check before reclassifying, and pair any `subsumed` with a combined
mutant.

- [ ] **Step 3: Full run and commit**

Run: `./tests/mutate.sh`
Expected: no regression among the pre-existing mutants.

```bash
git add tests/mutate.sh
git commit -m "test: mutants for seal and compact"
```

---

## Task 6: Benchmark and documentation

**Files:** `zsbench.c`, `doc/benchmarking.md`, `doc/overview.md`, `CLAUDE.md`

- [ ] **Step 1: Benchmark**

Add `bench_compact`: build a database with several generations and a known
proportion of deletions, then report compaction wall-clock and the size before
and after, so D-26's reclamation has a number. Report bytes-reclaimed as a
percentage — that is the figure someone deciding whether to run it wants.

- [ ] **Step 2: Run it**

Run: `make bench && ./zsbench -n 20000`
Expected: `--selftest` passes; the compaction row shows a size reduction
proportional to the deletions. Record the numbers.

- [ ] **Step 3: Documentation**

`doc/benchmarking.md`: a `compaction` section with the measured numbers and the
warning that it is unbounded.

`doc/overview.md`: seal and compaction in plain language, including that
compaction is the only thing that reclaims deleted space and that it rewrites
everything.

`CLAUDE.md`: `zs_db_seal` / `zs_db_compact` in the API list, and under *things
that look like bugs and are not*:

```markdown
- **Sealing converts the active file in place rather than rolling over first.** A conversion output covers its input's range (D-5a), so the newest file becomes in-order and there is simply no active file until the next write. Rolling over first reaches the same layout while consuming a generation per seal, and generations are finite (D-9c).
- **Compaction takes the repack lock and then the write lock**, and releases the write lock before merging (D-28). Nothing else takes both, so this establishes the order; taking write outermost would invert it against a conforming peer, and holding it through the merge would block writers for an unbounded time.
- **`zsi_repack_select` has a `full` flag rather than compaction calling `zsi_repack_merge` directly.** One merge entry point means D-17 to D-23 are implemented once. A second call site is exactly how the retention rules would drift apart.
```

- [ ] **Step 4: Final verification**

```bash
make clean && make check
make asan
make leaks
./tests/mutate.sh
./tests/conformance.sh
./tests/tool.sh
make bench
```

- [ ] **Step 5: Commit**

```bash
git add zsbench.c doc CLAUDE.md
git commit -m "docs: seal and compaction, measured"
```

---

## Self-review notes

**Spec coverage.** D-24, D-24a, D-24b, D-24c, D-25, D-25a, D-26, D-27, D-28,
A-10 and A-11 each have a test in Tasks 2–4 and a conformance row in Task 1.

**Known soft spots, flagged rather than hidden:**

- Task 4's lock-order test is described rather than written out, because it must
  follow whichever `test_mp_*` fork helper exists; writing it blind would produce
  a test that compiles against a harness that isn't there. It needs a timeout, or
  a wrong lock order hangs the suite instead of failing it.
- Task 5's first mutant depends on a rollover helper being reachable; the
  fallback is given.
- `test_compact_drops_tombstones` asserts the output shrinks. If the tombstones
  are a small fraction of the data the comparison could be noise, so the test
  deletes two thirds of the keys deliberately. If it proves flaky, count records
  through `zs_db_dump` rather than comparing bytes.
