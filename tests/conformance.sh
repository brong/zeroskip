#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Check doc/conformance.md against the spec (T-11).
#
# This does NOT verify that the cited tests are adequate -- no script can. What it
# verifies is that the MAP is complete and honest in both directions:
#
#   - every labelled requirement in the spec appears in the map, so a requirement
#     cannot be added without someone deciding how it is enforced;
#   - every label in the map exists in the spec, so a requirement cannot be
#     silently deleted while its row lingers as false comfort;
#   - every test the map cites actually exists, so a row cannot point at a test
#     that was renamed or removed.
#
# The third is the one that rots on its own. A citation is only evidence while the
# test it names is still there.

set -u
cd "$(dirname "$0")/.." || exit 1

SPEC=doc/specification.md
MAP=doc/conformance.md

fail=0

for f in "$SPEC" "$MAP"; do
    [ -r "$f" ] || { echo "conformance: cannot read $f" >&2; exit 1; }
done

# --- labels ------------------------------------------------------------------
spec_labels=$(grep -oE '\b(G|F|D|C|R|A|P|S|T)-[0-9]+[a-z]*' "$SPEC" | sort -u)
map_labels=$(grep -oE '^\| `(G|F|D|C|R|A|P|S|T)-[0-9]+[a-z]*`' "$MAP" \
             | grep -oE '(G|F|D|C|R|A|P|S|T)-[0-9]+[a-z]*' | sort -u)

missing=$(comm -23 <(echo "$spec_labels") <(echo "$map_labels"))
extra=$(comm -13 <(echo "$spec_labels") <(echo "$map_labels"))

if [ -n "$missing" ]; then
    echo "requirements in the spec with no row in $MAP:"
    echo "$missing" | sed 's/^/  /'
    fail=1
fi

if [ -n "$extra" ]; then
    echo "rows in $MAP naming requirements the spec no longer has:"
    echo "$extra" | sed 's/^/  /'
    fail=1
fi

# --- cited tests exist -------------------------------------------------------
# Citations are backticked names in the "Enforced by" column -- the FOURTH
# |-field of a requirement row, so backticked words in the requirement text
# (`end == 0` and friends) are not mistaken for citations.  Resolve each
# against the test sources; a citation naming a test that no longer exists is
# worse than no citation, because it reads as coverage.
#
# An earlier version matched only rows whose whole cell was one backtick pair,
# which skipped every multi-citation row -- so a test removal left rows
# pointing at nothing and the check still passed.
cited=$(awk -F'|' '/^\| `/ { print $4 }' "$MAP" \
        | sed 's/ — .*//' | tr -d '`' | tr ',' '\n' \
        | sed 's/^ *//; s/ *$//' | grep -vE '^\+[0-9]+ more$' \
        | grep -E '^(crash/)?[a-z0-9_.]+$' | sort -u)

for c in $cited; do
    case "$c" in
        crash/*)
            name="test_${c#crash/}"
            grep -q "static void $name(void)" zstest-crash.c \
                || { echo "cited test does not exist: $c"; fail=1; }
            ;;
        tool.sh|corpus|zsbench|none)
            ;;
        *)
            grep -q "static void test_$c(void)" zstest.c \
                || { echo "cited test does not exist: $c"; fail=1; }
            ;;
    esac
done

# --- every gap has a reason --------------------------------------------------
# A gap without an explanation is indistinguishable from an oversight.
gapcount=$(grep -c '| \*\*none\*\* |$' "$MAP" || true)
reasons=$(sed -n '/^## Gaps/,/^## /p' "$MAP" | grep -c '^- \*\*`' || true)

if [ "$gapcount" -ne "$reasons" ]; then
    echo "$gapcount requirements have no test but $reasons are explained under ## Gaps"
    fail=1
fi

nspec=$(echo "$spec_labels" | wc -l | tr -d ' ')
echo "conformance: $nspec requirements, $gapcount gaps, all explained: $([ "$fail" -eq 0 ] && echo yes || echo NO)"
exit $fail
