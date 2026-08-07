#!/bin/bash
#
# Mutation testing: verify that the test suite can actually fail.
#
# A test that passes but cannot fail is worse than no test, because it reads as
# coverage.  This script introduces, one at a time, the specific bugs the suite
# claims to guard against, and reports whether the suite noticed.  Every mutant
# should be reported as "caught"; anything else needs explaining.
#
#     ./tests/mutate.sh              run every mutant
#     ./tests/mutate.sh compar       run mutants whose name matches a substring
#
# Two categories of expected non-catch, both of which the report labels rather
# than hides:
#
#   EQUIVALENT  the mutation does not change observable behaviour, so no test
#               could catch it.  Recorded here so nobody adds a bogus test
#               chasing one.  (For example, unsigned wraparound already produces
#               roundup8's saturating answer, so dropping its guard is a no-op.)
#   PATTERN     the perl pattern no longer matches the source.  These patterns
#               are tied to exact source text and WILL rot when the code is
#               refactored -- fix the pattern, do not delete the mutant.
#
# Builds are deliberately not silenced past a failure check: a mutant that fails
# to compile would otherwise leave the previous binary in place, and the stale
# binary's pass would be read as a test gap that isn't one.  That mistake was
# made once already while writing this.

set -u
cd "$(dirname "$0")/.." || exit 1

FILTER="${1:-}"
WORK="$(mktemp -d)"
BAK="$WORK/zeroskip.c.orig"
trap 'cp "$BAK" zeroskip.c 2>/dev/null; rm -rf "$WORK"' EXIT

cp zeroskip.c "$BAK"
caught=0 missed=0 equivalent=0 broken=0

# mutant <name> <expectation: catch|equivalent> <perl expression>
mutant() {
    local name="$1" expect="$2" expr="$3"
    [ -n "$FILTER" ] && case "$name" in *"$FILTER"*) ;; *) return ;; esac

    cp "$BAK" zeroskip.c
    perl -0pi -e "$expr" zeroskip.c

    if diff -q zeroskip.c "$BAK" >/dev/null 2>&1; then
        printf '  %-46s PATTERN ROTTED\n' "$name"
        broken=$((broken + 1)); cp "$BAK" zeroskip.c; return
    fi

    rm -f zstest
    if ! make zstest >"$WORK/build.log" 2>&1; then
        printf '  %-46s BUILD FAILED (inconclusive)\n' "$name"
        sed 's/^/        /' "$WORK/build.log" | grep -m2 error
        broken=$((broken + 1)); cp "$BAK" zeroskip.c; return
    fi

    if ./zstest >"$WORK/run.log" 2>&1; then
        if [ "$expect" = equivalent ]; then
            printf '  %-46s equivalent (as documented)\n' "$name"
            equivalent=$((equivalent + 1))
        else
            printf '  %-46s NOT CAUGHT  <-- test gap\n' "$name"
            missed=$((missed + 1))
        fi
    else
        local where
        where=$(grep -m1 'FAIL' "$WORK/run.log" | sed 's/^ *//; s/^FAIL //')
        if [ "$expect" = equivalent ]; then
            printf '  %-46s CAUGHT but marked equivalent -- reclassify\n' "$name"
            missed=$((missed + 1))
        else
            printf '  %-46s caught: %s\n' "$name" "$where"
            caught=$((caught + 1))
        fi
    fi
    cp "$BAK" zeroskip.c
}

echo "primitives (Task 2)"

mutant "compar: signed char" catch \
  's/const unsigned char \*ua = \(const unsigned char \*\)a;\n    const unsigned char \*ub = \(const unsigned char \*\)b;/const signed char *ua = (const signed char *)a;\n    const signed char *ub = (const signed char *)b;/'

mutant "compar: longer key first" catch \
  's/return alen < blen \? -1 : 1;/return alen < blen ? 1 : -1;/'

# memcmp is defined to compare as unsigned char, and the length tie-break below
# it is kept, so this variant is genuinely correct.  F-11a's objection is to
# memcmp *alone* (no length ordering) and to its unspecified magnitude.
mutant "compar: memcmp for the prefix" equivalent \
  's/for \(size_t i = 0; i < n; i\+\+\) \{\n        if \(ua\[i\] != ub\[i\]\) return ua\[i\] < ub\[i\] \? -1 : 1;\n    \}/(void)ua; (void)ub;\n    { int c = memcmp(a, b, n); if (c) return c; }/'

mutant "csum: empty-input short-circuit" catch \
  's/\{\n    return \(uint32_t\)\(XXH3_64bits\(buf, len\) & 0xFFFFFFFFu\);\n\}/{\n    if (!len) return 0;\n    return (uint32_t)(XXH3_64bits(buf, len) \& 0xFFFFFFFFu);\n}/'

mutant "csum: keep high 32 bits" catch \
  's/return \(uint32_t\)\(XXH3_64bits\(buf, len\) & 0xFFFFFFFFu\);/return (uint32_t)(XXH3_64bits(buf, len) >> 32);/'

mutant "csum: nonzero seed" catch \
  's/return \(uint32_t\)\(XXH3_64bits\(buf, len\) & 0xFFFFFFFFu\);/return (uint32_t)(XXH3_64bits_withSeed(buf, len, 1) \& 0xFFFFFFFFu);/'

mutant "accessors: big-endian get32" catch \
  's/    return \(uint32_t\)u\[0\] \| \(\(uint32_t\)u\[1\] << 8\)\n         \| \(\(uint32_t\)u\[2\] << 16\) \| \(\(uint32_t\)u\[3\] << 24\);/    return (uint32_t)u[3] | ((uint32_t)u[2] << 8)\n         | ((uint32_t)u[1] << 16) | ((uint32_t)u[0] << 24);/'

mutant "guards: unchecked add" catch \
  's/    if \(a > SIZE_MAX - b\) return false;\n    \*out = a \+ b;\n    return true;/    *out = a + b;\n    return true;/'

# Unsigned overflow is well-defined and (n+7)&~7 already yields 0 for every input
# the guard rejects, so removing it changes nothing observable.  The guard is
# there to state the contract, not to implement it.
mutant "guards: roundup8 without guard" equivalent \
  's/    if \(n > SIZE_MAX - 7\) return 0;\n    return \(n \+ 7\) & ~\(size_t\)7;/    return (n + 7) \& ~(size_t)7;/'

mutant "uuid: accept uppercase" catch \
  's/    if \(c >= .a. && c <= .f.\) return c - .a. \+ 10;/    if (c >= 0x61 \&\& c <= 0x66) return c - 0x61 + 10;\n    if (c >= 0x41 \&\& c <= 0x46) return c - 0x41 + 10;/'

mutant "uuid: emit uppercase" catch \
  's/%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x/%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X/'

echo
echo "header (Task 3)"

mutant "magic: only 8 bytes checked" catch \
  's/if \(memcmp\(buf \+ ZSI_HDR_OFF_MAGIC, zsi_magic, ZSI_MAGIC_LEN\) != 0\)/if (memcmp(buf + ZSI_HDR_OFF_MAGIC, zsi_magic, 8) != 0)/'

mutant "csum: header never verified" catch \
  's/    if \(zsi_get32\(buf \+ ZSI_HDR_OFF_CSUM\) != csum\(buf, ZSI_HDR_OFF_CSUM\)\)\n        return ZS_BADCHECKSUM;/    (void)csum;/'

mutant "csum: covers 72 not 68" catch \
  's/zsi_put32\(buf \+ ZSI_HDR_OFF_CSUM, csum\(buf, ZSI_HDR_OFF_CSUM\)\);/zsi_put32(buf + ZSI_HDR_OFF_CSUM, csum(buf, ZSI_HEADER_LEN));/'

mutant "reserved: rejected not ignored" catch \
  's/    \/\* F-9: generations start at 1/    if (zsi_get32(buf + ZSI_HDR_OFF_RESERVED1)) return ZS_BADFORMAT;\n    if (zsi_get32(buf + ZSI_HDR_OFF_RESERVED2)) return ZS_BADFORMAT;\n\n    \/* F-9: generations start at 1/'

mutant "version: write gate in decoder" catch \
  's/    out->version_read  = vread;/    if ((uint8_t)buf[ZSI_HDR_OFF_VWRITE] > ZSI_VERSION_WRITE) return ZS_BADFORMAT;\n    out->version_read  = vread;/'

mutant "version: read gate removed" catch \
  's/    if \(vread > ZSI_VERSION_READ\) return ZS_BADFORMAT;/    \/* gate removed *\//'

mutant "layout: start/end swapped" catch \
  's/#define ZSI_HDR_OFF_START      40   \/\*  4 \*\/\n#define ZSI_HDR_OFF_END        44   \/\*  4 \*\//#define ZSI_HDR_OFF_START      44   \/* 4 *\/\n#define ZSI_HDR_OFF_END        40   \/* 4 *\//'

mutant "layout: uuid at 25" catch \
  's/#define ZSI_HDR_OFF_UUID       24   \/\* 16 \*\//#define ZSI_HDR_OFF_UUID       25   \/* 16 *\//'

mutant "layout: header length 64" catch \
  's/#define ZSI_HEADER_LEN         72/#define ZSI_HEADER_LEN         64/'

mutant "layout: flags big-endian" catch \
  's/zsi_put16\(buf \+ ZSI_HDR_OFF_FLAGS, hdr->flags\);/{ unsigned char *fu = (unsigned char *)(buf + ZSI_HDR_OFF_FLAGS); fu[0] = (unsigned char)(hdr->flags >> 8); fu[1] = (unsigned char)(hdr->flags \& 0xFF); }/'

mutant "compar name: 6 bytes not 16" catch \
  's/memcpy\(buf \+ ZSI_HDR_OFF_COMPAR, hdr->compar_name, ZSI_COMPAR_NAME_LEN\);/memcpy(buf + ZSI_HDR_OFF_COMPAR, hdr->compar_name, 6);/'

mutant "engine id: wrong bits" catch \
  's/return \(unsigned\)\(zsi_get16\(buf \+ ZSI_HDR_OFF_FLAGS\) & ZSI_CSUM_MASK\);/return (unsigned)((zsi_get16(buf + ZSI_HDR_OFF_FLAGS) >> 4) \& ZSI_CSUM_MASK);/'

mutant "F-9: start==0 allowed" catch \
  's/    if \(out->start == 0\) return ZS_BADFORMAT;/    \/* check removed *\//'

mutant "encoder: no memset (dirty padding)" catch \
  's/    memset\(buf, 0, ZSI_HEADER_LEN\);\n\n    memcpy\(buf \+ ZSI_HDR_OFF_MAGIC/    memcpy(buf + ZSI_HDR_OFF_MAGIC/'

echo
printf '%d caught, %d equivalent, %d NOT CAUGHT, %d inconclusive\n' \
    "$caught" "$equivalent" "$missed" "$broken"

cp "$BAK" zeroskip.c
rm -f zstest
make zstest >/dev/null 2>&1

[ "$missed" -eq 0 ] && [ "$broken" -eq 0 ]
