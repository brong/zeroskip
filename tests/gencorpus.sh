#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Generate the golden corpus (T-0, T-1).
#
# This writes tests/corpus/<case>/ for each case: the database's files, plus a
# case.txt describing it in the portable format documented in
# tests/corpus/README.md.
#
# The checked-in bytes are the contract.  This script exists to ADD cases, not to
# resolve a diff -- if it changes an existing case, that is a format change and
# needs a spec commit, not a corpus commit hiding it.  So it refuses to overwrite
# unless asked.

set -u
cd "$(dirname "$0")/.." || exit 1

ZSTOOL=./zstool
CORPUS=tests/corpus
UUID=4941da54-9406-4faa-a457-c4b65beae3eb

# The corpus is hex end to end -- keys carry NULs and newlines, and case.txt is
# compared as text -- so every invocation here opts in.  The tool's default is
# raw, for humans.
# --noautorepack throughout: the corpus is the FORMAT contract, and repack
# selection is explicitly not normative (D-16).  A case that let the cascade
# fire would pin this implementation's policy into golden bytes, and a peer with
# a different policy would read a spurious diff.  Cases that want a merge say so
# with an explicit `op repack`.
TOOL() { "$ZSTOOL" "$@" --hex --noautorepack; }

FORCE=0
[ "${1:-}" = "--force" ] && FORCE=1

if [ ! -x "$ZSTOOL" ]; then
    echo "gencorpus: build zstool first (make)" >&2
    exit 1
fi

# newest_file <dir> -- the highest-generation data file, for truncate/garbage
newest_file() {
    ls "$1" | grep '^zeroskip-' | sort | tail -1
}

# begin_case <name>
begin_case() {
    CASE="$CORPUS/$1"
    if [ -e "$CASE" ] && [ "$FORCE" -ne 1 ]; then
        echo "  skip $1 (exists; --force to regenerate)"
        SKIP=1
        return
    fi
    SKIP=0
    rm -rf "$CASE"
    mkdir -p "$CASE"
    DB="$CASE/db"
    TXT="$CASE/case.txt"
    IDXDIR=""
    : > "$TXT"
    echo "  $1"
}

# finish_case: move the database's files up beside case.txt, then record the
# expectations by asking the tool.
finish_case() {
    [ "$SKIP" -eq 1 ] && return

    {
        echo ""
        echo "expect files"
        ls "$DB" | grep '^zeroskip-' | sort
        echo ""
        echo "expect scan"
        TOOL "$DB" scan
        echo ""
        echo "expect check $(TOOL "$DB" check)"

        # The pointer table cache (spec section 8), when the case has one.  Its
        # bytes are shipped so a peer can prove it reads OUR table rather than
        # merely writing a table of its own -- which is the only way to tell a
        # shared format from a coincidentally similar one.
        if [ -n "$IDXDIR" ]; then
            echo ""
            echo "expect index"
            TOOL "$DB" index-dump --index-dir "$IDXDIR"
        fi
    } >> "$TXT"

    # The data files live beside case.txt, not in a subdirectory: an implementation
    # validating the corpus should be able to point its open() at the case directory.
    #
    # The lock file is NOT included: it holds no state (D-3c) and is recreated on
    # open if absent (D-3a), so shipping it would add a byte-identical file to
    # every case for no reason -- and would invite someone to think it mattered.
    for f in "$DB"/zeroskip-*; do mv "$f" "$CASE"/; done
    rm -f "$DB/zeroskip.lock"
    rmdir "$DB"
}

emit() { [ "$SKIP" -eq 1 ] || echo "$1" >> "$TXT"; }

hdr() {
    emit "# $1"
    emit "uuid $UUID"
    emit "engine $2"
    emit "comparator memcmp"
    [ -n "${3:-}" ] && emit "rollover $3"
    emit ""
}

mkdir -p "$CORPUS"
echo "generating corpus in $CORPUS"

# ---------------------------------------------------------------------------
# 1. empty: a database that has only ever been created (D-8a).
# ---------------------------------------------------------------------------
begin_case empty
if [ "$SKIP" -eq 0 ]; then
    hdr "A newly created database: one active file, a 72-byte header, no spans." 1
    TOOL "$DB" create --uuid "$UUID"
    finish_case
fi

# ---------------------------------------------------------------------------
# 2. active-records: records still in the active file, no conversion (T-12).
# ---------------------------------------------------------------------------
begin_case active-records
if [ "$SKIP" -eq 0 ]; then
    hdr "Records in the active unordered file: one span per store." 1
    TOOL "$DB" create --uuid "$UUID"
    for kv in "61 3031" "62 3032" "63 3033"; do
        set -- $kv
        TOOL "$DB" store "$1" "$2"
        emit "op store $1 $2"
    done
    finish_case
fi

# ---------------------------------------------------------------------------
# 3. multi-record-span: one transaction, several records (T-0a's batch).
# ---------------------------------------------------------------------------
begin_case multi-record-span
if [ "$SKIP" -eq 0 ]; then
    hdr "One transaction holding several records, so a multi-record span exists." 1
    TOOL "$DB" create --uuid "$UUID"
    emit "op batch"
    emit "store 6b31 7631"
    emit "store 6b32 7632"
    emit "store 6b33 7633"
    emit "op end"
    printf 'store 6b31 7631\nstore 6b32 7632\nstore 6b33 7633\n' \
        | TOOL "$DB" batch
    finish_case
fi

# ---------------------------------------------------------------------------
# 3a. rolled-back-span: an aborted transaction between two commits (C-8, F-25).
# ---------------------------------------------------------------------------
# INJECTED, not aborted, and the reason is C-8b.  A writer whose aborted span
# never left its buffer writes nothing -- there is no record in the file to void
# -- so whether an abort leaves a rolled-back span depends on the writer's buffer
# size, and no conforming writer can be required to produce one.  Aborting a
# two-record transaction here would compare our buffering against a peer's rather
# than the format, and would produce nothing at all on this implementation since
# ZSI_TXN_CHUNK grew.
#
# What is normative is the READER's side, which is what the case tests: the middle
# span's keys must be invisible while the spans on either side stay live, which is
# exactly F-25's per-span visibility.  The bytes below are one such span -- two
# records and a ROLLBACK terminator, engine 1 -- and every implementation's driver
# reproduces them through `op garbage`.
ROLLED_BACK_SPAN=0101020062003032000000004cfa0bf8010102006300303300000000d1c95fe612200000c1ab777a
begin_case rolled-back-span
if [ "$SKIP" -eq 0 ]; then
    hdr "A rolled-back span sitting between two committed ones (C-8, F-25).
#
# The span is INJECTED as raw bytes rather than produced by aborting a
# transaction.  Since C-8b a writer whose aborted span never left its buffer
# writes nothing -- no record reached the file, so there is nothing to void --
# and whether an abort leaves a rolled-back span on disk therefore depends on
# the writer's buffer size.  No conforming writer can be required to produce
# one, so a case that aborted a two-record transaction would be comparing this
# implementation's buffering against a peer's rather than comparing the format.
#
# What IS normative is the reader's side, and it is what this case tests: a
# rolled-back span may sit between two committed ones (F-25) and its records
# are void (F-21)." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    printf "$(echo "$ROLLED_BACK_SPAN" | sed 's/../\\x&/g')" >> "$DB/$(newest_file "$DB")"
    emit "op garbage $ROLLED_BACK_SPAN"
    TOOL "$DB" store 64 3034; emit "op store 64 3034"
    finish_case
fi

# ---------------------------------------------------------------------------
# 3b. sealed-active: a SEAL terminator, and what a writer must do about it.
#
# The marker is INJECTED for the same reason the rolled-back span above is: it is
# written by C-7e after a failed durability gate, which no driver subcommand can
# reproduce and no conforming writer can be required to produce on demand.  What
# IS normative is everything after it -- F-21a's encoding, that the records below
# stay live and the file stays CLEAN, and that a writer meeting the marker MUST
# NOT append to that file.
#
# The store after the injection is what makes this a test rather than a byte
# dump: a writer that ignored the marker would append to generation 1 and leave
# no in-order file at all, so `expect files` is what separates the two.
#
# The 20 bytes are a SEAL for engine 1: nibble 0xA with keylen 0 (0a 00), vallen
# 16 (10 00), a span length of 0, and the F-19 checksum over the empty span
# followed by the terminator's own first 12 bytes.
# ---------------------------------------------------------------------------
SEAL_TERM=0a001000000000000000000016dd693933f6c2e8
begin_case sealed-active
if [ "$SKIP" -eq 0 ]; then
    hdr "A SEAL terminator ending the active file (F-21a, C-7e).
#
# The marker is injected as raw bytes, not produced by a writer: C-7e writes one
# only after a durability gate has failed, and no driver subcommand can make that
# happen.  What the case pins is the reader's and the next writer's side.
#
# A SEAL is an ordinary empty span -- zero records, span length 0, an F-19
# checksum like any other terminator -- so the file remains CLEAN and every
# record below it stays live.  The marker is not a truncation, which is what the
# scan below asserts.
#
# What it changes is that the file MUST NOT be appended to (F-21a, D-9).  So the
# store that follows cannot extend generation 1: a writer converts it to its
# in-order form and starts generation 2, which is what the file list shows.  An
# implementation that ignored the marker would leave one .current file and no
# in-order file at all." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    printf "$(echo "$SEAL_TERM" | sed 's/../\\x&/g')" >> "$DB/$(newest_file "$DB")"
    emit "op garbage $SEAL_TERM"
    TOOL "$DB" store 62 3032; emit "op store 62 3032"
    finish_case
fi

# ---------------------------------------------------------------------------
# 4. deletion: a tombstone, and an empty value beside it (A-1).
# ---------------------------------------------------------------------------
begin_case deletion
if [ "$SKIP" -eq 0 ]; then
    hdr "A deletion, and an empty value -- which are distinct states (A-1)." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 6b6565 76616c;  emit "op store 6b6565 76616c"
    TOOL "$DB" store 656d7074 "";    emit "op store 656d7074 "
    TOOL "$DB" delete 6b6565;        emit "op delete 6b6565"
    finish_case
fi

# ---------------------------------------------------------------------------
# 5. binary-keys: NUL bytes and newlines in keys and values (F-13).
# ---------------------------------------------------------------------------
begin_case binary-keys
if [ "$SKIP" -eq 0 ]; then
    hdr "Keys and values containing NUL and newline; lengths are authoritative." 1
    TOOL "$DB" create --uuid "$UUID"
    for kv in "00 00" "000a00 0a000a" "6100 006100" "ff00ff 00ff00"; do
        set -- $kv
        TOOL "$DB" store "$1" "$2"
        emit "op store $1 $2"
    done
    finish_case
fi

# ---------------------------------------------------------------------------
# 6. encoding-boundaries: the short/big form thresholds (F-15).
# ---------------------------------------------------------------------------
begin_case encoding-boundaries
if [ "$SKIP" -eq 0 ]; then
    hdr "Keys of 255 and 256 bytes, values of 65535 and 65536, so both forms appear." 1
    TOOL "$DB" create --uuid "$UUID"
    K255=$(printf '61%.0s' $(seq 255))
    K256=$(printf '62%.0s' $(seq 256))
    V65535=$(printf '78%.0s' $(seq 65535))
    V65536=$(printf '79%.0s' $(seq 65536))
    TOOL "$DB" store "$K255" 73; emit "op store $K255 73"
    TOOL "$DB" store "$K256" 62; emit "op store $K256 62"
    TOOL "$DB" store 7631 "$V65535"; emit "op store 7631 $V65535"
    TOOL "$DB" store 7632 "$V65536"; emit "op store 7632 $V65536"
    finish_case
fi

# ---------------------------------------------------------------------------
# 7. converted: a single-generation in-order file (D-12).
# ---------------------------------------------------------------------------
begin_case converted
if [ "$SKIP" -eq 0 ]; then
    hdr "A converted single-generation in-order file, with its pointer section." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    TOOL "$DB" store 62 3032; emit "op store 62 3032"
    # An unclean active file forces a new generation, leaving gen 1 convertible.
    printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
    emit "op garbage deadbeefdeadbeef"
    TOOL "$DB" store 63 3033; emit "op store 63 3033"
    TOOL "$DB" convert;       emit "op convert"
    finish_case
fi

# ---------------------------------------------------------------------------
# 8. repacked: a merged multi-generation file (D-16).
# ---------------------------------------------------------------------------
begin_case repacked
if [ "$SKIP" -eq 0 ]; then
    hdr "A merged multi-generation in-order file, after a repack." 1
    TOOL "$DB" create --uuid "$UUID"
    for i in 1 2 3 4; do
        TOOL "$DB" store "3$i" "763$i"
        emit "op store 3$i 763$i"
        printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
        emit "op garbage deadbeefdeadbeef"
    done
    TOOL "$DB" store 39 7639; emit "op store 39 7639"
    TOOL "$DB" convert;       emit "op convert"
    TOOL "$DB" repack;        emit "op repack"
    finish_case
fi

# ---------------------------------------------------------------------------
# 9. empty-inorder: a repack that dropped every key (D-22, F-26g).
# ---------------------------------------------------------------------------
begin_case empty-inorder
if [ "$SKIP" -eq 0 ]; then
    hdr "A repack output holding ZERO records: exactly 96 bytes, PTRS32 (F-26g).
# The second generation is deliberately the larger one, so that the merge is
# selected by size; the file it produces is what the case is about." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 6f6e6c79 76; emit "op store 6f6e6c79 76"
    printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
    emit "op garbage deadbeefdeadbeef"
    TOOL "$DB" delete 6f6e6c79;   emit "op delete 6f6e6c79"
    TOOL "$DB" store 78 79;       emit "op store 78 79"
    TOOL "$DB" delete 78;         emit "op delete 78"
    printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
    emit "op garbage deadbeefdeadbeef"
    TOOL "$DB" store 7a 7a;       emit "op store 7a 7a"
    TOOL "$DB" convert;           emit "op convert"
    TOOL "$DB" repack;            emit "op repack"
    finish_case
fi

# ---------------------------------------------------------------------------
# 10. torn-tail: a crash mid-append (F-24).
# ---------------------------------------------------------------------------
begin_case torn-tail
if [ "$SKIP" -eq 0 ]; then
    hdr "Trailing garbage after the last valid span: complete short of the end." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    TOOL "$DB" store 62 3032; emit "op store 62 3032"
    printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
    emit "op garbage deadbeefdeadbeef"
    finish_case
fi

# ---------------------------------------------------------------------------
# 11a. cached-index: a published pointer table beside the data (spec section 8).
# ---------------------------------------------------------------------------
# The cache lives in a SUBDIRECTORY, not beside the data files: P-2 forbids the
# cache directory from being the database directory, so a case that put them
# together could not be opened the way it was built.
begin_case cached-index
if [ "$SKIP" -eq 0 ]; then
    IDXDIR="$CASE/index"
    mkdir -p "$IDXDIR"
    hdr "A published pointer table over the active file (spec section 8)." 1
    emit "indexdir index"
    emit ""
    TOOL "$DB" create --uuid "$UUID" --index-dir "$IDXDIR"
    TOOL "$DB" store 61 3031 --index-dir "$IDXDIR"; emit "op store 61 3031"
    TOOL "$DB" store 62 3032 --index-dir "$IDXDIR"; emit "op store 62 3032"
    TOOL "$DB" store 61 3033 --index-dir "$IDXDIR"; emit "op store 61 3033"
    finish_case
fi

# ---------------------------------------------------------------------------
# 12. engine0: the same shape with no checksums (F-5, F-5c).
# ---------------------------------------------------------------------------
begin_case engine0
if [ "$SKIP" -eq 0 ]; then
    hdr "Engine 0: checksum fields are zero and nothing is verified." 0
    TOOL "$DB" create --uuid "$UUID" --nochecksum
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    TOOL "$DB" store 62 3032; emit "op store 62 3032"
    finish_case
fi

# ---------------------------------------------------------------------------
# 13. cross-generation-update: a key updated and deleted ACROSS generations
# (F-18).
#
# This is the case the format used to encode differently.  A record whose key
# was last written in an OLDER file carried a 4-byte ancestor and a distinct
# type byte -- KEYVALUE_ANC or DELETION_ANC -- so the same store produced
# different bytes depending on the database's history.  It no longer does: a
# record is a function of its own key and value, so every record here is a
# plain KEYVALUE or DELETION and is byte-identical to the same store into an
# empty database.
#
# It is worth a case precisely because the corpus never had one.  Every
# existing case writes into the generation it created, so none of them ever
# exercised the ancestor forms -- which is why removing those forms changed no
# golden byte, and why nothing here would have caught an implementation that
# kept writing them.
# ---------------------------------------------------------------------------
begin_case cross-generation-update
if [ "$SKIP" -eq 0 ]; then
    hdr "A key updated and deleted across generations: no back-reference (F-18)." 1
    TOOL "$DB" create --uuid "$UUID"
    TOOL "$DB" store 61 3031; emit "op store 61 3031"
    TOOL "$DB" store 62 3032; emit "op store 62 3032"
    # An unclean active file forces a new generation, so the stores below land
    # in generation 2 while the keys they supersede stay in generation 1 --
    # which is the whole point of the case.  A plain `convert` here would be a
    # no-op: only a NON-ACTIVE unordered file is convertible (D-12b).
    printf '\336\255\276\357\336\255\276\357' >> "$DB/$(newest_file "$DB")"
    emit "op garbage deadbeefdeadbeef"
    TOOL "$DB" store 61 3039; emit "op store 61 3039"
    TOOL "$DB" delete 62;     emit "op delete 62"
    TOOL "$DB" convert;       emit "op convert"
    finish_case
fi

echo "done"
