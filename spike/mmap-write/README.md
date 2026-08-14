# mmap-write spike (throwaway — **concluded: do not land**)

> **Answer, 2026-08-14.** Measured on ZFS, the filesystem that decides it: the
> plain `write(2)` writer beats every mmap shape on the gated transaction, by
> 60% against the v4 design. The `/tmp` run that suggested otherwise was
> measuring `/tmp`'s ~1157 µs syncs, not the stream path. Details in
> [ZFS, measured](#zfs-measured-2026-08-14-stl-imap-09-mntvfstmpbrong--the-answer)
> below. The branch stays as the record of the question and the harness that
> answered it; the `-DZS_MMAP_WRITE` code is not a landing candidate.

`-DZS_MMAP_WRITE` replaces the streaming writer's chunk-buffer + `write(2)`
with a giant per-generation `PROT_READ|PROT_WRITE MAP_SHARED` window over the
active file and memcpy stores. Physical extension precedes every store (a
MAP_SHARED store past `st_size` is SIGBUS, not extension); the invariant
`st_size == logical end` holds at every unlocked state, so peers, the C-4i
probe, and R-4 recovery are untouched. Extension policy, measured on Darwin:
exact 1-byte `pwrite` per store by default (no slop ever exists), switching
to chunked ftruncate-ahead + one exact down-truncate at the terminator when
the previous span predicts a bulk load.

Built and measured 2026-08-14 on macOS/APFS. **Verdict there: parity at
best.** Gated single-store / rollover / fetch are parity; 10–100-per-txn
batches lose 27–55%; bulk ~19%; through sqlite's SQL layer (zskvbench) the
difference vanishes. Root cause, measured with `ftbench*.c`: any `ftruncate`
whose EOF page is dirty in a MAP_SHARED mapping costs ~57µs (extending) to
~190µs (shrinking) on Darwin — the shrink stays ~190µs even right after
`fdatasync` — while a 1-byte `pwrite` extends over the same dirty page in
~1.7µs and memcpy + soft faults are free.

## The Linux question (why this branch exists)

Linux's truncate path has no `ubc_setsize` equivalent, so the penalty may
not exist there. To find out, in order of increasing effort — and **on the
filesystem that will run it**, which for us means a ZFS dataset, not `/tmp`
(`$ZD` below):

```
# 1. Primitives: does Linux punish ftruncate near dirty mapped pages?
cc -O2 -std=c99 spike/mmap-write/ftbench.c  -o ftbench  && ./ftbench  $ZD/ft1.dat
cc -O2 -std=c99 spike/mmap-write/ftbench2.c -o ftbench2 && ./ftbench2 $ZD/ft2.dat
cc -O2 -std=c99 spike/mmap-write/ftbench3.c -o ftbench3 && ./ftbench3 $ZD/ft3.dat
# Each prints the path and filesystem it ran on; check that line says zfs.
# macOS baseline: A 1.079us, A' 0.020us, B 159us, C 4.5us, E/F ~0.015us,
#                 G 57us, H 1.7us; K 51us, J 234us, L 40us, M 39us per txn.
# If B/G/J are ~as cheap as C, the naive chunk+truncate design wins there
# and the pwrite/heuristic complexity in zeroskip.c can be simplified away.

# 2. End-to-end, sync-free (isolates the stream path).  Built through make, so
# zeroskip.c gets the platform defines (_GNU_SOURCE) the library needs:
make clean && make -s libzeroskip.a && \
  cc -O2 -std=c99 -I. spike/mmap-write/nsbench.c libzeroskip.a -o nsb-plain
make clean && make -s libzeroskip.a EXTRA_CFLAGS=-DZS_MMAP_WRITE && \
  cc -O2 -std=c99 -I. spike/mmap-write/nsbench.c libzeroskip.a -o nsb-mmap
for per in 1 100 1000; do ./nsb-plain $ZD/nsdb 20000 $per; ./nsb-mmap $ZD/nsdb 20000 $per; done

# 3. Full suites and matrix:
make check
make clean && make check EXTRA_CFLAGS=-DZS_MMAP_WRITE
make clean && make zsbench && ./zsbench --reps 5           # plain
make clean && make zsbench EXTRA_CFLAGS=-DZS_MMAP_WRITE && ./zsbench --reps 5
```

## Linux primitives, measured 2026-08-14 (stl-imap-09, `/tmp`)

**Not ZFS, and ZFS is the case that decides this.** These runs took the default
path under `/tmp` on that host — not tmpfs (a gated txn costing 1157 µs rules
that out) but not the deployment filesystem either. ZFS is the one whose answer
matters, and it is the one most likely to differ: it keeps its own ARC pages
behind the page cache, so a truncate over a dirty mapped range and an
`fdatasync` through the ZIL are both its own code paths, not the generic ones
these numbers priced. Each `ftbench*` now prints the path and filesystem it ran
on, so a pasted result says which — check that line before believing a number.

Per-op, `ftbench*`, next to the Darwin figures above:

| | scenario | Linux | Darwin |
|---|---|---|---|
| A | `write(2)` append                       | 0.4 µs | 1.9 µs |
| B | chunked ft-up + memcpy + exact ft-down  | 19.3 µs | 169 µs |
| C | ftruncate pair, no mapping              | 4.5 µs | 4.9 µs |
| D | ftruncate pair, 1GB window present      | 12.0 µs | — |
| E | memcpy into a pre-sized file            | 0.1 µs | ~0.01 µs |
| F | chunked ft-up + memcpy, no down-truncate| 0.1 µs | ~0.01 µs |
| G | exact ft-up per store + memcpy          | 9.3 µs | 57 µs |
| H | pwrite-extend per store + memcpy        | 0.8 µs | 1.7 µs |
| I | exact ft-up per 10-store batch          | 1.0 µs | — |
| K | gated txn, plain writer                 | 1157 µs | 51 µs |
| J | gated txn, v4 mmap shape                | 998 µs | 234 µs |
| L | gated txn, exact-pwrite only            | 1145 µs | 40 µs |
| M | J minus the down-truncate               | 935 µs | 39 µs |

**Linux punishes it too, just far less.** A truncate pair costs 4.5 µs
unmapped and 12.0 µs with the window present, so the dirty-mapping surcharge is
real (~7.5 µs) but an order of magnitude below Darwin's. It is still enough
that exact `ftruncate` per store (G, 9.3 µs) loses to `pwrite`-extension (H,
0.8 µs) by 12x, so **the answer to the branch's question is no**: the naive
chunk+truncate design does not win on Linux either, and the extension policy in
`zeroskip.c` keeps earning its complexity.

**Under real durability the verdict inverts from Darwin's.** *(Superseded — see
the ZFS section below. This paragraph was measuring `/tmp`'s syncs, not the
stream path, exactly as its own last sentence suspected. Kept because being
wrong for a stated reason is the useful part of the record.)* A gated txn costs
1157 µs here against 51 µs on the mac, and every shape carries the same two
gates (C-7), so whatever the per-sync cost is, it is ~20x Darwin's and it
swamps a 9–19 µs truncate: the mmap shapes come out 14% (J) to 19% (M) *faster*
per gated transaction than the plain writer, where on macOS J was 4.6x slower.
That makes step 2 (`nsbench`, sync-free) and step 3 (the suites) worth running
here — on a host whose syncs are this expensive, the gated numbers say little
about the stream path either way.

Provisional: single runs, taken before the spike sources carried their platform
defines, so `ftruncate`/`pwrite`/`fdatasync` were implicitly declared. On
x86-64 that still passes a 64-bit `off_t` correctly, so the figures should
stand — but the gated rows (K/J/L/M) are within 20% of each other on a host
whose syncs cost ~1 ms, which is exactly where one run proves nothing. Replace
them with a repeated post-fix run.

## ZFS, measured 2026-08-14 (stl-imap-09, `/mnt/vfs/tmp/brong`) — the answer

Four runs of each binary, post-fix, filesystem line confirmed `zfs`. Median,
against the same host's `/tmp` and the Darwin baseline:

| | scenario | **ZFS** | /tmp | Darwin |
|---|---|---|---|---|
| A | `write(2)` append, one syscall per record | 5.5 µs | 0.4 µs | 1.9 µs |
| B | chunked ft-up + memcpy + exact ft-down | 23.7 µs | 19.3 µs | 169 µs |
| C | ftruncate pair, no mapping | 6.8 µs | 4.5 µs | 4.9 µs |
| D | ftruncate pair, 1GB window present | 8.8–16.2 µs | 12.0 µs | — |
| E | memcpy into a pre-sized file | 0.6 µs | 0.1 µs | 0.015 µs |
| F | chunked ft-up + memcpy, no down-truncate | 0.6 µs | 0.1 µs | 0.014 µs |
| G | exact ft-up per store + memcpy | 12.0 µs | 9.3 µs | 57 µs |
| H | pwrite-extend per store + memcpy | 6.4 µs | 0.8 µs | 1.7 µs |
| I | exact ft-up per 10-store batch | 1.6 µs | 1.0 µs | — |
| K | **gated txn, plain writer** | **195 µs** | 1157 µs | 51 µs |
| J | **gated txn, v4 mmap shape** | **313 µs** | 998 µs | 234 µs |
| L | gated txn, exact-pwrite only | 231 µs | 1145 µs | 40 µs |
| M | J minus the down-truncate | 245 µs | 935 µs | 39 µs |

**Verdict: no. Close the spike.** On the gated transaction — the shape C-7
defines and the only one a caller actually pays — the plain writer wins on ZFS
by a margin nothing here closes: K 195 µs against J 313 µs (**60% slower**),
and every other mmap variant loses too (L +18%, M +26%).

**The `/tmp` result that looked like a win was an artifact of `/tmp` being
slow.** Syncs there cost ~1157 µs per gated txn and swamped everything, which
is what made J come out 14% ahead. ZFS's ZIL makes the same transaction **6x
cheaper** (195 µs), so the sync no longer hides the truncate — and the ordering
snaps back to Darwin's, just milder. The lesson is the one the section above
already warned about: a benchmark whose dominant term is not the thing under
test ranks noise.

**The extension policy stops paying for itself.** `pwrite`-extension is what
the Darwin measurements bought all that complexity for, at 1.7 µs against
`ftruncate`'s 57 µs. On ZFS it costs 6.4 µs (H) — indistinguishable from just
calling `write()` (A, 5.5 µs). The policy's whole justification is a Darwin
number that does not survive the move to the deployment filesystem.

**And the surviving per-store win was against a strawman.** F (0.6 µs) beating
A (5.5 µs) 9x was the last argument for the design, but A is one `write(2)`
per record and `zsi_txn_stream` has never done that: it memcpys into a 64KB
`ZSI_TXN_CHUNK` and calls `write` when it fills, so ~430 records share a
syscall. Case **A′** now measures the incumbent as it really is. On APFS it is
0.020 µs/op against A's 1.079 — a 54x gap, the batching factor — while F is
0.014 µs. So even where the mapping wins it wins by ~1.4x over a reused hot
buffer, not 9x over a syscall per record. Re-run `ftbench` on ZFS for A′; the
expectation is that A′ lands near E/F rather than near A, because a 64KB buffer
that stays in cache has no per-page fault to pay and E/F's 0.6 µs on ZFS is
mostly minor faults over the 1GB window.

**One more reason not to want it:** D is the only case that moved between runs
— 8.8, 10.8, 12.6 and 16.2 µs across four — while C, unmapped, held 6.7–6.9 µs.
A truncate near a dirty mapping is not just more expensive on ZFS, it is less
predictable, and a database cares about the tail more than the mean.

What would reopen this: a workload dominated by per-store cost rather than by
syncs — a bulk load of one enormous transaction, where A′ vs F is the whole
question and the gated rows never apply. That is worth knowing before anyone
revisits, but it is not the workload zeroskip is tuned for, and D's variance
argues against the mapping even there.

## "Reserve the address space and stage through RAM" — cases N and O

The recurring idea for killing the extension cost: reserve a huge `PROT_NONE`
range, `MAP_FIXED` the file into the front of it, `MAP_FIXED` tmpfs or
anonymous memory immediately after, write records into the RAM part, and later
slide the file mapping forward over the same addresses. Contiguous addresses,
no `ftruncate` in the store path, no SIGBUS from storing past EOF.

**The mechanism works. Every step of it is real** — `MAP_NORESERVE` reservation
is free, `MAP_FIXED` replaces atomically, and a reader's pointers stay valid
across the slide because the bytes at those addresses do not change.

**It cannot pay for itself, for a reason that has nothing to do with mapping
tricks.** `MAP_FIXED`ing the file over the staging region *discards* the
staging region — the new mapping shows file contents, not what was in tmpfs.
So the bytes have to reach the file first, by `write`/`pwrite` from the staging
pages, and there is no zero-copy path from tmpfs or anonymous pages into a ZFS
file: not `copy_file_range` (a copy, and no reflink across those filesystems),
not `vmsplice`+`splice` (the final hop goes through the fs write path, and page
gifting has lifetime hazards that make a buffer reuse silently corrupt data).
That leaves memcpy-into-RAM **plus** a write of the same bytes — the incumbent's
two copies, but into freshly faulted pages instead of a 64KB buffer that stays
cache-hot, and with extra `mmap` syscalls on top. Strictly more work than A′.

**And N is the ceiling on the whole family.** Rather than argue it, `ftbench3`
measures it: N and O run over a file pre-sized up front, so neither pays any
extension at all — same bytes, same offsets, same two gates, differing only in
whether the bytes were dirtied by a store through a `MAP_SHARED` mapping or by
`pwrite`. No reservation scheme, tmpfs staging region or `fallocate`-ahead can
do better than a file that was already the right size, so **N bounds every one
of them.**

| | APFS |
|---|---|
| K plain writer | 37.2 µs |
| M J minus down-truncate | 36.5 µs |
| **N pre-sized, memcpy + sync** | **36.6 µs** |
| **O pre-sized, pwrite + sync** | **34.8 µs** |
| J v4 shape (has the down-truncate) | 230.9 µs |

On Darwin N ≈ O ≈ M ≈ K: with extension removed the mapping is free, and J's
6x penalty is the down-truncate and nothing else. So on Darwin the idea would
work — it just has nothing left to win, since K is already there.

**ZFS is the case that decides it, and the prediction is that N loses.** M on
ZFS chunk-extends by 1MB while each txn adds 192 bytes, so across N=2000 it
pays roughly *one* extension for the entire run — extension is already
amortized to nothing there — and it still cost 245 µs against K's 195 µs. If
that 26% is real it cannot be extension, which leaves `fdatasync` over a dirty
`MAP_SHARED` range being more expensive on ZFS than `fdatasync` after a write:
ZFS keeps its own ARC pages behind the page cache and a mapped store has to be
reconciled into them at sync time. N vs O tests exactly that, with extension
removed from both. **If N > O on ZFS, the mmap write path is not losing because
of how it extends, and no addressing scheme — including this one — can rescue
it.** Run `ftbench3` on the ZFS dataset; N and O are the last two lines.

## Known gaps if this ever lands

- `zstest-crash`'s NOSYNC sweep precondition fails under the flag: mmap
  stores are invisible to `zs_hook_write`, leaving only 3 hookable calls.
  A landing needs an ftruncate/pwrite hook to restore those crash points.
- `pwrite` on an `O_APPEND` fd APPENDS on Linux (ignores the offset) — the
  flag drops `O_APPEND` from the stream descriptor (`ZSI_WFD_OPEN_FLAGS`).
- A transaction outgrowing the 1GB window falls back to the accumulate list
  (addresses stay valid, contiguity ends). The unbounded-contiguous upgrade
  is a PROT_NONE anonymous reservation with file pieces MAP_FIXED'd in.
- Spec work: a "slop MUST NOT outlive the write lock" clause, and the crash
  tests' sync-signature wording.
