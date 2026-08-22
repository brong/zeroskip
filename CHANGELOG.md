# Changelog

Semver: MAJOR for an ABI break, MINOR for features and for observable changes in
behaviour, PATCH for fixes.

## 2.9.2 — 2026-08-26

- **`XXH_NO_INLINE_HINTS` is no longer set for optimised builds, and hashing is
  about 3x faster because of it.** The flag makes every internal xxhash function
  a plain `static`, which costs roughly 3x on *all* hashing — one-shot as well as
  streaming — so every span checksum, conversion, repack and consistency check
  was paying it. Measured over 21MB on aarch64: gcc 14 goes 19.4 → 59.0 GB/s,
  clang 19 goes 24.3 → 62.5. About **+8%** on a bulk load. It is still required
  below `-O2` on gcc/aarch64, so the Makefile's low-optimisation targets pass it
  themselves (`XXH_LOWOPT_DEF`); a hand-rolled `-Og` build without it now fails
  at compile time rather than silently paying 3x.

- **`XXH3_STREAM_USE_STACK` is now set explicitly.** xxhash defines it for every
  compiler except clang, and without it clang keeps the eight XXH3 accumulator
  lanes in the heap-allocated state instead of registers, so streaming ran at a
  third of the one-shot rate. With it they are at parity (51.5 against 51.4 GB/s
  over 64MB). No digest, format or API change: both flags affect inlining only.

- **The `VERSION` in the Makefile said 2.5.0 and had done since the 2.5 series**,
  so `zeroskip.pc` and the library's version string were nine minor releases
  behind the changelog. Corrected to match; no code change.

- Benchmark fix: `zsbench`'s key generator overflowed `int` above 271181
  records, so any read figure recorded above that size on an earlier build was
  measuring a smaller working set than its row claimed.

## 2.9.1 — 2026-08-19

- Documentation fixes, no behaviour change. `rollover_txns` said a handle with
  `index_dir` set would never reach the span bound; that stopped being true at
  P-13, and it is the bound that ends generations for a caller committing one
  record at a time — so it is also what a `zs_db_seal` cadence has to pace
  against. `zs_db_seal` now documents that cadence, and that sealing frequently
  with the repack cascade armed *raises* write amplification.

- `test_lock_two_threads_two_handles` verifies the thread-safety contract 2.9.0
  documented, including C-1j's spinlock under contention.

## 2.9.0 — 2026-08-19

- **The merge path now tells the kernel it is about to read an input.** Both a
  conversion and a repack open with a pass that hashes a whole input file, and on a
  filesystem whose page cache is separate from its own cache — ZFS — that pass is
  dominated by servicing one synchronous fault per page rather than by hashing.
  A `posix_madvise(POSIX_MADV_WILLNEED)` per input, which downstream's call graph
  makes the largest single item in a bulk-load profile. Advisory: no behaviour
  change, and unmeasurable where the pages are already resident.

- **Thread safety is now documented** in `zeroskip.h`, which said nothing about it.
  The answer was already in the spec (G-5) but not where a caller would look: a
  handle is not thread-safe, and a second thread with its **own** handle is
  supported and excludes the first exactly as a second process would (C-1j).

- `zs_db_seal` documents that it is the latency lever for conversions, which
  `ZS_NOAUTOREPACK` is not: a conversion is unavoidable per generation and by
  default lands on the commit that crosses `rollover_size`.

## 2.8.0 — 2026-08-19

- **A large `rollover_size` is no longer punished by the private index.** The
  index's delta was merged into its base once per fixed number of inserts, and each
  merge is proportional to the whole index, so the merging was quadratic in
  generation size — which is why a 64MB `rollover_size` was slower than the 2MB
  default while rewriting a quarter of the bytes. The bound is now proportional to
  the index, making it linear: 2M records with random keys at a 64MB rollover went
  from 7.1–7.4s to 1.9–2.2s. Behaviour at the default is unchanged — measured
  identical merge counts, not merely similar. No API or format change.

## 2.7.0 — 2026-08-19

- **An abort whose records never left the writer's buffer now writes nothing at
  all** — no records, no `ROLLBACK`. With nothing in the file there is nothing for
  a `ROLLBACK` to void, so probe-then-abort callers stop paying a write and stop
  leaving dead bytes in the active file. See C-8b.

- Consequence for anyone comparing bytes across implementations: **a rolled-back
  span is no longer something a conforming writer can be required to produce**,
  since whether one appears depends on how much the writer buffers. Reading them
  is unchanged and still mandatory (F-21, F-25). `tests/corpus/rolled-back-span`
  now injects the span rather than aborting a transaction; no golden byte changed.

- Fixes a latent bug found while doing the above: a failed buffer flush during a
  read inside a transaction (`zsi_txn_at`) left the transaction committable over a
  possibly-torn span. A failed flush now poisons the span, so commit refuses.

## 2.6.0 — 2026-08-18

- **A commit now syncs once, not twice.** The sync between a span's records and
  its terminator is gone; the terminator's checksum already makes a terminator
  that reaches disk without its data read as absent (F-22), so ordering the
  writes made that case impossible rather than merely detectable. Worth +81% on
  one-record transactions and +11% at a thousand. See C-7.

- Behaviour change for callers handling a commit error: a failed commit is now an
  **unknown** outcome, never "it did not happen". Under two gates a first-gate
  failure guaranteed no terminator had been written, but both gates returned the
  same error, so no caller could tell them apart. Check rather than assume. See
  C-7a.

- **The writer's append buffer grows on demand**, from 64KB to a 4MB ceiling,
  instead of being fixed at 64KB. With one gate a still-buffered span is written
  once together with its terminator, so the buffer now decides how much commit
  traffic takes that path: a 1000-record transaction goes from three writes to
  one, and a 20 000-record one from 42 to 2. Growing rather than allocating the
  ceiling keeps a small transaction costing what it did before.

- No format change: same records, same terminators, same checksums.

## 2.5.0 — 2026-08-18

- **A commit no longer publishes a pointer table.** Publishing amortises a replay,
  and the commit path never replays — it folds (D-13b) — so a table written there
  cost the writer with no benefit to it, and at the default `rollover_size` was
  mostly waste besides: a load publishes many tables into each generation before
  sealing it, and a sealed generation needs none. Worth 22% of a cached
  2M-record load, 38% at a 64MB rollover. The tables still appear, published by
  whoever built an index by replaying — including the same writer at its own open.
  What is given up is a consumer that can never publish, such as a read-only
  mount whose writer never replayed: it replays at every open. See P-13.

- Note for callers with a cache configured and small transactions: because a
  writer no longer moves its own replay window, `rollover_txns` (D-9d) now governs
  as it does without a cache, so such a load seals more often. Measured faster
  overall regardless (200k records one per transaction: 9.82s to 9.15s).

## 2.4.0 — 2026-08-18

- A **read-only handle now creates the `ZS_INDEX_LOCAL` cache directory** instead
  of running without a cache when it is absent. It could always publish tables
  into that directory, so refusing only the directory creation meant enabling the
  flag on a read-mostly database did nothing until an unrelated write came along.
  A read-only mount is unaffected — the creation fails and the handle continues
  uncached. See P-2b and R-3.

## 2.3.0 — 2026-08-18

- **The default pointer-table publish threshold now scales with the file** it
  describes rather than being an absolute 32KB, which it still works out to at the
  default `rollover_size`. It fixes two opposite problems: a large generation
  published ~2000 times whatever its size, each rewriting the whole table, so the
  cost was quadratic in generation size (4.70GB of writes and 4.69s against 1.91s
  on a 2M-record load at a 64MB rollover); and a small database with a large
  rollover configured barely published at all and paid for it at every open. A
  caller passing a non-zero `index_threshold` is unaffected. See A-9.

## 2.2.1 — 2026-08-18

- Fixes a regression in 2.2.0: single-record transactions were about 20% slower
  (144k to 121k stores/s at 20k records under `ZS_NOSYNC`), because the commit
  fold allocated and copied the whole delta per commit. It now merges in place,
  which is faster than 2.2.0 at every transaction size and than 2.1.1 at all of
  them. Reported from the SQLite engine's `nosync` row, where the two commit
  gates do not hide it.

## 2.2.0 — 2026-08-17

- New `zs_db_stats(db, struct zs_db_stats *)` reports what this handle has
  rewritten since it was opened — repacks and conversions counted separately,
  with records, bytes and time for each. It answers "how much of my write cost
  went on rewriting what I had already written", which a caller could not
  previously tell from outside. Observability only; no policy changes. See A-17.

- Committing is faster, and no longer slower per record for a larger
  transaction: the commit fold merges the span as a sorted run rather than
  inserting each record. 21% at 1000 records per transaction, 95% with 200k in
  one. See D-13b in the spec.

- Storing is faster: one walk of the pending set per store rather than two.
  About 15% at 1000 records per transaction, 42% with 200k in one.

## 2.1.1 — 2026-08-17

- New `zs_open_data.repack_max_size` (default 512MB) bounds what one repack
  rewrites: a file above it no longer starts a merge. A no-op below that size.
  See A-16 and D-16 in the spec for the trade and the file-count bound.

- `struct zs_open_data` grows a field — rebuild anything that links
  `libzeroskip` rather than vendoring `zeroskip.c`.

## 2.1.0 — 2026-08-17

- **Repack behaviour has changed**: it merges sooner and leaves fewer files. A
  database repacked by 2.1.0 ends up with a different file layout than one
  repacked by 2.0.0. Both are valid and readable by either version; the on-disk
  format is unchanged.

- `zsbench` gains `full scan, no verify`, which prices `ZS_NOCSUM`.

## 2.0.0 — 2026-08-16

First release of this implementation — a rewrite rather than a revision. It does
not read databases written by the original zeroskip.

- Append-only ordered key-value store: a directory of immutable and append-only
  files, with no manifest. Filenames carry each file's generation range.
- Lock-free readers with snapshot isolation. One writer, enforced by `fcntl`
  locks a peer implementation can observe.
- Nothing is ever mutated: files are appended to, or published by `rename`, so a
  crash needs no recovery pass beyond opening the database.
- Format, protocol and recovery specified normatively in
  [`doc/specification.md`](doc/specification.md), with a language-neutral golden
  corpus (`tests/corpus/`) as the byte-level contract.
- C99 and POSIX, no external libraries.
- Three checksum engines, out-of-band repack and compaction, in-place seal, and a
  salvage path that reads what the ordinary read path refuses to.
