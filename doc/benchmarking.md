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
| `snapshot open` | the per-open replay cost, without a pointer table |
| `open (cached)` | the same open with one — what spec section 8 buys |
| `publish threshold` | what P-13's threshold trades, in both directions |
| `compaction` | what compacting costs and what it reclaims (D-26, D-27) |

The `store, one txn each` figure is dominated by `fdatasync` and therefore measures
the *filesystem*, not zeroskip. Compare it against `store, 1000 per txn` on the same
machine: the ratio is the cost of durability, not of this library.

## The pointer table cache (spec section 8)

The spec's second open item asked whether the per-open replay of the active file
was worth removing, and said the answer should not be guessed. It was worth
removing, and by more than the question implied.

### Opening

Without a table, open cost is linear in the number of records in the active file
— roughly **1.5 ms** at the 2 MB default `rollover_size`, flat at about
0.1 µs/record. With one it is flat in absolute terms, because the replay is
bounded by the threshold rather than by the file:

| records in the active file | active file | plain open | cached open | speedup |
|---|---|---|---|---|
| 250 | 31 KB | 0.06 ms | 0.08 ms | 0.8× |
| 1 000 | 125 KB | 0.13 ms | 0.08 ms | 1.6× |
| 4 000 | 500 KB | 0.41 ms | 0.08 ms | 4.9× |
| 16 000 | 2 000 KB | 1.56 ms | 0.09 ms | **17.7×** |

**Note the first row.** Below about 500 records the cache is a small *loss*:
opening a table, reading it and validating it costs more than the replay it
saves. That is the honest shape of the trade, and it is why the feature is
opt-in rather than on by default.

### Writing, which is the bigger effect

A write transaction refreshes its snapshot at begin (C-4), and that refresh
replays the active file **from the last published `valid_upto`**. With no table
there is no published point, so every commit replays the whole file and a
one-store-per-transaction load is quadratic. 16 000 such transactions over a
2 MB active file:

| threshold | store | open | steady-state table |
|---|---|---|---|
| no cache | 26.8 s | 1.60 ms | — |
| 1 byte | 6.5 s | 0.09 ms | 125 KB |
| 4 KB | **2.7 s** | 0.09 ms | 125 KB |
| 32 KB | **2.8 s** | 0.10 ms | 124 KB |
| 256 KB | 6.5 s | 0.37 ms | 112 KB |
| 1 MB | 18.6 s | 1.33 ms | 64 KB |

So the cache is a **10× improvement on writes** in this workload, not only on
opens — and that was not the effect it was built for.

### Reading the threshold curve

It is U-shaped, and both ends are real:

- **Too low**, and every commit pays an O(records) merge and rewrites the whole
  table. Threshold 1 byte writes roughly a gigabyte of tables over one
  generation. It costs 2.4× the minimum rather than something catastrophic,
  because a table is never `fsync`ed (P-14).
- **Too high**, and the refresh replay grows without bound. This is the
  expensive end: 1 MB costs 7× the minimum, and approaches having no cache
  at all.

The default is **32 KB**, from the flat region, capped at a quarter of
`rollover_size` so a caller using small generations still publishes within one.
It is an absolute byte count rather than a fraction of `rollover_size`, because
the knee is set by how much data a replay walks — which has nothing to do with
how large a caller lets a generation grow. An earlier `rollover_size / 8` put
the default at 256 KB, squarely on the wrong side.

### What it costs

The cache directory must be scoped to the database instance (P-17): a table
outlives an out-of-band restore of the database directory, and P-10's binding
checks one span rather than the whole prefix. A per-boot temporary directory
satisfies that; restoring a database from backup means discarding its tables.

## Compaction (D-26, D-27)

`zs_db_compact` merges the whole database into one file. The cost is
straightforward — it rewrites everything — so the number worth having is what it
**reclaims**, measured against a database that has already been sealed and
repacked as far as D-16's rule goes. That is the baseline a caller already has,
so the column shows what compaction adds rather than what repacking would have
done anyway.

20 000 records, 100-byte values, 64 KB rollover:

| deleted | after repack | after compact | reclaimed | compact |
|---|---|---|---|---|
| 0% | 2 426 KB | 2 422 KB | 0.2% | 7.5 ms |
| 25% | 2 563 KB | 1 817 KB | 29.1% | 8.2 ms |
| 50% | 2 699 KB | 1 211 KB | 55.1% | 7.7 ms |
| 75% | 2 836 KB | 606 KB | **78.7%** | 8.6 ms |

Two things to read from it:

- **Reclamation tracks the deletion rate almost exactly.** That is D-27 working:
  only a merge whose output spans the whole generation interval can drop a
  tombstone, because only then does D-19's containment test succeed for every
  key. A partial repack must keep them, since a file outside its input set may
  still hold the key (D-19a) — which is why the "after repack" column *grows*
  with the deletion rate while the compacted one shrinks.
- **With nothing deleted there is almost nothing to reclaim.** Compaction is not
  a general-purpose optimisation; it is for databases that have deleted a lot,
  or that want to be a single file.

**Compaction is unbounded** (D-29). These timings are for a 2 MB database; the
cost is linear in total size, and one call rewrites all of it while writers
continue. That is spec open item 1's unboundedness made a deliberate API entry
point rather than an emergent property of D-16's cascade. `zs_db_seal` is the
bounded half — at most `rollover_size` of work — and is what to reach for if the
goal is only to stop readers replaying the active file.

## Reading the rollover rows

The `store, rollover Nk` figures are not monotonic, and that is expected rather
than noise. A smaller rollover means more frequent conversions, each individually
cheaper (D-12d), but also more files — so lookups during the run get slower, and the
ancestor search a write performs (F-17) walks more sources. The interesting
question is not which row is fastest but whether any row *collapses*, which would
mean a writer's inline cost is not bounded after all.
