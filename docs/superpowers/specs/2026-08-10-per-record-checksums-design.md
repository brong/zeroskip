# Per-record checksums — design

**Date:** 2026-08-10
**Status:** approved pending final review
**Decisions made with brong:** records carry their own checksum in both file
kinds (it is part of the key-value format itself, not an in-order add-on);
clean format break with a version bump, no compatibility path for version-1
files; trailing placement, keeping the format's single checksum convention.

## Why

The format stores no checksum per record. An unordered file's records are
covered per-span (verified at replay), an in-order file's per-region (verified
only by `zs_db_check_consistency`, and since D-20b by repack). Nothing verifies
a record's bytes at read time, so a bit flipped in place in an in-order file is
served to the caller undetected. twom verifies each pair on every read;
zeroskip should offer the same guarantee.

## Format change (spec §4.4, F-numbering)

Every data record gains a 4-byte checksum as **the last 4 bytes of the padded
record**, covering `[0, len-4)` in one contiguous hash:

```
KEYVALUE (0x01)
  +0      1  type
  +1      1  keylen
  +2      2  vallen
  +4      .  key NUL value NUL pad->8
  +len-4  4  csum      covers [0, len-4)
  len = roundup8(4 + keylen + 1 + vallen + 1 + 4)
```

Same shape for all eight data forms: the existing layout diagrams keep their
field order, the len formula gains `+ 4`, and the checksum sits in the final 4
bytes after padding. Terminators, the pointer section, and the trailer are
unchanged; the span checksum and the records-region checksum remain (C-4f
liveness and the one-pass D-20b check depend on them — per-record checksums add
granularity, not structure).

This preserves the format's one checksum rule, now stated explicitly in the
spec: **every checksum in this format is the last field of the thing it covers,
and covers everything before it** (header F-4, terminator F-19, trailer F-26b,
and now records). Coverage includes the pad bytes.

- Engine: **the containing file's engine** (the F-5a rule), seed 0, low 32
  bits, little-endian. Engine 0 writes zero and verifies nothing.
- Locating the checksum requires the length fields, exactly as locating a
  terminator's checksum does. A corrupt length hashes the wrong range against
  the wrong field and fails verification with the same confidence as any other
  corruption; bounds are enforced by the existing bounds-checked accessor
  before any read.

## Version (clean break)

The header writes `version_read = version_write = 2`. Readers MUST reject
version 1: the spec states version 1 was never released. The corpus is fully
regenerated (this is precisely the format change the corpus rules reserve a
spec commit for). The pointer-table cache format is unaffected (it stores
offsets, not record bytes) and its version does not change.

## When verification happens — and deliberately does not

- **Read path:** a record is verified when it is materialized for a caller —
  lookup returns and cursor yields — skipped under `ZS_NOCSUM` (F-5e, the
  read-path flag). Failure is `ZS_BADCHECKSUM` for that key; other keys stay
  readable.
- **`zs_db_check_consistency`:** verifies every record, reporting per record.
- **Salvage:** verifies per record, so `ZS_SALVAGE_KEY_UNVERIFIED` shrinks to
  engine-0 files.
- **D-20b (convert/repack/seal/compact):** unchanged — the span-chain and
  records-region passes remain the pre-copy gate.
- **Deliberately NOT during span replay or pointer-section load.** Replay's
  contract is F-24: an invalid record completes the file at that point,
  discarding everything after it. Verifying record checksums there would let
  one flipped byte truncate the live tail of an unordered file — a G-3
  data-loss bug, the same trap CLAUDE.md documents for non-canonical records.
  A bad record inside a valid span is in-place corruption; it fails at
  materialization, not at structure-walking time. This goes into CLAUDE.md's
  "things that look like bugs and are not".

## Repack/conversion interaction

A record copied byte-for-byte keeps its checksum, and that is only valid when
the output file's engine matches the input's. Both conversion and repack write
their output under the handle's create engine (`zsi_write_inorder` and
`zsi_repack_merge` both use `db->create_csum_id`), so both get the same rule:
copy verbatim when the engines match, re-encode — which recomputes the
checksum — when they differ. The merge already re-encodes when the ancestor
form changes; this adds one more trigger.

## Implementation surface

- `zsi_rec_encoded_len`: `+ 4`.
- `zsi_rec_encode`: takes the engine, computes and stores the checksum.
- `zsi_rec_decode`: bounds-reads the stored checksum into `struct zsi_rec`.
- New `zsi_rec_verify(f, rec)`: one choke point used by the yield paths,
  `check`, and salvage.
- Yield-path call sites: `zsi_lookup`'s return, `zsi_cursor_next`'s emit (the
  txn arm's pending records are not file records and are not verified).
- `ZSI_VERSION_READ`/`WRITE` constants; header decode rejects `!= 2`.
- Test builders `ib_rec`/`sb_rec` updated; golden corpus regenerated; byte
  layout asserted against literals per form (the symmetric-swap mutant class);
  T-2c-style vectors updated.
- zstool/zsbench: no interface or output change. Per-record failures surface
  through `check` and the error callback, not through dump.

## Tests

- Record byte layout against literals, every form, both engines.
- Read path: corrupt one value byte in an in-order file → that key returns
  `ZS_BADCHECKSUM`, sibling keys still read, a `ZS_NOCSUM` handle still reads
  the corrupt value.
- Same for a record inside a valid span in an unordered file (span checksum
  recomputed over the corrupted bytes so the span still validates — pure
  record-level corruption).
- Replay does NOT truncate: an unordered file whose mid-file record has a bad
  record checksum (but valid span) keeps every later record visible.
- Engine 0: field written as zero, nothing verified.
- Version: a version-1 header is rejected; version 2 round-trips.

## Mutants

- Skip verify at yield (the headline gap).
- Checksum computed over the wrong range (off-by-pad).
- Verify during replay (the tempting wrong version — caught by the
  no-truncation test).
- Encode under the handle's engine instead of the file's (the A-6/F-5a class).

## Commit order

1. `spec:` — layouts, the stated checksum convention, version 2, new F-labels,
   conformance rows, corpus regeneration.
2. `feat:`/`fix:` — implementation, tests, mutants.
