# Benchmarking zeroskip

```
make bench          # --selftest, a small smoke run, then the phase check
./zsbench           # the full run
./zsbench -n 100000 --valsize 500 --reps 5
./zsbench store     # only workloads whose name contains "store"
./zsbench --csv out.csv
```

`--selftest` exists because a benchmark that silently ran against a failed open
would report excellent numbers. It stores and reads back 500 records and runs the
consistency checks, then exits.

Every workload runs `--reps` times and reports the **median**. Until 2026-08-14
only `store, one txn each` did — the other nine timed a single run while the
banner printed the repetition count over the whole report — so figures recorded
before that date are single runs whatever they say. The median is used rather
than the best because the minimum is the luckiest run, and for the workloads
dominated by `fdatasync` it is the one where the filesystem happened to be idle.

Repetitions are not free of setup: a workload that mutates rebuilds its database
each time. `repack cascade` most of all, since a cascade consumes the file
layout it measures — timing the same database twice would find nothing to merge
on the second pass and post an excellent number for doing nothing.

## Comparing against twom

The sibling `twom` library's `twombench` writes the same CSV schema, and
`tests/benchcmp.sh` runs both with matching flags and joins the results:

```
./tests/benchcmp.sh -n 200000 --keysize 16
TWOMBENCH=/path/to/twombench ./tests/benchcmp.sh
```

The long options (`--records`, `--keysize`, `--valsize`, `--reps`, `--csum`,
`--path`, `--csv`, and a bare name filter) are spelled as twombench spells them
so both tools take the same words. zsbench's original `--value` and `--dir` still
work. `--keysize` defaults to 11, zsbench's historical `key%08d` shape, so the
default run is unchanged and every figure below remains comparable; a
side-by-side run should pass the same `--keysize` to both.

**The overlap is partial, and the script prints what it could not pair.** Only
one row is durability-matched — twom's `fillsync` against `store, one txn each`,
both committing per record. `fillseq` puts every record in one transaction while
zeroskip's nearest batched figure commits every 1000, so that row compares
batching policy as much as it compares libraries. twom has no file set, so
nothing on its side corresponds to rollover, conversion, pointer tables or
compaction; zeroskip has no mutable-file operations to answer `overwrite` or
`deleterandom`.

## What each workload is for

| Workload | What it measures |
|---|---|
| `store, one txn each` | the worst case: two `fdatasync` calls per record (C-7) |
| `store, N per txn` | how much batching amortises those two syncs (C-7b) |
| `store, all in one txn, random` | the same bulk load with keys arriving in a different ORDER — the pending set's insertion cost, which was quadratic until it became a skiplist |
| `fetch (N files)` | lookup cost, before and after a repack — it is proportional to the **number of files** (D-14d) |
| `full scan` | the merge cursor over whatever file set exists — but see below: its files do not overlap, so the merge never merges |
| `full scan, interleaved` | the same scan with files that DO overlap: D-14e's re-sort moving an arm at nearly every step |
| `full scan, shadowed` | every key in two files: duplicate suppression and the full re-sort |
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

### Writing: who pays the replay

A snapshot **rebuild** replays the active file from the last published
`valid_upto` — and with no table there is no published point, so the replay is
the whole file. What changed on 2026-08-13 is *when a writer rebuilds*: a
write begin runs the same C-4i probe a read does, and a sole writer's steady
state rebuilds nothing at all — its commits keep the snapshot current through
the D-13b fold. The replay is paid at open, and at any begin that follows
another process's commit. (Before the probe, every write begin rebuilt, and
single-record commit throughput decayed linearly with the active file's size —
a sawtooth against `rollover_size`, found by a downstream benchmark.)

16 000 single-store transactions over a 2 MB active file, in both shapes —
"rebuild per begin" is measured with the pre-probe writer, which is what an
alternation of one-store transactions across processes still pays, since every
begin there follows another process's commit:

| threshold | sole writer | rebuild per begin | open | steady-state table |
|---|---|---|---|---|
| no cache | **1.2 s** | 13.2 s | 1.5 ms | — |
| 1 byte | 5.0 s | 5.7 s | 0.09 ms | 125 KB |
| 4 KB | 1.4 s | **2.0 s** | 0.09 ms | 125 KB |
| 32 KB | **1.3 s** | **2.0 s** | 0.09 ms | 124 KB |
| 256 KB | 1.2 s | 3.5 s | 0.13 ms | 121 KB |
| 1 MB | 1.3 s | 8.8 s | 0.13 ms | 121 KB |

So for the alternating shape the cache is still a **6× improvement on
writes**, not only on opens. For the sole writer it is no longer a write-side
effect at all — no cache is its fastest configuration, because the only
replays left in that run are a handful of opens.

### Reading the threshold curve

The two shapes disagree about which end is expensive, and the default must
serve both:

- **Too low** costs the *writer*, in both shapes: every commit pays an
  O(records) merge and rewrites the whole table. Threshold 1 byte writes
  roughly a gigabyte of tables over one generation, and is the one setting
  that makes a sole writer 4× *slower* than having no cache at all. Never
  catastrophic, because a table is never `fsync`ed (P-14).
- **Too high** costs whoever *rebuilds*: at 1 MB the alternating shape pays
  4× its minimum and approaches having no cache at all. The sole writer never
  notices — its curve is flat from 4 KB up — which is why this end has to be
  measured with a rebuild forced per begin: the shape that pays it is not the
  one a single-process benchmark runs.

The default is **32 KB**, from the region both shapes tolerate, capped at a
quarter of `rollover_size` so a caller using small generations still publishes
within one. It is an absolute byte count rather than a fraction of
`rollover_size`, because the knee is set by how much data a replay walks —
which has nothing to do with how large a caller lets a generation grow. An
earlier `rollover_size / 8` put the default at 256 KB, squarely on the wrong
side of the rebuilding shape's knee.

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

## The machines these numbers came from

An ops/sec figure means little without one, and the two used here differ by
about 3.5x per record:

| | |
|---|---|
| **laptop** | Apple M5, macOS, clang |
| **server** | AMD EPYC 7402P (Zen 2), 24C/48T, ~2.75 GHz effective, L1d 32 KiB/core, L2 512 KiB/core, L3 16 MiB/CCX, Linux, GCC |

Per record, which travels better than ops/sec:

| | 4-file scan | compacted (k=1) | merge overhead |
|---|---|---|---|
| EPYC 7402P | 52.8 ns (145 cycles) | 36.8 ns (101 cycles) | 16.0 ns / 44 cycles |
| Apple M5 | 18.9 ns | 12.4 ns | 6.5 ns |

Two things follow that are easy to get wrong the other way round. The **arms
array is about 640 bytes** for a four-file cursor, which is 2% of the server's
32 KiB L1d — so there is no cache *capacity* effect to find on either machine,
and any per-arm size effect can only be stride and line-crossing. And the merge
overhead is **44 cycles per record** on the server against roughly a third of
that on the laptop, so a merge-loop optimisation measured only on fast silicon
is being measured where it matters least.

Most figures recorded in this file and in CLAUDE.md were taken on the laptop.
Treat them as ratios, not absolutes.

**The compiler is part of the machine.** A GCC `-O2` build of the merge loop
carries four call frames a clang build does not have symbols for — GCC's
auto-inline budget is much tighter than clang's — which is why `zsi_cur_order`,
`zsi_ptrs_at`, `zsi_ptrs_rec` and the fast half of `zsi_cursor_refresh` say
`inline` explicitly. Before that, the EPYC profile spent about 12% of its
samples in those calls; saying `inline` was worth +8.8% on a four-file scan and
+8.0% compacted, and nothing at all on clang. So a scan figure is a
(machine, compiler) pair, and the two columns above were also a comparison of
clang against GCC without saying so.

## Fixtures are built with ZS_NOSYNC

A read-only workload's database is setup, not measurement, so `bench_fetch`,
`bench_scan` and the pre-existing database `store into N files` writes into are
all built with `ZS_NOSYNC` (`FIXTURE_FLAGS`). That skips C-7's two commit gates
and **nothing else** (C-6b), so the fixture is byte-for-byte the file set a
durable build produces — still one span per record, which is the fragmented
shape those workloads exist to read. Only the durability of writing it differs,
and nothing measures that.

The timed regions are untouched: `store into N files` reopens without it,
because its subject *is* the store.

It matters more than it sounds. On a network-backed mount a 500k-record scan
fixture took **435 seconds to build and 0.25 seconds to scan**, so `perf stat`
over the whole process reported the `fdatasync` storm and said nothing at all
about the merge loop. Locally the same run went from 20.5s to 3.8s with the
scan rate unchanged. A benchmark you cannot profile is most of the way to a
benchmark you cannot trust.

## Profiling only the run phase

`ZS_NOSYNC` took the `fdatasync` storm out of the fixture build. `--setup` and
`--run` take the build out of the **process**, which is what a profiler samples:

```
./zsbench --path=/tmp/fix --setup -n 200000 scan
perf record -g ./zsbench --path=/tmp/fix --run --reps 20 scan
```

At 200 000 records that is 4.2 s of setup against **0.04 s** in the profiled
process, reporting the same per-record rates as the combined run — so every
sample belongs to a merge cursor rather than to a writer. `--setup` prints the
second line for you, filter and all.

`--setup` builds the fixtures under `--path` and exits, leaving them behind.
`--run` builds nothing: it times what is there and leaves it, so the same
fixture can be profiled again with different events. Both need an explicit
`--path`, because the default working directory carries the pid.

**Raise `--reps`.** A run phase is short by design — 40 ms for the three scan
lines at 200 000 records — which is too few samples to profile. `--reps` is not
part of the fixture, so it is the free knob; `-n` is the other, and costs a
rebuild.

**A `--run` adopts the parameters it was not given.** `--setup` writes
`zsbench.setup` recording `-n`, `--keysize`, `--valsize` and `--csum`, and
`--run` takes each of them from there unless it was told otherwise — so the
invocation anyone types, `--path=DIR --run scan`, works at any fixture size. A
parameter that *is* given must match, and the run is refused if it does not: a
run phase at the wrong `-n` would fetch keys that were never stored and report
an excellent number for missing every time, which is the failure `--selftest`
exists to prevent arriving by a different door.

That refusal used to be reachable by copy-pasting the command `--setup` printed,
which omitted `-n`. It is worth knowing what a refusal looks like from inside
`perf`, because it does not look like an error:

```
[ perf record: Captured and wrote 0.019 MB perf.data (12 samples) ]
   34.24%  zsbench  [kernel.kallsyms]     [k] zpl_getattr
   30.17%  zsbench  ld-linux-x86-64.so.2  [.] do_lookup_x
```

Twelve samples, all of them the dynamic linker and the `stat` of the stamp: a
profile of a process that exited before it started. `tests/benchphases.sh` runs
the command `--setup` prints, for this reason.

**The filter belongs to the run phase.** `--setup` builds all five fixtures
whatever filter it is given, so the two invocations cannot disagree about which
exist; `fetch (N files)` could not be tested against a filter before the files
it names have been built anyway.

**Only the read-only workloads split**, and the run phase names the ones it
skipped rather than leaving a short report to be read as a full one:

| splits | reason it does not |
|---|---|
| `fetch`, `fetch, repacked` | |
| `full scan`, `full scan, compacted` | |
| `scan in a write txn, few pending` | partly — its `nrecs/16` pending overwrites are redone in the run phase |
| | the store workloads: setup **is** the measurement |
| | `scan in a write txn`: its setup is a live transaction, which is process state |
| | rollover, repack cascade, the open and threshold sweeps, compaction: each mutates what it measures and rebuilds per repetition |

One consequence in every mode, not only the phase ones: `fetch, repacked` and
`full scan, compacted` now get **their own fixture directory**, copied from the
first rather than stored again. They used to repack and compact the fixture they
had just timed, in place, which is fine while one process does both lines in
order and wrong the moment the fixture is reused — a second `--run` would have
timed the compacted database under the fragmented line's name and reported the
merge overhead those two lines exist to isolate as zero. The cost is disk: a
scan or fetch fixture is on disk twice.

## `full scan` does not measure a merge

Worth knowing before optimising anything in the merge loop, and it took a
profile to notice. zsbench stores keys in ascending order, so every generation —
and every repack output built from them — holds a contiguous key range that no
other file touches:

```
gen 00000001-00000080  131072 recs  key00000000 .. key00131071
gen 00000081-000000C0   65536 recs  key00131072 .. key00196607
gen 000000C1-000000C2    2048 recs  key00196608 .. key00198655
gen 000000C3-000000C3    1024 recs  key00198656 .. key00199679
```

One arm is live and the rest sit on first keys above everything being yielded.
D-14e's re-sort declines to move at every step but the three file boundaries,
and the duplicate scan breaks on its first comparison every time. So the line
measures decode and checksums with three idle arms, and the ~30% it gives up
against the compacted line is arm-array stride plus one extra comparison per
record — not merge work.

`full scan, interleaved` and `full scan, shadowed` are the shapes that do merge.
They differ from `full scan` in the **order of the stores** and nothing else:

- **interleaved** stores keys in a strided permutation, so each generation is
  scattered across the whole key range and every file overlaps every other. The
  winning arm changes at nearly every step, so `zsi_cur_resort_head` lifts a
  160-byte arm and shifts the array instead of returning after one comparison.
- **shadowed** stores every key twice, in two files no cascade merges — one
  transaction per pass, since a commit crossing `rollover_size` seals the
  generation itself (D-25d), and `ZS_NOAUTOREPACK` so the cascade does not put
  the two passes back together. Every step finds its key duplicated, so step 3
  advances the stale arm too and the step ends in a full `zsi_cur_sort`.

At 20 000 records, 100-byte values, on the laptop:

| | records/s | vs `full scan` |
|---|---|---|
| `full scan, compacted` (k=1) | 58.0M | +20% |
| `full scan` (4 files, disjoint) | 48.2M | — |
| `full scan, interleaved` | 37.8M | −22% |
| `full scan, shadowed` | 29.1M | −40% |

So the merge machinery costs about 40% when it is actually used, and roughly
nothing on the workload that was being profiled. A change to
`zsi_cur_resort_head` or to the arm layout has to be measured against the lower
two rows; measured against `full scan` it would be measuring a workload that
never calls the expensive half.

## The pending set was quadratic in a transaction's size

The read-path lesson again, from the other end. Every store workload here wrote
keys in **ascending order**, which is the best case for `zsi_pend_set`: it keeps
a transaction's pending records in one sorted array and splices each new key in
with a `memmove`, and an ascending key always lands at the end and moves
nothing. A key arriving anywhere else moves half the array.

`store, all in one txn, random` is that same bulk load over the same keys in a
strided permutation. Its throughput used to **halve with every doubling of `n`**
— total time going up by four — and now tracks the ascending line:

| n | ascending | random, sorted array | random, skiplist |
|---|---|---|---|
| 25 000 | 2 308 000/s | 466 000/s | 1 933 000/s |
| 50 000 | 2 153 000/s | 245 000/s | 1 791 000/s |
| 100 000 | 1 691 000/s | 124 000/s | 1 481 000/s |
| 200 000 | 1 090 000/s | ~40 000/s | 1 110 000/s |

The pending set is a skiplist now — in one arena, linked by offset, the shape of
Cyrus's skiplist file — so a store is O(log n) with no bulk movement and the
arrival order stops mattering: at 200 000 records the random line and the
ascending line are the same figure. **It costs about 10% on the ascending case**
(1 697 000/s → 1 519 000/s at 100 000, medians of seven), which is the shape the
array was perfect for — an append into a slot that already existed. The cost is
monotonic in transaction size and disappears at the small end, where the two
commit gates swamp it:

| records per transaction | sorted array | skiplist |
|---|---|---|
| 10 | 201 284/s | 201 602/s |
| 100 | 1 018 599/s | 962 557/s |
| 1 000 | 2 241 840/s | 2 058 453/s |
| 100 000 | 1 696 903/s | 1 519 389/s |

Readers never paid it: a commit that large crosses `rollover_size` and seals
itself (D-25d), so a reader gets a pointer section rather than a replay.

## Key length, key shape, and the pending set's inline bound

A comparison's cost depends on where two keys first differ, so both matter and
`--keysize` alone cannot express it: use `--keyshape head` (varies early — a
message-id, a `G`+40-random key, a sqlite index key) or `--keyshape shared`
(agrees for a long head — Cyrus's `Ndomain!user.foo` runs).

The pending set inlines a key up to **64 bytes** in its node and reads the
record only when two keys both run past that and agree within it. Inlining the
whole key instead is faster nearly everywhere, and was rejected on memory: an
entry becomes O(keylen), so a million 1 200-byte message-ids in one transaction
is 1.2 GB of pending set against ~112 MB. Random-order bulk load, 100 000 keys,
medians of five:

| keysize | shape | whole key inlined | bounded at 64 |
|---|---|---|---|
| 12 | head | 1 613 293/s | 1 487 695/s (−7.8%) |
| 41 | head | 1 562 183/s | 1 506 661/s (−3.6%) |
| 200 | head | 1 091 107/s | 1 063 988/s (−2.5%) |
| 1 000 | head | 542 847/s | 569 062/s (**+4.8%**) |
| 41 | shared | 1 472 125/s | 1 375 326/s (−6.6%) |
| 200 | shared | 872 189/s | 717 690/s (−17.7%) |

Two alternatives were measured and rejected. Storing **no key at all** and
deriving it from the record every time costs 16–23%: an inlined key shares the
node's cache line, a derived one is a second line in a multi-megabyte mapping
plus a decode. A **32-byte** bound is worse than 64 wherever keys share a
structured head — `Ndomain!user.foo` runs measured 40% down — and no better
anywhere else.

## Measuring on the server: pair the runs, and print what you measured

Two things went wrong here on one afternoon, and both produced numbers that
looked like results.

**Unpaired runs cannot see 6%.** A single `--run` of the same workload on the
same fixture varies by more than that on a shared machine: the scan figures that
prompted a regression hunt were 11.5M/s and 9.4M/s from two such runs, and the
real difference was 6.3%. Alternate the builds inside one loop, take medians of
five or more pairs, and count how many pairs agree — 5 of 5 in one direction is
a result, 3 of 5 is noise.

**Print the commit you are about to measure.** Three separate runs measured the
wrong tree: a stale checkout whose `zsbench` predated `--path` (its usage dump
scrolled past inside a `grep`), a `git pull` onto a detached head that silently
did nothing, and a fetch against a stale origin that resolved "latest" to a
commit from two days earlier. Each printed something plausible. The loop below
asserts its own state instead:

```
for i in 1 2 3 4 5; do
  for d in /tmp/zs-old /tmp/zs-hd; do
    printf '%-9s %-40s ' "$(git -C $d rev-parse --short HEAD)" \
                         "$(git -C $d log -1 --format=%s | cut -c1-38)"
    $d/zsbench --path=FIXTURE --run --reps 10 shadowed | grep shadowed
  done
done
```

And when a regression does appear, `perf stat -e cycles,instructions,cache-misses,branch-misses,L1-icache-load-misses`
separates the cases before any guessing: flat instructions with more cycles is a
memory or layout effect, more instructions is real work.

## Reading the rollover rows

The `store, rollover Nk` figures are not monotonic, and that is expected rather
than noise. A smaller rollover means more frequent conversions, each individually
cheaper (D-12d), but also more files — so lookups during the run get slower. The
interesting question is not which row is fastest but whether any row *collapses*,
which would mean a writer's inline cost is not bounded after all.

Writes themselves no longer scale with the file count at all. They used to: a
store had to resolve its record's ancestor first, which searched every source
for the key (F-17, retired 2026-08-15). `store into N files` measures what that
cost — about 0.09µs per file in the set, per store — and it is now zero, with
stores into a populated database running at the empty-database rate.
