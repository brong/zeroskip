# Benchmarking zeroskip

```
make bench          # --selftest, then a small smoke run
./zsbench           # the full run
./zsbench -n 100000 --value 500 --reps 5
```

`--selftest` exists because a benchmark that silently ran against a failed open
would report excellent numbers. It stores and reads back 500 records and runs the
consistency checks, then exits.

Workloads are kept comparable with the sibling `twom` library's `twombench`, so
numbers can be read side by side.

## What each workload is for

| Workload | What it measures |
|---|---|
| `store, one txn each` | the worst case: two `fdatasync` calls per record (C-7) |
| `store, N per txn` | how much batching amortises those two syncs (C-7b) |
| `fetch (N files)` | lookup cost, before and after a repack — it is proportional to the **number of files** (D-14d) |
| `full scan` | the merge cursor over whatever file set exists |
| `store, rollover Nk` | whether a writer's inline conversion really is bounded by `rollover_size` (D-12d) |
| `repack cascade` | one unbounded cascade (D-16b, open item 1) |
| `snapshot open` | **open item 2's number** — see below |

The `store, one txn each` figure is dominated by `fdatasync` and therefore measures
the *filesystem*, not zeroskip. Compare it against `store, 1000 per txn` on the same
machine: the ratio is the cost of durability, not of this library.

## Open item 2: is a shared index worth reintroducing?

The spec's second open item asks whether the per-open replay of the active file is
worth removing, and says explicitly that the answer should not be guessed:

> Every snapshot now replays the active file, bounded by `rollover_size` but paid
> per open. If measurement shows that cost matters, the way to share it without
> reintroducing in-place mutation is an append-only `(key, offset)` log per file
> published by a single aligned atomic — readers read the immutable prefix and sort
> privately. […] Not worth building before there is a number.

`zsbench` produces the number. A representative run (Apple M-series, APFS,
100-byte values, a rollover large enough that nothing converts):

| records in the active file | active file | open | per record |
|---|---|---|---|
| 250 | 31 KB | 0.06 ms | 0.24 µs |
| 1 000 | 125 KB | 0.12 ms | 0.12 µs |
| 4 000 | 500 KB | 0.41 ms | 0.10 µs |
| 16 000 | 2 000 KB | 1.54 ms | 0.10 µs |

**Per-record cost is flat, so open cost is linear in the number of records in the
active file.** At the 2 MB default `rollover_size` that is a ceiling of roughly
**1.5 ms per open**.

How to read that:

- For a long-lived handle it is irrelevant — it is paid once.
- For a process that opens a database per request it is the dominant cost, and the
  shared index would be worth building.
- The ceiling is set by `rollover_size`, so a caller that opens frequently can buy
  most of the win today by lowering it — at the cost of more files, and therefore
  slower lookups (D-14d) and more repacking.

That last point is the one to check before writing any new code: **measure with a
smaller `rollover_size` first.** `./zsbench` prints `store, rollover Nk` rows for
exactly that comparison.

## Reading the rollover rows

The `store, rollover Nk` figures are not monotonic, and that is expected rather
than noise. A smaller rollover means more frequent conversions, each individually
cheaper (D-12d), but also more files — so lookups during the run get slower, and the
ancestor search a write performs (F-17) walks more sources. The interesting
question is not which row is fastest but whether any row *collapses*, which would
mean a writer's inline cost is not bounded after all.
