# Changelog

Semver: MAJOR for an ABI break, MINOR for features and for observable changes in
behaviour, PATCH for fixes.

## 2.1.2 — 2026-08-17

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
