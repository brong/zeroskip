# Format 4: one record encoding, pointers last

**Status: DESIGN, agreed 2026-08-26.** Nothing is implemented and nothing here is
normative; the spec gets the authoritative offsets when this is written up.

**One sentence:** both file kinds store the same self-framing `KEYVALUE` records
with key and value adjacent, and an in-order file appends a pointer array *after*
its data, with one checksum over the whole file.

```
in-order:   [HEADER][KEYVALUE*][PAD][POINTERS][TRAILER]
unordered:  [HEADER][KEYVALUE* + terminator]*                (spans, as today)
```

**What it replaces.** Format 3 split an in-order file into a dense keys region
pointing at a region of bare values, on the argument that a seek then touches a
structure smaller than the records region by the value:key ratio. Measured
locally that delivered — `fetch (repacked)` +14% at 500k and +18% at 2M. Measured
downstream on ZFS *through SQLite* at 2M it inverted: −6..−11% on rowid fetch,
and negative in all twelve `WITHOUT ROWID` cells regardless of value size, which
is the layout every SQLite index uses. Format 4 returns to adjacent key and value
— format 2's locality — with a cleaner encoding and a much simpler writer.

**What is not yet settled** is how much of format 3's *scan* win (+19..61%
downstream) was the split at all: format 2 verifies a per-record checksum on
every read and format 3 verifies nothing, and the format-2 arm additionally
hashed ~3x slower from `XXH_NO_INLINE_HINTS`. Every read comparison in existence
measures those three together. The discriminator is the scan rows with
`--csum null` on flag-matched arms, and it has not been run. See
`doc/benchmarking.md`.

## The record

Two forms, selected by one flag bit. Fields are little-endian and read through
`memcpy` at literal offsets, so nothing on disk describes C layout (G-0).

```
small   [flags:4 | keylen:12][vallen:16]                      4 bytes
big     [flags:4 | keylen:60][vallen:64]                     16 bytes

then    key bytes, NUL, value bytes, NUL
```

So `keylen ≤ 4095` and `vallen ≤ 65535` in the small form, and the big form's
escape covers everything above. The big form spends 60 bits on a key length that
is almost always small — the case that forces it is nearly always a large *value*
— which wastes 7 bytes on a record that is by definition large. A third,
intermediate form was considered and rejected: it costs a flag bit and a decode
branch to save 12 bytes on a rare record.

**The flag bits.**

```
bit 0   IsBig        the header is 16 bytes, not 4
bit 1   IsDelete     a tombstone; vallen MUST be 0
bit 2   IsRollback   defined ONLY when keylen == 0
bit 3   reserved     written 0
keylen == 0          not a record: a span terminator (F-14 forbids a 0-length key)
```

`keylen == 0` as the terminator escape is what keeps bit 3 spare, and a spare bit
is worth having: reserving `0x08` rather than recycling it is why removing the
four `*_ANC` forms changed no golden corpus byte. The alternative assignment —
`IsTerminator` on bit 2, `IsRollback` on bit 3 — reads more explicitly and spends
the whole nibble.

**`IsDelete` cannot be folded into `vallen == 0`.** An empty value and a deletion
are distinct states (A-1), and `ZS_IFCHANGED` rests on the distinction; comparing
value bytes and lengths was already rejected as a way to detect a redundant
store.

**Flags must sit at a fixed bit position in byte 0**, because `IsBig` selects the
width of the very word it lives in: a decoder has to read the flags before it
knows whether it is looking at a 2-byte or an 8-byte first field.

**A terminator** is `[flags | keylen=0][vallen=16]` followed by
`[spanlen:8][csum:8]` — always the small form, always 20 bytes. Putting `spanlen`
in the payload rather than in the length field is deliberate: a 16-bit `vallen`
would cap spans at 64KB and force every terminator above that into the big form
for nothing.

### The size arithmetic

```
reclen = hdr + keylen + 1 + (IsDelete ? 0 : vallen + 1)
                                            hdr = 4 (small) | 16 (big)
```

**A deletion carries the key's NUL and no value NUL**, which is what every
format so far has done — `zsi_rec_encoded_len` already reads
`if (isdelete) { /* key NUL, no value at all */ }`. The NUL is the *value's*
terminator and a deletion has no value, so there is nothing for it to terminate;
F-13 puts the terminators outside the stored lengths precisely because they
belong to the key and the value rather than to the record.

An unconditional `+ 2` was considered, on the grounds that a conditional in the
expression deciding where the next record starts is a resync hazard. It was
rejected twice over.

It defends against exactly one bug — a writer that emits value bytes on a
deletion — while being equally undefended against its mirror, a writer that sets
`vallen` and emits nothing. No length field is protected from corruption here: a
wrong `keylen` desynchronises the stream identically.

And **the conditional is free because the branch is already there.** A lookup
never computes a record's length at all: it is pointer-driven, so it reads the
header, `keylen`, and the key to compare, then `vallen` and the value on a hit —
which is what `zsi_ptrs_rec` already does, locating by pointer and decoding
fields without ever summing them. `reclen` is needed only by a *sequential* walk,
and every sequential walk that yields records must already test `IsDelete`,
because a tombstone shadows its key and must not be handed back (D-17b). So the
flag is already extracted and already branched on at exactly the point the length
is wanted. The one path where the conditional adds a branch that was not there is
an unordered file's replay, which does not care about delete-ness while building
an index — one predictable test on a path that is already decoding a key and
inserting it into a sorted structure.

A useful structural consequence of the first half: an in-order file's records do
not need to be *walkable* for the read path at all. Walkability is for merges and
for salvage.

Three clauses make the conditional unambiguous, and all three need to be
normative:

- a deletion's `vallen` MUST be 0;
- a reader MUST NOT consult `vallen` when framing a deletion, so there is never
  more than one candidate length for a record;
- a deletion's `vallen` does not participate in form selection either — only its
  key can promote it to the big form, which is what the current code's
  `big = keylen > MAX || (!isdelete && vallen > MAX)` already says.

A non-zero `vallen` on a deletion is therefore a `zs_db_check_consistency`
report, not a framing change.

**What the byte is worth**, since it was measured before being argued about: the
NUL is `1/(keylen+5)` of a tombstone, so 5.9% at a 12-byte sqlite rowid, 4.8% at
16 bytes, 2.2% at a 41-byte conversations key, and 0.08% at a message-id. In a
mixed workload it disappears — a 16-byte key with a 200-byte value is a 222-byte
store against a 22-byte delete. The one place it could reach a profile is via
`rollover_size`, which is a byte threshold, so fatter tombstones cross it sooner
and a delete-dominated load creates proportionally more generations — and every
generation is a file created, converted, unlinked and `readdir`-ed, at 1.8ms per
`unlink` on ZFS. Zero after a compaction, which spans everything and so drops
every tombstone (D-19).

**Canonicality**, following F-15: a writer MUST use the small form when it fits,
and a reader MUST accept a big form that would have fitted rather than rejecting
it. Rejecting non-canonical input is a data-loss bug here for the reason it
already is — F-24 turns a rejection into the loss of every record after it — so
the canonicality check belongs in `zs_db_check_consistency`, which reports the
divergence while still reading the data.

## Padding and the walk

Padding between the last record and the pointer array is **allowed**, so the
array can start at a multiple of the pointer size. Pad bytes are zero, which
keeps a file canonical and reproducible for the corpus.

**The record walk is bounded by `nptrs` from the trailer, not by an offset**, so
padding is never decoded. This matters because the tempting justification for
NUL padding is wrong: *a record header can be NUL*. With flags in the low nibble
and `keylen` above them, byte 0 is `flags | ((keylen & 0x0F) << 4)`, so a plain
record with no flags and a **16-byte key** has byte 0 == `0x00`. That is not an
exotic case — 16 bytes is the benchmark default and roughly sqlite's rowid-index
key. Putting `keylen` in the low bits moves the collision to `keylen == 256`, and
biasing `keylen` by −1 (legal, since F-14 says ≥ 1) moves it to `keylen == 1`. No
bit layout gets the property for free; buying it costs a must-be-one marker bit,
which spends the reserved bit, and is not worth it.

Salvage is the one reader that cannot use `nptrs`, because S-6 requires it to
walk the data without trusting the pointer region. It will meet the pad and fail
to decode it — which is correct, since the pad follows the last record, so "stop
here" is exactly right. Worth stating in the spec as a consequence rather than
leaving it to look like a hole.

## The trailer

Carries `POINTERSTART`, `nptrs`, `ptrsize` and the file checksum. The three
position fields are redundant on purpose and MUST agree exactly:

```
trailer_start − nptrs × ptrsize == POINTERSTART
96 ≤ POINTERSTART ≤ trailer_start
every pointer within [96, POINTERSTART)
```

Checked for exact equality so a truncated or padded file is rejected rather than
read short. A reader consults the trailer *before* verifying the checksum —
verification is on demand, so an unverified file is the normal case — which is
why the bounds check is load-bearing rather than defensive. This is the trap the
pointer table already sprang: widening its checksum from 4 to 8 bytes without
updating the loader's `+ 4` made every table fail its size equality, so the cache
silently did nothing while appearing to work, and three tests failed without
naming the cause.

`ptrsize` in the trailer is also what removes format 3's width fixed point
(F-26c): the trailer is written last, so the width is *chosen* after the maximum
offset is known and simply recorded. There is nothing to iterate.

## One checksum

One XXH3-64 over `[0, filesize − 8)` — header, records, pad and pointers. The
data plus the pointers is the file.

Covering the header costs 96 bytes of hashing and binds the header to its body,
so a valid header cannot be grafted onto a different one. The header keeps its
own checksum at offset 88 regardless, because D-1b's generation discovery, D-10
and salvage all have to validate a header without hashing the file.
`KEYSLEN`/`VALSLEN` at offsets 64 and 72 no longer carry anything and are
**reserved, not reused**.

### When it is verified

- **An in-order file's checksum:** only by the verification API, by a repack
  reading the file as an input, and by salvage. Never on open, never on a read —
  open stays O(1), as F-33a already requires.
- **An unordered file's span terminators:** whenever a span is *replayed*,
  in every durability mode. This is not integrity, it is the commit boundary: the
  terminator's checksum covers the span and the terminator precisely so a
  terminator that reaches disk before its data reads as **absent** (C-4f), which
  is what makes reading a live file safe without a lock.

**Checking only the final span is unsafe**, and it was considered. Five spans,
`ZS_NOSYNC`, crash: nothing ordered the write-back, so the kernel may have
flushed span 5's pages and span 3's terminator while span 3's data pages never
landed. Check only span 5 and it passes, and span 3's garbage is served as
committed records. Verifying each replayed span is what finds the truncation
point, after which F-24 discards 4 and 5 along with it. Since records no longer
carry their own checksums the span checksum is the *only* thing protecting those
bytes (F-22a), so the rule is more load-bearing than it was, not less.

What is true is that nothing need verify spans below `cached_upto`: a pointer
table's `valid_upto` is a persisted, checksummed claim that whoever published it
verified them (P-12). So the rule is **"verify every span you actually replay; a
table lets you replay fewer"**. The cost is bounded either way — a replay window
is `min(rollover_size, rollover_txns spans)`, 2MB or 1024 spans at the defaults,
so a full one is ~40µs of hashing at the rate measured on 2026-08-26.

## What this does to the writer

The whole file is written in **strictly ascending offset order**: header complete
up front, records streamed, pad, pointer array, trailer. Nothing is back-patched.
That single property deletes most of the current writer.

- **One sink, not two.** The two-sink design and its alternating `lseek`+`write`
  between two distant offsets exists only because keys and values were separate
  regions. It also disposes of the untested guess that those ~1800 alternating
  writes were the APFS-only 6% in the `merge_memory` sweep — there is nothing
  left to alternate between.
- **One streaming digest**, accumulated as bytes are produced. This is free only
  because of `f365bc4`: streaming XXH3 was measured at 21 GB/s against 54
  one-shot until that turned out to be `XXH3_STREAM_USE_STACK` being unset on
  clang, and at the shipped flags it is 51.5 against 51.4. Under the old flags a
  design whose digest *must* stream would have looked 2.5x worse at the checksum.
- **`merge_memory` (A-20) loses its reason to exist.** The only per-merge memory
  is the pointer array, which is not optional and not a budget. The knob can be
  removed rather than re-defaulted.
- **3.1.0's descriptor array is obsoleted, not backported.** A merge copies each
  record's bytes once, at the moment it is produced, and holds only an 8-byte
  output offset: **8 bytes per record instead of 32**, one walk, no widths fixed
  point, and no D-20c requirement to know every section length before the first
  byte. The layout change subsumes the memory work.
- **No fixed point anywhere.** Both of format 3's iterations — the pointer width
  and the valptr width — were consequences of the pointer region preceding the
  data.

Putting the pointer region last is what makes one pointer per record sufficient
in a single walk. That is the thing two passes over the inputs were trying to buy
and could not: the two-walk scheme's extra pass re-faults the *inputs* from disk
precisely when the merge is large enough for the memory to matter.

**The O(1) escape hatch, if 16MB at 2M records ever became the constraint:**
stream the data, then re-read the just-written output — self-framing, and the
hottest thing in the page cache — to emit the pointers. Unlike re-walking the
inputs, this cannot re-fault from disk. Not worth doing at 8 bytes per record;
worth knowing nothing here has a memory ceiling a caller would have to be warned
about.

## Open questions

- **The reserved bit's reader rule.** An old reader meeting bit 3 set can ignore
  it and possibly misread, or reject and — under F-24 — complete the file at that
  point and lose everything after. The same asymmetry that makes accepting
  non-canonical records mandatory applies here, and the answer should be argued
  rather than assumed. An in-order file has an independent structure (the pointer
  array) to cross-check against; an unordered file does not.
- **The 16 flag combinations should be enumerated normatively**, so
  `zsi_type_valid` stays a `switch` over a stated table rather than becoming a
  computed bit predicate. F-12's table is normative today for exactly this
  reason: a computed predicate is a second specification that can drift.
- **`zs_db_check_consistency` should rebuild the pointer array from the record
  stream and compare.** Self-framing records make the pointers derivable, so a
  pointer array that disagrees with the data can be caught without trusting
  either — which format 3 could not do, since its keys region could only be
  located from the header. This is what makes one whole-file checksum tolerable.
- **The scan measurement is still missing.** Until the `--csum null` comparison
  on flag-matched arms exists, nobody knows how much of format 3's scan win was
  the split rather than the removed read-path verification, and therefore how
  much returning to adjacent key/value gives up.
