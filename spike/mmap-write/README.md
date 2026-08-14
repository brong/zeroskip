# mmap-write spike (throwaway — not for merging as-is)

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
not exist there. To find out, in order of increasing effort — step 1 is now
answered, see "Linux primitives" below:

```
# 1. Primitives: does Linux punish ftruncate near dirty mapped pages?
cc -O2 -std=c99 spike/mmap-write/ftbench.c  -o ftbench  && ./ftbench
cc -O2 -std=c99 spike/mmap-write/ftbench2.c -o ftbench2 && ./ftbench2
cc -O2 -std=c99 spike/mmap-write/ftbench3.c -o ftbench3 && ./ftbench3
# macOS baseline: A 1.9us, B 169us, C 4.9us, E/F ~0.01us, G 57us, H 1.7us;
#                 K 51us, J 234us, L 40us, M 39us per txn.
# If B/G/J are ~as cheap as C on Linux, the naive chunk+truncate design wins
# and the pwrite/heuristic complexity in zeroskip.c can be simplified away.

# 2. End-to-end, sync-free (isolates the stream path).  Built through make, so
# zeroskip.c gets the platform defines (_GNU_SOURCE) the library needs:
make clean && make -s libzeroskip.a && \
  cc -O2 -std=c99 -I. spike/mmap-write/nsbench.c libzeroskip.a -o nsb-plain
make clean && make -s libzeroskip.a EXTRA_CFLAGS=-DZS_MMAP_WRITE && \
  cc -O2 -std=c99 -I. spike/mmap-write/nsbench.c libzeroskip.a -o nsb-mmap
for per in 1 100 1000; do ./nsb-plain /tmp/nsdb 20000 $per; ./nsb-mmap /tmp/nsdb 20000 $per; done

# 3. Full suites and matrix:
make check
make clean && make check EXTRA_CFLAGS=-DZS_MMAP_WRITE
make clean && make zsbench && ./zsbench --reps 5           # plain
make clean && make zsbench EXTRA_CFLAGS=-DZS_MMAP_WRITE && ./zsbench --reps 5
```

## Linux primitives, measured 2026-08-14 (stl-imap-09)

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

**Under real durability the verdict inverts from Darwin's.** A gated txn costs
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
