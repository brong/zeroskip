# Changelog

Semver: MAJOR for an ABI break, MINOR for features and for observable changes in
behaviour, PATCH for fixes.

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
