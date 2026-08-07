# Pointer Table Cache Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let any process publish a sorted pointer table over an unordered file into a configured cache directory, so that opening a database replays only the suffix beyond the newest published table instead of the whole active file.

**Architecture:** A new `POINTER TABLE CACHE` section in `zeroskip.c`, sitting between `PRIVATE INDEX` and `PER-FILE CURSOR`. It loads a self-validating 96-byte-header table file, seeds `struct zsi_index`'s base array from it, and publishes an updated table by writing a temp file and `rename`ing it into place. `SNAPSHOT` and `WRITE PATH` call into it. Nothing is written into the database directory, so G-6 and R-3 still hold for the database itself.

**Tech Stack:** C99, POSIX (`mmap`, `fcntl`, `rename`, `readdir`), no external libraries. Vendored xxHash 0.8.3.

**Design doc:** `docs/superpowers/specs/2026-08-07-pointer-table-cache-design.md` — read it before Task 1.

## Global Constraints

- **The spec is normative.** `doc/specification.md` wins over code. Spec changes land in their own commit, *before* the code that implements them (Task 1).
- Default `CFLAGS` are `-Wall -Wextra -g -O2 -fno-strict-aliasing -std=c99`. Append with `EXTRA_CFLAGS=...`; never override `CFLAGS`.
- Every function returns `enum zs_ret` (`ZS_OK = 0`, `ZS_DONE = 1`, negatives for errors). Output through pointer parameters.
- Internal types and functions take the `zsi_` prefix and are `static`. Macros are UPPERCASE. Public API is `zs_db_*` / `zs_txn_*` / `zs_cursor_*`.
- **Sections in `zeroskip.c` may only call *upwards*** into sections listed before them. The new section goes between `PRIVATE INDEX` and `PER-FILE CURSOR`, so it may call into `PRIVATE INDEX`, `FILE OBJECT`, `RECORDS`, `CHECKSUMS`, `LIBRARY SUPPORT`; and `SNAPSHOT` / `WRITE PATH` may call into it.
- Integers on disk are little-endian, via `zsi_put16/32/64` and `zsi_get16/32/64`. Never punt to a cast.
- Every offset arithmetic that could overflow goes through `zsi_add_sz` / `zsi_mul_sz` (G-0b).
- All file data access goes through `zsi_file_at()` — the single bounds-checked accessor. Do not index `f->base` directly.
- **Every target builds a binary called `zstest`**, so `make asan` and `make leaks` clean first. After `make asan`, a bare `make check` is running the sanitizer binary.
- Add a `tests/mutate.sh` mutant whenever you add a test for a requirement.
- Commit messages end with:
  `Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>`

### Normative values fixed by this feature

Copy these exactly; they are interoperability surface.

- Table magic, 16 bytes: `89 7A 73 69 6E 64 65 78 31 0D 0A 1A 0A 00 00 00` (`"\x89zsindex1\r\n\x1a\n\0\0\0"`)
- Table header length: **96 bytes**. Total file size: `96 + 8 * nptrs + 4`.
- Published name: `zeroskip.index-<uuid>-<GEN8hex>` — lowercase hyphenated uuid, uppercase 8-digit hex generation.
- Staging name: `zeroskip.tmp.<pid>.<8 random lowercase hex digits>`
- Data file header length `H` = 72. Record offsets in a table lie in `[72, valid_upto)`.

---

## File Structure

No new source files. This codebase is deliberately one implementation file with labelled sections, and a new file would break the "may only call upwards" layering rule that the section order encodes.

| File | Change |
|---|---|
| `doc/specification.md` | New top-level §8 "Pointer table cache" with `P-n` requirements; amend R-3, G-6, preamble; renumber §8–§11 to §9–§12; close open item 2 |
| `doc/conformance.md` | A row per `P-n` |
| `doc/overview.md` | A paragraph on the cache |
| `doc/benchmarking.md` | Replace the open-item-2 section with the before/after |
| `CLAUDE.md` | Architecture section list; interop surface list; a "looks like a bug and is not" entry |
| `zeroskip.h` | `index_dir` and `index_threshold` in `struct zs_open_data`; updated initializer |
| `zeroskip.c` | New `POINTER TABLE CACHE` section; `zsi_unordered_replay` start offset; `zsi_index_build_from`; `zsi_index_flatten`; hooks in `SNAPSHOT`, `WRITE PATH`, `OPEN AND CLOSE` |
| `zstool.c` | `--index-dir` option; `index-dump` subcommand |
| `zstest.c` | Tests, registered in the `tests[]` table at the end |
| `tests/mutate.sh` | One mutant per new requirement |
| `tests/gencorpus.sh` | A `cached-index` corpus case |
| `tests/corpus/README.md` | Description of the new case |
| `zsbench.c` | `open (cached)` workload |

---

## Task 1: Spec

**Files:**
- Modify: `doc/specification.md`
- Modify: `doc/conformance.md`
- Modify: `doc/implementation-plan.md` (its §-number map)

No code and no tests in this task. The project rule is that the spec changes deliberately, in its own commit, before the code.

- [ ] **Step 1: Renumber the trailing sections**

`doc/specification.md` currently ends with `## 8. C binding`, `## 9. Conformance suite`, `## 10. Non-goals`, `## 11. Open items`. Renumber them to 9, 10, 11, 12. Then update the four existing `§8` references — three in the preamble around lines 19–33 (`§8 gives a C API`, `Only §8's zero-copy pointer lifetimes (A-4)`) and one at line 1113 (`The semantics below are normative; the spelling is a C binding (§1).` — check its neighbours for a `§8`). Update `doc/implementation-plan.md`'s section map at lines 111–124 if any entry names a renumbered section.

- [ ] **Step 2: Add the preamble mention**

In the "What is normative for every implementation" sentence (line ~19), add the pointer table cache: it is normative *when present*, and a conforming implementation must produce identical results with it absent.

- [ ] **Step 3: Write the new §8**

Insert after `## 7. Open and recovery`, before the renumbered `## 9. C binding`:

```markdown
## 8. Pointer table cache

An unordered file has no pointer section, so key order for it is derived by
replaying its spans (D-13a). That replay is bounded by `rollover_size` but paid
on every open. The pointer table cache lets one process publish the result and
another load it.

It is **optional and never load-bearing**. A conforming implementation MUST
produce identical results with the cache directory empty, absent, or full of
tables it rejects.

- **P-1** A pointer table covers exactly one unordered file, identified by the
  database uuid and the file's generation. In-order files have a pointer section
  and MUST NOT have a table.
- **P-2** Tables live in a **cache directory** named by the caller. The cache
  directory MUST NOT be the database directory: an implementation MUST reject
  that configuration. Writing there is not a write to the database, so R-3 is
  not weakened — a read-only handle MAY publish a table.
- **P-3** A published table is named `zeroskip.index-<uuid>-<GEN8hex>`, using
  D-0's uuid form and D-1's generation form. The `zeroskip.` prefix places it in
  the metadata namespace (D-2), so it can never be parsed as a data file.
- **P-4** A table is published by writing a complete file under a staging name
  `zeroskip.tmp.<pid>.<8 hex digits>` in the cache directory and `rename`ing it
  over the published name. A table is never modified in place, never appended
  to, and never truncated.
- **P-5** A table has a 96-byte header, then `nptrs` 8-byte little-endian record
  offsets, then a 4-byte checksum over those offsets. Its size is exactly
  `96 + 8 * nptrs + 4`.

  | offset | size | field |
  |---|---|---|
  | 0 | 16 | magic (P-6) |
  | 16 | 1 | version_read |
  | 17 | 1 | version_write |
  | 18 | 2 | flags: low 4 bits the checksum engine, bit 4 "checksums verified" |
  | 20 | 4 | reserved, written zero, ignored on read |
  | 24 | 16 | uuid |
  | 40 | 4 | start — the covered file's generation |
  | 44 | 4 | reserved, written zero, ignored on read |
  | 48 | 16 | comparator name |
  | 64 | 8 | valid_upto |
  | 72 | 8 | term_off |
  | 80 | 8 | nptrs |
  | 88 | 4 | term_csum |
  | 92 | 4 | checksum over [0, 92) |

- **P-6** The magic is the 16 bytes
  `89 7A 73 69 6E 64 65 78 31 0D 0A 1A 0A 00 00 00`. It is constructed on the
  same principles as F-6's, and is deliberately different from it, so the two
  artefacts are distinguishable by content as well as by name. A reader MUST
  validate all 16 bytes.
- **P-7** A table's checksums use **the engine the covered data file's header
  names**, not the engine the reading or writing handle would choose for a new
  file. This is F-5a applied to the table: a peer able to read the data file can
  always validate its table.
- **P-8** `valid_upto` is the data-file offset the table covers. It MUST be a
  span boundary: the offset immediately after a valid span's terminator, or the
  data file's header length when the file has no valid spans.
- **P-9** The offsets are the record offsets of every distinct key committed
  below `valid_upto`, each being that key's newest such record (D-14), sorted
  ascending by key under the named comparator. Rolled-back spans contribute
  nothing (F-25).
- **P-10** `term_off` is the offset of the terminator immediately below
  `valid_upto`, and `term_csum` is the checksum that terminator carries. When the
  file has no valid spans, `valid_upto == term_off ==` the data file's header
  length, `term_csum == 0` and `nptrs == 0`. `term_off` is recorded because
  terminators are located by walking spans forward, so a reader given only
  `valid_upto` could not check the binding without the replay the table exists to
  avoid.
- **P-11** A reader MUST use a table only if **all** of the following hold, and
  MUST otherwise ignore it and build the index by replay. A rejected table is
  never an error and MUST NOT be reported as corruption.
  - the magic matches, the header checksum validates, and the pointer-array
    checksum validates;
  - `version_read` does not exceed the reader's;
  - the file size is exactly `96 + 8 * nptrs + 4`;
  - uuid and `start` match the data file;
  - the comparator name matches both the data file's field and the reader's own;
  - flags bit 4 is set, unless the reader is itself not verifying checksums
    (F-5e) — an index built without verification may contain records a verifying
    reader would reject;
  - `H <= valid_upto <= ` the data file's size, where `H` is the data file's
    header length;
  - every offset lies in `[H, valid_upto)`;
  - either the no-spans case of P-10 holds, or `H <= term_off < valid_upto`, the
    bytes at `term_off` decode as a terminator, `term_off + term_len ==
    valid_upto`, and its checksum equals `term_csum`.
- **P-12** Having accepted a table, a reader takes its offsets as the index's
  ordered base and replays spans from `valid_upto` onwards, folding the result in
  (D-13b). Beginning a replay at a span boundary is sound because a span is
  self-delimiting and self-validating.
- **P-13** After building or extending an index over an unordered file, a
  process SHOULD publish a table covering the file's complete point if the
  distance from the loaded table's `valid_upto` (or from `H`, if none was
  loaded) reaches an implementation-defined threshold. The threshold bounds both
  the publisher's write amplification and the next reader's catch-up replay.
  Publishing on every commit makes a bulk load quadratic and is why the
  threshold is required rather than optional.
- **P-14** A table MUST NOT be `fsync`ed before publication. It is rebuildable,
  and a torn or zero-filled file after a crash is rejected by P-11. Syncing it
  would put a sync on the commit path.
- **P-15** A failure to publish MUST NOT fail the operation that triggered it.
- **P-16** A process MAY unlink tables in the cache directory whose uuid matches
  its database and whose generation is not present as an unordered file in its
  snapshot. Unlinking is safe against a concurrent reader: a descriptor already
  open survives it, and a reader that misses a table replays instead.
- **P-17** The binding of P-10 detects a data file whose covered prefix has
  changed, which within the format cannot happen — files are append-only and
  generations are never reissued (D-9b). It exists for out-of-band events. It
  checks one span, so it cannot detect divergence confined to an earlier one:
  a cache directory MUST therefore be scoped to the lifetime of the database
  instance, and a caller that restores a database directory out of band MUST
  discard its tables.
```

- [ ] **Step 4: Amend R-3 and G-6**

R-3 currently reads "A reader MUST NOT write." and ends "There is no shared cache for it to update (D-13c)." Change to:

```markdown
- **R-3** A reader MUST NOT write **to the database**. Opening a damaged database
  read-only is side-effect-free: no conversion, no repack, no new active file, no
  removal. There is no shared cache inside the database for it to update
  (D-13c). Publishing a pointer table into a separately configured cache
  directory (§8) is not a write to the database, and a read-only handle MAY do
  it.
```

G-6 currently ends "There is no manifest and no shared cache, so correctness cannot depend on one, and nothing needs cleaning up when a process dies." Append:

```markdown
  The optional pointer table cache (§8) does not weaken this: it lives outside
  the database, its tables are published by rename and never modified, and a
  conforming implementation produces identical results with it absent.
```

- [ ] **Step 5: Add the A-n entries**

In the C binding section, alongside the existing `zs_open_data` field requirements, add two requirements (take the next free `A-` numbers) covering `index_dir` (NULL disables the cache; naming the database directory is `ZS_BADUSAGE`; the library does not create it) and `index_threshold` (0 selects a default derived from `rollover_size`).

- [ ] **Step 6: Close open item 2**

Replace open item 2's body with a note that it is resolved by §8, and that the shape chosen differs from the one sketched: publishing the *sorted* table rather than an append-only `(key, offset)` log removes the sort as well as the scan, and keeping it outside the database directory leaves D-1/D-2/D-4 and the file-set scan untouched. Renumber the remaining item if needed.

- [ ] **Step 7: Add conformance rows**

`doc/conformance.md` maps every normative requirement to the test enforcing it. Add a row per `P-n`. The test names are those created in Tasks 3–8; use exactly these:

| Req | Test |
|---|---|
| P-1 | `test_idxcache_only_unordered_files` |
| P-2 | `test_idxcache_rejects_db_dir` |
| P-3 | `test_idxcache_published_name` |
| P-4 | `test_idxcache_publishes_by_rename` |
| P-5 | `test_idxcache_header_byte_layout` |
| P-6 | `test_idxcache_header_byte_layout` |
| P-7 | `test_idxcache_uses_file_engine` |
| P-8 | `test_idxcache_valid_upto_is_span_boundary` |
| P-9 | `test_idxcache_matches_full_build` |
| P-10 | `test_idxcache_rejects_bad_term_binding` |
| P-11 | `test_idxcache_rejection_rules` |
| P-12 | `test_idxcache_replays_the_suffix` |
| P-13 | `test_idxcache_threshold` |
| P-14 | `test_idxcache_no_fsync_on_publish` |
| P-15 | `test_idxcache_publish_failure_is_not_fatal` |
| P-16 | `test_idxcache_sweeps_dead_generations` |
| P-17 | `test_idxcache_rejects_bad_term_binding` |

- [ ] **Step 8: Commit**

```bash
git add doc/specification.md doc/conformance.md doc/implementation-plan.md
git commit -m "$(cat <<'EOF'
spec: pointer table cache

New section 8 with requirements P-1..P-17: an optional cache directory
holding sorted pointer tables over unordered files, published by rename,
so a reader replays only the suffix beyond the newest table.

Amends R-3 (a reader must not write TO THE DATABASE; a cache directory is
not the database) and G-6 (the cache holds no mutable state either, and
results must be identical with it absent). Closes open item 2, noting that
publishing the sorted table rather than an append-only log removes the sort
as well as the scan.

Sections 8 to 11 renumber to 9 to 12.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Public API fields

**Files:**
- Modify: `zeroskip.h` (`struct zs_open_data`, `ZS_OPEN_DATA_INITIALIZER`)
- Modify: `zeroskip.c` (`struct zs_db`, `zsi_db_open_common` or equivalent in `OPEN AND CLOSE`)
- Test: `zstest.c`

**Interfaces:**
- Produces: `struct zs_open_data` gains `const char *index_dir;` and `size_t index_threshold;`. `struct zs_db` gains `char *index_dir;` (owned copy, NULL when disabled) and `size_t index_threshold;` (resolved, never 0 when `index_dir` is set).

- [ ] **Step 1: Write the failing test**

Add to `zstest.c`, above the `tests[]` table:

```c
/* P-2: the cache directory must not be the database directory.  Allowing it
 * would let a read-only handle write into the database, which is exactly what
 * R-3 forbids -- the amendment permits writing to a cache directory precisely
 * because it is somewhere else. */
static void test_idxcache_rejects_db_dir(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.index_dir = dbdir;

    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    ASSERT_NULL(db);
}

/* index_threshold 0 resolves to a default derived from rollover_size, so a
 * caller that sets index_dir and nothing else still gets bounded publishing. */
static void test_idxcache_threshold_defaults(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cachedir[PATH_MAX];

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.rollover_size = 8192;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->index_threshold, 8192u / 8u);
    ASSERT_STR_EQ(db->index_dir, cachedir);
    ASSERT_OK(zs_db_close(&db));
}
```

Register both in the `tests[]` table at the end of `zstest.c`:

```c
    { "test_idxcache_rejects_db_dir",   test_idxcache_rejects_db_dir },
    { "test_idxcache_threshold_defaults", test_idxcache_threshold_defaults },
```

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest idxcache`
Expected: build failure — `struct zs_open_data` has no member `index_dir`.

- [ ] **Step 3: Add the fields**

In `zeroskip.h`:

```c
struct zs_open_data {
    uint32_t     flags;
    zs_compar   *compar;         /* NULL = byte order */
    const char  *compar_name;    /* stored in every file header */
    zs_csum     *csum;           /* required for engine 2 */
    size_t       rollover_size;  /* 0 = default 2MB */
    void       (*error)(const char *msg, const char *fmt, ...);

    /* Pointer table cache (spec section 8).  NULL disables it entirely, which
     * is the default: the library never picks a directory itself, because a
     * planted table yields wrong records and a world-writable default such as
     * /tmp would make planting one trivial.  MUST NOT name the database
     * directory (P-2). */
    const char  *index_dir;
    size_t       index_threshold;  /* 0 = rollover_size / 8 */
};

#define ZS_OPEN_DATA_INITIALIZER { 0, NULL, NULL, NULL, 0, NULL, NULL, 0 }
```

In `zeroskip.c`, add to `struct zs_db`:

```c
    char   *index_dir;        /* owned; NULL when the cache is disabled */
    size_t  index_threshold;
```

In the open path, after `rollover_size` is resolved and before the first
refresh, add:

```c
    /* P-2.  Same directory means a read-only handle would write into the
     * database, and the R-3 amendment permits publishing only because a cache
     * directory is somewhere else.  Compared by resolved path so "." and a
     * trailing slash cannot slip past. */
    if (setup && setup->index_dir) {
        char rd[PATH_MAX], rc[PATH_MAX];
        if (realpath(dir, rd) && realpath(setup->index_dir, rc)
            && strcmp(rd, rc) == 0) {
            zsi_db_free(db);
            return ZS_BADUSAGE;
        }
        db->index_dir = strdup(setup->index_dir);
        if (!db->index_dir) { zsi_db_free(db); return ZS_INTERNAL; }
        db->index_threshold = setup->index_threshold
                            ? setup->index_threshold
                            : db->rollover_size / 8;
        if (db->index_threshold == 0) db->index_threshold = 1;
    }
```

`realpath` needs `#include <limits.h>` and `<stdlib.h>`, both already present;
add `#ifndef PATH_MAX / #define PATH_MAX 4096 / #endif` near the other portability
shims if it is not already defined. Free `db->index_dir` wherever `db->dir` is
freed in `zs_db_close`.

Note the ordering constraint: this must run **after** `db->rollover_size` is set,
because the default threshold derives from it. If the database directory does not
yet exist (`ZS_CREATE`), `realpath(dir, rd)` fails and the check is skipped —
that is correct, because a directory that does not exist cannot be the cache
directory either; the check re-runs on every subsequent open.

- [ ] **Step 4: Run to verify it passes**

Run: `make zstest && ./zstest idxcache`
Expected: both tests `ok`.

- [ ] **Step 5: Run the whole suite**

Run: `make check`
Expected: all tests pass, no new failures.

- [ ] **Step 6: Commit**

```bash
git add zeroskip.h zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
feat: index_dir and index_threshold open options

The pointer table cache is opt-in (P-2): NULL index_dir keeps every current
behaviour. The library never picks a directory itself, because a planted
table yields wrong records and a world-writable default would make planting
one trivial. Naming the database directory is ZS_BADUSAGE, since that would
let a read-only handle write into the database.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Replay from an offset, and a seeded index build

**Files:**
- Modify: `zeroskip.c` — `zsi_unordered_replay` (`UNORDERED FILE`), `zsi_index_build` (`PRIVATE INDEX`)
- Test: `zstest.c`

**Interfaces:**
- Consumes: nothing from Task 2.
- Produces:
  - `static int zsi_unordered_replay(struct zsi_file *f, size_t from, bool nocsum, zsi_replay_cb *cb, void *rock)` — one new `from` parameter, second position. Every existing caller passes `ZSI_HEADER_LEN`.
  - `static int zsi_index_build_from(struct zsi_file *f, zs_compar *compar, bool nocsum, size_t *base, size_t nbase, size_t from)` — builds `f->index` seeded with an already-sorted, already-deduplicated `base` of `nbase` offsets (ownership transfers to the index; may be NULL with `nbase == 0`), then replays from `from`, folding records in through `zsi_index_insert`. `zsi_index_build` becomes a one-line wrapper passing `NULL, 0, ZSI_HEADER_LEN`.
  - `static int zsi_index_flatten(struct zsi_index *ix, zs_compar *compar, size_t **out, size_t *nout)` — a freshly malloc'd merged base+delta array in key order, delta winning ties. Does not mutate the index. Caller frees.
  - `struct zsi_file` gains `size_t cached_upto;` — the `valid_upto` of the table this file's index was seeded from, or `ZSI_HEADER_LEN`. `struct zsi_index` gains `size_t term_off;` and `uint32_t term_csum;`, recording the last terminator seen by the replay so publication has them without a second walk.

- [ ] **Step 1: Write the failing test**

```c
/* P-12: replaying from a span boundary yields exactly the records at or after
 * it.  This is the property that makes a partial index safe to extend, so it is
 * asserted directly rather than only through the cache. */
struct idxcache_replay_count { size_t n; size_t first_off; };

static int idxcache_replay_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct idxcache_replay_count *c = rock;
    (void)rec;
    if (!c->n) c->first_off = off;
    c->n++;
    return 0;
}

static void test_idxcache_replays_the_suffix(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    struct idxcache_replay_count all, tail;
    size_t boundary;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* Three separate commits, so three spans. */
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "3", 1, 0));

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* Replay everything, and note where the second span starts by replaying
     * from the header and stopping after the first record's span. */
    memset(&all, 0, sizeof(all));
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, false,
                                   idxcache_replay_cb, &all));
    ASSERT_EQU(all.n, 3u);

    /* The index records the boundary after the LAST span; replaying from there
     * must yield nothing. */
    boundary = f->complete;
    memset(&tail, 0, sizeof(tail));
    ASSERT_OK(zsi_unordered_replay(f, boundary, false,
                                   idxcache_replay_cb, &tail));
    ASSERT_EQU(tail.n, 0u);

    ASSERT_OK(zs_db_close(&db));
}

/* A seeded build must agree, key for key, with a build from scratch (P-9). */
static void test_idxcache_matches_full_build(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    size_t *flat = NULL, nflat = 0;
    size_t *seed = NULL, nseed = 0;
    size_t seed_upto;
    char key[32];

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 40; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "v", 1, 0));
    }

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* Snapshot the index as it stands: this is what a table would hold. */
    ASSERT_OK(zsi_index_flatten(f->index, db->compar, &flat, &nflat));
    seed_upto = f->complete;

    /* Twenty more records, then a fresh full build. */
    for (int i = 40; i < 60; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "v", 1, 0));
    }

    ASSERT_OK(zsi_index_build(f, db->compar, false));
    ASSERT_OK(zsi_index_flatten(f->index, db->compar, &seed, &nseed));

    /* Now rebuild seeded from the earlier snapshot plus the suffix, and
     * compare.  flat's ownership passes to the index. */
    ASSERT_OK(zsi_index_build_from(f, db->compar, false, flat, nflat,
                                   seed_upto));
    flat = NULL;

    {
        size_t *after = NULL, nafter = 0;
        ASSERT_OK(zsi_index_flatten(f->index, db->compar, &after, &nafter));
        ASSERT_EQU(nafter, nseed);
        for (size_t i = 0; i < nafter; i++)
            ASSERT_EQU(after[i], seed[i]);
        free(after);
    }

    free(seed);
    ASSERT_OK(zs_db_close(&db));
}
```

Register:

```c
    { "test_idxcache_replays_the_suffix", test_idxcache_replays_the_suffix },
    { "test_idxcache_matches_full_build", test_idxcache_matches_full_build },
```

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest idxcache`
Expected: build failure — `zsi_unordered_replay` takes 4 arguments, `zsi_index_build_from` and `zsi_index_flatten` are undeclared.

- [ ] **Step 3: Parameterise the replay**

Change the signature and the two lines that set the starting position. Only these lines change:

```c
static int zsi_unordered_replay(struct zsi_file *f, size_t from, bool nocsum,
                                zsi_replay_cb *cb, void *rock)
```

and, replacing `size_t pos = ZSI_HEADER_LEN;`:

```c
    /* P-12: a span is self-delimiting and self-validating, so a walk may begin
     * at any span boundary.  A caller seeding from a pointer table passes that
     * table's valid_upto; everyone else passes ZSI_HEADER_LEN.  An out-of-range
     * or non-boundary value is not trusted -- the walk simply finds no valid
     * span and reports the file complete there, which is F-24's ordinary
     * outcome rather than a special case. */
    size_t pos = from < ZSI_HEADER_LEN ? ZSI_HEADER_LEN : from;
    if (pos > f->size) pos = ZSI_HEADER_LEN;
    f->complete = pos;
```

Also record the terminator each accepted span ends with, so publication does not
need a second walk. Add near `f->complete = after;`:

```c
        f->last_term_off  = p;
        f->last_term_csum = term.csum;
```

and initialise both alongside `f->complete = pos;`:

```c
    f->last_term_off  = pos;
    f->last_term_csum = 0;
```

Add to `struct zsi_file`, in the unordered group:

```c
    size_t            cached_upto;     /* P-10: table this index was seeded from */
    size_t            last_term_off;   /* offset of the terminator at complete */
    uint32_t          last_term_csum;  /* that terminator's checksum */
```

Update the existing callers of `zsi_unordered_replay` to pass `ZSI_HEADER_LEN`
as the second argument. Find them with `grep -n zsi_unordered_replay zeroskip.c`.

Note: when seeding from a table, `last_term_off`/`last_term_csum` must be
initialised from the table rather than from the replay, because a replay that
finds no new span leaves them at their starting values. `zsi_index_build_from`
handles that below.

- [ ] **Step 4: Add the seeded build and the flatten helper**

In `PRIVATE INDEX`, replace `zsi_index_build` with:

```c
/* Build the private index for an unordered file, optionally seeded.
 *
 * base/nbase, when given, is an already-sorted, already-deduplicated array of
 * record offsets covering everything committed below `from` -- exactly what a
 * pointer table holds (P-9).  Ownership transfers to the index.  The replay then
 * starts at `from` and folds the suffix in through zsi_index_insert, which
 * already maintains the delta and its bounded merge (P-12).
 *
 * With base NULL and from ZSI_HEADER_LEN this is the original full build: it
 * reflects COMMITTED spans only, and for each key only its newest committed
 * record (D-13a).  Building it means replaying spans and skipping rolled-back
 * ones -- never simply walking every record, which would resurrect aborted
 * writes.  That is why this goes through zsi_unordered_replay rather than
 * scanning the file directly, and why test_index_committed_only exists.
 *
 * Sets f->complete as a side effect, since the replay establishes it. */
static int zsi_index_build_from(struct zsi_file *f, zs_compar *compar,
                                bool nocsum, size_t *base, size_t nbase,
                                size_t from)
{
    struct zsi_index_build b;
    struct zsi_index *ix;
    int r;

    zsi_index_free(&f->index);

    ix = zsi_zmalloc(sizeof(*ix));
    if (!ix) { free(base); return ZS_INTERNAL; }
    ix->file = f;

    memset(&b, 0, sizeof(b));
    r = zsi_unordered_replay(f, from, nocsum, zsi_index_build_cb, &b);
    if (r != ZS_OK || b.oom) {
        free(b.offs);
        free(base);
        free(ix);
        return b.oom ? ZS_INTERNAL : r;
    }

    if (base) {
        /* Seeded: the base is already in key order, so the suffix goes in one
         * record at a time.  That is the same path a writer uses at commit
         * (D-13b), so there is one insertion path rather than two. */
        ix->base = base;
        ix->nbase = nbase;
        f->index = ix;

        for (size_t i = 0; i < b.n; i++) {
            r = zsi_index_insert(ix, compar, b.offs[i]);
            if (r != ZS_OK) { free(b.offs); zsi_index_free(&f->index); return r; }
        }

        free(b.offs);
        return ZS_OK;
    }

    if (b.n) {
        struct zsi_ksort ks = { f, compar };
        size_t *tmp = malloc(b.n * sizeof(*tmp));
        if (!tmp) { free(b.offs); free(ix); return ZS_INTERNAL; }

        zsi_msort(b.offs, b.n, tmp, &ks);
        free(tmp);

        /* Keep the first of each equal-key run, which the offset-descending
         * tie-break makes the newest (D-14). */
        size_t w = 0;
        for (size_t i = 0; i < b.n; i++) {
            if (w) {
                const char *ka, *kb;
                size_t la, lb;
                zsi_index_key_at(f, b.offs[w - 1], &ka, &la);
                zsi_index_key_at(f, b.offs[i], &kb, &lb);
                if (compar(ka, la, kb, lb) == 0) continue;
            }
            b.offs[w++] = b.offs[i];
        }
        b.n = w;
    }

    ix->base = b.offs;
    ix->nbase = b.n;
    f->index = ix;
    return ZS_OK;
}

static int zsi_index_build(struct zsi_file *f, zs_compar *compar, bool nocsum)
{
    f->cached_upto = ZSI_HEADER_LEN;
    return zsi_index_build_from(f, compar, nocsum, NULL, 0, ZSI_HEADER_LEN);
}
```

`zsi_index_insert` is defined *below* `zsi_index_build` today, so add a forward
declaration above `zsi_index_build_from`:

```c
static int zsi_index_insert(struct zsi_index *ix, zs_compar *compar, size_t off);
```

Then the flatten helper, placed after `zsi_index_insert`:

```c
/* The merged base+delta ordering as one freshly allocated array (P-9).
 *
 * Does NOT mutate the index.  Merging the delta in place would be cheaper and
 * would compact the index as a side effect, and is deliberately not done: an
 * index may be shared with a live cursor holding base and delta positions
 * (struct zsi_index_cur), and rewriting the arrays underneath it is exactly the
 * in-place mutation of something a reader is reading that G-6 forbids. */
static int zsi_index_flatten(struct zsi_index *ix, zs_compar *compar,
                             size_t **out, size_t *nout)
{
    size_t total;

    if (!zsi_add_sz(ix->nbase, ix->ndelta, &total)) return ZS_INTERNAL;

    size_t *merged = malloc(total ? total * sizeof(*merged) : 1);
    if (!merged) return ZS_INTERNAL;

    size_t bi = 0, di = 0, w = 0;
    while (bi < ix->nbase || di < ix->ndelta) {
        if (bi >= ix->nbase) { merged[w++] = ix->delta[di++]; continue; }
        if (di >= ix->ndelta) { merged[w++] = ix->base[bi++]; continue; }

        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[bi], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[di], &kd, &ld);
        int cmp = compar(kd, ld, kb, lb);

        /* Delta wins ties: it is the newer record (D-14). */
        if (cmp == 0)      { merged[w++] = ix->delta[di++]; bi++; }
        else if (cmp < 0)  { merged[w++] = ix->delta[di++]; }
        else               { merged[w++] = ix->base[bi++]; }
    }

    *out = merged;
    *nout = w;
    return ZS_OK;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `make zstest && ./zstest idxcache`
Expected: `test_idxcache_replays_the_suffix` ok, `test_idxcache_matches_full_build` ok.

- [ ] **Step 6: Run the whole suite**

Run: `make check`
Expected: all tests pass. This step is the real gate for this task — `zsi_unordered_replay`'s signature change touches conversion, consistency checking and dump.

- [ ] **Step 7: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
refactor: replay from an offset, and a seeded index build

zsi_unordered_replay gains a start offset, and zsi_index_build_from builds
an index seeded with an already-sorted base covering everything below it.
A span is self-delimiting and self-validating, so a walk may begin at any
span boundary (P-12); a non-boundary or out-of-range value simply finds no
valid span, which is F-24's ordinary outcome.

zsi_index_flatten returns the merged base+delta without mutating the index,
because an index may be shared with a live cursor holding positions into
both arrays.

No behaviour change: every existing caller passes ZSI_HEADER_LEN.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Table encode and decode

**Files:**
- Modify: `zeroskip.c` — new `POINTER TABLE CACHE` section between `PRIVATE INDEX` and `PER-FILE CURSOR`
- Test: `zstest.c`

**Interfaces:**
- Consumes: `zsi_index_flatten` from Task 3.
- Produces:
  - `#define ZSI_IDX_HEADER_LEN 96`, `ZSI_IDX_MAGIC_LEN 16`, `zsi_idx_magic[16]`, `ZSI_IDX_FLAG_CSUM_VERIFIED 0x0010`, and the `ZSI_IDX_OFF_*` field offsets.
  - `struct zsi_idxhdr { uint8_t version_read, version_write; uint16_t flags; zsi_uuid_t uuid; uint32_t start; char compar_name[ZSI_COMPAR_NAME_LEN]; uint64_t valid_upto, term_off, nptrs; uint32_t term_csum; };`
  - `static void zsi_idxhdr_encode(char *buf, const struct zsi_idxhdr *h, zs_csum *csum);`
  - `static int zsi_idxhdr_decode(const char *buf, size_t len, zs_csum *csum, struct zsi_idxhdr *out);`
  - `static unsigned zsi_idxhdr_engine_id(const char *buf);`

- [ ] **Step 1: Write the failing test**

The byte-layout test asserts against a literal, exactly as `test_header_byte_layout` does. This is not optional ceremony: mutation testing already found that several header tests passed under a *symmetric* layout change, which is precisely the bug that makes another implementation unable to read our files. Compute the two checksum literals in Step 4 and paste them in; write `0x00, 0x00, 0x00, 0x00` as a placeholder for now so the test fails loudly.

```c
/* P-5, P-6: the 96 bytes against a literal.  A matched encoder and decoder
 * round-trip perfectly under a symmetric layout change -- swap two fields in
 * both and nothing notices -- which is the exact bug class that leaves a peer
 * unable to read our tables.  Only a literal catches it. */
static void test_idxcache_header_byte_layout(void)
{
    static const unsigned char golden[ZSI_IDX_HEADER_LEN] = {
        /* 0  magic, all 16 bytes */
        0x89, 0x7A, 0x73, 0x69, 0x6E, 0x64, 0x65, 0x78,
        0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
        /* 16 vread, 17 vwrite, 18 flags (LE) = 0x0011, 20 reserved */
        0x01, 0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 24 uuid */
        0x49, 0x41, 0xDA, 0x54, 0x94, 0x06, 0x4F, 0xAA,
        0xA4, 0x57, 0xC4, 0xB6, 0x5B, 0xEA, 0xE3, 0xEB,
        /* 40 start = 0x01020304 LE, 44 reserved */
        0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* 48 comparator name, NUL-padded to 16 */
        0x6D, 0x65, 0x6D, 0x63, 0x6D, 0x70, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 64 valid_upto = 0x1122334455667788 LE */
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        /* 72 term_off = 0x00000000000000C8 LE */
        0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 80 nptrs = 0x0000000000000007 LE */
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 88 term_csum = 0xDEADBEEF LE, 92 checksum of [0, 92) */
        0xEF, 0xBE, 0xAD, 0xDE, 0x00, 0x00, 0x00, 0x00
    };

    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    struct zsi_idxhdr h;
    char buf[ZSI_IDX_HEADER_LEN];

    ASSERT_EQ(ZSI_IDX_HEADER_LEN, 96);
    ASSERT_EQ(ZSI_IDX_MAGIC_LEN, 16);

    memset(&h, 0, sizeof(h));
    h.version_read  = 1;
    h.version_write = 1;
    h.flags         = ZSI_CSUM_XXHASH | ZSI_IDX_FLAG_CSUM_VERIFIED;
    memcpy(h.uuid, u, 16);
    h.start      = 0x01020304;
    memcpy(h.compar_name, "memcmp", 6);
    h.valid_upto = 0x1122334455667788ULL;
    h.term_off   = 200;
    h.nptrs      = 7;
    h.term_csum  = 0xDEADBEEF;

    memset(buf, 0xAA, sizeof(buf));
    zsi_idxhdr_encode(buf, &h, zsi_csum_xxhash);

    for (size_t i = 0; i < ZSI_IDX_HEADER_LEN - 4; i++) {
        if ((unsigned char)buf[i] != golden[i]) {
            fprintf(stderr, "\n    FAIL byte %zu: got 0x%02X, expected 0x%02X\n",
                    i, (unsigned char)buf[i], golden[i]);
            current_test_failed = 1;
            return;
        }
    }

    /* Each field at its literal offset, so a failure names the field. */
    ASSERT_MEM_EQ(buf + 0, zsi_idx_magic, 16);
    ASSERT_EQ((unsigned char)buf[16], 1);
    ASSERT_EQ((unsigned char)buf[17], 1);
    ASSERT_EQU(zsi_get16(buf + 18), 0x0011u);
    ASSERT_EQU(zsi_get32(buf + 20), 0u);
    ASSERT_MEM_EQ(buf + 24, u, 16);
    ASSERT_EQU(zsi_get32(buf + 40), 0x01020304u);
    ASSERT_EQU(zsi_get32(buf + 44), 0u);
    ASSERT_MEM_EQ(buf + 48, "memcmp", 6);
    ASSERT_EQU(zsi_get64(buf + 64), 0x1122334455667788ULL);
    ASSERT_EQU(zsi_get64(buf + 72), 200u);
    ASSERT_EQU(zsi_get64(buf + 80), 7u);
    ASSERT_EQU(zsi_get32(buf + 88), 0xDEADBEEFu);
    ASSERT_EQU(zsi_get32(buf + 92), zsi_csum_xxhash(buf, 92));

    /* Round-trip. */
    {
        struct zsi_idxhdr back;
        memset(&back, 0, sizeof(back));
        ASSERT_EQ(zsi_idxhdr_engine_id(buf), ZSI_CSUM_XXHASH);
        ASSERT_OK(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back));
        ASSERT_EQU(back.flags, h.flags);
        ASSERT_MEM_EQ(back.uuid, u, 16);
        ASSERT_EQU(back.start, h.start);
        ASSERT_MEM_EQ(back.compar_name, h.compar_name, ZSI_COMPAR_NAME_LEN);
        ASSERT_EQU(back.valid_upto, h.valid_upto);
        ASSERT_EQU(back.term_off, h.term_off);
        ASSERT_EQU(back.nptrs, h.nptrs);
        ASSERT_EQU(back.term_csum, h.term_csum);
    }

    /* A corrupt byte anywhere before the checksum is rejected. */
    {
        struct zsi_idxhdr back;
        buf[40] ^= 0x01;
        ASSERT_EQ(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back),
                  ZS_BADCHECKSUM);
        buf[40] ^= 0x01;
    }

    /* Wrong magic is rejected before anything else. */
    {
        struct zsi_idxhdr back;
        buf[0] ^= 0x01;
        ASSERT_EQ(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back),
                  ZS_BADFORMAT);
        buf[0] ^= 0x01;
    }

    /* Short buffer is rejected. */
    {
        struct zsi_idxhdr back;
        ASSERT_EQ(zsi_idxhdr_decode(buf, ZSI_IDX_HEADER_LEN - 1,
                                    zsi_csum_xxhash, &back), ZS_BADFORMAT);
    }
}
```

Register it:

```c
    { "test_idxcache_header_byte_layout", test_idxcache_header_byte_layout },
```

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest idxcache_header`
Expected: build failure — `ZSI_IDX_HEADER_LEN` undeclared.

- [ ] **Step 3: Write the section**

Insert immediately before `/********** PER-FILE CURSOR *************/`:

```c
/********** POINTER TABLE CACHE *************/

/* Spec section 8.  An unordered file has no pointer section, so key order for it
 * is derived by replaying its spans -- bounded by rollover_size but paid on
 * every open.  A pointer table is that replay's result, published into a cache
 * directory the CALLER names, so another process can load it and replay only the
 * suffix beyond it.
 *
 * Three properties keep this from weakening anything:
 *
 *   - the cache directory is NOT the database (P-2), so R-3 still holds: a
 *     reader writes nothing the database's correctness depends on, and a
 *     read-only handle may publish;
 *   - a table is published by rename and never modified (P-4), so G-6's "nothing
 *     a reader may be reading is ever rewritten beneath it" is unchanged;
 *   - a table is self-validating and every failure means "ignore it and replay"
 *     (P-11).  Nothing here can turn a readable database into an unreadable one,
 *     which is why a rejected table is never an error and never reported as
 *     corruption.
 *
 * This section may call upwards into PRIVATE INDEX and FILE OBJECT.  SNAPSHOT
 * and WRITE PATH call into it. */

/* The same construction as the data-file magic (F-6) and deliberately different
 * bytes, so a table and a data file are distinguishable by content as well as by
 * name: high bit set so no text file is mistaken for one and an eighth-bit-
 * stripping transfer is detected, invalid UTF-8 at byte 0, a CR-LF trap, a DOS
 * end-of-file, a bare LF, NUL padding.  All 16 bytes are validated (P-6). */
#define ZSI_IDX_MAGIC_LEN 16

static const unsigned char zsi_idx_magic[ZSI_IDX_MAGIC_LEN] = {
    0x89, 0x7A, 0x73, 0x69, 0x6E, 0x64, 0x65, 0x78,
    0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00
};

/* Field offsets within the 96-byte header (P-5).  Spelled out rather than
 * derived by summing sizes, so a table in the spec maps to a table here. */
#define ZSI_IDX_HEADER_LEN        96
#define ZSI_IDX_OFF_MAGIC          0   /* 16 */
#define ZSI_IDX_OFF_VREAD         16   /*  1 */
#define ZSI_IDX_OFF_VWRITE        17   /*  1 */
#define ZSI_IDX_OFF_FLAGS         18   /*  2 */
#define ZSI_IDX_OFF_RESERVED1     20   /*  4 */
#define ZSI_IDX_OFF_UUID          24   /* 16 */
#define ZSI_IDX_OFF_START         40   /*  4 */
#define ZSI_IDX_OFF_RESERVED2     44   /*  4 */
#define ZSI_IDX_OFF_COMPAR        48   /* 16 */
#define ZSI_IDX_OFF_VALID_UPTO    64   /*  8 */
#define ZSI_IDX_OFF_TERM_OFF      72   /*  8 */
#define ZSI_IDX_OFF_NPTRS         80   /*  8 */
#define ZSI_IDX_OFF_TERM_CSUM     88   /*  4 */
#define ZSI_IDX_OFF_CSUM          92   /*  4, covers [0, 92) */

#define ZSI_IDX_VERSION_READ  1
#define ZSI_IDX_VERSION_WRITE 1

/* Bit 4 of flags: the index was built with checksum verification.  A table built
 * under ZS_NOCSUM may contain records a verifying reader would reject, so it
 * must not be handed to one (P-11).  The low 4 bits are the engine, exactly as
 * in a data file header. */
#define ZSI_IDX_FLAG_CSUM_VERIFIED 0x0010

struct zsi_idxhdr {
    uint8_t     version_read;
    uint8_t     version_write;
    uint16_t    flags;
    zsi_uuid_t  uuid;
    uint32_t    start;
    char        compar_name[ZSI_COMPAR_NAME_LEN];
    uint64_t    valid_upto;
    uint64_t    term_off;
    uint64_t    nptrs;
    uint32_t    term_csum;
};

static void zsi_idxhdr_encode(char *buf, const struct zsi_idxhdr *h,
                              zs_csum *csum)
{
    memset(buf, 0, ZSI_IDX_HEADER_LEN);

    memcpy(buf + ZSI_IDX_OFF_MAGIC, zsi_idx_magic, ZSI_IDX_MAGIC_LEN);
    buf[ZSI_IDX_OFF_VREAD]  = (char)h->version_read;
    buf[ZSI_IDX_OFF_VWRITE] = (char)h->version_write;
    zsi_put16(buf + ZSI_IDX_OFF_FLAGS, h->flags);
    /* RESERVED1 and RESERVED2 stay zero: written as zero, ignored on read.  The
     * memset above is what writes them. */
    memcpy(buf + ZSI_IDX_OFF_UUID, h->uuid, 16);
    zsi_put32(buf + ZSI_IDX_OFF_START, h->start);
    memcpy(buf + ZSI_IDX_OFF_COMPAR, h->compar_name, ZSI_COMPAR_NAME_LEN);
    zsi_put64(buf + ZSI_IDX_OFF_VALID_UPTO, h->valid_upto);
    zsi_put64(buf + ZSI_IDX_OFF_TERM_OFF, h->term_off);
    zsi_put64(buf + ZSI_IDX_OFF_NPTRS, h->nptrs);
    zsi_put32(buf + ZSI_IDX_OFF_TERM_CSUM, h->term_csum);

    /* The checksum is the last 4 bytes and covers everything before it, exactly
     * as F-4 does for a data file header.  No field-zeroing anywhere. */
    zsi_put32(buf + ZSI_IDX_OFF_CSUM, csum(buf, ZSI_IDX_OFF_CSUM));
}

/* Decode and validate the header alone.  The cross-checks against the data file
 * (uuid, generation, comparator, offsets, terminator binding) belong to the
 * loader, because they need the file. */
static int zsi_idxhdr_decode(const char *buf, size_t len, zs_csum *csum,
                             struct zsi_idxhdr *out)
{
    if (len < ZSI_IDX_HEADER_LEN) return ZS_BADFORMAT;

    if (memcmp(buf + ZSI_IDX_OFF_MAGIC, zsi_idx_magic, ZSI_IDX_MAGIC_LEN) != 0)
        return ZS_BADFORMAT;

    if (zsi_get32(buf + ZSI_IDX_OFF_CSUM) != csum(buf, ZSI_IDX_OFF_CSUM))
        return ZS_BADCHECKSUM;

    uint8_t vread = (uint8_t)buf[ZSI_IDX_OFF_VREAD];
    if (vread > ZSI_IDX_VERSION_READ) return ZS_BADFORMAT;

    out->version_read  = vread;
    out->version_write = (uint8_t)buf[ZSI_IDX_OFF_VWRITE];
    out->flags         = zsi_get16(buf + ZSI_IDX_OFF_FLAGS);
    memcpy(out->uuid, buf + ZSI_IDX_OFF_UUID, 16);
    out->start         = zsi_get32(buf + ZSI_IDX_OFF_START);
    memcpy(out->compar_name, buf + ZSI_IDX_OFF_COMPAR, ZSI_COMPAR_NAME_LEN);
    out->valid_upto    = zsi_get64(buf + ZSI_IDX_OFF_VALID_UPTO);
    out->term_off      = zsi_get64(buf + ZSI_IDX_OFF_TERM_OFF);
    out->nptrs         = zsi_get64(buf + ZSI_IDX_OFF_NPTRS);
    out->term_csum     = zsi_get32(buf + ZSI_IDX_OFF_TERM_CSUM);

    /* F-9 again: generations start at 1. */
    if (out->start == 0) return ZS_BADFORMAT;

    return ZS_OK;
}

/* The engine id, read as plain data before any verification, exactly as
 * zsi_header_engine_id does and for the same reason (F-5a): the checksum cannot
 * be verified until the engine is known, and the engine is recorded inside the
 * header the checksum protects.  A wrong value yields a failed checksum, not a
 * wrong interpretation.
 *
 * Requires len >= ZSI_IDX_HEADER_LEN; the caller checks that first. */
static unsigned zsi_idxhdr_engine_id(const char *buf)
{
    return (unsigned)(zsi_get16(buf + ZSI_IDX_OFF_FLAGS) & ZSI_CSUM_MASK);
}
```

Confirm `zsi_put64` and `zsi_get64` exist in `LIBRARY SUPPORT` with
`grep -n 'zsi_put64\|zsi_get64' zeroskip.c`. If they do not, add them beside
`zsi_put32`/`zsi_get32`, following the same `memcpy`-free byte-at-a-time style
the existing accessors use.

- [ ] **Step 4: Fill in the golden checksum**

Run: `make zstest && ./zstest idxcache_header`
Expected: FAIL at byte 92 or at the final `ASSERT_EQU`, reporting the actual
checksum. Paste those four bytes (little-endian) into `golden[92..95]`, and
change the loop bound from `ZSI_IDX_HEADER_LEN - 4` to `ZSI_IDX_HEADER_LEN` so
the checksum bytes are covered by the literal too.

- [ ] **Step 5: Run to verify it passes**

Run: `make zstest && ./zstest idxcache_header`
Expected: `ok`.

- [ ] **Step 6: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
feat: pointer table header encode and decode

The 96-byte header of P-5, with its own 16 magic bytes (P-6) built on the
same principles as the data-file magic and deliberately different, so the
two artefacts are distinguishable by content as well as by name.

test_idxcache_header_byte_layout asserts all 96 bytes against a literal.
A matched encoder and decoder round-trip perfectly under a symmetric layout
change, which is the exact bug that leaves a peer unable to read our
tables; mutation testing found that class once already.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Load a table

**Files:**
- Modify: `zeroskip.c` — `POINTER TABLE CACHE`
- Test: `zstest.c`

**Interfaces:**
- Consumes: Task 4's header codec; Task 3's `zsi_index_build_from`.
- Produces:
  - `struct zsi_idxcfg { const char *dir; size_t threshold; };`
  - `static void zsi_idx_name(char *out, size_t outlen, const zsi_uuid_t uuid, uint32_t gen);` — writes `zeroskip.index-<uuid>-<GEN8hex>`.
  - `static int zsi_idx_load(struct zsi_file *f, const struct zsi_idxcfg *cfg, zs_compar *compar, const char *compar_name, bool nocsum, size_t **base, size_t *nbase, size_t *valid_upto, size_t *term_off, uint32_t *term_csum);` — `ZS_OK` on acceptance with an owned array; `ZS_NOTFOUND` for every rejection, including a missing file.
  - `static int zsi_index_build_cached(struct zsi_file *f, zs_compar *compar, const char *compar_name, bool nocsum, const struct zsi_idxcfg *cfg);`

- [ ] **Step 1: Write the failing test**

```c
/* Build a database with `n` records, close it, and return the active file's
 * generation.  Used by the load tests, which then hand-build or corrupt a table
 * and check what the loader does with it. */
static uint32_t idxcache_make_db(const char *cachedir, int n)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    uint32_t gen = 0;
    char key[32];

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;      /* publish at every opportunity */

    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return 0;

    for (int i = 0; i < n; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        if (zs_db_store(db, key, strlen(key), "value", 5, 0) != ZS_OK) break;
    }

    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        if (act) gen = act->hdr.start;
    }

    zs_db_close(&db);
    return gen;
}

/* P-11: a table that passes every rule is used, and the resulting index agrees
 * with one built by full replay. */
static void test_idxcache_rejection_rules(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f = NULL;
    uint32_t gen;
    char *tab = NULL;
    size_t tablen = 0;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    gen = idxcache_make_db(cachedir, 30);
    ASSERT(gen != 0);

    /* A table must now exist for that generation (P-3). */
    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);

    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    ASSERT(tablen > ZSI_IDX_HEADER_LEN);

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* Baseline: the pristine table loads. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        struct zsi_idxcfg cfg = { cachedir, 1 };
        ASSERT_OK(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                               &base, &nbase, &vu, &to, &tc));
        ASSERT(nbase > 0);
        free(base);
    }

    /* Each rule, one at a time.  Every one must yield ZS_NOTFOUND -- "ignore it
     * and replay" -- and never an error, because a bad table must not be able to
     * turn a readable database into an unreadable one. */
    {
        struct zsi_idxcfg cfg = { cachedir, 1 };
        struct { const char *what; size_t off; unsigned char xor; } cases[] = {
            { "magic",        ZSI_IDX_OFF_MAGIC,      0x01 },
            { "header csum",  ZSI_IDX_OFF_CSUM,       0x01 },
            { "uuid",         ZSI_IDX_OFF_UUID,       0x01 },
            { "generation",   ZSI_IDX_OFF_START,      0x01 },
            { "comparator",   ZSI_IDX_OFF_COMPAR,     0x01 },
            { "valid_upto",   ZSI_IDX_OFF_VALID_UPTO, 0x01 },
            { "term_off",     ZSI_IDX_OFF_TERM_OFF,   0x01 },
            { "nptrs",        ZSI_IDX_OFF_NPTRS,      0x01 },
            { "term_csum",    ZSI_IDX_OFF_TERM_CSUM,  0x01 },
            { "first offset", ZSI_IDX_HEADER_LEN,     0x80 },
            { "array csum",   0,                      0x00 }   /* handled below */
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            size_t *base = NULL, nbase = 0, vu = 0, to = 0;
            uint32_t tc = 0;
            size_t off = cases[i].off;
            unsigned char x = cases[i].xor;

            if (x == 0x00) off = tablen - 4, x = 0x01;   /* the array checksum */

            tab[off] = (char)((unsigned char)tab[off] ^ x);

            /* Fields covered by the header checksum need it recomputed, or the
             * test would only ever exercise the checksum rule.  Rebuild it
             * unless this case IS the checksum. */
            if (off < ZSI_IDX_OFF_CSUM)
                zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                          zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));

            ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
            ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                                   &base, &nbase, &vu, &to, &tc),
                      ZS_NOTFOUND);
            ASSERT_NULL(base);

            tab[off] = (char)((unsigned char)tab[off] ^ x);
            if (off < ZSI_IDX_OFF_CSUM)
                zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                          zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
            ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        }
    }

    /* Truncated file: the size must be exactly 96 + 8n + 4 (P-11). */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        struct zsi_idxcfg cfg = { cachedir, 1 };
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen - 1));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    }

    /* A table built without checksum verification must not be handed to a
     * verifying reader, but IS acceptable to a non-verifying one. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        struct zsi_idxcfg cfg = { cachedir, 1 };
        uint16_t fl = zsi_get16(tab + ZSI_IDX_OFF_FLAGS);

        zsi_put16(tab + ZSI_IDX_OFF_FLAGS,
                  (uint16_t)(fl & ~ZSI_IDX_FLAG_CSUM_VERIFIED));
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));

        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        ASSERT_OK(zsi_idx_load(f, &cfg, db->compar, db->compar_name, true,
                               &base, &nbase, &vu, &to, &tc));
        free(base);
    }

    /* A missing table is ZS_NOTFOUND, not an error. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        struct zsi_idxcfg cfg = { cachedir, 1 };
        ASSERT_EQ(unlink(tabpath), 0);
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);
    }

    free(tab);
    ASSERT_OK(zs_db_close(&db));
}

/* P-10, P-17: the terminator binding.  A table whose recorded terminator does
 * not match the file it is being applied to is rejected, which is what catches a
 * database directory restored from backup under a surviving cache. */
static void test_idxcache_rejects_bad_term_binding(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    struct zsi_idxcfg cfg;
    uint32_t gen;
    char *tab;
    size_t tablen;
    size_t *base = NULL, nbase = 0, vu = 0, to = 0;
    uint32_t tc = 0;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);
    cfg.dir = cachedir;
    cfg.threshold = 1;

    gen = idxcache_make_db(cachedir, 30);
    ASSERT(gen != 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);
    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* term_csum that does not match the terminator actually at term_off. */
    zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM,
              zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM) ^ 0xFFFFFFFFu);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));

    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);

    /* term_off that is not where a terminator ending at valid_upto lives. */
    zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM,
              zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM) ^ 0xFFFFFFFFu);
    zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF,
              zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF) + 1);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));

    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar, db->compar_name, false,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);

    free(tab);
    ASSERT_OK(zs_db_close(&db));
}
```

Two file helpers are needed; put them beside the other test helpers:

```c
static char *idxcache_slurp(const char *path, size_t *lenp)
{
    struct stat sb;
    char *buf;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (fstat(fd, &sb) < 0) { close(fd); return NULL; }
    buf = malloc((size_t)sb.st_size ? (size_t)sb.st_size : 1);
    if (!buf) { close(fd); return NULL; }
    if (read(fd, buf, (size_t)sb.st_size) != (ssize_t)sb.st_size) {
        free(buf); close(fd); return NULL;
    }
    close(fd);
    *lenp = (size_t)sb.st_size;
    return buf;
}

static int idxcache_spew(const char *path, const char *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return ZS_IOERROR;
    if (write(fd, buf, len) != (ssize_t)len) { close(fd); return ZS_IOERROR; }
    close(fd);
    return ZS_OK;
}
```

Register:

```c
    { "test_idxcache_rejection_rules",  test_idxcache_rejection_rules },
    { "test_idxcache_rejects_bad_term_binding",
                                        test_idxcache_rejects_bad_term_binding },
```

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest idxcache`
Expected: build failure — `zsi_idx_load`, `zsi_name_format_index`, `struct zsi_idxcfg` undeclared.

- [ ] **Step 3: Implement the name formatter and the loader**

Append to the `POINTER TABLE CACHE` section:

```c
/* P-3.  The `zeroskip.` prefix is the metadata namespace (D-2), so a table can
 * never be parsed as a data file even if someone points the cache directory at a
 * database.  Uuid and generation forms are D-0's and D-1's, so the name is
 * readable next to the file it describes. */
#define ZSI_IDX_NAME_PREFIX "zeroskip.index-"

static void zsi_name_format_index(char *out, const zsi_uuid_t uuid,
                                  uint32_t gen)
{
    char ustr[ZSI_UUID_STR_LEN];

    zsi_uuid_format(ustr, uuid);
    snprintf(out, ZSI_NAME_MAX, "%s%s-%08X", ZSI_IDX_NAME_PREFIX, ustr, gen);
}

struct zsi_idxcfg {
    const char *dir;        /* NULL disables the cache entirely */
    size_t      threshold;  /* P-13 */
};

/* Load and fully validate a pointer table for this file (P-11).
 *
 * EVERY failure is ZS_NOTFOUND, including a missing file, a short read and a
 * rule violation.  That uniformity is deliberate: a table is an optimisation,
 * and the only correct response to any doubt about one is to ignore it and
 * replay.  Returning an error instead would let a corrupt file in a directory
 * the database does not depend on turn a readable database into an unreadable
 * one, which is precisely what P-11 forbids.
 *
 * On ZS_OK, *base is an owned array of *nbase offsets in key order, and the
 * caller takes ownership. */
static int zsi_idx_load(struct zsi_file *f, const struct zsi_idxcfg *cfg,
                        zs_compar *compar, const char *compar_name, bool nocsum,
                        size_t **base, size_t *nbase, size_t *valid_upto,
                        size_t *term_off, uint32_t *term_csum)
{
    char name[ZSI_NAME_MAX], path[PATH_MAX];
    struct zsi_idxhdr h;
    struct stat sb;
    char *buf = NULL;
    size_t len, want;
    size_t *offs = NULL;
    int fd = -1, rc = ZS_NOTFOUND;

    (void)compar;   /* the order is trusted; see the note below */

    *base = NULL; *nbase = 0;
    *valid_upto = ZSI_HEADER_LEN;
    *term_off = ZSI_HEADER_LEN;
    *term_csum = 0;

    if (!cfg || !cfg->dir) return ZS_NOTFOUND;
    if (!f->hdr_valid) return ZS_NOTFOUND;          /* D-10: nothing to index */

    zsi_name_format_index(name, f->hdr.uuid, f->hdr.start);
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, name)
        >= sizeof(path))
        return ZS_NOTFOUND;

    fd = open(path, O_RDONLY);
    if (fd < 0) return ZS_NOTFOUND;
    if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode)) goto out;

    len = (size_t)sb.st_size;
    if (len < ZSI_IDX_HEADER_LEN + 4) goto out;
    if (len > ZSI_IDX_MAX_BYTES) goto out;

    buf = malloc(len);
    if (!buf) goto out;
    if (read(fd, buf, len) != (ssize_t)len) goto out;

    /* The engine is the DATA FILE's, not ours (P-7).  Reading the table's own
     * flags first would let a table claim an engine the file does not use and
     * still validate, which is the interoperability hole F-5a closes for data
     * files.  So: resolve from the file, then require the table to agree. */
    if (!f->csum) goto out;
    if (zsi_idxhdr_engine_id(buf) != f->csum_id) goto out;
    if (zsi_idxhdr_decode(buf, len, f->csum, &h) != ZS_OK) goto out;

    if (memcmp(h.uuid, f->hdr.uuid, 16) != 0) goto out;
    if (h.start != f->hdr.start) goto out;
    if (memcmp(h.compar_name, f->hdr.compar_name, ZSI_COMPAR_NAME_LEN) != 0)
        goto out;

    /* Our own comparator too: a table is only meaningful under the order it was
     * sorted in, and a handle may legitimately open a file whose recorded name
     * it does not implement. */
    {
        char mine[ZSI_COMPAR_NAME_LEN];
        memset(mine, 0, sizeof(mine));
        if (compar_name) {
            size_t n = strlen(compar_name);
            memcpy(mine, compar_name, n < sizeof(mine) ? n : sizeof(mine));
        }
        if (memcmp(h.compar_name, mine, ZSI_COMPAR_NAME_LEN) != 0) goto out;
    }

    /* An index built without verification may hold records a verifying reader
     * would reject, so it must not be handed to one.  The converse is fine. */
    if (!nocsum && !(h.flags & ZSI_IDX_FLAG_CSUM_VERIFIED)) goto out;

    /* Exact size, so a truncated or padded table is rejected rather than
     * silently read short (P-11). */
    if (h.nptrs > (ZSI_IDX_MAX_BYTES - ZSI_IDX_HEADER_LEN - 4) / 8) goto out;
    if (!zsi_mul_sz((size_t)h.nptrs, sizeof(uint64_t), &want)) goto out;
    if (!zsi_add_sz(want, ZSI_IDX_HEADER_LEN + 4, &want)) goto out;
    if (want != len) goto out;

    if (zsi_get32(buf + len - 4)
        != f->csum(buf + ZSI_IDX_HEADER_LEN, len - ZSI_IDX_HEADER_LEN - 4))
        goto out;

    if (h.valid_upto < ZSI_HEADER_LEN || h.valid_upto > f->size) goto out;

    /* P-10's binding.  O(1): the table names the terminator's offset, because
     * terminators are located by walking spans FORWARD and a reader given only
     * valid_upto could not find it without the replay this exists to avoid. */
    if (h.valid_upto == ZSI_HEADER_LEN) {
        if (h.term_off != ZSI_HEADER_LEN || h.term_csum != 0 || h.nptrs != 0)
            goto out;
    } else {
        struct zsi_term term;
        const char *tb;
        size_t after;

        if (h.term_off < ZSI_HEADER_LEN || h.term_off >= h.valid_upto) goto out;
        tb = zsi_file_at(f, (size_t)h.term_off, 1);
        if (!tb) goto out;
        if (zsi_term_decode(tb, f->size - (size_t)h.term_off, &term) != ZS_OK)
            goto out;
        if (!zsi_add_sz((size_t)h.term_off, term.len, &after)) goto out;
        if (after != (size_t)h.valid_upto) goto out;
        if (term.csum != h.term_csum) goto out;
    }

    offs = malloc(h.nptrs ? (size_t)h.nptrs * sizeof(*offs) : 1);
    if (!offs) goto out;

    for (uint64_t i = 0; i < h.nptrs; i++) {
        uint64_t v = zsi_get64(buf + ZSI_IDX_HEADER_LEN + i * 8);
        /* Every offset must address a record inside the covered prefix.  This
         * is O(n) over the array but touches no data-file page, so it costs
         * nothing the cache is trying to save.  It does NOT verify that the
         * offsets are in key order: doing so needs a decode per entry, which is
         * the work being avoided, and the array checksum already stands behind
         * the ordering. */
        if (v < ZSI_HEADER_LEN || v >= h.valid_upto) { free(offs); offs = NULL; goto out; }
        if (v > (uint64_t)SIZE_MAX) { free(offs); offs = NULL; goto out; }
        offs[i] = (size_t)v;
    }

    *base = offs;
    *nbase = (size_t)h.nptrs;
    *valid_upto = (size_t)h.valid_upto;
    *term_off = (size_t)h.term_off;
    *term_csum = h.term_csum;
    offs = NULL;
    rc = ZS_OK;

out:
    free(offs);
    free(buf);
    if (fd >= 0) close(fd);
    return rc;
}

/* Build the private index, seeded from a table if one is usable (P-12). */
static int zsi_index_build_cached(struct zsi_file *f, zs_compar *compar,
                                  const char *compar_name, bool nocsum,
                                  const struct zsi_idxcfg *cfg)
{
    size_t *base = NULL, nbase = 0, vu = ZSI_HEADER_LEN, to = ZSI_HEADER_LEN;
    uint32_t tc = 0;
    int r;

    if (zsi_idx_load(f, cfg, compar, compar_name, nocsum,
                     &base, &nbase, &vu, &to, &tc) != ZS_OK) {
        f->cached_upto = ZSI_HEADER_LEN;
        return zsi_index_build_from(f, compar, nocsum, NULL, 0,
                                    ZSI_HEADER_LEN);
    }

    f->cached_upto = vu;
    r = zsi_index_build_from(f, compar, nocsum, base, nbase, vu);
    if (r != ZS_OK) return r;

    /* A replay that found no new span leaves last_term_* at their starting
     * values, so seed them from the table.  Without this, publishing after a
     * no-op catch-up would record a terminator that is not the one at the
     * complete point. */
    if (f->complete == vu) {
        f->last_term_off  = to;
        f->last_term_csum = tc;
    }

    return ZS_OK;
}
```

Add near the top of the section:

```c
/* A sanity ceiling on a table's size.  rollover_size is caller-configurable and
 * a crash can leave a larger file behind, so this is generous rather than tight:
 * its only job is to stop a corrupt nptrs from turning into a huge allocation
 * before the exact-size check runs. */
#define ZSI_IDX_MAX_BYTES ((size_t)1 << 31)
```

Check that `zsi_uuid_format`, `ZSI_NAME_MAX`, `zsi_mul_sz` and `zsi_term_decode`
exist with the names used here (`grep -n 'zsi_uuid_format\|ZSI_NAME_MAX\|zsi_mul_sz\|struct zsi_term' zeroskip.c`) and adjust if the spellings differ.
`struct zs_db` must expose `compar_name`; if it stores it only inside a header
template, read it from there.

- [ ] **Step 4: Run to verify it passes**

Run: `make zstest && ./zstest idxcache`
Expected: the rejection tests pass. They depend on Task 6's publishing, so if
publishing is not yet wired in, `idxcache_make_db` produces no table and the
tests fail at `ASSERT_NOT_NULL(tab)`. **Do Task 6 before re-running these two.**
Mark them expected-to-fail here and gate the commit on Task 6.

- [ ] **Step 5: Commit**

Defer this commit until Task 6 is done, then commit Tasks 5 and 6 together —
loading and publishing are not independently testable, so splitting the commit
would leave a revision whose tests do not pass.

---

## Task 6: Publish a table

**Files:**
- Modify: `zeroskip.c` — `POINTER TABLE CACHE`, `SNAPSHOT`, `WRITE PATH`
- Test: `zstest.c`

**Interfaces:**
- Consumes: Task 5's `struct zsi_idxcfg`, `zsi_name_format_index`; Task 3's `zsi_index_flatten`.
- Produces:
  - `static int zsi_idx_publish(struct zsi_file *f, const struct zsi_idxcfg *cfg, zs_compar *compar, const char *compar_name, bool nocsum);` — returns `ZS_OK` if a table was written, `ZS_DONE` if the threshold was not reached, an error otherwise. Never fatal to the caller.
  - `struct zs_db` grows `bool idx_publish_warned;`.

- [ ] **Step 1: Write the failing test**

```c
/* P-13: nothing is published below the threshold, and something is at it. */
static void test_idxcache_threshold(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat sb;
    uint32_t gen;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1024 * 1024;    /* far above anything we write */

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "value", 5, 0));
    }
    gen = zsi_snapshot_active(db->snap)->hdr.start;
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);

    ASSERT_EQ(stat(tabpath, &sb), -1);      /* nothing published */
    ASSERT_OK(zs_db_close(&db));

    /* Same data, threshold of one byte: a table appears. */
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "another", 7, "value", 5, 0));
    ASSERT_EQ(stat(tabpath, &sb), 0);
    ASSERT(sb.st_size > ZSI_IDX_HEADER_LEN);
    ASSERT_OK(zs_db_close(&db));
}

/* A cached open and an uncached open must agree on every key (P-9, P-12).
 * This is the test the whole feature exists for. */
static void test_idxcache_open_agrees(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data cached = ZS_OPEN_DATA_INITIALIZER;
    struct zs_open_data plain = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char key[32];

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    cached.flags = ZS_CREATE;
    cached.index_dir = cachedir;
    cached.index_threshold = 1;
    plain.flags = ZS_CREATE;

    /* Write in two halves, closing in between, so the second open loads a
     * table and replays the suffix. */
    ASSERT_OK(zs_db_open(dbdir, &cached, &db));
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "first", 5, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    ASSERT_OK(zs_db_open(dbdir, &cached, &db));
    for (int i = 25; i < 75; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "second", 6, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    /* Read it back both ways and compare every key. */
    for (int pass = 0; pass < 2; pass++) {
        ASSERT_OK(zs_db_open(dbdir, pass ? &plain : &cached, &db));
        for (int i = 0; i < 75; i++) {
            const char *v = NULL;
            size_t vl = 0;
            snprintf(key, sizeof(key), "key%03d", i);
            ASSERT_OK(zs_db_fetch(db, key, strlen(key), NULL, NULL, &v, &vl, 0));
            if (i < 25) { ASSERT_EQU(vl, 5u); ASSERT_MEM_EQ(v, "first", 5); }
            else        { ASSERT_EQU(vl, 6u); ASSERT_MEM_EQ(v, "second", 6); }
        }
        ASSERT_OK(zs_db_close(&db));
    }
}

/* P-4: published by rename, never written in place.  Verified by inode: a
 * republished table must be a different file, because writing in place would
 * expose a half-written table to a concurrent reader. */
static void test_idxcache_publishes_by_rename(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat a, b;
    uint32_t gen;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = zsi_snapshot_active(db->snap)->hdr.start;
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);
    ASSERT_EQ(stat(tabpath, &a), 0);

    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_EQ(stat(tabpath, &b), 0);
    ASSERT(a.st_ino != b.st_ino);

    /* No staging files left behind. */
    {
        DIR *d = opendir(cachedir);
        struct dirent *de;
        int staging = 0;
        ASSERT_NOT_NULL(d);
        while ((de = readdir(d)))
            if (!strncmp(de->d_name, ZSI_STAGING_PREFIX,
                         strlen(ZSI_STAGING_PREFIX)))
                staging++;
        closedir(d);
        ASSERT_EQ(staging, 0);
    }

    ASSERT_OK(zs_db_close(&db));
}

/* P-15: a cache directory that cannot be written to must not fail a commit. */
static void test_idxcache_publish_failure_is_not_fatal(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    const char *v = NULL;
    size_t vl = 0;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));

    /* Take the directory away underneath the handle. */
    {
        DIR *d = opendir(cachedir);
        struct dirent *de;
        char p[PATH_MAX];
        ASSERT_NOT_NULL(d);
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            snprintf(p, sizeof(p), "%s/%s", cachedir, de->d_name);
            unlink(p);
        }
        closedir(d);
        ASSERT_EQ(rmdir(cachedir), 0);
    }

    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_db_close(&db));
}

/* P-1: in-order files never get a table. */
static void test_idxcache_only_unordered_files(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    DIR *d;
    struct dirent *de;
    int tables = 0;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    setup.rollover_size = 512;      /* force rollovers and conversions */

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 60; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "0123456789", 10, 0));
    }
    ASSERT_OK(zs_db_repack(db));

    /* Every remaining table must name a generation that is still an unordered
     * file.  After a repack the only unordered file is the active one. */
    d = opendir(cachedir);
    ASSERT_NOT_NULL(d);
    while ((de = readdir(d)))
        if (!strncmp(de->d_name, ZSI_IDX_NAME_PREFIX,
                     strlen(ZSI_IDX_NAME_PREFIX)))
            tables++;
    closedir(d);

    ASSERT(tables <= 1);
    ASSERT_OK(zs_db_close(&db));
}
```

Register all five. `test_idxcache_only_unordered_files` also covers P-16 in
part; the dedicated sweep test is Task 7.

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest idxcache`
Expected: the new tests fail — no table is ever written.

- [ ] **Step 3: Implement publishing**

Append to `POINTER TABLE CACHE`:

```c
/* Publish a table covering this file's complete point (P-4, P-13).
 *
 * ZS_DONE means the threshold was not reached and nothing was written, which is
 * the common case and not a failure.  Any other non-OK is a real problem with
 * the cache directory, and the CALLER is required to swallow it (P-15): the data
 * is already durable, and failing an operation over a rebuildable cache would be
 * a regression rather than a safety measure.
 *
 * There is deliberately NO fsync (P-14).  A table is rebuildable, a torn or
 * zero-filled file after a crash is rejected by P-11's checksums, and syncing
 * would put a sync on the commit path -- which is the cost this whole mechanism
 * exists to reduce. */
static int zsi_idx_publish(struct zsi_file *f, const struct zsi_idxcfg *cfg,
                           zs_compar *compar, const char *compar_name,
                           bool nocsum)
{
    char name[ZSI_NAME_MAX], path[PATH_MAX], tmp[PATH_MAX];
    unsigned char rnd[4];
    struct zsi_idxhdr h;
    char hdr[ZSI_IDX_HEADER_LEN];
    size_t *offs = NULL, n = 0;
    char *arr = NULL;
    int fd = -1, rc = ZS_INTERNAL;

    if (!cfg || !cfg->dir) return ZS_DONE;
    if (!f->hdr_valid || !f->index) return ZS_DONE;
    if (!zsi_file_is_unordered(f)) return ZS_DONE;          /* P-1 */
    if (!f->csum) return ZS_DONE;

    /* P-13.  cached_upto is the valid_upto of the table this index was seeded
     * from, or the header length if none was.  Publishing below the threshold
     * is what would make a bulk load quadratic: the table is rewritten whole
     * every time, so one publication per commit is O(records) of I/O per
     * commit. */
    if (f->complete < f->cached_upto) return ZS_DONE;
    if (f->complete - f->cached_upto < cfg->threshold) return ZS_DONE;

    if (zsi_index_flatten(f->index, compar, &offs, &n) != ZS_OK)
        return ZS_INTERNAL;

    if (n > (ZSI_IDX_MAX_BYTES - ZSI_IDX_HEADER_LEN - 4) / 8) goto out;

    arr = malloc(n ? n * 8 : 1);
    if (!arr) goto out;
    for (size_t i = 0; i < n; i++)
        zsi_put64(arr + i * 8, (uint64_t)offs[i]);

    memset(&h, 0, sizeof(h));
    h.version_read  = ZSI_IDX_VERSION_READ;
    h.version_write = ZSI_IDX_VERSION_WRITE;
    /* The engine is the FILE's (P-7), never the handle's.  Using the handle's
     * would produce tables a conforming peer must reject -- the same silent
     * failure that using the handle's engine to append to an existing file
     * causes (A-6, F-5a). */
    h.flags = (uint16_t)(f->csum_id & ZSI_CSUM_MASK);
    if (!nocsum) h.flags |= ZSI_IDX_FLAG_CSUM_VERIFIED;
    memcpy(h.uuid, f->hdr.uuid, 16);
    h.start = f->hdr.start;
    memcpy(h.compar_name, f->hdr.compar_name, ZSI_COMPAR_NAME_LEN);
    h.valid_upto = (uint64_t)f->complete;
    h.term_off   = (uint64_t)f->last_term_off;
    h.nptrs      = (uint64_t)n;
    h.term_csum  = f->last_term_csum;
    (void)compar_name;

    zsi_idxhdr_encode(hdr, &h, f->csum);

    if (!zsi_random_bytes(rnd, sizeof(rnd))) {
        uint64_t e = zsi_weak_entropy();
        memcpy(rnd, &e, sizeof(rnd));
    }

    /* Random digits as well as the pid: two processes sharing a cache directory
     * across a network filesystem can have the same pid, and two of them writing
     * one temp name would interleave into a file that is then renamed into
     * place.  P-11 would reject it, but at the cost of both processes' work. */
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s/%s%d.%02x%02x%02x%02x",
                         cfg->dir, ZSI_STAGING_PREFIX, (int)getpid(),
                         rnd[0], rnd[1], rnd[2], rnd[3]) >= sizeof(tmp))
        goto out;

    zsi_name_format_index(name, f->hdr.uuid, f->hdr.start);
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, name)
        >= sizeof(path))
        goto out;

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { rc = ZS_IOERROR; goto out; }

    if (write(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        rc = ZS_IOERROR; goto out_unlink;
    }
    if (n && write(fd, arr, n * 8) != (ssize_t)(n * 8)) {
        rc = ZS_IOERROR; goto out_unlink;
    }
    {
        char csbuf[4];
        zsi_put32(csbuf, f->csum(arr, n * 8));
        if (write(fd, csbuf, 4) != 4) { rc = ZS_IOERROR; goto out_unlink; }
    }

    if (close(fd) < 0) { fd = -1; rc = ZS_IOERROR; goto out_unlink; }
    fd = -1;

    if (rename(tmp, path) < 0) { rc = ZS_IOERROR; goto out_unlink; }

    f->cached_upto = f->complete;
    rc = ZS_OK;
    goto out;

out_unlink:
    if (fd >= 0) { close(fd); fd = -1; }
    unlink(tmp);

out:
    if (fd >= 0) close(fd);
    free(arr);
    free(offs);
    return rc;
}
```

Note the empty-array checksum: `f->csum(arr, 0)` must be the engine's value for
empty input, not zero. `zsi_csum_xxhash` deliberately has no empty short-circuit
(F-26g), so passing `arr` with `n == 0` is correct as written — do not add one.

- [ ] **Step 4: Wire it into the snapshot**

In `zsi_snapshot_take`, replace the `zsi_index_build` call with the cached form
and publish afterwards. The function needs the config and the comparator name, so
add parameters `const struct zsi_idxcfg *idxcfg, const char *compar_name` and
pass them from `zsi_db_refresh`:

```c
            if (zsi_file_is_unordered(f)) {
                /* Step 4.  The replay sets f->complete, which IS this file's
                 * snapshot boundary: growth beyond it is invisible (C-4c). */
                r = zsi_index_build_cached(f, compar, compar_name, nocsum,
                                           idxcfg);
                if (r != ZS_OK) { ... }

                /* P-13.  A reader publishes on exactly the same rule as a
                 * writer: whoever builds a table over an unordered file and has
                 * moved far enough past the last published one writes it out.
                 * Safe from either side, because publication is a rename.
                 * P-15: never fatal. */
                (void)zsi_idx_publish(f, idxcfg, compar, compar_name, nocsum);
            }
```

In `zsi_db_refresh`, build the config from the handle:

```c
    struct zsi_idxcfg idxcfg = { db->index_dir, db->index_threshold };
```

- [ ] **Step 5: Wire it into commit**

In `zsi_txn_commit`, inside the incremental branch at the point the index has
just been extended (after the `zsi_index_insert` loop succeeds):

```c
        if (r == ZS_OK) {
            /* P-13, P-15.  The writer publishes on the same threshold rule as
             * a reader.  A failure here never fails the commit: the records are
             * already durable, and a cache is not something a caller can act
             * on. */
            int pr = zsi_idx_publish(act, &(struct zsi_idxcfg){
                                         db->index_dir, db->index_threshold },
                                     db->compar, db->compar_name, db->nocsum);
            if (pr != ZS_OK && pr != ZS_DONE && !db->idx_publish_warned) {
                db->idx_publish_warned = true;
                db->error("could not publish a pointer table; continuing "
                          "without the index cache",
                          "dir=<%s>", db->index_dir);
            }
        }
```

C99 compound literals are fine here, but if the surrounding style avoids them,
declare a local `struct zsi_idxcfg cfg = { db->index_dir, db->index_threshold };`
at the top of the branch instead. Add `bool idx_publish_warned;` to `struct zs_db`.

- [ ] **Step 6: Run to verify it passes**

Run: `make zstest && ./zstest idxcache`
Expected: every `idxcache` test passes, including Task 5's two.

- [ ] **Step 7: Run the whole suite**

Run: `make check`
Expected: all tests pass.

- [ ] **Step 8: Run the sanitizers**

Run: `make asan`
Expected: pass, no ASan or UBSan reports.

Then, because every target builds a binary called `zstest` and make cannot tell
an instrumented build from a plain one:

Run: `make leaks`
Expected: pass with no leaks. (`make leaks` cleans first; if it reports "malloc
replacement library without the required support", a stale ASan binary is being
run — clean and retry.)

- [ ] **Step 9: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
feat: load and publish pointer tables

zsi_idx_load validates a table against the file it covers and returns
ZS_NOTFOUND for every rejection including a missing file (P-11): a table is
an optimisation, and the only correct response to any doubt about one is to
ignore it and replay. Anything else would let a corrupt file in a directory
the database does not depend on turn a readable database into an unreadable
one.

zsi_idx_publish writes a temp file and renames (P-4), on one threshold rule
that readers and writers share (P-13). No fsync (P-14): the table is
rebuildable, a torn file is rejected by its checksums, and syncing would put
a sync back on the commit path. A publish failure never fails a commit
(P-15).

The table's checksums use the DATA FILE's engine, never the handle's (P-7) --
the same rule that governs appending, for the same reason: a table checksummed
under the wrong engine validates for nobody.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Sweep dead generations

**Files:**
- Modify: `zeroskip.c` — `POINTER TABLE CACHE`, `SNAPSHOT`
- Test: `zstest.c`

**Interfaces:**
- Consumes: Task 6.
- Produces: `static void zsi_idx_sweep(const struct zsi_idxcfg *cfg, const zsi_uuid_t uuid, const uint32_t *live, size_t nlive);`

- [ ] **Step 1: Write the failing test**

```c
/* P-16: a table whose generation is no longer an unordered file is unlinked;
 * one whose generation is still live is kept. */
static void test_idxcache_sweeps_dead_generations(void)
{
    char cachedir[PATH_MAX], p[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat sb;
    uint32_t gen;
    char junk[ZSI_IDX_HEADER_LEN + 4];

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = zsi_snapshot_active(db->snap)->hdr.start;

    /* A table for a generation that has never existed.  Its contents do not
     * matter: the sweep works on names. */
    zsi_name_format_index(name, db->uuid, gen + 100);
    snprintf(p, sizeof(p), "%s/%s", cachedir, name);
    memset(junk, 0, sizeof(junk));
    ASSERT_OK(idxcache_spew(p, junk, sizeof(junk)));

    /* And one for another database's uuid, which must be left alone. */
    {
        zsi_uuid_t other;
        char q[PATH_MAX];
        memcpy(other, db->uuid, 16);
        other[0] = (unsigned char)(other[0] ^ 0xFF);
        zsi_name_format_index(name, other, gen);
        snprintf(q, sizeof(q), "%s/%s", cachedir, name);
        ASSERT_OK(idxcache_spew(q, junk, sizeof(junk)));

        /* A store triggers a refresh, and with it a sweep. */
        ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

        ASSERT_EQ(stat(p, &sb), -1);        /* dead generation gone */
        ASSERT_EQ(stat(q, &sb), 0);         /* other database untouched */
    }

    /* The live generation's own table survives. */
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(p, sizeof(p), "%s/%s", cachedir, name);
    ASSERT_EQ(stat(p, &sb), 0);

    ASSERT_OK(zs_db_close(&db));
}
```

Register it.

- [ ] **Step 2: Run to verify it fails**

Run: `make zstest && ./zstest sweeps`
Expected: FAIL — the dead generation's table is still present.

- [ ] **Step 3: Implement the sweep**

Append to `POINTER TABLE CACHE`:

```c
/* P-16.  Unlink tables for this database whose generation is no longer an
 * unordered file.
 *
 * Takes the live generations as a plain array rather than a struct zsi_fileset,
 * because FILE SET is defined below this section and the layering runs one way.
 *
 * Safe against a concurrent reader: a descriptor already open survives the
 * unlink, and a reader that misses a table replays instead.  Errors are ignored
 * throughout -- a sweep that cannot run costs disk space, not correctness. */
static void zsi_idx_sweep(const struct zsi_idxcfg *cfg, const zsi_uuid_t uuid,
                          const uint32_t *live, size_t nlive)
{
    char want[ZSI_UUID_STR_LEN];
    size_t plen = strlen(ZSI_IDX_NAME_PREFIX);
    struct dirent *de;
    DIR *d;

    if (!cfg || !cfg->dir) return;

    zsi_uuid_format(want, uuid);

    d = opendir(cfg->dir);
    if (!d) return;

    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        uint32_t gen;
        bool alive = false;
        char path[PATH_MAX];

        if (strncmp(nm, ZSI_IDX_NAME_PREFIX, plen) != 0) continue;
        nm += plen;

        /* Another database's tables are not ours to remove. */
        if (strncmp(nm, want, 36) != 0) continue;
        nm += 36;
        if (*nm != '-') continue;
        nm++;

        if (zsi_parse_gen8(nm, &gen) != 8) continue;
        if (nm[8] != '\0') continue;

        for (size_t i = 0; i < nlive; i++)
            if (live[i] == gen) { alive = true; break; }
        if (alive) continue;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, de->d_name)
            < sizeof(path))
            unlink(path);
    }

    closedir(d);
}
```

- [ ] **Step 4: Call it from the snapshot**

At the end of `zsi_snapshot_take`, just before `*out = s; return ZS_OK;`:

```c
        /* P-16.  Done here rather than inside the publish, because this is the
         * one place that knows the whole file set.  Skipped when the cache is
         * off, and every failure inside is ignored: a sweep that cannot run
         * costs disk space, not correctness. */
        if (idxcfg && idxcfg->dir && s->nfiles) {
            uint32_t stackbuf[16];
            uint32_t *live = stackbuf;
            size_t nlive = 0;

            if (s->nfiles > sizeof(stackbuf) / sizeof(stackbuf[0]))
                live = malloc(s->nfiles * sizeof(*live));

            if (live) {
                for (size_t i = 0; i < s->nfiles; i++)
                    if (zsi_file_is_unordered(s->files[i]))
                        live[nlive++] = s->files[i]->hdr.start;

                zsi_idx_sweep(idxcfg, s->files[0]->hdr.uuid, live, nlive);
                if (live != stackbuf) free(live);
            }
        }
```

Use `s->files[0]->hdr.uuid` only when that file's header is valid; otherwise scan
for the first file with `hdr_valid` and skip the sweep if none has one.

- [ ] **Step 5: Run to verify it passes**

Run: `make zstest && ./zstest idxcache`
Expected: all pass.

- [ ] **Step 6: Run the whole suite and the sanitizers**

Run: `make check && make asan`
Expected: both pass.

- [ ] **Step 7: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
feat: sweep pointer tables for dead generations

P-16: whoever takes a snapshot unlinks tables for this database whose
generation is no longer an unordered file. Done at the snapshot rather than
inside the publish, because that is the one place that knows the whole file
set. Another database's tables in a shared cache directory are left alone.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Remaining requirement tests

**Files:**
- Modify: `zstest.c`

These cover the `P-n` rows in `doc/conformance.md` that Tasks 5–7 did not create
a test for. Each one exists because a requirement without a test is a claim
without evidence.

**Interfaces:**
- Consumes: everything from Tasks 2–7.

- [ ] **Step 1: Write the tests**

```c
/* P-3: the published name.  This is interoperability surface -- a peer looks
 * for exactly this string -- so it is asserted against a literal rather than
 * round-tripped. */
static void test_idxcache_published_name(void)
{
    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    char name[ZSI_NAME_MAX];

    zsi_name_format_index(name, u, 0x0000002A);
    ASSERT_STR_EQ(name,
        "zeroskip.index-4941da54-9406-4faa-a457-c4b65beae3eb-0000002A");

    /* The zeroskip. prefix is the metadata namespace (D-2), so this must never
     * parse as a data file even inside a database directory. */
    {
        zsi_uuid_t parsed;
        uint32_t s, e;
        ASSERT_EQ(zsi_name_parse(name, parsed, &s, &e), ZSI_NAME_OTHER);
    }
}

/* P-7: the table records the engine the DATA FILE names, not the one this
 * handle would choose for a new file.  A table checksummed under the handle's
 * engine validates for nobody -- the same silent failure that appending under
 * the handle's engine causes (A-6, F-5a). */
static void test_idxcache_uses_file_engine(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char *tab;
    size_t tablen;
    uint32_t gen;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    /* Create under engine 0. */
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = zsi_snapshot_active(db->snap)->hdr.start;
    ASSERT_OK(zs_db_close(&db));

    /* Reopen with the DEFAULT engine and write more.  The table must still say
     * engine 0, because that is what the file says. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    zsi_name_format_index(name, db->uuid, gen);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);
    ASSERT_OK(zs_db_close(&db));

    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    ASSERT_EQ(zsi_idxhdr_engine_id(tab), ZSI_CSUM_NONE);
    free(tab);
}

/* P-8: valid_upto is always a span boundary.  Asserted by walking the file's
 * spans independently and checking that the published value is one of the
 * boundaries the walk produces. */
struct idxcache_bounds { size_t at[64]; size_t n; };

static int idxcache_bounds_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    (void)rock; (void)rec; (void)off;
    return 0;
}

static void test_idxcache_valid_upto_is_span_boundary(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX], name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    char *tab;
    size_t tablen;
    uint64_t vu;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%02d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "v", 1, 0));
    }

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    zsi_name_format_index(name, db->uuid, f->hdr.start);
    snprintf(tabpath, sizeof(tabpath), "%s/%s", cachedir, name);

    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    vu = zsi_get64(tab + ZSI_IDX_OFF_VALID_UPTO);
    free(tab);

    /* An independent walk must agree that this is where the last valid span
     * ends. */
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, false,
                                   idxcache_bounds_cb, NULL));
    ASSERT_EQU(vu, f->complete);

    /* And a walk starting there finds nothing, which is what "boundary" means. */
    ASSERT_OK(zsi_unordered_replay(f, (size_t)vu, false,
                                   idxcache_bounds_cb, NULL));
    ASSERT_EQU(f->complete, vu);

    ASSERT_OK(zs_db_close(&db));
}

/* P-14: publishing must not sync.  Asserted structurally -- the publish path
 * contains no sync call -- because a timing assertion would be flaky.  The
 * companion mutant in tests/mutate.sh inserts one and checks the suite notices
 * the slowdown is not the point; correctness is: a synced table is still
 * correct, so this test guards the requirement's INTENT by pinning the code. */
static void test_idxcache_no_fsync_on_publish(void)
{
    /* The commit path must remain two syncs per transaction (C-7), unchanged by
     * the cache.  Count them through the existing hook. */
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    int before, after;

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    ASSERT_EQ(mkdir(cachedir, 0700), 0);

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));

    before = zsi_test_sync_count;
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    after = zsi_test_sync_count;

    /* Exactly the two durability gates of C-7 -- no third sync for the table. */
    ASSERT_EQ(after - before, 2);
    ASSERT_OK(zs_db_close(&db));
}
```

`test_idxcache_no_fsync_on_publish` needs a sync counter. Check whether one
already exists: `grep -n 'ZS_FDATASYNC\|sync_count\|zs_hook' zeroskip.c`. There
is already a `ZS_FDATASYNC` macro and a `ZS_SNAPSHOT_GAP` test hook pattern; add
a counter in the same style, guarded by the same test-build conditional, and
increment it inside `ZS_FDATASYNC`. If the existing hook mechanism makes a
counter awkward, count directory-and-file syncs by wrapping the macro:

```c
#ifdef ZS_TESTING
static int zsi_test_sync_count;
#endif
```

If no such conditional exists, define the counter unconditionally next to the
macro — it is one `int` and one increment, and `zstest.c` `#include`s
`zeroskip.c` so it can read it directly.

Register all four tests.

- [ ] **Step 2: Run to verify they fail, then pass**

Run: `make zstest && ./zstest idxcache`
Expected: initially some fail (notably the sync counter, which does not exist
yet); after adding the counter, all pass.

- [ ] **Step 3: Run the whole suite**

Run: `make check`
Expected: all pass.

- [ ] **Step 4: Commit**

```bash
git add zeroskip.c zstest.c
git commit -m "$(cat <<'EOF'
test: the remaining pointer table requirements

P-3's published name against a literal, since a peer looks for exactly that
string. P-7's engine selection, built by two zstool-style invocations under
different defaults -- the case that caught the equivalent appending bug.
P-8's span boundary, checked against an independent walk. P-14's absence of
a third sync on the commit path.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Mutants

**Files:**
- Modify: `tests/mutate.sh`

A test that passes but cannot fail reads as coverage while providing none. Every
requirement above gets a mutant.

- [ ] **Step 1: Add the mutants**

Add beside the existing ones, following the exact `mutant <name> <expect> <perl>`
form. Each pattern is tied to exact source text and will rot when the code is
refactored; `mutate.sh` reports `PATTERN ROTTED` rather than passing silently.

```bash
mutant 'idx-drops-comparator-check' catch \
  's{if \(memcmp\(h\.compar_name, f->hdr\.compar_name, ZSI_COMPAR_NAME_LEN\) != 0\)\n        goto out;}{}s'

mutant 'idx-drops-own-comparator-check' catch \
  's{if \(memcmp\(h\.compar_name, mine, ZSI_COMPAR_NAME_LEN\) != 0\) goto out;}{}'

mutant 'idx-drops-term-binding' catch \
  's{if \(term\.csum != h\.term_csum\) goto out;}{}'

mutant 'idx-drops-term-off-check' catch \
  's{if \(after != \(size_t\)h\.valid_upto\) goto out;}{}'

mutant 'idx-accepts-nocsum-table' catch \
  's{if \(!nocsum && !\(h\.flags & ZSI_IDX_FLAG_CSUM_VERIFIED\)\) goto out;}{}'

mutant 'idx-drops-exact-size-check' catch \
  's{if \(want != len\) goto out;}{}'

mutant 'idx-drops-offset-range-check' catch \
  's{if \(v < ZSI_HEADER_LEN \|\| v >= h\.valid_upto\) \{ free\(offs\); offs = NULL; goto out; \}}{}'

mutant 'idx-drops-array-checksum' catch \
  's{if \(zsi_get32\(buf \+ len - 4\)\n        != f->csum\(buf \+ ZSI_IDX_HEADER_LEN, len - ZSI_IDX_HEADER_LEN - 4\)\)\n        goto out;}{}s'

mutant 'idx-publishes-in-place' catch \
  's{fd = open\(tmp, O_WRONLY \| O_CREAT \| O_EXCL, 0600\);}{fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);}'

mutant 'idx-publishes-every-commit' catch \
  's{if \(f->complete - f->cached_upto < cfg->threshold\) return ZS_DONE;}{}'

mutant 'idx-uses-handle-engine' catch \
  's{h\.flags = \(uint16_t\)\(f->csum_id & ZSI_CSUM_MASK\);}{h.flags = (uint16_t)(ZSI_CSUM_XXHASH);}'

mutant 'idx-header-swaps-valid-upto-and-term-off' catch \
  's{#define ZSI_IDX_OFF_VALID_UPTO    64   /\*  8 \*/\n#define ZSI_IDX_OFF_TERM_OFF      72   /\*  8 \*/}{#define ZSI_IDX_OFF_VALID_UPTO    72   /*  8 */\n#define ZSI_IDX_OFF_TERM_OFF      64   /*  8 */}s'

mutant 'idx-sweeps-other-databases' catch \
  's{if \(strncmp\(nm, want, 36\) != 0\) continue;}{}'

mutant 'idx-syncs-before-rename' catch \
  's{if \(rename\(tmp, path\) < 0\)}{if (ZS_FDATASYNC(open(tmp, O_RDONLY)), rename(tmp, path) < 0)}'
```

`idx-header-swaps-valid-upto-and-term-off` is the symmetric-layout mutant. A
matched encoder and decoder round-trip perfectly under it, so only
`test_idxcache_header_byte_layout`'s literal can catch it — which is exactly why
that test asserts against bytes rather than through the API.

`idx-syncs-before-rename` is expected to be caught by
`test_idxcache_no_fsync_on_publish`'s sync count. If it turns out not to be —
because the extra sync lands outside the counted window — either move the
counter or reclassify the mutant honestly as `SUBSUMED` with a note, rather than
deleting it.

- [ ] **Step 2: Run the mutants**

Run: `./tests/mutate.sh idx`
Expected: every mutant reported `caught`. Any `PATTERN ROTTED` means the pattern
does not match the source as written — fix the pattern against the real text, do
not delete the mutant. Any `MISSED` means a test that cannot fail: fix the test.

- [ ] **Step 3: Run the full mutant suite**

Run: `./tests/mutate.sh`
Expected: no new misses among the pre-existing mutants.

- [ ] **Step 4: Commit**

```bash
git add tests/mutate.sh
git commit -m "$(cat <<'EOF'
test: mutants for the pointer table cache

One per new requirement. The symmetric-layout mutant swaps valid_upto and
term_off in both encoder and decoder: it round-trips perfectly, so only
test_idxcache_header_byte_layout's literal can catch it. That is the same
bug class that made test_header_byte_layout necessary.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: zstool and the golden corpus

**Files:**
- Modify: `zstool.c`
- Modify: `tests/gencorpus.sh`
- Modify: `tests/corpus/README.md`
- Create: `tests/corpus/cached-index/`
- Modify: `tests/tool.sh` if it enumerates subcommands

**Interfaces:**
- Consumes: everything above.
- Produces: `zstool --index-dir PATH <dir> <cmd> ...` and `zstool <dir> index-dump`.

- [ ] **Step 1: Add the option and the subcommand**

`zstool` currently takes `dir` as `argv[1]` and the command as `argv[2]`. Add an
option parsed before `dir`:

```c
    /* --index-dir PATH enables the pointer table cache (spec section 8) for
     * this invocation.  The interop runner needs it so one implementation can
     * publish a table another reads. */
    const char *index_dir = NULL;
    int argi = 1;

    if (argc > 2 && !strcmp(argv[argi], "--index-dir")) {
        index_dir = argv[argi + 1];
        argi += 2;
    }
```

Then set `setup.index_dir = index_dir;` wherever `setup` is initialised, and
shift the existing `argv` indices by `argi - 1`.

Add the subcommand, following the existing output-line style — one field per
line, `name=value`, so the runner compares text:

```c
    } else if (!strcmp(cmd, "index-dump")) {
        /* Reads the table for the active file and prints it.  Validation goes
         * through the same loader the read path uses, so the runner is testing
         * the real acceptance rules rather than a parallel parser. */
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        if (!act) { printf("table=none\n"); }
        else {
            char name[ZSI_NAME_MAX];
            zsi_name_format_index(name, act->hdr.uuid, act->hdr.start);
            printf("name=%s\n", name);
            printf("generation=%08X\n", act->hdr.start);
            printf("valid_upto=%zu\n", act->cached_upto);
            printf("nptrs=%zu\n", act->index ? act->index->nbase
                                             + act->index->ndelta : (size_t)0);
        }
    }
```

`zstool.c` links against the library rather than including it, so `zsi_*` is not
reachable. Either move this subcommand's body behind a small public helper, or —
simpler and consistent with how `zstool` already reaches internals for `dump` —
check how `dump` does it (`grep -n 'zs_db_dump' zstool.c zeroskip.c`) and follow
the same route: add `int zs_db_index_dump(struct zs_db *db);` to `zeroskip.h`
next to `zs_db_dump`, implement it in the `CONSISTENCY` section, and have
`zstool` call it.

- [ ] **Step 2: Test the tool path**

Run:
```bash
make zstool
rm -rf /tmp/zsdemo /tmp/zscache && mkdir -p /tmp/zsdemo /tmp/zscache
./zstool --index-dir /tmp/zscache /tmp/zsdemo create
./zstool --index-dir /tmp/zscache /tmp/zsdemo store 6b6579 76616c
./zstool --index-dir /tmp/zscache /tmp/zsdemo index-dump
ls /tmp/zscache
```
Expected: `index-dump` prints `name=`, `generation=`, `valid_upto=`, `nptrs=`;
`ls` shows one `zeroskip.index-...` file.

- [ ] **Step 3: Add the corpus case**

The corpus is language-neutral by design (T-0): data files plus a portable text
description, not fixtures in C. Add a `cached-index` case to
`tests/gencorpus.sh`, generated with `zstool --index-dir` pointed at the case
directory so the table ships alongside the data files. Follow the shape of an
existing case — read `tests/gencorpus.sh`'s `engine0` case first, since it is
also built by separate `zstool` invocations.

Describe it in `tests/corpus/README.md` in the same style as its neighbours:
what the case contains, what a conforming implementation must produce from it,
and specifically that a peer must either use the table or ignore it and replay,
producing identical results either way.

- [ ] **Step 4: Regenerate and check the diff**

Run: `make corpus && git status --short tests/corpus`
Expected: **only** the new `cached-index` directory appears. `make corpus` exists
to add cases, not to paper over a diff: if it changes an existing case's bytes,
that is a format change and it needs a spec commit, not a regenerated fixture.
Stop and investigate if anything else moved.

- [ ] **Step 5: Run the conformance runner**

Run: `./tests/conformance.sh && ./tests/tool.sh`
Expected: pass.

- [ ] **Step 6: Commit**

```bash
git add zstool.c zeroskip.h zeroskip.c tests/gencorpus.sh tests/corpus tests/tool.sh
git commit -m "$(cat <<'EOF'
feat: zstool --index-dir and index-dump, plus a corpus case

The pointer table format is interoperability surface, so it needs a golden
case and a driver-contract way to inspect one as text (T-0a). The corpus
case is built by separate zstool invocations, which is what caught the
equivalent engine-selection bug for appending.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Benchmark and documentation

**Files:**
- Modify: `zsbench.c`
- Modify: `doc/benchmarking.md`
- Modify: `doc/overview.md`
- Modify: `CLAUDE.md`

- [ ] **Step 1: Add the benchmark workload**

`zsbench` already has a `snapshot open` workload producing the open-item-2
number. Add `open (cached)` beside it: the same measurement with `index_dir`
pointed at a scratch directory under `--dir`, the cache warmed by one open
before timing. Add a `--index-dir PATH` option so the row can be run against a
tmpfs.

- [ ] **Step 2: Run it**

Run: `make bench && ./zsbench -n 16000`
Expected: `--selftest` passes, and the `open (cached)` row is materially faster
than `snapshot open` at the larger record counts. Record the actual numbers.

- [ ] **Step 3: Choose the threshold default**

`rollover_size / 8` was a guess. Run the benchmark at a few thresholds:

```bash
for t in 4096 32768 262144 1048576; do ./zsbench -n 16000 --index-threshold $t; done
```

Pick the knee, and change the default in `zeroskip.c` if the data says something
other than `rollover_size / 8`. **Record the numbers in `doc/benchmarking.md`
whichever way it goes** — a default with a measurement behind it is the point.

- [ ] **Step 4: Rewrite the benchmarking doc's open-item-2 section**

Replace "Open item 2: is a shared index worth reintroducing?" with a section on
the pointer table cache: the before-and-after table, what the threshold trades,
and the standing advice that a caller who opens frequently should also consider
a smaller `rollover_size`.

- [ ] **Step 5: Update `doc/overview.md` and `CLAUDE.md`**

`doc/overview.md`: a paragraph on the cache, its opt-in nature, and the
restore-from-backup caveat.

`CLAUDE.md`, three places:

1. The section list in **Architecture** gains
   `POINTER TABLE CACHE   spec §8, load/publish/sweep` between `PRIVATE INDEX`
   and `PER-FILE CURSOR`.
2. The **Interoperability surface** list gains the table's 16 magic bytes, its
   96-byte header layout, the pointer array encoding and trailing checksum, and
   the published and staging name formats.
3. **Things that look like bugs and are not** gains:

```markdown
- **A pointer table is checksummed with the DATA FILE's engine, not the
  handle's.** Same rule as appending, same reason (P-7, A-6, F-5a): a table
  checksummed under the handle's engine validates for nobody, so every reader
  silently rejects it and the cache does nothing while appearing to work.
- **Every pointer-table rejection is `ZS_NOTFOUND`, not an error.** A table is
  an optimisation in a directory the database does not depend on. Reporting a
  corrupt one as corruption would let a file outside the database make a
  readable database look unreadable, which is the opposite of what G-3 wants.
- **A pointer table is never `fsync`ed** (P-14). It is rebuildable and
  self-validating, and syncing it would put a third sync on the commit path
  that C-7 defines as two.
- **`zsi_index_flatten` does not merge the delta in place**, even though that
  would be cheaper and would compact the index. An index may be shared with a
  live `struct zsi_index_cur` holding positions into both arrays, and rewriting
  them underneath it is exactly the in-place mutation G-6 forbids.
- **`term_off` in the table is not redundant with `valid_upto`.** Terminators
  are located by walking spans forward, so a reader given only `valid_upto`
  cannot find the terminator below it without the replay the table exists to
  avoid.
```

- [ ] **Step 6: Final verification**

Run each, in this order, and confirm the output before claiming anything:

```bash
make clean && make check
make asan
make leaks
./tests/mutate.sh
./tests/conformance.sh
./tests/tool.sh
make bench
```

Expected: all pass. Remember `make asan` and `make leaks` clean first, and a bare
`make check` straight after `make asan` runs the sanitizer binary.

- [ ] **Step 7: Commit**

```bash
git add zsbench.c doc CLAUDE.md zeroskip.c
git commit -m "$(cat <<'EOF'
docs: measure the pointer table cache, and record what it costs

zsbench gains an `open (cached)` row beside `snapshot open`, so the claim
that the cache removes the per-open replay has a number behind it rather
than an argument. The threshold default is chosen from the measurement.

CLAUDE.md gains the section, the interop surface, and five entries under
"things that look like bugs and are not".

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
EOF
)"
```

---

## Self-review notes

**Spec coverage.** Every `P-n` in Task 1 has a test in Tasks 5–8 and a row in the
conformance table. R-3, G-6 and the two `A-n` entries are Task 1; the `A-n`
behaviour is tested in Task 2.

**Known soft spots, flagged rather than hidden:**

- Task 8's `test_idxcache_no_fsync_on_publish` needs a sync counter that may not
  exist yet. The step says to check and, if absent, add one in the existing hook
  style. If the hook mechanism makes it genuinely awkward, count syncs by a
  plain `static int` next to `ZS_FDATASYNC` — `zstest.c` includes `zeroskip.c`,
  so it can read the counter directly.
- Task 10's `index-dump` may need a small public entry point, because `zstool.c`
  links the library rather than including it. The step says to follow whatever
  route `zs_db_dump` already takes.
- Task 5's tests depend on Task 6's publishing, so the two commit together. That
  is called out in Task 5 Step 5 rather than left to be discovered.
- The `zsi_idx_load` signature takes `compar` and does not use it (`(void)compar`).
  It is kept because verifying the array's key order is a plausible future
  addition and removing the parameter later is a wider change than adding the
  check. If that reads as dead weight during review, drop it — the loader has
  `f->index`'s comparator available through its callers.
