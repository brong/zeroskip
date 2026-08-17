# Changelog

Semver: MAJOR for an ABI break, MINOR for features and for observable changes in
behaviour, PATCH for fixes.

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
