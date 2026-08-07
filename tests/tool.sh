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
check "create names the file after the uuid" \
    "zeroskip-$UUID-00000001" \
    "$(ls "$DB" | grep '^zeroskip-')"
check "create makes a lock file" "zeroskip.lock" \
    "$(ls "$DB" | grep '^zeroskip\.')"

# --- store / get -------------------------------------------------------------
$TOOL "$DB" store 6b6579 76616c
check "get returns the value in hex" "76616c" "$($TOOL "$DB" get 6b6579)"
check "get of an absent key" "NOTFOUND" "$($TOOL "$DB" get 6e6f7065)"

# --- an empty value is distinct from an absent key (A-1) ---------------------
$TOOL "$DB" store 656d70747970 ""
check "an empty value reads back empty, not NOTFOUND" "" \
    "$($TOOL "$DB" get 656d70747970)"

# --- delete ------------------------------------------------------------------
$TOOL "$DB" delete 6b6579
check "get after delete" "NOTFOUND" "$($TOOL "$DB" get 6b6579)"

# --- keys and values containing NUL and newline ------------------------------
# 00 0a 00 as a key, 0a 00 0a as a value.  This is why the format is hex: raw
# output could not express either, and T-12 compares scan output byte for byte.
$TOOL "$DB" store 000a00 0a000a
check "a key with NUL and newline round-trips" "0a000a" \
    "$($TOOL "$DB" get 000a00)"

# --- batch: several operations in ONE transaction ----------------------------
# A tool that could only do one operation per invocation could never produce a
# span with more than one record, so multi-record spans would be untestable.
printf 'store 61 3031\nstore 62 3032\nstore 63 3033\ndelete 62\n' \
    | $TOOL "$DB" batch
check "batch stored a" "3031" "$($TOOL "$DB" get 61)"
check "batch deleted b" "NOTFOUND" "$($TOOL "$DB" get 62)"
check "batch stored c" "3033" "$($TOOL "$DB" get 63)"

# Three records, not four: a transaction is a MAP, so `store 62` followed by
# `delete 62` coalesces to one pending entry (a deletion).  That is what makes
# read-your-own-writes consistent (A-1a), and it means the span reflects the
# transaction's final state rather than its history.
check "the batch made one span of three records" "1" \
    "$($TOOL "$DB" dump | grep -c 'records=3')"

# --- scan: every visible pair, in comparator order ---------------------------
# Shorter keys sort first (F-11a), so 000a00 precedes 61.
check "scan output" \
"000a00 0a000a
61 3031
63 3033
656d70747970 " \
    "$($TOOL "$DB" scan)"

check "scan --prefix" "61 3031" "$($TOOL "$DB" scan --prefix 61)"
check "scan --prefix matching nothing" "" "$($TOOL "$DB" scan --prefix ff)"

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

# --- convert -----------------------------------------------------------------
# A second generation, then convert: the first becomes an in-order file.
DB2="$WORK/db2"
$TOOL "$DB2" create --uuid "$UUID"
$TOOL "$DB2" store 61 3031
# Force a new generation by making the active file unclean, as a crash would.
printf '\336\255\276\357\336\255\276\357' >> "$DB2/zeroskip-$UUID-00000001"
$TOOL "$DB2" store 62 3032
check "an unclean active file forces a new generation" "2" \
    "$(ls "$DB2" | grep -c '^zeroskip-')"
$TOOL "$DB2" convert
check "convert produced an in-order file" "1" \
    "$($TOOL "$DB2" dump | grep -c 'kind=inorder')"
check "convert kept the data" "3031" "$($TOOL "$DB2" get 61)"
check "check after convert" "OK" "$($TOOL "$DB2" check)"

# --- repack ------------------------------------------------------------------
DB3="$WORK/db3"
$TOOL "$DB3" create --uuid "$UUID"
for i in 1 2 3 4 5 6 7 8; do
    $TOOL "$DB3" store "3$i" "763$i"
    printf '\336\255\276\357\336\255\276\357' \
        >> "$DB3/$(ls "$DB3" | grep '^zeroskip-' | sort | tail -1)"
done
$TOOL "$DB3" convert
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
check "data survived the repack" "7631" "$($TOOL "$DB3" get 31)"

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
    printf 'store 61 3031\nstore 62 3032\ndelete 61\n' | $TOOL "$d" batch
    $TOOL "$d" store 63 3033
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
    "$($TOOL "$DB" get 6b65790 >/dev/null 2>&1; echo $?)"
check "opening a missing database fails" "1" \
    "$($TOOL "$WORK/nope" get 61 >/dev/null 2>&1; echo $?)"
check "an unknown command prints usage" "2" \
    "$($TOOL "$DB" frobnicate >/dev/null 2>&1; echo $?)"

echo
printf '%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
