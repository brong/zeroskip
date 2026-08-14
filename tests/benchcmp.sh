#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Run zsbench and the sibling twom library's twombench with MATCHING flags and
# join their results.
#
# Both tools write the same CSV schema, which is the whole reason this can be a
# join rather than a reimplementation:
#
#     benchmark,ops,reps,median_ms,min_ms,max_ms,ops_per_sec,n,keysize,valsize,csum
#
# The overlap is PARTIAL and the unpaired workloads are printed rather than
# hidden.  zeroskip has no mutable-file operations to compare against twom's
# overwrite and delete paths, and twom has no file set, so it has nothing to say
# about rollover, conversion, pointer tables or compaction.  A comparison that
# quietly dropped both halves would read as a fuller answer than it is.
#
#     ./tests/benchcmp.sh                     defaults, matched on both sides
#     ./tests/benchcmp.sh -n 200000 --keysize 16
#     TWOMBENCH=/path/to/twombench ./tests/benchcmp.sh

set -u
cd "$(dirname "$0")/.." || exit 1

TWOMBENCH=${TWOMBENCH:-../twom/twombench}
ZSBENCH=${ZSBENCH:-./zsbench}

# Defaults chosen to be legal for BOTH tools: twombench's minimum keysize is 5,
# zsbench's is 4, so 16 clears both and matches twombench's own default.
records=20000
keysize=16
valsize=100
bench_reps=3
csum=xxh64

while [ $# -gt 0 ]; do
    case "$1" in
        -n|--records) records=$2; shift 2 ;;
        --keysize)    keysize=$2; shift 2 ;;
        --valsize)    valsize=$2; shift 2 ;;
        --reps)       bench_reps=$2; shift 2 ;;
        --csum)       csum=$2; shift 2 ;;
        -h|--help)
            sed -n '9,25p' "$0" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *) echo "benchcmp: unknown option $1" >&2; exit 2 ;;
    esac
done

for tool in "$ZSBENCH" "$TWOMBENCH"; do
    if [ ! -x "$tool" ]; then
        echo "benchcmp: $tool not found or not executable" >&2
        echo "  build it first, or point TWOMBENCH/ZSBENCH at it" >&2
        exit 1
    fi
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/benchcmp.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT

# The MAPPING.  Each line is "label|twom_slug|zeroskip_slug".
#
# Only fillsync/store_1_per_txn is a like-for-like pair: both commit one
# transaction per record, so both pay their durability cost per record.  The
# others are the nearest equivalent and are labelled as such -- in particular
# twom's fillseq puts every record in ONE transaction, while zeroskip's closest
# batched figure commits every 1000, so that row compares batching policies as
# much as it compares libraries.
MAP='one txn per record (synced)|fillsync|store_1_per_txn
bulk load, batched|fillseq|store_1000_per_txn
random point lookup|readrandom|fetch
full scan|readseq|scan
repack / merge|repack|repack'

echo "zeroskip vs twom"
echo "  $records records, ${keysize}-byte keys, ${valsize}-byte values," \
     "$bench_reps reps, csum $csum"
echo

echo "running twombench..." >&2
"$TWOMBENCH" -n "$records" --keysize "$keysize" --valsize "$valsize" \
    --reps "$bench_reps" --csum "$csum" --csv "$tmp/twom.csv" >/dev/null || {
        echo "benchcmp: twombench failed" >&2; exit 1; }

echo "running zsbench..." >&2
"$ZSBENCH" -n "$records" --keysize "$keysize" --valsize "$valsize" \
    --reps "$bench_reps" --csum "$csum" --csv "$tmp/zs.csv" >/dev/null || {
        echo "benchcmp: zsbench failed" >&2; exit 1; }

printf '  %-28s %14s %14s %8s\n' "workload" "twom ops/s" "zeroskip ops/s" "ratio"
printf '  %-28s %14s %14s %8s\n' \
    "----------------------------" "--------------" "--------------" "--------"

echo "$MAP" | while IFS='|' read -r label tslug zslug; do
    t=$(awk -F, -v k="$tslug" '$1==k {print $7}' "$tmp/twom.csv")
    z=$(awk -F, -v k="$zslug" '$1==k {print $7}' "$tmp/zs.csv")
    [ -n "$t" ] || t=""
    [ -n "$z" ] || z=""
    # A zero rate is "this workload had nothing to do at this -n" -- the repack
    # row does that below a few thousand records, because no cascade is due.
    # Printing 0.00x there would read as "infinitely slower" rather than "did
    # not run", so the ratio is withheld instead.
    if [ -z "$t" ] || [ -z "$z" ]; then
        printf '  %-28s %14s %14s %8s\n' "$label" "${t:-absent}" "${z:-absent}" "-"
    elif [ "$t" = "0" ] || [ "$z" = "0" ]; then
        printf '  %-28s %14s %14s %8s\n' "$label" "$t" "$z" "n/a"
    else
        printf '  %-28s %14s %14s %7sx\n' "$label" "$t" "$z" \
            "$(awk -v a="$z" -v b="$t" 'BEGIN{ printf "%.2f", a/b }')"
    fi
done

# Everything the mapping does not name, on both sides.  A workload with no
# counterpart is a real gap in the comparison, not noise to suppress.
paired_t=$(echo "$MAP" | cut -d'|' -f2 | paste -sd'|' -)
paired_z=$(echo "$MAP" | cut -d'|' -f3 | paste -sd'|' -)

echo
echo "  unpaired, twom only (no zeroskip equivalent):"
awk -F, -v p="^(${paired_t})$" 'NR>1 && $1 !~ p {printf "    %s\n", $1}' \
    "$tmp/twom.csv"

echo
echo "  unpaired, zeroskip only (no twom equivalent):"
awk -F, -v p="^(${paired_z})$" 'NR>1 && $1 !~ p {printf "    %s\n", $1}' \
    "$tmp/zs.csv"

echo
echo "  ratio is zeroskip/twom: above 1.00 means zeroskip did more ops/second."
echo "  Only the first row is durability-matched; see the mapping in this script."
