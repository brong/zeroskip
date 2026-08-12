# Commit-End Sealing and Cache Layout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A commit that grows the active file past `rollover_size` seals it
before releasing the write lock; pointer tables live in a per-database
subdirectory of a configured cache root, or in an opt-in `zeroskip.cache`
directory inside the database directory.

**Architecture:** Three coordinated changes. (1) The write path gains a
commit-tail seal, assigning conversion cost to the transaction that incurred
it — a bulk load (cvt_cyrusdb's single giant transaction) ends with an
in-order file instead of an oversized unordered one. (2) The cache directory
is resolved **once, at open**, into `db->index_dir` — either
`<configured root>/<uuid>/` or `<dbdir>/zeroskip.cache` — so every downstream
consumer (publish, load, sweep, index dump) is unchanged. (3) The Cyrus
wrapper turns the in-database cache on whenever no `zeroskip_index_path` is
configured.

**Tech Stack:** C99, POSIX. Spec-first: each behavioural change lands as a
`spec:` commit before the code that implements it (CLAUDE.md: the spec is
normative).

## Global Constraints

- The spec wins; every behaviour change gets its own `spec:` commit first.
- Every new test gets a mutant in `tests/mutate.sh`; run new mutants by name,
  and `./tests/mutate.sh --rot-only` after refactoring `zeroskip.c`.
- The cache is optional and never load-bearing: no cache failure may fail an
  open or a commit (P-15), and every table rejection is `ZS_NOTFOUND`.
- A pointer table is checksummed with the data file's engine (P-7) and never
  fsynced (P-14). Nothing in this plan touches those rules.
- Commits end with `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- `make corpus` may only be used here for the deliberate cached-index layout
  change (Task 5), landed with its spec commit.

---

### Task 1: Spec — commit-end sealing (D-25d)

**Files:**
- Modify: `doc/specification.md` (§5.3 near D-12d, §5.6 after D-25c)

**Interfaces:**
- Produces: requirement label **D-25d**, referenced by Task 2's code comments
  and mutant names.

- [ ] **Step 1: Add D-25d after D-25c**

```markdown
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
```

- [ ] **Step 2: Amend D-12d with the bound's honest limit**

Append to D-12d:

```markdown
  The bound assumes files grown by transactions smaller than `rollover_size`;
  a single larger transaction produces a proportionally larger unordered
  file, which is why D-25d converts it in the commit that created it rather
  than leaving it for a later writer.
```

- [ ] **Step 3: Amend D-12a's transience note**

In D-12a, after "between a rollover and the next writer's conversion, or
after a crash left an unclean file behind (D-10)", add:

```markdown
  With D-25d in play the rollover case is rarer still — a commit normally
  seals the file it overgrew — so a non-active unordered file is chiefly a
  crash artefact.
```

- [ ] **Step 4: Commit**

```bash
git add doc/specification.md
git commit -m "spec: a commit seals the active file it grew past rollover_size (D-25d)"
```

---

### Task 2: Implement commit-tail sealing

**Files:**
- Modify: `zeroskip.c` — `zsi_txn_commit` tail (after the `zsi_convert_pending`
  block at ~5295), the P-13 publish gate in the incremental branch (~5273)
- Modify: `zstest.c` — tests asserting the old rollover-then-convert shape
  (`test_write_rollover` ~6566, the D-12 conversion tests ~7002, T-10a ~7063)
- Test: `zstest.c` new tests; `tests/mutate.sh` new mutants

**Interfaces:**
- Consumes: `zsi_convert_one(struct zs_db *, struct zsi_file *)`,
  `zsi_snapshot_active`, `zsi_db_refresh` — all existing.
- Produces: no new symbols; behaviour only.

- [ ] **Step 1: Write the failing tests**

```c
/* D-25d: a commit that grows the active file past rollover_size seals it,
 * so a one-transaction bulk load ends with an in-order file. */
static void test_commit_seals_oversized_active(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    char pad[512];
    memset(pad, 'x', sizeof(pad));

    setup.flags = ZS_CREATE;
    setup.rollover_size = 4096;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* One transaction well past rollover_size. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int i = 0; i < 32; i++) {
        char key[16];
        snprintf(key, sizeof(key), "key%04d", i);
        ASSERT_OK(zs_txn_store(txn, key, strlen(key), pad, sizeof(pad), 0));
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* The newest (only) file is in-order: sealed in place, no new
     * generation (D-25a via D-25d). */
    ASSERT_EQU(count_files_matching(dbdir, "-00000001-00000001"), 1);
    ASSERT_EQU(count_files_matching(dbdir, "-00000001\0"), 0); /* exact-name helper */

    /* And everything reads back. */
    const char *val; size_t vallen;
    ASSERT_OK(zs_db_fetch(db, "key0007", 7, NULL, NULL, &val, &vallen, 0));
    ASSERT_EQU(vallen, sizeof(pad));
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* The inverse gate: a commit below rollover_size does NOT seal, or every
 * commit would pay a conversion and the active file could never grow. */
static void test_commit_below_rollover_stays_unordered(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 65536;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));

    /* Still one unordered file. */
    ASSERT_EQU(count_files_matching(dbdir, "-00000001-00000001"), 0);
    ASSERT_OK(zs_db_close(&db));
}

/* D-25e: the sealing commit publishes no table for the file it seals. */
static void test_seal_at_commit_skips_table_publish(void)
{
    /* open with index_dir = cachedir, threshold 1, rollover 4096;
     * one big transaction as above; assert the cache directory contains no
     * zeroskip.index-* entry for generation 00000001. */
}
```

(Adapt `count_files_matching` to whatever directory-scan helper the suite
already uses — grep for `readdir` in zstest.c and follow the local pattern.)

- [ ] **Step 2: Run to verify the seal tests fail**

Run: `make && ./zstest commit_seal` — expected: FAIL (file stays unordered).
`test_commit_below_rollover_stays_unordered` passes already (guards the gate).

- [ ] **Step 3: Implement**

In `zsi_txn_commit`, gate the P-13 publish (incremental branch, ~5273):

```c
        /* D-25e: no table for a file this commit is about to seal. */
        bool sealing = act->size >= db->rollover_size;
        if (r == ZS_OK && db->index_dir && !sealing) {
```

After the `zsi_convert_pending` block (~5298), still under the write lock:

```c
    /* D-25d: a commit that grew the active file past rollover_size seals it
     * now, while the write lock is still held, so the conversion cost lands
     * on the transaction that incurred it rather than on the next writer.
     * After zsi_convert_pending, so D-12b's oldest-first order holds.  Never
     * fatal: the records are durable, and an unsealed oversized file is
     * exactly what D-9a's rollover already recovers at the next commit. */
    if (r == ZS_OK) {
        struct zsi_file *oversized = zsi_snapshot_active(db->snap);
        if (oversized && oversized->hdr_valid
            && oversized->size >= db->rollover_size
            && oversized->complete > ZSI_HEADER_LEN) {
            int sr = zsi_convert_one(db, oversized);
            if (sr == ZS_OK) (void)zsi_db_refresh(db);
        }
    }
```

- [ ] **Step 4: Run the new tests, then the whole suite**

Run: `./zstest commit_seal && ./zstest rollover && make check`

Expected: the new tests pass; tests asserting the OLD shape fail — T-10a
(~7063) asserts exactly one unordered file after each rollover (now zero at
the crossing commit), and the D-12 test at ~7002 forces a rollover to observe
the next writer's conversion. Rework those to the spec'd behaviour: T-10a's
invariant becomes "at most one unordered file"; the D-12 conversion tests
must create their non-active unordered file the way it now arises — via a
crash artefact (the suite's existing fork/kill helpers) or by building the
file with a raw-write helper, not via a clean oversized commit. Do not
weaken what they prove: D-12 conversion must still be exercised.

- [ ] **Step 5: Add mutants**

In `tests/mutate.sh` (patterns tied to the exact source text of Step 3):

```bash
# D-25d: the commit-tail seal.  Without it a one-transaction bulk load leaves
# an oversized unordered file that every open must replay.
mutant "commit: oversized active never sealed" catch \
  's/int sr = zsi_convert_one(db, oversized);/int sr = ZS_OK; (void)oversized;/'

# D-25d's gate inverted: sealing every commit pays a conversion per commit.
mutant "commit: seal threshold inverted" catch \
  's/\&\& oversized->size >= db->rollover_size/\&\& oversized->size < db->rollover_size/'

# D-25e: publishing a table for a file the same commit seals.
mutant "commit: publishes a table for the file it seals" catch \
  's/if (r == ZS_OK \&\& db->index_dir \&\& !sealing) {/if (r == ZS_OK \&\& db->index_dir) {/'
```

- [ ] **Step 6: Run the new mutants and the rot check**

Run: `./tests/mutate.sh "commit:"` — all caught.
Run: `./tests/mutate.sh --rot-only` — no rot.

- [ ] **Step 7: Commit**

```bash
git add zeroskip.c zstest.c tests/mutate.sh
git commit -m "write path: a commit seals the active file it grew past rollover_size (D-25d)"
```

---

### Task 3: Spec — per-database cache directories (P-2a, P-2b, A-8a)

**Files:**
- Modify: `doc/specification.md` — §5.1 table, R-3, P-2, P-16, P-17, A-8

**Interfaces:**
- Produces: labels **P-2a**, **P-2b**, **A-8a**; the reserved name
  `zeroskip.cache`; the layout `<root>/<uuid>/` — all consumed by Task 4.

- [ ] **Step 1: Rewrite P-2 and add P-2a/P-2b**

```markdown
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
```

- [ ] **Step 2: Reword R-3's carve-out**

Replace R-3's last two sentences ("There is no shared cache ... database
directory.") with:

```markdown
  There is no shared cache inside the database for it to update (D-13c);
  `zeroskip.cache` (P-2b), when present, is outside the file set — nothing in
  it parses as a data file (D-2, P-3). Publishing a pointer table (§8) is
  therefore not a write to the database, and a read-only handle MAY publish
  into an existing cache directory — but MUST NOT create `zeroskip.cache`,
  because creating a directory inside the database is a visible side effect.
```

- [ ] **Step 3: Add the §5.1 table row**

```markdown
| `zeroskip.cache/` | directory | opt-in pointer table cache (§8, P-2b) |
```

- [ ] **Step 4: Touch P-16 and P-17**

P-16: change "tables in the cache directory" to "tables in its per-database
cache directory (P-2a, P-2b)" and append: "In `zeroskip.cache` (P-2b) the
uuid restriction is relaxed as that rule states."

P-17: append: "An in-database cache directory (P-2b) travels with its
database: a consistent copy restores tables P-11 accepts, and the hazard
here arises only when a cache outlives or predates the data files beside it."

- [ ] **Step 5: Rewrite A-8 and add A-8a**

```markdown
- **A-8** `index_dir` names the pointer table cache **root** (§8); tables
  live under `<index_dir>/<uuid>/` (P-2a). A null or absent value disables
  the cache unless A-8a's flag is set, and disabled is the default: an
  implementation MUST NOT choose a location itself (P-2). Naming the
  database directory is a usage error (`ZS_BADUSAGE`). The library creates
  the per-database subdirectory but never the root; a root that is missing
  or unwritable disables the cache for that handle rather than failing the
  open (P-15).
- **A-8a** `ZS_INDEX_LOCAL` selects P-2b's in-database cache directory.
  Setting it together with a non-null `index_dir` is a usage error
  (`ZS_BADUSAGE`): the two name different locations for the same tables.
```

- [ ] **Step 6: Commit**

```bash
git add doc/specification.md
git commit -m "spec: per-database cache directories, and an in-database option (P-2a, P-2b, A-8a)"
```

---

### Task 4: Implement cache directory resolution

**Files:**
- Modify: `zeroskip.h` — `ZS_INDEX_LOCAL = 1<<19` flag + comment; extend the
  `zs_open_data.index_dir` comment ("names the cache ROOT; tables live in a
  per-uuid subdirectory")
- Modify: `zeroskip.c` — `zsi_db_open` (~6695 checks, resolution after the
  realpath block ~6781), `zsi_idxcfg` gains `bool local`, `zsi_idx_sweep`
  (~3021) foreign-uuid relaxation, the two `zsi_idxcfg` constructions (~4037,
  ~5274) and `zs_db_index_dump`'s (~6550)
- Modify: `zstest.c` — every test planting or asserting a table path under a
  configured cache dir (~10281–11549) gains the `<uuid>/` level
- Test: new tests + mutants

**Interfaces:**
- Consumes: `zsi_uuid_unparse`, `zsi_zmalloc`, `ZSI_UUID_STR_LEN`.
- Produces: `ZS_INDEX_LOCAL` (public), `ZSI_CACHE_DIR_NAME` ("zeroskip.cache",
  in FILENAMES), `db->index_local` / `zsi_idxcfg.local`.

- [ ] **Step 1: Write the failing tests**

```c
/* A-8a/P-2b: the flag creates zeroskip.cache and publishes into it. */
static void test_index_local_publishes(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cache[PATH_MAX];

    setup.flags = ZS_CREATE | ZS_INDEX_LOCAL;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

    snprintf(cache, sizeof(cache), "%s/zeroskip.cache", dbdir);
    ASSERT_EQU(count_files_matching(cache, "zeroskip.index-"), 1);
    ASSERT_OK(zs_db_close(&db));
}

/* R-3/P-2b: a read-only open never creates the directory. */
static void test_index_local_readonly_creates_nothing(void)
{
    /* create db WITHOUT the flag; close; reopen ZS_SHARED|ZS_INDEX_LOCAL;
     * fetch; assert stat("<dbdir>/zeroskip.cache") == ENOENT. */
}

/* A-8a: both locations named is ambiguous. */
static void test_index_local_and_dir_is_badusage(void)
{
    /* setup.flags |= ZS_INDEX_LOCAL; setup.index_dir = cachedir;
     * ASSERT_EQU(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE); */
}

/* P-2a: a configured root gets a per-uuid subdirectory. */
static void test_index_dir_uuid_subdir(void)
{
    /* zs_db_open_with_uuid with a fixed uuid; threshold 1; two stores;
     * assert the table exists at <cachedir>/<uuid>/zeroskip.index-<uuid>-...
     * and that <cachedir> itself contains no zeroskip.index-* entry;
     * reopen and assert the cached table is used (follow the existing
     * cached-load assertion pattern near zstest.c:11081). */
}

/* P-2b: in zeroskip.cache, a foreign-uuid table is garbage and is swept;
 * under a shared root the existing preserve-foreign test still holds. */
static void test_index_local_sweeps_foreign_uuid(void)
{
    /* open with ZS_INDEX_LOCAL; plant a well-formed table name with a
     * different uuid inside zeroskip.cache; trigger a snapshot take
     * (zs_db_store); assert the planted name is gone. */
}

/* The database remains a plain file set: zeroskip.cache never perturbs the
 * fileset scan or the C-4i freshness probe. */
static void test_fileset_ignores_cache_dir(void)
{
    /* open with ZS_INDEX_LOCAL; store; close; reopen WITHOUT the flag;
     * fetch; ASSERT_OK(zs_db_check_consistency(db)). */
}
```

- [ ] **Step 2: Run to verify they fail**

Run: `make && ./zstest index_local && ./zstest uuid_subdir`
Expected: FAIL — `ZS_INDEX_LOCAL` undefined (compile error) first; then
behavioural failures once declared.

- [ ] **Step 3: Implement**

`zeroskip.h`:

```c
    ZS_INDEX_LOCAL   = 1<<19,  /* open: cache pointer tables in zeroskip.cache
                                  inside the database directory (P-2b).  Only a
                                  writable handle creates the directory; a
                                  read-only handle uses it if present.  Mutually
                                  exclusive with index_dir (A-8a). */
```

`zeroskip.c` FILENAMES section:

```c
#define ZSI_CACHE_DIR_NAME  "zeroskip.cache"
```

`zsi_idxcfg` (~2709) gains `bool local;`, and every construction site passes
`db->index_local`. `struct zs_db` gains `bool index_local;`.

In `zsi_db_open`, the early block (~6695): reject the A-8a conflict before
anything is created, keep the existing string-equality check for a configured
root, and lift the threshold defaulting out so it applies to both modes:

```c
    db->index_local = (setup->flags & ZS_INDEX_LOCAL) != 0;
    if (db->index_local && setup->index_dir) { /* A-8a */
        free(db->dir); free(db);
        return ZS_BADUSAGE;
    }
```

After the realpath check (~6781; the uuid is known and the database directory
exists by here), resolve the effective directory:

```c
    /* A-8/A-8a: resolve the effective cache directory ONCE, so publish, load,
     * sweep and dump all see a per-database path and none of them changes. */
    if (db->index_local) {
        char path[PATH_MAX];
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, ZSI_CACHE_DIR_NAME);
        /* P-2b/R-3: only a writable handle creates it; a read-only handle
         * uses it if present and is otherwise without a cache. */
        if (!db->readonly && mkdir(path, 0700) != 0 && errno != EEXIST)
            db->error("could not create the cache directory; continuing "
                      "without the index cache", "dir=<%s>", path);
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            db->index_dir = strdup(path);
            if (!db->index_dir) { zs_db_close(&db); return ZS_INTERNAL; }
        }
    } else if (db->index_dir) {
        char uu[ZSI_UUID_STR_LEN], path[PATH_MAX];
        struct stat st;
        zsi_uuid_unparse(db->uuid, uu);
        snprintf(path, sizeof(path), "%s/%s", db->index_dir, uu);
        /* P-2a: any handle may create the per-uuid level; the ROOT is never
         * created (A-8), so a missing root fails this mkdir with ENOENT and
         * the cache is disabled for the handle rather than the open failing. */
        (void)mkdir(path, 0700);
        free(db->index_dir);
        db->index_dir = NULL;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            db->index_dir = strdup(path);
            if (!db->index_dir) { zs_db_close(&db); return ZS_INTERNAL; }
        }
    }
```

(The threshold block moves with it: compute `db->index_threshold` whenever
`db->index_dir || db->index_local`, keeping the existing quarter-of-rollover
cap and its comment.)

`zsi_idx_sweep` (~3047), the uuid filter becomes conditional and the P-2b
relaxation explicit:

```c
        /* Another database's tables, in a cache directory serving several,
         * are not ours to remove -- except inside zeroskip.cache (P-2b),
         * which serves exactly one database, where a foreign uuid is garbage
         * by construction. */
        if (!cfg->local && strncmp(nm, want, 36) != 0) continue;
```

(The name-shape checks — prefix, `-`, `gen8`, terminating NUL — stay
unconditional, so staging files and stray names are never touched.)

- [ ] **Step 4: Update the existing cache tests for the `<uuid>/` level**

Every path built as `<cachedir>/zeroskip.index-...` in zstest.c
(~10281–11549) becomes `<cachedir>/<uuid>/zeroskip.index-...`; tests that
plant tables must `mkdir` the uuid level first; the `ASSERT_STR_EQ(
db->index_dir, cachedir)` at ~10319 now expects the resolved child. Keep the
A-8 BADUSAGE tests (~10281–10296) exactly as they are — they check the ROOT.

- [ ] **Step 5: Run the full suite**

Run: `make check` — all pass.

- [ ] **Step 6: Add mutants, fix the rotted one**

```bash
# P-2a: tables published into the shared root defeat per-database sweeps.
mutant "cache: uuid subdirectory dropped" catch \
  's|snprintf(path, sizeof(path), "%s/%s", db->index_dir, uu);|snprintf(path, sizeof(path), "%s", db->index_dir);|'

# P-2b/R-3: a read-only handle creating a directory inside the database.
mutant "cache: read-only handle creates zeroskip.cache" catch \
  's/if (!db->readonly \&\& mkdir(path, 0700) != 0/if (mkdir(path, 0700) != 0/'

# A-8a: both locations accepted silently.
mutant "open: index_dir and ZS_INDEX_LOCAL together accepted" catch \
  's/if (db->index_local \&\& setup->index_dir) {/if (0) {/'

# P-2b: the local sweep sparing foreign uuids strands them forever.
mutant "idx: local sweep spares foreign uuids" catch \
  's/if (!cfg->local \&\& strncmp(nm, want, 36) != 0) continue;/if (strncmp(nm, want, 36) != 0) continue;/'
```

And UPDATE the now-rotted pattern of `"idx: sweeps other databases' tables"`
to match the new conditional line (its replacement removes the whole check):

```bash
mutant "idx: sweeps other databases' tables" catch \
  's/if (!cfg->local \&\& strncmp(nm, want, 36) != 0) continue;/\/* P-16 uuid check removed *\//'
```

- [ ] **Step 7: Run the new mutants and the rot check**

Run: `./tests/mutate.sh cache && ./tests/mutate.sh "idx:" && ./tests/mutate.sh open: && ./tests/mutate.sh --rot-only`

- [ ] **Step 8: Commit**

```bash
git add zeroskip.h zeroskip.c zstest.c tests/mutate.sh
git commit -m "cache: per-database directories, and an in-database option (P-2a, P-2b, A-8a)"
```

---

### Task 5: Corpus and conformance doc

**Files:**
- Modify: `tests/corpus/cached-index/` — the table moves to
  `index/<uuid>/zeroskip.index-...` (bytes unchanged; location only)
- Modify: `zstest.c` `test_corpus_index_table` (~10643) — the scratch copy
  reproduces the uuid level
- Modify: `doc/conformance.md` — wherever the T-0a driver's `--index-dir` /
  `indexdir` contract describes table location, note the P-2a layout

**Interfaces:**
- Consumes: Task 4's resolved layout via zstool's existing `--index-dir`
  (zstool needs NO change: the library resolves the subdirectory).

- [ ] **Step 1: Regenerate the cached-index case**

Run: `make corpus`
Expected diff: `tests/corpus/cached-index/index/zeroskip.index-...` moves to
`tests/corpus/cached-index/index/4941da54-9406-4faa-a457-c4b65beae3eb/
zeroskip.index-...`. The table's BYTES must be identical (`cmp` old vs new)
and `case.txt`'s `expect index` block unchanged — the dump names the table,
not its path. Any other corpus case changing is a STOP: investigate, do not
commit.

- [ ] **Step 2: Update `test_corpus_index_table`'s scratch copy and run it**

Run: `./zstest corpus`

- [ ] **Step 3: Update doc/conformance.md's indexdir wording**

- [ ] **Step 4: Commit**

```bash
git add tests/corpus doc/conformance.md zstest.c
git commit -m "corpus: cached-index follows the per-uuid cache layout (P-2a)"
```

---

### Task 6: Project docs

**Files:**
- Modify: `CLAUDE.md` — interop-surface bullet for the table name gains the
  `<root>/<uuid>/` and `zeroskip.cache` locations; add a "looks like a bug"
  entry: the sealing commit skips the P-13 publish (D-25e), and read-only
  handles never create `zeroskip.cache` (R-3)
- Modify: `doc/overview.md` — cache section mentions both locations

- [ ] **Step 1: Edit both files**
- [ ] **Step 2: Commit**

```bash
git add CLAUDE.md doc/overview.md
git commit -m "docs: cache locations and commit-end sealing"
```

---

### Task 7: Cyrus wrapper (in ../cyrus-imapd, branch `zeroskip`)

**Files:**
- Modify: `lib/cyrusdb_zeroskip.c` `_setup_init` (~82)
- Modify: `lib/imapoptions/zeroskip_index_path` — default text
- Modify: `lib/zeroskip.c`, `lib/zeroskip.h` — refresh the vendored copies
  from zeroskip2 (verify first how they were vendored: `diff -q`)

**Interfaces:**
- Consumes: `ZS_INDEX_LOCAL` from the vendored `zeroskip.h`.

- [ ] **Step 1: Enable the in-database cache when no path is configured**

```c
    setup->index_dir = libcyrus_config_getstring(CYRUSOPT_ZEROSKIP_INDEX_PATH);
    setup->flags = 0;
    /* No configured cache root: cache inside each database (P-2b).  The
     * directory inherits the database's permissions, deletes with it, and
     * needs no configuration. */
    if (!setup->index_dir) setup->flags |= ZS_INDEX_LOCAL;
```

- [ ] **Step 2: Update the imapoptions description**

```
If not specified, each database caches its pointer tables in a
`zeroskip.cache` directory inside its own database directory.  Set this to
put all caches under one root instead (each database still gets its own
subdirectory there); a tmpfs is a good choice ...
```

- [ ] **Step 3: Confirm the wrapper's directory handling is already safe**

`myunlink` uses `removedir` (recursive — check `lib/util.c:359` walks
subdirectories) and `myarchive` copies only `zeroskip-` prefixed names, so
`zeroskip.cache` is skipped. Verify by reading, and run the cyrus `zeroskip`
cunit tests if the tree builds.

- [ ] **Step 4: Commit (cyrus repo)**

```bash
git -C ../cyrus-imapd add lib/cyrusdb_zeroskip.c lib/imapoptions lib/zeroskip.c lib/zeroskip.h
git -C ../cyrus-imapd commit -m "cyrusdb_zeroskip: cache pointer tables inside the database by default"
```

---

### Task 8: Verification

- [ ] **Step 1:** `make check` — clean.
- [ ] **Step 2:** `make asan` — clean (rebuilds first; remember the stale-binary trap).
- [ ] **Step 3:** `make clean && make leaks` — the new owned path strings and
  the resolution branch are exactly the shape `make leaks` catches and ASan
  does not.
- [ ] **Step 4:** `./tests/mutate.sh "commit:" && ./tests/mutate.sh cache && ./tests/mutate.sh "idx:" && ./tests/mutate.sh open:` — all caught.
- [ ] **Step 5:** `./tests/mutate.sh --rot-only` — no rot anywhere.
- [ ] **Step 6:** `make bench` — the publish-threshold numbers should be
  unaffected; sealing changes only the over-rollover commit.

## Self-Review Notes

- D-25d is SHOULD, not MUST: a start-of-commit-only writer stays conforming,
  so the interop suite gains no new required behaviour.
- The resolution-at-open design means `zsi_idx_publish`, `zsi_index_build_cached`,
  `zsi_idx_sweep` and `zs_db_index_dump` keep their signatures; only
  `zsi_idxcfg` grows a field.
- The pre-existing `"idx: sweeps other databases' tables"` mutant pattern rots
  in Task 4 Step 3 and is repaired in Step 6 — `--rot-only` in Step 7 proves it.
- Tests reworked in Task 2 Step 4 must keep exercising D-12 conversion via a
  genuinely non-active unordered file; sealing makes that state crash-only, so
  use the suite's fork/kill helpers rather than deleting the coverage.
