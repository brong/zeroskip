#!/bin/bash
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
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
set +m          # no async job-control messages: a crashing mutant's
                # "Segmentation fault" line would otherwise print next to an
                # unrelated mutant's result and appear to belong to it
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

    # Run detached from the shell's job control, so a crashing mutant does not
    # print an async "Segmentation fault" line that lands next to an unrelated
    # mutant's result and misattributes it.
    ./zstest >"$WORK/run.log" 2>&1
    local rc=$?

    if [ "$rc" -eq 0 ]; then
        if [ "$expect" = equivalent ]; then
            printf '  %-46s equivalent (as documented)\n' "$name"
            equivalent=$((equivalent + 1))
        else
            printf '  %-46s NOT CAUGHT  <-- test gap\n' "$name"
            missed=$((missed + 1))
        fi
    elif [ "$expect" = equivalent ]; then
        printf '  %-46s CAUGHT but marked equivalent -- reclassify\n' "$name"
        missed=$((missed + 1))
    elif [ "$rc" -gt 128 ]; then
        # Killed by a signal rather than failing an assertion.  Still detected,
        # but worth naming: it means the mutation corrupts memory before any
        # assertion runs, so the suite is catching it by crashing rather than by
        # checking.  Usually a sign the mutant breaks an invariant the tests rely
        # on to even execute -- not a problem, but not the same evidence.
        printf '  %-46s caught by crash (signal %d)\n' "$name" "$((rc - 128))"
        caught=$((caught + 1))
    else
        local where
        where=$(grep -m1 'FAIL' "$WORK/run.log" | sed 's/^ *//; s/^FAIL //')
        printf '  %-46s caught: %s\n' "$name" "$where"
        caught=$((caught + 1))
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

# Anchored on a string unique to zsi_header_decode.  An earlier version anchored
# on the "F-9: generations start at 1" comment, which zsi_name_parse later grew
# too -- and since perl replaces the first match and FILENAMES precedes FILE
# HEADER, the mutation landed in a function with no `buf` in scope and failed to
# build.  A pattern can rot by matching the WRONG place, not only by not matching.
mutant "reserved: rejected not ignored" catch \
  's/    memcpy\(out->compar_name, buf \+ ZSI_HDR_OFF_COMPAR, ZSI_COMPAR_NAME_LEN\);/    memcpy(out->compar_name, buf + ZSI_HDR_OFF_COMPAR, ZSI_COMPAR_NAME_LEN);\n    if (zsi_get32(buf + ZSI_HDR_OFF_RESERVED1)) return ZS_BADFORMAT;\n    if (zsi_get32(buf + ZSI_HDR_OFF_RESERVED2)) return ZS_BADFORMAT;/'

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
echo "filenames (Task 5)"

# The D-1a property D-5's resolution rule rests on: adding an extension to data
# files reverses the sort order of an unordered name against the in-order name for
# the same generation, and overlap resolution silently picks the wrong file.
mutant "names: .zs extension added" catch \
  's/        snprintf\(out, ZSI_NAME_MAX, "%s%s-%08X", ZSI_NAME_PREFIX, ustr, start\);/        snprintf(out, ZSI_NAME_MAX, "%s%s-%08X.zs", ZSI_NAME_PREFIX, ustr, start);/'

mutant "names: lowercase generations" catch \
  's/"%s%s-%08X", ZSI_NAME_PREFIX, ustr, start\);/"%s%s-%08x", ZSI_NAME_PREFIX, ustr, start);/'

mutant "names: unpadded generations" catch \
  's/"%s%s-%08X", ZSI_NAME_PREFIX, ustr, start\);/"%s%s-%X", ZSI_NAME_PREFIX, ustr, start);/'

mutant "names: 4-digit generations" catch \
  's/"%s%s-%08X-%08X",\n                 ZSI_NAME_PREFIX, ustr, start, end\);/"%s%s-%04X-%04X",\n                 ZSI_NAME_PREFIX, ustr, start, end);/'

mutant "names: decimal generations" catch \
  's/"%s%s-%08X", ZSI_NAME_PREFIX, ustr, start\);/"%s%s-%08u", ZSI_NAME_PREFIX, ustr, start);/'

mutant "parse: lowercase hex accepted" catch \
  's/        else if \(c >= .A. && c <= .F.\) d = c - .A. \+ 10;/        else if (c >= 0x41 \&\& c <= 0x46) d = c - 0x41 + 10;\n        else if (c >= 0x61 \&\& c <= 0x66) d = c - 0x61 + 10;/'

# Anchored on the comment rather than on the code, to sidestep matching an escaped
# NUL literal through two layers of quoting -- which is what defeated the previous
# attempt at this pattern.
mutant "parse: trailing junk allowed" catch \
  's/    \/\* No extension, and nothing trailing \(D-1a\)\. \*\/\n    if [^\n]*\n/    \/* trailing check removed *\/\n/'

mutant "parse: generation 0 allowed" catch \
  's/    if \(s == 0\) return ZSI_NAME_OTHER;/    \/* F-9 check removed *\//'

mutant "parse: backwards range allowed" catch \
  's/    if \(e == 0 \|\| e < s\) return ZSI_NAME_OTHER;/    if (e == 0) return ZSI_NAME_OTHER;/'

mutant "parse: metadata matches data pattern" catch \
  's/    if \(strncmp\(name, ZSI_NAME_PREFIX, ZSI_NAME_PREFIX_LEN\) != 0\)\n        return ZSI_NAME_OTHER;/    if (strncmp(name, "zeroskip", 8) != 0)\n        return ZSI_NAME_OTHER;/'

mutant "parse: 7 hex digits accepted" catch \
  's/    for \(size_t i = 0; i < 8; i\+\+\) \{\n        unsigned char c = \(unsigned char\)p\[i\];/    for (size_t i = 0; i < 7; i++) {\n        unsigned char c = (unsigned char)p[i];/'

echo
echo "records and terminators (Task 4)"

mutant "type: computed instead of tabled" catch \
  's/    switch \(type\) \{\n    case ZSI_KEYVALUE:/    if (type \& 0xC0) return false;\n    return true;\n    switch (type) {\n    case ZSI_KEYVALUE:/'

mutant "type: 0x00 accepted" catch \
  's/    case ZSI_PTRS64:\n        return true;\n    \}\n\n    return false;/    case ZSI_PTRS64:\n    case 0x00:\n        return true;\n    }\n\n    return false;/'

mutant "type: reserved bit ignored" catch \
  's/static bool zsi_type_valid\(uint8_t type\)\n\{\n    switch \(type\) \{/static bool zsi_type_valid(uint8_t type)\n{\n    type \&= 0x3F;\n    switch (type) {/'

mutant "keylen boundary: 256 stays short" catch \
  's/bool big = keylen > ZSI_SHORT_KEYLEN_MAX\n            \|\| \(!isdelete && vallen > ZSI_SHORT_VALLEN_MAX\);\n\n    if \(isdelete\) \{\n        hdr = big \? ZSI_HDRLEN_BIGDELETION/bool big = keylen > 256\n            || (!isdelete \&\& vallen > ZSI_SHORT_VALLEN_MAX);\n\n    if (isdelete) {\n        hdr = big ? ZSI_HDRLEN_BIGDELETION/'

mutant "vallen boundary: 65536 stays short" catch \
  's/    bool big = keylen > ZSI_SHORT_KEYLEN_MAX\n            \|\| \(!isdelete && vallen > ZSI_SHORT_VALLEN_MAX\);\n\n    if \(isdelete\) \{\n        if \(big\) return store_ancestor/    bool big = keylen > ZSI_SHORT_KEYLEN_MAX\n            || (!isdelete \&\& vallen > 65536);\n\n    if (isdelete) {\n        if (big) return store_ancestor/'

# Changes BOTH zsi_rec_type_for and zsi_rec_encoded_len, so the two stay
# consistent with each other and the mutant is a plausible bug rather than an
# internally contradictory state.  Mutating only one made the encoder write at big
# offsets into a short-length buffer, so it was "caught" by a heap overflow --
# detected, but not by any assertion, and therefore no evidence about the tests.
mutant "ancestor promotes to big form" catch \
  's/bool big = keylen > ZSI_SHORT_KEYLEN_MAX\n            \|\| \(!isdelete && vallen > ZSI_SHORT_VALLEN_MAX\);/bool big = store_ancestor || keylen > ZSI_SHORT_KEYLEN_MAX\n            || (!isdelete \&\& vallen > ZSI_SHORT_VALLEN_MAX);/g'

mutant "lengths include the NUL terminators" catch \
  's/        if \(!zsi_add3_sz\(keylen, vallen, 2, &body\)\) return 0;/        if (!zsi_add3_sz(keylen, vallen, 0, \&body)) return 0;/'

mutant "vallen stored at +3 not +2" catch \
  's/            zsi_put16\(buf \+ 2, \(uint16_t\)vallen\);/            zsi_put16(buf + 3, (uint16_t)vallen);/'

mutant "big keylen at +16, vallen at +8" catch \
  's/            zsi_put64\(buf \+ 8, \(uint64_t\)keylen\);\n            zsi_put64\(buf \+ 16, \(uint64_t\)vallen\);/            zsi_put64(buf + 16, (uint64_t)keylen);\n            zsi_put64(buf + 8, (uint64_t)vallen);/'

mutant "short ancestor at +2 not +4" catch \
  's/            if \(store_ancestor\) \{\n                zsi_put32\(buf \+ 4, ancestor\);\n                body = ZSI_HDRLEN_KEYVALUE_ANC;/            if (store_ancestor) {\n                zsi_put32(buf + 2, ancestor);\n                body = ZSI_HDRLEN_KEYVALUE_ANC;/'

mutant "encoder: no memset (dirty record padding)" catch \
  's/    memset\(buf, 0, total\);\n    buf\[0\] = \(char\)type;/    buf[0] = (char)type;/'

# The explicit NUL writes are redundant with the memset that precedes them: it
# covers the whole record, and both NUL positions are inside it.  So removing
# either changes nothing observable.  They stay in the source because they state
# F-13's "key NUL value NUL" structure at the point it is built, and because
# narrowing the memset to just the padding -- a plausible future optimisation --
# would make them load-bearing again.
mutant "no NUL after value" equivalent \
  's/buf\[body \+ keylen \+ 1 \+ vallen\] = /(void)0; \/\/ /'

mutant "no NUL after key" equivalent \
  's/    buf\[body \+ keylen\] = /    (void)0; \/\/ /'

# ...and the pairing that proves the claim above: with the memset gone AND the
# NUL writes gone, the trailing NULs really are absent and a test must object.
mutant "no memset and no NULs" catch \
  's/    memset\(buf, 0, total\);\n    buf\[0\] = \(char\)type;/    buf[0] = (char)type;/; s/buf\[body \+ keylen \+ 1 \+ vallen\] = /(void)0; \/\/ /; s/    buf\[body \+ keylen\] = /    (void)0; \/\/ /'

mutant "omitted ancestor resolves to 0" catch \
  's/    ancestor = hasanc \? zsi_get32\(buf \+ 4\) : file_start;/    ancestor = hasanc ? zsi_get32(buf + 4) : 0;/'

mutant "F-14: zero keylen allowed" catch \
  's/    if \(keylen < 1\) return ZS_BADFORMAT;             \/\* F-14 \*\//    \/* F-14 check removed *\//'

mutant "decode: total not bounded by len" catch \
  's/    if \(total > len\) return ZS_BADFORMAT;/    \/* bound removed *\//'

mutant "decode: unchecked keylen+vallen" catch \
  's/        if \(!zsi_add3_sz\(keylen, vallen, 2, &body\)\) return ZS_BADFORMAT;/        body = keylen + vallen + 2;/'

mutant "decode: data record accepts COMMIT" catch \
  's/    if \(!\(type & ZSI_HASKEY\)\) return ZS_BADFORMAT;   \/\* not a data record \*\//    \/* family check removed *\//'

mutant "terminator: span boundary off by one" catch \
  's/    return spanlen <= ZSI_SHORT_SPANLEN_MAX \? ZSI_TERMLEN_SHORT\n                                            : ZSI_TERMLEN_LONG;/    return spanlen < ZSI_SHORT_SPANLEN_MAX ? ZSI_TERMLEN_SHORT\n                                          : ZSI_TERMLEN_LONG;/'

mutant "terminator csum: span only, not terminator" catch \
  's/        zsi_put32\(buf \+ 4, zsi_csum2\(csum, csum_id, spandata, \(size_t\)spanlen,\n                                     buf, ZSI_TERMLEN_SHORT - 4\)\);/        zsi_put32(buf + 4, csum(spandata, (size_t)spanlen));/'

mutant "terminator: rollback bit dropped" catch \
  's/        buf\[0\] = \(char\)\(rollback \? ZSI_ROLLBACK : ZSI_COMMIT\);/        buf[0] = (char)ZSI_COMMIT;/'

mutant "terminator: long spanlen at +16" catch \
  's/        zsi_put64\(buf \+ 8, spanlen\);/        zsi_put64(buf + 16, spanlen);/'

mutant "terminator: long csum at +16 not +20" catch \
  's/        zsi_put32\(buf \+ 20, zsi_csum2\(csum, csum_id, spandata, \(size_t\)spanlen,\n                                      buf, ZSI_TERMLEN_LONG - 4\)\);/        zsi_put32(buf + 16, zsi_csum2(csum, csum_id, spandata, (size_t)spanlen,\n                                      buf, ZSI_TERMLEN_LONG - 4));/'

# The data-loss bug this task originally shipped with: rejecting a decodable but
# non-canonical record.  Combined with F-24 that discards every committed record
# after it, which G-3 forbids.  The mutant restores the bug; a test must object.
mutant "decode: reject non-canonical (data loss)" catch \
  's/    \/\* The ancestor: stored when HasAncestor, otherwise the containing file.s/    if (big \&\& keylen <= ZSI_SHORT_KEYLEN_MAX\n            \&\& (isdelete || vallen <= ZSI_SHORT_VALLEN_MAX))\n        return ZS_BADFORMAT;\n\n    \/* The ancestor: stored when HasAncestor, otherwise the containing file'"'"'s/'

mutant "canonical: ancestor==start not flagged" catch \
  's/    if \(anc_stored && r->ancestor == file_start\) return false;/    \/* F-17 check removed *\//'

echo
printf '%d caught, %d equivalent, %d NOT CAUGHT, %d inconclusive\n' \
    "$caught" "$equivalent" "$missed" "$broken"

cp "$BAK" zeroskip.c
rm -f zstest
make zstest >/dev/null 2>&1

[ "$missed" -eq 0 ] && [ "$broken" -eq 0 ]
