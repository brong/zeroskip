# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

zeroskip is an append-only ordered key-value store: a directory of immutable and append-only files, with lock-free readers and a single writer. It's a C library (libzeroskip) with a CLI tool (zstool), a test suite (zstest) and a benchmark tool (zsbench).

Its sibling library `twom` (`../twom/`) is a mutable single-file skiplist. zeroskip suits workloads that are append-heavy, want readers that never take a lock, and tolerate compaction happening out of band.

**The spec is normative.** `doc/specification.md` specifies the on-disk format, the database layout, the concurrency and durability protocol, and recovery, so that independent implementations in different languages interoperate on the same database concurrently. Requirements are labelled (`F-n` format, `D-n` database, `C-n` concurrency, `R-n` recovery, `A-n` API, `G-n` guarantee, `T-n` tests); **MUST** and **MUST NOT** are normative. When code and spec disagree, the spec wins — or the spec gets changed deliberately, in its own commit.

`doc/implementation-plan.md` is the implementation plan.

## Build Commands

```bash
make                # libzeroskip.a, libzeroskip.so/.dylib, zstool, zstest, zsbench
make check          # run the test suite (alias: make test)
./zstest            # run all tests directly
./zstest record     # run tests matching a substring filter
make asan           # rebuild under ASan + UBSan and run the suite
make leaks          # rebuild plain and leak-check (LSan on Linux, leaks(1) on macOS)
make mutate         # verify the suite can actually fail (see Testing below)
make corpus         # regenerate the golden corpus (see below before using)
make bench          # zsbench --selftest, then a small smoke benchmark
make clean
```

Requires: C99 compiler, POSIX (`mmap`, `fcntl` locking, `/dev/urandom`). Builds on Linux, macOS and the BSDs with no external libraries.

Default `CFLAGS` are `-Wall -Wextra -g -O2 -fno-strict-aliasing -std=c99`. Append to them with `EXTRA_CFLAGS=...` rather than overriding `CFLAGS`, which would drop the platform feature defines.

**Every target builds a binary called `zstest`**, so make cannot tell an instrumented build from a plain one and will reuse whichever was built last. `asan` and `leaks` therefore clean first, and you should assume `make check` straight after `make asan` is running the *sanitizer* binary. This is not hypothetical: it made `make leaks` fail with "malloc replacement library without the required support", which reads like a tooling problem and was really ASan's `malloc` in a stale binary. `-fno-strict-aliasing` is precautionary rather than known to be required: the little-endian accessors go through `memcpy`, so unlike `twom.c` they do not punt on aliasing.

## Source Layout

- `zeroskip.h` — public API, opaque types, error codes, flags
- `zeroskip.c` — full implementation, organised in labelled sections
- `zstool.c` — CLI tool, implementing the T-0a driver contract
- `zstest.c` — test suite with custom assertion macros
- `zsbench.c` — benchmark tool
- `xxhash.h` — vendored xxHash 0.8.3, for record checksums
- `tests/corpus/` — language-neutral golden corpus (T-0)
- `doc/overview.md`, `doc/conformance.md`

## Architecture

**Two file kinds, distinguishable from the header alone.** `end == 0` means an *unordered* file: one generation, records in append order, no pointer section. `end != 0` means an *in-order* file: a range of generations, records in key order, with a pointer section. A reader always knows, before reading anything else, whether a pointer section must be present.

**The directory is the file set.** There is no manifest. Filenames carry each file's generation range, so one `readdir` yields the set and every range without opening a file. Overlapping files are *resolved*, not rejected (D-5).

**Nothing is ever mutated.** Files are only appended to or created. A new file is published by `rename`. Every index is private to the process that built it. Nothing needs cleaning up when a process dies.

**Sections in `zeroskip.c`**, in dependency order — each may only call downwards into those above it:

```
TUNING              constants, limits
LIBRARY SUPPORT     zmalloc, random bytes, UUID, LE load/store, overflow guards
COMPARATORS         F-11a total order
CHECKSUMS           three engines
FILENAMES           D-0/D-1 format and parse
FILE HEADER         72-byte header
RECORDS             14 type bytes, ancestors, terminators
FILE OBJECT         open, mmap, kind detection, the one bounds-checked accessor
UNORDERED FILE      span chain replay, complete-at
POINTER SECTION     section + trailer, binary search  (needs an open file)
PRIVATE INDEX       base+delta ordered index
POINTER TABLE CACHE spec section 8: load, publish, sweep
PER-FILE CURSOR     uniform seek/next over both kinds
FILE SET            readdir, D-5 resolution, D-6 tiling
SNAPSHOT            C-4 protocol
FILE LOCKING        three fcntl byte locks + in-process mutexes
READ PATH           D-14 lookup, D-14e merge cursor
WRITE PATH          spans, commit, two durability gates
CONVERSION          unordered -> in-order
REPACK              selection and merge
CONSISTENCY         F-28, F-26f, dump
OPEN AND CLOSE      open is recovery
PUBLIC API          the three entry-point forms
```

**Naming conventions:**
- Public API: `zs_db_*`, `zs_txn_*`, `zs_cursor_*`
- Internal types and functions: `zsi_` prefix, all `static`
- Macros: UPPERCASE

**Error handling:** every function returns `enum zs_ret` (`ZS_OK = 0`, `ZS_DONE = 1`, negatives for errors). Output through pointer parameters.

## Things that look like bugs and are not

Each of these has cost someone an afternoon. They are load-bearing.

- **`zsi_csum_xxhash` does not short-circuit on empty input**, unlike twom's equivalent. F-26g requires the engine's value for empty input, and a zero-record in-order file depends on it.
- **The default comparator compares through `unsigned char *`, and orders by length when the common prefix is equal** (F-11a). `memcmp` alone is not enough — it says nothing about keys of differing length, so a key and its own prefix come out wrong — and its return *magnitude* is unspecified, so only the sign may be used. The platform hazard is comparing plain `char`, whose signedness varies: that misorders keys above `0x7F` on ARM versus x86 and produces pointer sections the other platform cannot read, silently, past every ASCII test.
- **`zsi_type_valid` is a `switch`, not a bit-property computation.** F-12's table is normative; a computed predicate is a second specification that can drift.
- **Repack writes a record even when a newer file already shadows the key.** Being shadowed does not permit dropping it; only D-19 does. The retained record carries the chain's reach, which no other file records (D-19a).
- **A failed `fdatasync` is never retried.** A second call can succeed after the dirty pages were discarded, so treating success as proof of durability is wrong (C-7a).
- **The lock file is never unlinked**, and `flock` is never used. Both silently break mutual exclusion (D-3b, C-1e).
- **There is no in-process mutex, and adding one would be a regression.** `fcntl` locks are per-process (C-1f), so two handles on one database within a process are excluded by nothing — not by `fcntl`, which sees one owner, and not by a per-handle mutex, which would be two different objects. A mutex buys exclusion only between threads *sharing a handle*, which is not what G-5 reads as promising. An earlier version had one; two threads with separate handles overlapped 398 times in the write section. **Handles are not thread-safe; concurrent writers are separate processes.** If intra-process exclusion is ever genuinely needed, use `F_OFD_SETLK` (C-1i), which is scoped to an open file description.
- **The terminator checksum, not a lock, is what makes reading a live file safe** (C-4f). It covers the span *and* the terminator, so a terminator whose data has not landed reads as absent.
- **Appending uses the ACTIVE FILE's checksum engine, not the handle's.** A `ZS_CSUM_*` flag chooses the engine for files a handle *creates* and never overrides what an existing file records (A-6, F-5a). Using the handle's engine to checksum a span appended to a file recording a different one is silent data loss: the terminator validates under neither, so the next reader rejects the whole span (F-22 doing its job) and every record in it vanishes. Found by the corpus's engine-0 case, because that case is built by separate `zstool` invocations and so the second one had a different default.
- **This writer never produces a ROLLBACK span, but the reader must handle them.** Transactions are buffered in memory and written at commit, so an abort has nothing on disk to void. C-8 and F-21 describe a *streaming* writer, for which a ROLLBACK is essential — without one, a later commit's span would enclose the aborted records. Both writers are conforming; the trade is that a transaction must fit in memory. The read path handles ROLLBACK spans anywhere (F-25) because a streaming peer produces them, and `test_dump_shows_rollback` hand-builds one for exactly that reason.
- **Record checksums (F-32) are not verified during replay or pointer-section load**, only at materialization — the lookup return and the cursor yield. Verifying in replay looks like defence in depth and is a data-loss bug: F-24 completes a file at its first invalid record, so a verifying replay turns one flipped value byte into the silent loss of every record after it. The span checksum guards structure and liveness (C-4f); the record checksum guards the bytes a caller is about to consume. And they answer different questions in salvage too: a record checksum proves bytes, a terminator proves the transaction was *committed* (S-8), so salvage's span walk deliberately ignores record checksums while its in-order walk — where publication by rename implies commitment (D-21) — trusts them per record. `test_record_csum_replay_no_truncate` holds the read half apart; `test_salvage_unverified_needs_the_flag` the salvage half.
- **Decoding accepts non-canonical records and terminators; it does not reject them.** A big form whose lengths would have fitted the short form, or a stored ancestor equal to the file's own `start`, is something a conforming writer never produces (F-15, F-17) — but rejecting it on read would be a *data-loss* bug. A record that fails to validate makes an unordered file complete at that point (F-24), discarding everything after it, and G-3 forbids corruption costing committed data. So a peer with a canonicalisation bug would silently cost us every record it wrote after its first non-canonical one. `zsi_rec_is_canonical` / `zsi_term_is_canonical` exist so `zs_db_check_consistency` reports the divergence while still reading the data, which is the precedent T-6 sets explicitly.
- **A pointer table is checksummed with the DATA FILE's engine, not the handle's.** Same rule as appending, same reason (P-7, A-6, F-5a): a table checksummed under the handle's engine validates for nobody, so every reader silently rejects it and the cache does nothing while appearing to work.
- **Every pointer-table rejection is `ZS_NOTFOUND`, not an error.** A table is an optimisation in a directory the database does not depend on. Reporting a corrupt one as corruption would let a file outside the database make a readable database look unreadable, which is the opposite of G-3.
- **A pointer table is never `fsync`ed** (P-14). It is rebuildable and self-validating, and syncing it would put a third sync on a commit path C-7 defines as two.
- **`zsi_index_flatten` does not merge the delta in place**, though that would be cheaper and would compact the index as a side effect. An index may be shared with a live `struct zsi_index_cur` holding positions into *both* arrays, and rewriting them underneath it is exactly the in-place mutation G-6 forbids.
- **`term_off` in a pointer table is not redundant with `valid_upto`.** Terminators are located by scanning forward (F-20), so a reader given only `valid_upto` cannot find the terminator below it without performing the very replay the table exists to avoid.
- **The publish threshold's expensive end is the HIGH one**, which is the opposite of the intuition and was wrong in the spec until measured. A write transaction refreshes its snapshot at begin (C-4), and that refresh replays from the last published point — so publishing rarely leaves the replay unbounded. 16000 single-store transactions cost 26.8s with no table and 2.7s with a 32KB threshold. Both ends of the curve cost; `doc/benchmarking.md` has it.

- **A commit that grows the active file past rollover_size seals it itself** (D-25d), so the steady state is AT MOST one unordered file — zero right after a crossing commit — and a one-transaction bulk load ends with an in-order file. The start-of-commit rollover check still exists and is not dead: it recovers oversized actives a crash or a non-sealing peer left behind, and is the fallback when a seal fails. The sealing commit also skips the P-13 table publish (D-25e); publishing there is not a bug a test can see — the seal's own refresh sweeps the table — but it writes and deletes a whole table for nothing, megabytes on a bulk load.
- **The commit-site incremental fold requires `refcount == 2`, and 2 is correct.** db->snap and txn->snap both reference the snapshot at that point, so two references mean "nobody else" — a cursor makes it three and forces the refresh fallback (G-6). It was `== 1` from the first commit, which made the branch dead code and every commit a double rebuild; a mutant that could not be caught found it. Do not "simplify" the count.
- **A read-only handle never creates `zeroskip.cache`, only uses one already present** (P-2b, R-3). Creating a directory inside the database on a read-only open is a visible side effect on a forensic copy or read-only mount. The per-uuid level under a configured root is different: ANY handle may create it, because it is outside the database.
- **Sealing converts the active file in place rather than rolling over first.** A conversion output covers its input's range (D-5a), so the newest file becomes in-order and there is simply no active file until the next write. Rolling over first reaches the same layout while consuming a generation per seal, and generations are finite (D-9c). `test_seal_creates_no_new_generation` is the only thing that tells the two apart.
- **Compaction takes the repack lock and then the write lock** (C-1d), which required *amending* C-1d: it previously said nothing holds both. The locks are now one chain, repack → write → remove, and the assertion in `zsi_lock_take` enforces it — it fired the first time compaction ran, because the spec had been amended without the code following.
- **Compaction merges every maximal RUN of adjacent in-order files, not the in-order prefix** (D-26b). D-16's geometric selection does not apply to it, but adjacency (D-19) still does, so a file nothing can convert splits the set. Taking only the prefix merges *nothing at all* when such a file sits second — exactly the damaged database where "best effort" has to mean something.
- **`zsi_repack_run` is the single merge entry point.** Both `zsi_repack` and `zsi_compact` go through it so D-17 to D-23 are implemented once. A second call site for `zsi_repack_merge` is how two sets of retention rules would drift apart, and D-19's tombstone rule is the one nobody would notice diverging.
- **The fileset scan copies `d_name` with an explicit length, not `snprintf`.** `zsi_name_parse` has already bounded the name at 63 characters, but the compiler cannot see that and `d_name` is a declared `char[NAME_MAX]`, so `-Wformat-truncation` fires — and Cyrus builds `-Werror`. The explicit copy states the bound where it is relied on instead of suppressing the warning.

- **Salvage does not share the read path, and must not.** It exists to read what §5 and §7 refuse — a set that does not tile, a header that fails, a pointer section that will not load, spans after a bad one. Routing it through `zsi_snapshot_take` would make it refuse exactly the databases it is for.
- **Resync checksums its candidate before believing it** (S-7). A terminator carries `spanlen`, so a candidate implies a span start, and that span can be verified. Skipping the check would make salvage a guess rather than a recovery, and everything it produced unverified while claiming otherwise.
- **Salvage never recovers a rolled-back span** (S-9), under any flag. Those records were deliberately aborted and no conforming reader has ever shown them.
- **A salvaged value may be an older one.** If a key's newest version was in lost bytes, salvage emits the newest that survives and reports the key as possibly stale (S-10). The report is the mitigation, so it must not be dropped as noise — and it needs no second pass, because files are scanned oldest first and positions ascend, so the first loss is discovered before any record beyond it is applied.

- **Reverse iteration is the same merge with the key comparison flipped — and ONLY the key** (D-14k). The generation tie-break stays newest-first in both directions, or step 3 would suppress the wrong duplicate and yield overwritten values. `ZS_FETCHPREV` is a throwaway reverse cursor exactly as `ZS_FETCHNEXT` is a forward one (D-14l, G-7): the fetch family is two shapes, not three, and predecessor lookup structurally cannot disagree with reverse iteration.
- **A reverse prefix scan seeks the prefix's byte-successor, never the prefix itself.** The largest key ≤ the prefix is BELOW every key carrying it (F-11a sorts the bare prefix first), so seeking the prefix reports a populated range empty. The successor — increment the last non-0xFF byte, truncate after it — is the least upper bound of the prefix range, and the seek at it must be EXCLUSIVE: the successor may exist as a real key. All-0xFF has no successor and correctly means "from the end", because every key above it carries it. Computed once at open into `c->rev_succ`, so a D-14j refresh re-derives the open's bound rather than trusting a stale arm.
- **A refresh re-seeks the transaction arm from the CURSOR's resume point, never from the arm's own position.** The arm's position is the last key consumed *from that arm*, which lags the merge — an arm exhausted at open has consumed nothing at all — so re-positioning it from its own state resurfaces any key stored into the gap, and the merge yields it BEHIND the last key returned. Found downstream (cyrus aaa-db `foreach_changes`): a key stored mid-walk at a position already passed came out of the traversal out of order, shifting everything after it by one. `zsi_cursor_reseek_arm` exists so the txn-only re-seek and the full re-seek share one resume rule; the mutant "cursor: txn arm resumes from its own position" is the bug preserved.
- **A transaction cursor arm holds the KEY it reached, not an index** (D-14j-a). The pending array is sorted and a write during a traversal inserts into it, shifting every element from the insertion point on — so an index stops referring to the record it referred to and the cursor re-yields a key it has already returned. Reported from the Cyrus integration, and silent: the traversal simply processed a record twice.
- **`struct zsi_fcur` owns memory now, so it must not be copied casually.** `zsi_fcur_find` takes a scratch copy to share the seek logic; since the transaction arm gained an owned position key, that copy owns one too and has to be released on every exit. A struct that was trivially copyable and then grows an owned pointer is exactly where this goes wrong — `make leaks` caught it, ASan did not.
- **A cursor takes its own snapshot reference.** It used to borrow the transaction's, which was fine while a cursor's snapshot never changed; D-14j lets a handle-live cursor swap to a newer one, and releasing a borrowed reference frees a snapshot the transaction still points at.
- **`c->handle_live` cannot be derived from `c->txn`.** A read-only implicit transaction is passed to the cursor as NULL, so the `zs_db_*` wrapper that created it has to say so — which is why `struct zs_txn` carries `implicit`.

## Testing

Tests use a custom harness with `ASSERT()`, `ASSERT_EQ()`, `ASSERT_EQU()`, `ASSERT_OK()`, `ASSERT_SIGN()`, `ASSERT_STR_EQ()`, `ASSERT_MEM_EQ()`, and `CB_`-prefixed variants for callbacks. Each test gets a fresh temp directory via `setup()`/`teardown()`; `basedir` exists, `dbdir` deliberately does not, so tests exercise `ZS_CREATE`.

`tests/corpus/` is **language-neutral by design** (T-0) — data files plus a portable text description, not fixtures in C source. `make corpus` exists to add cases, not to paper over a diff: if it changes an existing case's bytes, that is a format change and needs a spec commit.

### `make mutate`

`tests/mutate.sh` introduces, one at a time, the specific bugs the suite claims to guard against, and reports whether the suite noticed. Add a mutant whenever you add a test for a requirement — a test that passes but cannot fail reads as coverage while providing none.

It has already earned this: it found that several header tests passed under a *symmetric* layout change (swap the `start` and `end` offsets and a matched encoder/decoder round-trips perfectly), which is exactly the bug class that makes another implementation unable to read our files. `test_header_byte_layout` exists because of that finding, asserting the 72 bytes against a literal.

Two mutants are marked `equivalent` rather than expected-to-be-caught, because they genuinely do not change behaviour: delegating the comparator's prefix compare to `memcmp`, and dropping `roundup8`'s overflow guard. They are listed so nobody writes a bogus test chasing them. If you find a mutant that *should* be equivalent but gets caught, the classification is wrong — investigate rather than reclassifying.

The perl patterns are tied to exact source text and will rot when the code is refactored. The script reports `PATTERN ROTTED` rather than silently passing; fix the pattern, don't delete the mutant.

**The full run is not part of the standard loop.** Each mutant is a rebuild plus both test binaries, and at 230+ mutants that is **several hours** — a 2026-08-12 full audit measured roughly two minutes per mutant, an overnight job, not an hour (the earlier estimate had rotted as the suite grew). It is priced for releases and deliberate suite audits. Day to day: run the mutants you are adding by name (`./tests/mutate.sh <substring>`), and `./tests/mutate.sh --rot-only` after refactoring `zeroskip.c` — it applies every pattern with no build and no run, so pattern rot (the thing that silently accumulates between full runs) is checked in seconds.

**`mutate.sh` snapshots the sources into a temp directory and mutates the copy.** The checkout is never written, an interrupted run cannot leave a mutant in the tree, and concurrent runs each have their own copy. It used to mutate `zeroskip.c` in place, and every failure mode that design permits actually happened: a concurrent edit silently reverted, two overlapping runs corrupting each other's verdicts — and a stale run from an interrupted session that sat parked on a stopped child for days, holding a pre-fix backup it would have restored over the repo the moment it advanced. The flip side of the snapshot: edits made to the repo after a run starts are not part of that run's verdict, so re-run after changing anything.

**Killing a `mutate.sh` run still orphans stopped `zstest` children.** It sets `set +m`, so the fork-based tests' children are left in `T` state rather than reaped, and they survive for hours holding descriptors. They cost no CPU, so the symptom is not an obvious hang but an inexplicably slow machine much later. After interrupting a run: `pkill -9 -f '^\./zstest'`, and check `ps aux | grep zstest` before blaming anything else for being slow. A killed run's mutant `zstest` binaries stay in its temp directory, not in the repo — the repo binaries are whatever they were before the run.

**A test must not be able to damage `tests/corpus/`.** `test_corpus_index_table` originally pointed a live cache directory at the checked-in corpus, and since a cache directory is one the library *unlinks from* (P-16), the two `sweeps` mutants deleted the golden table outright — which then made five unrelated mutants read as caught. It copies the case to scratch first. A deleted corpus file reads like a corpus that needs regenerating, which is exactly the diff `make corpus` must never be used to resolve.

## Interoperability surface

Changing any of these breaks other implementations, so they need a spec change first:

- the 16 magic bytes, the 72-byte header layout, the 14 type bytes, the record and terminator layouts, the pointer section and its 16-byte trailer
- XXH3-64 with seed 0, truncated to the low 32 bits, little-endian
- the default comparator's total order and the exact bytes of the `memcmp` name field
- filename format: lowercase hyphenated UUID, uppercase 8-digit hex generations, **no extension** (D-1a — the unordered name must sort before the in-order name for the same generation)
- `fcntl` record locking on `zeroskip.lock` bytes 0/1/2
- the pointer table's 16 magic bytes, its 96-byte header layout, the offset array
  and its trailing checksum, and the `zeroskip.index-<uuid>-<GEN8hex>` name —
  located at `<cache root>/<uuid>/` for a configured root (P-2a), or directly in
  `zeroskip.cache/` inside the database directory under `ZS_INDEX_LOCAL` (P-2b)
- `zstool --hex`'s line format, which the interop runner compares as text (the raw default is for humans, and is not interop surface)

Locks are ordered *within* one database. A caller holding locks on several databases while writing must impose its own consistent order (C-1h).
