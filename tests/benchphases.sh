#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Check zsbench's --setup / --run split: that a fixture built by one invocation
# is timed by the next, that the run phase measures the same thing the combined
# run does, and that a mismatched run phase is REFUSED rather than measured.
#
# The last one is the point of the stamp file.  A --run given a different -n
# would fetch keys that were never stored and scan a database of the wrong size,
# and report an excellent number for missing every time -- the same failure mode
# --selftest exists to prevent, arriving by a different door.
#
#     ./tests/benchphases.sh

set -u
cd "$(dirname "$0")/.." || exit 1

ZSBENCH=${ZSBENCH:-./zsbench}
N=2000
FIX=$(mktemp -d "${TMPDIR:-/tmp}/zsbench-phases.XXXXXX") || exit 1
OUT=$(mktemp -d "${TMPDIR:-/tmp}/zsbench-phases-out.XXXXXX") || exit 1

fail=0
ok()   { printf '  ok      %s\n' "$1"; }
bad()  { printf '  FAILED  %s\n' "$1"; fail=$((fail + 1)); }
check() { if [ "$2" = "$3" ]; then ok "$1"; else bad "$1: expected [$3], got [$2]"; fi; }

cleanup() { rm -rf "$FIX" "$OUT"; }
trap cleanup EXIT

# The workloads that can be split.  Everything else in zsbench either measures
# its own setup (the store rows) or has setup that cannot cross a process
# boundary (a live write transaction), so the run phase skips it.
SPLIT="fetch fetch_repacked scan scan_compacted txn_scan_mixed"

# slug,ops for the splittable rows only, sorted, so two CSVs can be diffed
# whatever order they were produced in.
slugops() {
    awk -F, -v want="$SPLIT" '
        NR == 1 { next }
        { n = split(want, w, " ")
          for (i = 1; i <= n; i++) if ($1 == w[i]) print $1 "," $2 }
    ' "$1" | sort
}

echo "zsbench --setup / --run"

# Each of these must be refused for the REASON given, not merely refused: an
# unrecognised option exits 2 as well, so a status check alone would pass
# against a zsbench that had never heard of --setup.
refused() {   # name, expected-message-fragment, args...
    local name=$1 want=$2; shift 2
    local out; out=$("$@" 2>&1); local st=$?
    if [ $st -ne 2 ]; then bad "$name: exit $st, wanted 2"
    else case "$out" in
        *"$want"*) ok "$name" ;;
        *)         bad "$name: message was [$out]" ;;
    esac; fi
}

# A phase without --path cannot work: the default workdir carries the pid, so
# the two invocations would never name the same directory.
refused "--setup without --path is refused" "require --path" \
        $ZSBENCH --setup -n $N
refused "--setup and --run are exclusive" "exclusive" \
        $ZSBENCH --path="$FIX" --setup --run -n $N
refused "--run without fixtures is refused" "run --setup" \
        $ZSBENCH --path="$OUT/never" --run -n $N
refused "--selftest takes no phase" "no phase" \
        $ZSBENCH --path="$FIX" --setup --selftest

# Setup builds and reports; it must time nothing.
if setup_out=$($ZSBENCH --path="$FIX" --setup -n $N 2>&1); then
    ok "--setup exits 0"
else
    bad "--setup exits 0 (got $?)"
fi
case "$setup_out" in
    *"/s "*) bad "--setup times nothing" ;;
    *)       ok "--setup times nothing" ;;
esac
for d in fetch fetch-repacked scan scan-compacted txnscanmix; do
    if [ -d "$FIX/$d" ]; then ok "fixture $d built"; else bad "fixture $d built"; fi
done
if [ -f "$FIX/zsbench.setup" ]; then ok "stamp written"; else bad "stamp written"; fi

# The run phase measures, and leaves the fixture behind so it can be profiled
# more than once.
if $ZSBENCH --path="$FIX" --run -n $N --reps 1 --csv "$OUT/run1.csv" >/dev/null 2>&1
then ok "--run exits 0"; else bad "--run exits 0"; fi
if $ZSBENCH --path="$FIX" --run -n $N --reps 1 --csv "$OUT/run2.csv" >/dev/null 2>&1
then ok "--run is repeatable"; else bad "--run is repeatable"; fi
if [ -n "$(slugops "$OUT/run1.csv")" ]; then ok "the run phase reported rows"
else bad "the run phase reported rows"; fi
check "the fixture survives a run" \
      "$(slugops "$OUT/run1.csv")" "$(slugops "$OUT/run2.csv")"

# ... and it measures the same workloads as the combined run, over the same
# number of records.  If a fixture were being consumed -- scan's compaction used
# to happen in place -- this is the row that would disagree.
$ZSBENCH -n $N --reps 1 --csv "$OUT/all.csv" >/dev/null 2>&1
check "run phase matches the combined run" \
      "$(slugops "$OUT/run1.csv")" "$(slugops "$OUT/all.csv")"

want=""
for s in $SPLIT; do want="$want$s,$N
"; done
check "every splittable row is present, at -n $N" \
      "$(slugops "$OUT/run1.csv")" "$(printf '%s' "$want" | sort)"

# The stamp: a run phase whose parameters do not match what was built.
refused "a different -n is refused" "match the fixtures" \
        $ZSBENCH --path="$FIX" --run -n $((N + 1))
refused "a different --valsize is refused" "match the fixtures" \
        $ZSBENCH --path="$FIX" --run -n $N --valsize 999

# The filter belongs to the run phase: a filtered --setup still builds every
# fixture, so the two invocations cannot disagree about which ones exist.
FIX2=$(mktemp -d "${TMPDIR:-/tmp}/zsbench-phases2.XXXXXX") || exit 1
$ZSBENCH --path="$FIX2" --setup -n $N fetch >/dev/null 2>&1
missing=""
for d in fetch fetch-repacked scan scan-compacted txnscanmix; do
    [ -d "$FIX2/$d" ] || missing="$missing $d"
done
check "a filtered --setup builds every fixture" "$missing" ""

# ... and a fixture that is gone is NAMED, rather than reported as a database
# that cannot be opened, which would send the reader looking for corruption.
rm -rf "$FIX2/scan"
refused "a missing fixture is named" "is missing" \
        $ZSBENCH --path="$FIX2" --run -n $N "full scan"
rm -rf "$FIX2"

if [ $fail -eq 0 ]; then
    echo "benchphases: ok"
else
    echo "benchphases: $fail FAILED"
fi
exit $((fail ? 1 : 0))
