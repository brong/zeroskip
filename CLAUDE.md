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
- **Decoding accepts non-canonical records and terminators; it does not reject them.** A big form whose lengths would have fitted the short form, or a stored ancestor equal to the file's own `start`, is something a conforming writer never produces (F-15, F-17) — but rejecting it on read would be a *data-loss* bug. A record that fails to validate makes an unordered file complete at that point (F-24), discarding everything after it, and G-3 forbids corruption costing committed data. So a peer with a canonicalisation bug would silently cost us every record it wrote after its first non-canonical one. `zsi_rec_is_canonical` / `zsi_term_is_canonical` exist so `zs_db_check_consistency` reports the divergence while still reading the data, which is the precedent T-6 sets explicitly.
- **A pointer table is checksummed with the DATA FILE's engine, not the handle's.** Same rule as appending, same reason (P-7, A-6, F-5a): a table checksummed under the handle's engine validates for nobody, so every reader silently rejects it and the cache does nothing while appearing to work.
- **Every pointer-table rejection is `ZS_NOTFOUND`, not an error.** A table is an optimisation in a directory the database does not depend on. Reporting a corrupt one as corruption would let a file outside the database make a readable database look unreadable, which is the opposite of G-3.
- **A pointer table is never `fsync`ed** (P-14). It is rebuildable and self-validating, and syncing it would put a third sync on a commit path C-7 defines as two.
- **`zsi_index_flatten` does not merge the delta in place**, though that would be cheaper and would compact the index as a side effect. An index may be shared with a live `struct zsi_index_cur` holding positions into *both* arrays, and rewriting them underneath it is exactly the in-place mutation G-6 forbids.
- **`term_off` in a pointer table is not redundant with `valid_upto`.** Terminators are located by scanning forward (F-20), so a reader given only `valid_upto` cannot find the terminator below it without performing the very replay the table exists to avoid.
- **The publish threshold's expensive end is the HIGH one**, which is the opposite of the intuition and was wrong in the spec until measured. A write transaction refreshes its snapshot at begin (C-4), and that refresh replays from the last published point — so publishing rarely leaves the replay unbounded. 16000 single-store transactions cost 26.8s with no table and 2.7s with a 32KB threshold. Both ends of the curve cost; `doc/benchmarking.md` has it.

## Testing

Tests use a custom harness with `ASSERT()`, `ASSERT_EQ()`, `ASSERT_EQU()`, `ASSERT_OK()`, `ASSERT_SIGN()`, `ASSERT_STR_EQ()`, `ASSERT_MEM_EQ()`, and `CB_`-prefixed variants for callbacks. Each test gets a fresh temp directory via `setup()`/`teardown()`; `basedir` exists, `dbdir` deliberately does not, so tests exercise `ZS_CREATE`.

`tests/corpus/` is **language-neutral by design** (T-0) — data files plus a portable text description, not fixtures in C source. `make corpus` exists to add cases, not to paper over a diff: if it changes an existing case's bytes, that is a format change and needs a spec commit.

### `make mutate`

`tests/mutate.sh` introduces, one at a time, the specific bugs the suite claims to guard against, and reports whether the suite noticed. Add a mutant whenever you add a test for a requirement — a test that passes but cannot fail reads as coverage while providing none.

It has already earned this: it found that several header tests passed under a *symmetric* layout change (swap the `start` and `end` offsets and a matched encoder/decoder round-trips perfectly), which is exactly the bug class that makes another implementation unable to read our files. `test_header_byte_layout` exists because of that finding, asserting the 72 bytes against a literal.

Two mutants are marked `equivalent` rather than expected-to-be-caught, because they genuinely do not change behaviour: delegating the comparator's prefix compare to `memcmp`, and dropping `roundup8`'s overflow guard. They are listed so nobody writes a bogus test chasing them. If you find a mutant that *should* be equivalent but gets caught, the classification is wrong — investigate rather than reclassifying.

The perl patterns are tied to exact source text and will rot when the code is refactored. The script reports `PATTERN ROTTED` rather than silently passing; fix the pattern, don't delete the mutant.

**Nothing may touch `zeroskip.c` while `mutate.sh` runs, and two runs must never overlap.** It keeps its own backup and `cp`s it back between mutants, so a concurrent edit is silently reverted and two concurrent runs corrupt each other's verdicts. Both happened while the pointer table cache was being written, and the second produced a whole run that had to be thrown away. It also runs for ten to fifteen minutes, which is long enough to be tempted.

**A test must not be able to damage `tests/corpus/`.** `test_corpus_index_table` originally pointed a live cache directory at the checked-in corpus, and since a cache directory is one the library *unlinks from* (P-16), the two `sweeps` mutants deleted the golden table outright — which then made five unrelated mutants read as caught. It copies the case to scratch first. A deleted corpus file reads like a corpus that needs regenerating, which is exactly the diff `make corpus` must never be used to resolve.

## Interoperability surface

Changing any of these breaks other implementations, so they need a spec change first:

- the 16 magic bytes, the 72-byte header layout, the 14 type bytes, the record and terminator layouts, the pointer section and its 16-byte trailer
- XXH3-64 with seed 0, truncated to the low 32 bits, little-endian
- the default comparator's total order and the exact bytes of the `memcmp` name field
- filename format: lowercase hyphenated UUID, uppercase 8-digit hex generations, **no extension** (D-1a — the unordered name must sort before the in-order name for the same generation)
- `fcntl` record locking on `zeroskip.lock` bytes 0/1/2
- the pointer table's 16 magic bytes, its 96-byte header layout, the offset array
  and its trailing checksum, and the `zeroskip.index-<uuid>-<GEN8hex>` name
- `zstool`'s output line format, which the interop runner compares as text

Locks are ordered *within* one database. A caller holding locks on several databases while writing must impose its own consistent order (C-1h).
