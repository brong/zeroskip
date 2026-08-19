#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Exercise zstool's driver contract (T-0a).
#
# This tests the tool's LINE FORMAT, not the library: those lines are what a
# language-neutral runner compares between implementations (T-12, T-13), so a
# change to them breaks other implementations' test runs rather than just a
# human's expectations.  Everything is asserted against literal expected output.
#
# The runner drives the tool with --hex; the default is raw, for humans.  Both
# modes are pinned here -- the hex lines because a peer compares them, the raw
# lines because nothing else would notice them regressing.

set -u
cd "$(dirname "$0")/.." || exit 1

TOOL=./zstool
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

pass=0
fail=0

# check <name> <expected> <actual>
check() {
    if [ "$2" = "$3" ]; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf '  FAIL %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
    fi
}

UUID=4941da54-9406-4faa-a457-c4b65beae3eb
DB="$WORK/db"

# --- create, with an explicit UUID so output is reproducible (T-1) ------------
$TOOL "$DB" create --uuid "$UUID" || { echo "  FAIL create"; exit 1; }
check "create names the active file .current, not a generation (D-1b)" \
    "zeroskip-$UUID.current" \
    "$(ls "$DB" | grep '^zeroskip-')"
# The dot is what keeps the generation-named glob clean: no active file in it.
check "the generation glob excludes the active file" "0" \
    "$(ls "$DB" | grep -c "^zeroskip-$UUID-")"
check "create makes a lock file" "zeroskip.lock" \
    "$(ls "$DB" | grep '^zeroskip\.')"

# --- store / get -------------------------------------------------------------
$TOOL "$DB" store 6b6579 76616c --hex
check "get returns the value in hex" "76616c" "$($TOOL "$DB" get 6b6579 --hex)"
check "get of an absent key" "NOTFOUND" "$($TOOL "$DB" get 6e6f7065 --hex)"

# --- an empty value is distinct from an absent key (A-1) ---------------------
$TOOL "$DB" store 656d70747970 "" --hex
check "an empty value reads back empty, not NOTFOUND" "" \
    "$($TOOL "$DB" get 656d70747970 --hex)"

# --- delete ------------------------------------------------------------------
$TOOL "$DB" delete 6b6579 --hex
check "get after delete" "NOTFOUND" "$($TOOL "$DB" get 6b6579 --hex)"

# --- keys and values containing NUL and newline ------------------------------
# 00 0a 00 as a key, 0a 00 0a as a value.  This is why the runner's mode is hex:
# raw output could not express either, and T-12 compares scan output byte for
# byte.
$TOOL "$DB" store 000a00 0a000a --hex
check "a key with NUL and newline round-trips" "0a000a" \
    "$($TOOL "$DB" get 000a00 --hex)"

# --- batch: several operations in ONE transaction ----------------------------
# A tool that could only do one operation per invocation could never produce a
# span with more than one record, so multi-record spans would be untestable.
printf 'store 61 3031\nstore 62 3032\nstore 63 3033\ndelete 62\n' \
    | $TOOL "$DB" batch --hex
check "batch stored a" "3031" "$($TOOL "$DB" get 61 --hex)"
check "batch deleted b" "NOTFOUND" "$($TOOL "$DB" get 62 --hex)"
check "batch stored c" "3033" "$($TOOL "$DB" get 63 --hex)"

# FOUR records, not three: the writer STREAMS (C-8), so `store 62` followed by
# `delete 62` appends both -- the span reflects the transaction's history, and
# the later record shadows the earlier by offset order (D-17b).  Reads still
# see a MAP: the pending index repoints to the newest record per key, which is
# what keeps read-your-own-writes consistent (A-1a).
check "the batch made one span of four records" "1" \
    "$($TOOL "$DB" dump | grep -c 'records=4')"

# --- batch abort: nothing written at all (T-0a, C-8b) ------------------------
# This batch is far smaller than the writer's append buffer, so no record ever
# reached the file and there is nothing for a ROLLBACK to void: the abort writes
# NOTHING, and the file is byte-identical to what it was before.  It used to
# assert the opposite -- a natively written ROLLBACK span -- which is why the
# span count is asserted as zero rather than simply dropped.  A rolled-back span
# on disk is still legal and still readable; it is just no longer something a
# buffering writer produces, so tests/corpus/rolled-back-span injects one.
SIZE_BEFORE=$(wc -c < "$DB/$(ls "$DB" | grep '^zeroskip-' | sort | tail -1)" | tr -d ' ')
printf 'store 6464 3031\nstore 6465 3032\nabort\n' | $TOOL "$DB" batch --hex
check "aborted store is absent" "NOTFOUND" "$($TOOL "$DB" get 6464 --hex)"
check "abort left no rollback span" "0" \
    "$($TOOL "$DB" dump | grep -c 'ROLLBACK')"
check "abort wrote no bytes" "$SIZE_BEFORE" \
    "$(wc -c < "$DB/$(ls "$DB" | grep '^zeroskip-' | sort | tail -1)" | tr -d ' ')"
$TOOL "$DB" store 6466 3033 --hex
check "a commit after the abort lands" "3033" "$($TOOL "$DB" get 6466 --hex)"
check "check passes after the abort" "OK" "$($TOOL "$DB" check | tail -1)"
$TOOL "$DB" delete 6466 --hex

# --- scan: every visible pair, in comparator order ---------------------------
# Shorter keys sort first (F-11a), so 000a00 precedes 61.
check "scan output" \
"000a00 0a000a
61 3031
63 3033
656d70747970 " \
    "$($TOOL "$DB" scan --hex)"

check "scan --prefix" "61 3031" "$($TOOL "$DB" scan --prefix 61 --hex)"
check "scan --prefix matching nothing" "" "$($TOOL "$DB" scan --prefix ff --hex)"

# --- check -------------------------------------------------------------------
check "check on a clean database" "OK" "$($TOOL "$DB" check)"

# --- dump line format --------------------------------------------------------
check "dump names the file kind" "1" \
    "$($TOOL "$DB" dump | grep -c 'kind=unordered')"
check "dump reports the generation range" "1" \
    "$($TOOL "$DB" dump | grep -c 'start=1 end=0')"
check "dump reports the checksum engine" "1" \
    "$($TOOL "$DB" dump | grep -c 'csum=1')"
check "dump --detail shows records" "1" \
    "$($TOOL "$DB" dump --detail 1 | grep -c 'key=61')"

# --- raw mode (the default): arguments are the bytes themselves --------------
# For humans at a shell, not for the runner: raw cannot express a NUL or a
# newline, which is what --hex is for.  Output pairs are key<TAB>value, since a
# value may contain spaces.
DBR="$WORK/dbraw"
$TOOL "$DBR" create --uuid "$UUID"
$TOOL "$DBR" store greeting "hello world"
check "raw get prints the raw value" "hello world" "$($TOOL "$DBR" get greeting)"
check "raw get of an absent key fails" "1" \
    "$($TOOL "$DBR" get nope >/dev/null 2>&1; echo $?)"
$TOOL "$DBR" store empty ""
check "raw empty value reads back empty" "" "$($TOOL "$DBR" get empty)"
printf 'store\tbatchkey\tbatch value\ndelete\tgreeting\n' | $TOOL "$DBR" batch
check "raw batch lines are tab-separated" "batch value" \
    "$($TOOL "$DBR" get batchkey)"
check "raw batch deleted" "1" \
    "$($TOOL "$DBR" get greeting >/dev/null 2>&1; echo $?)"
check "raw scan output is key<TAB>value" \
    "$(printf 'batchkey\tbatch value\nempty\t')" \
    "$($TOOL "$DBR" scan)"
check "raw scan --prefix" "$(printf 'batchkey\tbatch value')" \
    "$($TOOL "$DBR" scan --prefix batch)"

# --- create --nochecksum: engine 0 -------------------------------------------
DBN="$WORK/dbn"
$TOOL "$DBN" create --uuid "$UUID" --nochecksum
$TOOL "$DBN" store k v
check "nochecksum writes engine-0 files" "1" \
    "$($TOOL "$DBN" dump | grep -c 'csum=0')"
check "nochecksum data reads back" "v" "$($TOOL "$DBN" get k)"

# --- convert -----------------------------------------------------------------
# A second generation, then convert: the first becomes an in-order file.
DB2="$WORK/db2"
$TOOL "$DB2" create --uuid "$UUID"
$TOOL "$DB2" store 61 3031 --hex
# Force a new generation by making the active file unclean, as a crash would.
# D-12b: the writer converts the unclean file before taking the name for the
# next generation, so this leaves an in-order file plus a new active one -- not
# two unordered files, which D-1b makes unrepresentable.
printf '\336\255\276\357\336\255\276\357' >> "$DB2/zeroskip-$UUID.current"
$TOOL "$DB2" store 62 3032 --hex --noautorepack
check "an unclean active file forces a new generation" "2" \
    "$(ls "$DB2" | grep -c '^zeroskip-')"
$TOOL "$DB2" convert --noautorepack
check "convert produced an in-order file" "1" \
    "$($TOOL "$DB2" dump | grep -c 'kind=inorder')"
check "convert kept the data" "3031" "$($TOOL "$DB2" get 61 --hex)"
check "check after convert" "OK" "$($TOOL "$DB2" check)"

# --- repack ------------------------------------------------------------------
DB3="$WORK/db3"
$TOOL "$DB3" create --uuid "$UUID"
# --noautorepack: the point of this case is that `repack` reduces the count, so
# the layout has to survive being built.  D-16e merges it away otherwise, and
# the case would pass for the wrong reason -- nothing left to merge (A-14).
for i in 1 2 3 4 5 6 7 8; do
    $TOOL "$DB3" store "3$i" "763$i" --hex --noautorepack
    printf '\336\255\276\357\336\255\276\357' \
        >> "$DB3/$(ls "$DB3" | grep '^zeroskip-' | sort | tail -1)"
done
$TOOL "$DB3" convert --noautorepack
before=$(ls "$DB3" | grep -c '^zeroskip-')
$TOOL "$DB3" repack
after=$(ls "$DB3" | grep -c '^zeroskip-')
if [ "$after" -lt "$before" ]; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    printf '  FAIL repack did not reduce the file count (%s -> %s)\n' \
        "$before" "$after"
fi
check "check after repack" "OK" "$($TOOL "$DB3" check)"
check "data survived the repack" "7631" "$($TOOL "$DB3" get 31 --hex)"

# --- hold-write --------------------------------------------------------------
# The runner needs to know the lock is HELD before starting the process it expects
# to block, or the test races.  So hold-write announces it on stdout.
DB4="$WORK/db4"
$TOOL "$DB4" create --uuid "$UUID"
out=$($TOOL "$DB4" hold-write --for 50)
check "hold-write announces that it holds the lock" "HELD" "$out"

# --- reproducibility: the same UUID and operations give identical bytes ------
# The single-implementation half of T-12a.  Encoding is canonical (F-15) and no
# timestamps enter the format, so this must hold exactly.
A="$WORK/repro-a"
B="$WORK/repro-b"
for d in "$A" "$B"; do
    $TOOL "$d" create --uuid "$UUID"
    printf 'store 61 3031\nstore 62 3032\ndelete 61\n' | $TOOL "$d" batch --hex
    $TOOL "$d" store 63 3033 --hex
done
if diff -r "$A" "$B" >/dev/null 2>&1; then
    pass=$((pass + 1))
else
    fail=$((fail + 1))
    echo "  FAIL identical operations produced different bytes"
    diff -r "$A" "$B" | head -5
fi

# --- errors ------------------------------------------------------------------
check "malformed hex is rejected" "2" \
    "$($TOOL "$DB" get 6b65790 --hex >/dev/null 2>&1; echo $?)"
check "opening a missing database fails" "1" \
    "$($TOOL "$WORK/nope" get 61 >/dev/null 2>&1; echo $?)"
check "an unknown command prints usage" "2" \
    "$($TOOL "$DB" frobnicate >/dev/null 2>&1; echo $?)"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
