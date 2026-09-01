# Replication: log shipping by span

**Status: DESIGN, 2026-09-01.** Nothing is implemented and nothing here is
normative. The spec gets requirement labels if and when this is built; until then
every "must" below is a design decision, not a conformance rule.

**One sentence:** a follower tails the master's active file, archives each
committed span verbatim, and replays the master's *publication events* — a
generation sealing, a range merging — as instructions rather than as bytes, so
the network carries the write stream once and never carries a repack.

**Scope: backup, not failover.** The follower never becomes a master. That
exclusion is doing real work — see [What this deliberately does not
do](#what-this-deliberately-does-not-do) — and most of what makes the design
small comes from it.

## Why the format suits this

Five properties carry the whole design, and three of them are recent.

- **A span is a self-framing, self-verifying unit** (F-19, F-23). The terminator
  carries the span length and a checksum over the span's bytes plus its own. Ship
  the bytes and you have framing, integrity and the commit/abort verdict in one
  structure — no wire framing, no wire checksum, no separate commit record.
- **A span is position-independent.** Nothing in a record or a terminator encodes
  an absolute offset; F-3's offsets belong to the pointer section and trailer,
  which unordered files do not have. So a span validates wherever it is placed.
  This is what lets a follower drop rolled-back spans, or start a generation at a
  different offset, without recomputing anything.
- **Records are context-free** (F-18). Since the ancestor was removed, a record
  describes only itself, so a span means the same thing on the follower as on the
  master. A format where a record named the generation holding its predecessor
  could not do this.
- **Every commit is detectable with one `stat`** (C-4i). Size changes on an
  append, inode changes on a rollover, and nothing else makes committed data
  visible. A follower needs no notification channel.
- **Durability is observable from the file, one span behind** — new with C-7d,
  see below.

Two smaller ones worth knowing. A span's terminator checksum is effectively a
content hash of the transaction, so echoing it back is a free applied-correctly
ack. And conversion is byte-deterministic: key order, canonical encoding (F-15),
the pad (F-26d), the pointer width (F-26c) and the checksum are all determined by
the input records, so a follower converting locally produces the same file the
master would have.

## The axis that decides everything

The obvious way to classify a follower is by what it can do — can it repack, can
it only move files. That is the wrong axis. The one that constrains the design is
**whether it shares the master's generation space**, because generation numbers
are the only identity the file set has (there is no manifest, D-2/D-6) and they
are in the filename.

| | Generation-locked | Generation-free |
|---|---|---|
| Accepts in-order files from the master | yes, any time | seed only |
| Repacks on its own schedule | **never** | yes |
| Converts locally | yes | yes |
| Rollover policy | must mirror the master's | its own |
| Comparable against the master by range | yes | no |
| Re-seed cost | incremental, by file | full |

**The middle ground is not conforming.** A follower that both merges on its own
schedule and accepts in-order files will eventually hold two ranges where neither
contains the other, and D-5c says that "cannot arise from any legal sequence and
MUST be reported as corruption". Concretely: master holds `[1-2] [3-4] [5-6]`;
follower merges `[3-4]+[5-6] → [3-6]`; master independently merges
`[1-2]+[3-4] → [1-4]` and ships it. The follower now has `[1-4]` and `[3-6]` and
is unopenable.

This design picks **generation-locked**, and gets there without ever shipping a
file after the seed, because of the next section.

### Publication ranges are laminar

Over a master's lifetime, the set of ranges it publishes is **laminar**: any two
are nested or disjoint, never partially overlapping.

A merge always takes a contiguous run of the *current* tiling, and ranges only
ever combine — no operation in D-16 through D-26 produces a subrange of an
existing file. So a previously published range is either wholly inside a current
member or disjoint from it, and a merge's output is a union of whole current
members. `[1-4]` and `[3-6]` cannot both be things one master published.

Two consequences:

1. A follower that only ever *accepts* can take any file the master publishes, in
   any order, at any time, and D-5c is unreachable. Overlaps are total and D-5
   resolves them.
2. A follower that reproduces the master's ranges **by instruction** stays inside
   the same laminar family, so it keeps that property while shipping no file
   bytes at all.

### Never ship a repack output

A repack output contains no information the follower lacks. D-17's newest version
per key and D-19's retention are both computable from the spans the follower
already holds. So a merge costs a **range**, not a file: the instruction is
`merge → [a-b]`, and the follower does the IO.

The output range alone is sufficient, and it is idempotent and self-healing.
Given a tiling, `[a-b]` uniquely determines the inputs — every file whose range
falls inside it — so applying it to a follower holding `[1-1][2-2][3-4]` gives the
same result as one holding `[1-2][3-4]`, and applying it to one that already holds
`[1-4]` is a no-op. A follower that fell behind on merges and caught up out of
order converges. The master's selection state never crosses the wire.

What this buys is that the network carries the write stream **once**. Measured
locally, an armed cascade rewrites about 3× the data it converts (`doc/
benchmarking.md`); none of that crosses the wire.

## Durability: the follower is never ahead

Under C-7d a failed durability gate seals the generation, so **no further span
can ever be appended to a file whose gate failed.** That turns a property that
used to be nearly-true into an exact one:

> Within one generation, the presence of *any* later span proves that span *n*'s
> commit gate **succeeded**.

Before C-7d a writer could keep appending after a failed gate, so a later span
only proved the gate had *returned*. Now it cannot. The rule covers a `ROLLBACK`
as well as a `COMMIT`, since either is written after the previous commit returned.

The consequence is the recommended shape:

**A pull follower that lags one span is exactly correct, with zero master-side
changes.** It reads bytes eagerly, holds the last span it has seen as
*provisional*, and commits it to the archive when either the next span appears or
the generation is sealed (C-6b syncs a conversion output before the rename, so the
seal proves the whole generation durable).

Every failure case resolves without a protocol:

- Gate fails, seal succeeds, writer continues in *N+1*. The seal's D-20b replay
  either read span *n* — in which case it is now durable in `[N-N]` and the
  follower's copy is right — or it did not, in which case the seal aborts.
- Gate fails, seal fails, writer blocked. No later span ever appears, so the
  follower's provisional span is never committed. Correct.
- Master crashes mid-span. A span is atomic to the follower (it is self-verifying),
  so there is never a partial one to resume past. The master's R-4 recovery and
  the follower's view converge automatically, because both are defined by F-24's
  complete point.

A push hook after the gate is still an option and cuts the one-span lag, at the
cost of master-side changes. It is not needed for correctness.

**Under `ZS_NOSYNC` none of this holds** — there is no gate (C-7c), so the
caller's `zs_db_sync` is the only durability point, and the follower must be
driven from it. C-7d reaches that path too.

## The archive

The follower's directory is **a valid zeroskip database at all times**, holding
the master's generations 1..*N*−1 with the master's exact ranges, plus one extra
file: the open generation's archive.

```
<follower>/zeroskip-<uuid>-00000001-00000008      mirrors the master's ranges
<follower>/zeroskip-<uuid>-00000009-00000009
<follower>/zeroskip.archive-<uuid>-0000000A       the open generation
```

Restore is the directory as it stands, optionally with the archive converted
first for the last generation's worth of writes.

**The archive is an unordered file by content and not by name.** Its bytes are
the master's 80-byte header verbatim, then committed spans concatenated — which is
exactly the layout F-23 describes, so it can be read by the existing replay and
converted by the existing conversion. The `zeroskip.` prefix puts it in the
metadata namespace (D-2), which is the same mechanism P-3 uses for pointer tables,
so nothing parses it as a data file and it never joins the file set.

That naming is what makes it **freely truncatable**, and truncation is the whole
reason not to make it `.current`. A published data file cannot be truncated
(G-1, R-4) and a byte-identical replica therefore has no way to roll back a tail
it should not have taken. The archive is read by nobody but the follower, so
rolling it back is local surgery on a private file.

Three things the archive must carry that are not in any span:

- **The generation's header**, verbatim. It carries the engine (F-5a), the
  comparator name (F-11), the versions and the start generation. Rebuild it
  locally and a wrong engine makes every terminator fail validation.
- **Committed spans only.** Rolled-back spans are dropped, which S-9 already
  requires of salvage and which position-independence makes free. Offsets shift
  relative to the master's file; nothing cares, because position is
  `(generation, committed-span-number)` and never an offset.
- **Nothing else.** No position file, no manifest. The follower re-derives its
  resume point by replaying its own archive and counting committed spans, which
  is the same quantity the master counts. There is no state that can disagree
  with the data.

## The stream

Five messages. Position is `(gen, n)` throughout, monotone because generations
are (D-9b) and *n* is within one.

```
HEADER  gen, 80 bytes           a generation opened
SPAN    gen, n, bytes           committed span n of gen
SEAL    gen, [a-b], nspans      gen was published as the in-order range [a-b]
MERGE   [a-b]                   merge to this range
ACK     gen, n, term_csum       follower applied through here (optional)
```

**One stream, in order.** `MERGE [1-4]` is only meaningful once generation 4 is
complete. Two channels race, and the failure is silent: the follower builds
`[1-4]` from three of the four generations and it still tiles.

`SEAL` carries the range because of C-1l. A plain conversion publishes `[N-N]`,
but the compacting seal publishes `[a-N]`, folding a repack decision into a
conversion — if the master does that and the follower converts plainly, the
layouts diverge. Recoverable, since the follower's finer partition merges to the
same place later, but it silently costs the range-comparability the design is
paying for.

`SEAL` carries `nspans` so the follower can resolve its provisional tail without
an extra round trip: it says how many committed spans the generation ended up
with, so a follower holding more knows exactly what to discard.

`ACK` echoes the terminator's checksum, which is a content hash of the
transaction (F-19), so it is an applied-*the-same-bytes* ack rather than an
applied-something ack. Free, since the value is already in the span.

### Reconnect

The follower announces `(gen, n)`. Three answers:

- **continue** — the master's generation *gen* is still open and has at least *n*
  committed spans;
- **`SEAL gen, [a-b], nspans`** — the generation closed; if `n > nspans` the
  follower truncates its archive to `nspans`, which is the C-7a "unknown outcome"
  window resolving against it. This is the case that needs the archive to be
  truncatable and is the reason it is not `.current`;
- **reseed** — the master no longer holds *gen* (it was merged away and the
  follower is far enough behind that the spans are gone).

## Seeding

Ship the master's current file set from a **held C-4 snapshot**, plus the active
file's spans from its start.

Holding the snapshot is what makes this safe: `readdir` is not atomic (D-7) and
D-23 retires files mid-copy, so a list-then-copy can pick up `[1-4]` and `[5-5]`,
then find `[5-5]` gone because `[1-8]` was published — leaving a gap at 5..8. A
C-4 snapshot has the inodes open, and C-4g keeps each alive through `unlink`, so
the seeder ships exactly what it resolved.

The cost is C-5: a long seed pins every file the repacker would otherwise have
reclaimed. On a large database that is real disk space for the duration.

The cutover point is the snapshot's active file complete point — a
`(generation, offset)`, which is exactly P-8's `valid_upto` shape — and the stream
resumes there.

## What the library must grow

Three calls. Nothing in today's `zeroskip.h` exposes any of this.

```c
/* Read committed spans from an unordered file, from a span boundary onward.
   The follower's entire master-side requirement. */
int zs_db_read_spans(struct zs_db *db, uint32_t gen, size_t from,
                     zs_span_cb *cb, void *rock);

/* Publish an archive as the in-order file [gen..gen]: convert, sync, rename,
   directory sync.  D-25 with the input named rather than discovered. */
int zs_db_ingest_generation(struct zs_db *db, uint32_t gen,
                            const char *archive_path);

/* Merge to an explicit range instead of D-16's selection.  Every other repack
   rule applies unchanged (D-17 to D-23, adjacency included). */
int zs_db_repack_range(struct zs_db *db, uint32_t start, uint32_t end);
```

Appending a span to the archive needs no library support — it is a plain file
append of bytes the follower already has.

A push variant would add a commit hook firing after the gate with
`(gen, n, bytes, len)`. Not required; see the durability section.

## What this deliberately does not do

**Failover.** The follower never becomes a master. Nothing in the format
identifies which writer produced a file — no epoch, no writer id, and F-11 forces
the same UUID on every file — so two forked generation-*N* files have the *same
filename*, and publication is a `rename`, which silently replaces. A fork is
either resolved silently by D-5 (the last file at that start wins, arbitrarily) or
reported as corruption by D-5c. The silent case is the dangerous one.

Supporting failover would need an epoch or writer id in the header **and** in the
filename. Header offsets 20 (4 bytes) and 64 (8 bytes) are reserved, but F-8 says
reserved fields are written zero and ignored, so it is a spec change either way.
Out of scope here.

**Eventual consistency.** D-17b's total order is `(generation, offset)` — a
position in one writer's history, not a logical clock. There is nothing to merge
on.

**Restoring alongside the original.** A restored copy has the master's UUID
(F-11), so putting both into the same infrastructure is exactly the undetectable
fork. Restore-by-replay can re-stamp a fresh UUID for free, since it is writing
new files anyway (`zs_db_open_with_uuid` already exists); a restore-by-copy
cannot.

### Considered and rejected

- **A byte-identical replica.** Requires appending at the master's exact offsets,
  so a follower crash mid-span leaves an unclean active file it may not append
  past (D-9, R-4) and may not truncate (G-1). Its only legal move is a local
  rollover, which diverges the generation space permanently. Owning the archive
  format is what avoids this.
- **A logical operation stream.** Loses the free end-to-end integrity, and
  operations and bytes disagree in three ways: an abort may write nothing at all
  (C-8b), `ZS_IFCHANGED` writes nothing (A-1d), and `store k` then `delete k` in
  one transaction is two records shadowed by offset order (D-17b), not one.
- **Shipping repack outputs.** Pure derived data; see above.
- **One archive file per span.** D-12a makes at most one unordered file
  representable, so each span would need its own generation. Generations are
  32-bit, never reused, and exhaustion is fatal (D-9c) with dump-and-reload into a
  fresh UUID as the only remedy — which re-seeds every follower. At 1000 tx/s that
  is **50 days**; at 100/s, 14 months.

## Open questions

1. **Flow control.** A `MERGE` instruction is unbounded (D-29) while spans keep
   arriving. The follower's ingester holds the write lock and a merge wants the
   repack lock, so C-1d's write → repack order lets them interleave in one
   process — but nothing bounds how far behind the ingester falls during a
   compaction-sized merge, and the archive grows meanwhile.
2. **Instructions versus free pruning.** Instructions buy the layout mirror, and
   the mirror buys post-seed file shipping and range comparability. What they cost
   is coupling the follower's merge IO schedule to the master's, so a follower on
   slower storage falls behind specifically on merges. A follower that would never
   accept a file after the seed could prune freely with no coupling at all. Which
   is right depends on whether incremental re-seed matters.
3. **Byte identity is not guaranteed, only likely.** The same instructions on the
   same inputs do produce the same D-19 answers, so the bytes normally match — but
   D-19c explicitly permits erring toward retention, and A-6 takes a conversion or
   repack output's engine from the *handle's* `ZS_CSUM_*` (`zeroskip.c:8046`,
   `:8691`), not the input's. So digest comparison as a verification tool pins the
   follower to the same library version and open flags. Range comparison does not.
4. **A generic follower cannot repack every database.** Executing a `MERGE` means
   merging by key order, so it needs the caller's comparator (F-11) and, for
   engine 2, the caller's checksum function (F-5d). A follower that only archives
   spans needs neither.
5. **The pointer table cache.** P-2a keys the cache directory on the UUID, which a
   master and its follower share by F-11, so they must not share a cache root;
   P-17's binding check only catches divergence in the last span. `ZS_INDEX_LOCAL`
   (P-2b) sidesteps it by making the cache travel with each copy.
6. **Local writes to a follower are undetectable at the format level.** There is
   no read-only marker on disk, and an accidental local span is valid and
   self-consistent. The ingester holding the write lock excludes other processes
   (C-1, G-5); a size check against the expected offset catches the rest.
