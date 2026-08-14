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
not exist there. To find out, in order of increasing effort:

```
# 1. Primitives: does Linux punish ftruncate near dirty mapped pages?
cc -O2 -std=c99 spike/mmap-write/ftbench.c  -o ftbench  && ./ftbench
cc -O2 -std=c99 spike/mmap-write/ftbench2.c -o ftbench2 && ./ftbench2
cc -O2 -std=c99 spike/mmap-write/ftbench3.c -o ftbench3 && ./ftbench3
# macOS baseline: A 1.9us, B 169us, C 4.9us, E/F ~0.01us, G 57us, H 1.7us;
#                 K 51us, J 234us, L 40us, M 39us per txn.
# If B/G/J are ~as cheap as C on Linux, the naive chunk+truncate design wins
# and the pwrite/heuristic complexity in zeroskip.c can be simplified away.

# 2. End-to-end, sync-free (isolates the stream path):
cc -O2 -std=c99 -I. spike/mmap-write/nsbench.c zeroskip.c -o nsb-plain
cc -O2 -std=c99 -DZS_MMAP_WRITE -I. spike/mmap-write/nsbench.c zeroskip.c -o nsb-mmap
for per in 1 100 1000; do ./nsb-plain /tmp/nsdb 20000 $per; ./nsb-mmap /tmp/nsdb 20000 $per; done

# 3. Full suites and matrix:
make check
make clean && make check EXTRA_CFLAGS=-DZS_MMAP_WRITE
make clean && make zsbench && ./zsbench --reps 5           # plain
make clean && make zsbench EXTRA_CFLAGS=-DZS_MMAP_WRITE && ./zsbench --reps 5
```

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
