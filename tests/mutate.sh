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
#     ./tests/mutate.sh              run every mutant (a full rebuild and two
#                                    test binaries PER MUTANT -- the better part
#                                    of an hour, and growing with the suite)
#     ./tests/mutate.sh compar       run mutants whose name matches a substring
#     ./tests/mutate.sh --rot-only   apply every pattern with no build and no
#                                    run, reporting only PATTERN ROTTED: seconds,
#                                    not an hour, and the right check after
#                                    refactoring source the patterns anchor on
#
# The full run is NOT part of the standard loop -- at a rebuild per mutant it is
# priced for releases and suite audits, not for every change.  Day to day: run
# the mutants you are adding by name, and --rot-only after touching zeroskip.c.
#
# Two categories of expected non-catch, both of which the report labels rather
# than hides:
#
#   EQUIVALENT  the mutation does not change observable behaviour, so no test
#               could catch it.  Recorded here so nobody adds a bogus test
#               chasing one.  (For example, unsigned wraparound already produces
#               roundup8's saturating answer, so dropping its guard is a no-op.)
#   SUBSUMED    the mutation IS observable in principle, but every input the
#               suite constructs is rejected by a sibling check first, so no
#               existing test isolates it.  Distinct from EQUIVALENT: a
#               sufficiently contrived file would tell them apart.  Each group of
#               subsumed checks must be paired with a combined mutant removing
#               the whole group, so the defence is demonstrated even where the
#               individual layers are not.
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

ROTONLY=0
if [ "${1:-}" = "--rot-only" ]; then ROTONLY=1; shift; fi
FILTER="${1:-}"

# Mutate a COPY, never the checkout.  Everything the two test binaries need to
# build and run is snapshotted into a temp directory up front and mutated
# there, so an edit made in the repo during a run cannot be silently reverted,
# an interrupted run cannot leave a mutant in the tree, and two concurrent runs
# cannot corrupt each other's verdicts -- each has its own copy.  (All three
# happened while this mutated the checkout in place; one cost a whole run.)
# The flip side: repo changes made after launch are not part of this run.
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cp zeroskip.c zeroskip.h zstest.c zstest-crash.c xxhash.h Makefile "$WORK/"
mkdir -p "$WORK/tests"
cp -R tests/corpus "$WORK/tests/corpus"
cd "$WORK" || exit 1

BAK="$WORK/zeroskip.c.orig"
cp zeroskip.c "$BAK"
caught=0 missed=0 equivalent=0 broken=0 intact=0

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

    # --rot-only stops here: the pattern matched, which is all it asks.  It
    # says nothing about whether the mutant would be caught.
    if [ "$ROTONLY" -eq 1 ]; then
        intact=$((intact + 1)); cp "$BAK" zeroskip.c; return
    fi

    rm -f zstest zstest-crash
    if ! make zstest zstest-crash EXTRA_CFLAGS=-O0 >"$WORK/build.log" 2>&1; then
        printf '  %-46s BUILD FAILED (inconclusive)\n' "$name"
        sed 's/^/        /' "$WORK/build.log" | grep -m2 error
        broken=$((broken + 1)); cp "$BAK" zeroskip.c; return
    fi

    # Run detached from the shell's job control, so a crashing mutant does not
    # print an async "Segmentation fault" line that lands next to an unrelated
    # mutant's result and misattributes it.
    # Watchdog: a mutation can cause non-termination (dropping a bound, losing a
    # progress check), and without this one such mutant consumes the entire run.
    # A timeout is itself a catch -- the suite hanging IS the detection -- but it
    # is reported distinctly, because "hangs" and "fails an assertion" are
    # different evidence about the tests.
    # BOTH binaries.  An earlier version ran only ./zstest, which left every
    # property tested solely in zstest-crash -- the whole snapshot-gap and
    # sync-failure surface -- unprotected by mutation testing.  The C-4 "ENOENT is
    # fatal" mutant went uncaught for exactly that reason.
    local rc=0
    for bin in ./zstest ./zstest-crash; do
        ZS_TEST_NO_FORK= "$bin" >"$WORK/run.log" 2>&1 &
        local pid=$!
        ( sleep 40; kill -9 $pid 2>/dev/null ) >/dev/null 2>&1 &
        local wd=$!
        wait $pid
        rc=$?
        kill $wd 2>/dev/null
        [ "$rc" -ne 0 ] && break
    done

    if [ "$rc" -eq 0 ]; then
        if [ "$expect" = equivalent ]; then
            printf '  %-46s equivalent (as documented)\n' "$name"
            equivalent=$((equivalent + 1))
        elif [ "$expect" = subsumed ]; then
            printf '  %-46s subsumed by a sibling check (as documented)\n' "$name"
            equivalent=$((equivalent + 1))
        else
            printf '  %-46s NOT CAUGHT  <-- test gap\n' "$name"
            missed=$((missed + 1))
        fi
    elif [ "$expect" = equivalent ] || [ "$expect" = subsumed ]; then
        printf '  %-46s CAUGHT but marked %s -- reclassify\n' "$name" "$expect"
        missed=$((missed + 1))
    elif [ "$rc" -eq 137 ]; then
        printf '  %-46s caught by TIMEOUT (non-termination)\n' "$name"
        caught=$((caught + 1))
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
echo "snapshot (Task 12)"

# C-4 step 3: a file unlinked between the scan and the open is a stale scan, not
# an error -- retry.  Treating ENOENT as fatal makes an ordinary packer retiring
# an input into a failed read.
mutant "C-4: ENOENT is fatal" catch \
  's/            if \(r == ZS_NOTFOUND\) \{ retry = true; break; \}/            if (r == ZS_NOTFOUND) { zsi_snapshot_release(\&s); zsi_fileset_fini(\&fs); return r; }/'

# C-4h: bounded retries.  Unbounded, a directory that never tiles livelocks.
mutant "C-4h: unbounded retries" catch \
  's/    for \(int attempt = 0; attempt < ZSI_SNAPSHOT_RETRIES; attempt\+\+\) \{/    for (int attempt = 0; ; attempt++) {/'

# D-10a: a non-active file with a bad header is an error; only the active file
# gets D-10 tolerance.  Getting the position test backwards either loses committed
# data or refuses to open after an ordinary crash.
# D-10a as amended by D-10b: a non-active file with an invalid header is REPORTED,
# not fatal.  Suppressing the report makes it silent, which is the hazard the rule
# actually names.
mutant "D-10a: bad non-active header not reported" catch \
  's/            if \(!f->hdr_valid && !is_last && report\)\n                report\("non-active file has an invalid header",\n                       "file=<%s>", f->fname\);/            \/* report suppressed *\//'

# D-10: the ACTIVE file has an ordinary invalid header after a crash, and it must
# NOT be reported -- reporting it would cry wolf every time, which is how a real
# report comes to be ignored.
mutant "D-10: active bad header reported too" catch \
  's/            bool is_last = \(i \+ 1 == fs.nresolved\);/            bool is_last = false;/'

# Step 4 must build an index for every unordered file, or a reader sees an empty
# source where committed records are.
mutant "step 4: index not built" catch \
  's/                r = zsi_index_build_cached\(f, compar, compar_name, idxcfg\);/                r = ZS_OK; (void)compar; (void)compar_name; (void)idxcfg;/'

mutant "step 4: pointers not loaded" catch \
  's/                r = zsi_ptrs_load\(f\);/                r = ZS_OK;/'

# The active file is the highest-generation UNORDERED file.  Returning an in-order
# file would have a writer append to a file that has a pointer section.
mutant "active: in-order file returned" catch \
  's/    return zsi_file_is_unordered\(last\) \? last : NULL;/    return last;/'

mutant "active: lowest generation returned" catch \
  's/    struct zsi_file \*last = s->files\[s->nfiles - 1\];/    struct zsi_file *last = s->files[0];/'

# Refcounting: releasing a shared snapshot must not free files another holder is
# still reading (C-4g).
mutant "refcount: released regardless" catch \
  's/    if \(--s->refcount > 0\) \{ \*sp = NULL; return; \}/    \/* refcount ignored *\//'

echo
echo "file set (Task 11)"

# D-5: take the LAST file whose start matches.  Taking the first picks a repack
# input over the output that encloses it, discarding committed generations.
mutant "D-5: takes the first not the last" catch \
  's/        for \(size_t i = 0; i < fs->nall; i\+\+\)\n            if \(fs->all\[i\].start == cur\) pick = \(ssize_t\)i;/        for (size_t i = 0; i < fs->nall; i++)\n            if (fs->all[i].start == cur \&\& pick < 0) pick = (ssize_t)i;/'

# The sweep must advance past the taken file'"'"'s whole range, not just its start.
mutant "D-5: advances by start not end" catch \
  's/        uint32_t last = e->end \? e->end : e->start;/        uint32_t last = e->start;/'

# The sort is what makes "last" mean "widest".  Without it, readdir order decides.
mutant "sort: names not sorted" catch \
  's/    if \(fs->nall\)\n        qsort\(fs->all, fs->nall, sizeof\(\*fs->all\), zsi_entry_cmp\);/    \/* sort removed *\//'

mutant "sort: descending" catch \
  's/    return strcmp\(\(\(const struct zsi_entry \*\)a\)->name,\n                  \(\(const struct zsi_entry \*\)b\)->name\);/    return strcmp(((const struct zsi_entry *)b)->name,\n                  ((const struct zsi_entry *)a)->name);/'

# D-6: tiling IS the completeness test.  Without it a gap reads as a complete set
# and committed data goes missing silently.
mutant "D-6: tiling not checked" catch \
  's/    if \(reached != highest\) return ZS_AGAIN;/    \/* tiling check removed *\//'

# D-5c: a partial overlap has no correct interpretation and must be reported.
mutant "D-5c: partial overlap resolved anyway" catch \
  's/            if \(s2 > cur && s2 <= last && e2 > last\) return ZS_BADFORMAT;/            (void)s2; (void)e2;/'

# D-4a: disagreeing UUIDs are an error, never a majority vote.
mutant "D-4a: uuid mismatch ignored" catch \
  's/            if \(want_uuid\) continue;\n            zsi_fileset_fini\(fs\);\n            return ZS_BADFORMAT;/            continue;/'

# D-9b: the next generation comes from ALL files, not the resolved set -- a
# superseded file still pins its generation until it is removed.
mutant "D-9b: next gen from resolved set" catch \
  's/    for \(size_t i = 0; i < fs->nall; i\+\+\) \{\n        uint32_t e = fs->all\[i\].end \? fs->all\[i\].end : fs->all\[i\].start;\n        if \(e > highest\) highest = e;\n    \}\n\n    if \(highest == 0xFFFFFFFFu\) return ZS_FULL;/    for (size_t i = 0; i < fs->nresolved; i++) {\n        uint32_t e = fs->resolved[i].end ? fs->resolved[i].end : fs->resolved[i].start;\n        if (e > highest) highest = e;\n    }\n\n    if (highest == 0xFFFFFFFFu) return ZS_FULL;/'

# D-9c: allocating past the ceiling must fail rather than reissue generation 1.
mutant "D-9c: generation wraps instead of ZS_FULL" catch \
  's/    if \(highest == 0xFFFFFFFFu\) return ZS_FULL;/    \/* ZS_FULL check removed *\//'

# The lowest generation present may be above 1: older files are removed once
# repacked, so assuming 1 rejects every mature database.
mutant "sweep: starts at generation 1" catch \
  's/    uint32_t cur = fs->all\[0\].start;\n    for \(size_t i = 1; i < fs->nall; i\+\+\)\n        if \(fs->all\[i\].start < cur\) cur = fs->all\[i\].start;/    uint32_t cur = 1;/'

echo
echo "per-file cursor (Task 10)"

# D-14g: the transaction sorts as though above every file, so its records win
# equal keys with no special case in the merge comparator.
mutant "txn generation not above all files" catch \
  's/#define ZSI_GEN_TXN UINT32_MAX/#define ZSI_GEN_TXN 0/'

# The kind must come from the file, not be assumed.
mutant "cursor: every file treated as in-order" catch \
  's/    fc->kind = zsi_file_is_unordered\(f\) \? ZSI_SRC_UNORDERED : ZSI_SRC_INORDER;/    fc->kind = ZSI_SRC_INORDER;/'

mutant "cursor: every file treated as unordered" catch \
  's/    fc->kind = zsi_file_is_unordered\(f\) \? ZSI_SRC_UNORDERED : ZSI_SRC_INORDER;/    fc->kind = ZSI_SRC_UNORDERED;/'

mutant "cursor: gen not taken from the file" catch \
  's/    fc->gen = f->hdr.start;/    fc->gen = 0;/'

# Seek must be a lower bound, and exhaustion must be reported rather than
# producing a stale record.
mutant "seek: in-order lands one early" catch \
  's/        fc->pi = idx;/        fc->pi = idx ? idx - 1 : 0;/'

mutant "load: exhaustion not detected in-order" catch \
  's/        \} else if \(fc->pi >= fc->file->nptrs\) \{/        } else if (fc->pi > fc->file->nptrs) {/'

mutant "load: unordered never exhausts" catch \
  's/        fc->exhausted = \(r != ZS_OK\);/        fc->exhausted = false; (void)r;/'

# Subsumed: zsi_fcur_load re-derives exhaustion from the position on every call,
# so the guard changes nothing a test can reach.  It is observable in principle --
# 2^64 next() calls on an exhausted in-order cursor would wrap pi back into range
# -- which is why this is subsumed rather than equivalent, and why the guard stays.
mutant "next: advances an exhausted cursor" subsumed \
  's/    if \(fc->exhausted\) return ZS_OK;\n\n    switch \(fc->kind\) \{\n    case ZSI_SRC_INORDER:\n        if \(fc->reverse\) fc->pi--;/    switch (fc->kind) {\n    case ZSI_SRC_INORDER:\n        if (fc->reverse) fc->pi--;/'

# find must report an exact hit only, or a point lookup silently returns a
# neighbouring key -- which the read path would then treat as the answer.
mutant "find: inexact hit returned" catch \
  's/        if \(!exact\) return ZS_NOTFOUND;/        \/* exactness ignored *\//'

# A cursor that hid tombstones would let an older file's value resurface, since
# the merge is what turns a deletion into "absent" (D-14e step 4).
mutant "find: deletions hidden by the cursor" catch \
  's/        const char \*b = zsi_file_at\(fc->file, off, 1\);\n        if \(!b\) return ZS_BADFORMAT;\n        return zsi_rec_decode\(b, fc->file->size - off, fc->file->hdr.start, out\);/        const char *b = zsi_file_at(fc->file, off, 1);\n        if (!b) return ZS_BADFORMAT;\n        { int rr = zsi_rec_decode(b, fc->file->size - off, fc->file->hdr.start, out);\n          if (rr == ZS_OK \&\& !out->val) return ZS_NOTFOUND;\n          return rr; }/'

echo
echo "pointer section (Task 9)"

# F-26g: the empty in-order file.  Every property of it is pinned, because it is
# byte-identical every time it is produced and other implementations must agree.
# F-26g: an empty records region checksums to the ENGINE'S value for empty input,
# not to zero.  This is the trap twom's csum_null-style short-circuit sets.
mutant "empty file: records csum forced to zero" catch \
  's/    zsi_put32\(buf \+ seclen \+ 8, records_csum\);/    zsi_put32(buf + seclen + 8, n ? records_csum : 0);/'

mutant "empty file: PTRS64 when count is 0" catch \
  's/    bool wide = records_end > 0xFFFFFFFFu;/    bool wide = n == 0 || records_end > 0xFFFFFFFFu;/'

# F-26b: the section checksum covers section + padding + back pointer + records
# checksum, up to the checksum field itself.
mutant "section csum: covers section only" catch \
  's/    zsi_put32\(buf \+ seclen \+ 12, csum\(buf, seclen \+ 12\)\);/    zsi_put32(buf + seclen + 12, csum(buf, seclen));/'

mutant "section csum: not verified on load" catch \
  's/    if \(f->csum\(cbase, covered\) != sec_csum\) return ZS_BADCHECKSUM;/    (void)cbase; (void)covered;/'

# F-26f: the records checksum is verified on DEMAND, never on open, which must
# stay O(1).  Verifying it on open makes opening proportional to file size.
mutant "records csum: verified on open" catch \
  's/    f->records_csum = rec_csum;/    f->records_csum = rec_csum;\n    { size_t rl = ptr_off - ZSI_HEADER_LEN;\n      const char *rp = zsi_file_at(f, ZSI_HEADER_LEN, rl);\n      if (f->csum(rp ? rp : "", rl) != rec_csum) return ZS_BADCHECKSUM; }/'

mutant "records csum: over the wrong region" catch \
  's/    size_t len = f->ptr_off - ZSI_HEADER_LEN;/    size_t len = f->ptr_off;/'

# F-26a: the back pointer is plain data, but must be bounds-checked and aligned.
mutant "back pointer: alignment not checked" subsumed \
  's/    if \(back % 8 != 0\) return ZS_BADFORMAT;/    \/* alignment check removed *\//'

mutant "back pointer: lower bound not checked" subsumed \
  's/    if \(back < ZSI_HEADER_LEN\) return ZS_BADFORMAT;/    \/* lower bound removed *\//'

mutant "back pointer: may overlap the trailer" subsumed \
  's/    if \(sec_end > f->size - ZSI_TRAILER_LEN\) return ZS_BADFORMAT;/    if (sec_end > f->size) return ZS_BADFORMAT;/'

mutant "section type: anything accepted" subsumed \
  's/    else                         return ZS_BADFORMAT;/    else                         wide = false;/'

# The section length must exactly account for the rest of the file.
mutant "section length: bound not equality" subsumed \
  's/    if \(want_end != f->size - ZSI_TRAILER_LEN\) return ZS_BADFORMAT;/    if (want_end > f->size - ZSI_TRAILER_LEN) return ZS_BADFORMAT;/'

# ...and the combined mutant: strip the WHOLE back-pointer validation group at
# once, so nothing structural stands between a corrupt trailer and the pointer
# array.  Each layer above is individually subsumed by its siblings; this shows
# the group as a whole is load-bearing rather than uniformly redundant.
mutant "back pointer: no validation at all" catch \
  's/    if \(back < ZSI_HEADER_LEN\) return ZS_BADFORMAT;\n    if \(back % 8 != 0\) return ZS_BADFORMAT;/    \/* all back pointer checks removed *\//; s/    if \(sec_end > f->size - ZSI_TRAILER_LEN\) return ZS_BADFORMAT;/    if (sec_end > f->size) return ZS_BADFORMAT;/; s/    else                         return ZS_BADFORMAT;/    else                         wide = false;/; s/    if \(want_end != f->size - ZSI_TRAILER_LEN\) return ZS_BADFORMAT;/    if (want_end > f->size) return ZS_BADFORMAT;/'

# F-27: every pointer 8-aligned and inside the records region.
mutant "F-27: pointer bounds not checked" catch \
  's/        if \(off >= ptr_off\) return ZS_BADFORMAT;/        \/* upper bound removed *\//'

mutant "F-27: pointer alignment not checked" catch \
  's/        if \(off % 8 != 0\) return ZS_BADFORMAT;/        \/* alignment removed *\//'

# F-26d: the narrow section pads to a multiple of 8 so the trailer is aligned.
mutant "F-26d: no padding to 8" catch \
  's/    return zsi_roundup8\(total\);\n\}/    return total;\n}/'

# F-26c: PTRS32 whenever the offsets fit, so encoding is canonical.
mutant "F-26c: always PTRS64" catch \
  's/    bool wide = records_end > 0xFFFFFFFFu;/    bool wide = true;/'

# The search must be a lower bound over a strictly ordered array.
mutant "search: probe returns wrong end" catch \
  's/        if \(c > 0\) \{\n            \*idx = f->nptrs;            \/\* past every key \*\/\n            return ZS_OK;\n        \}/        if (c > 0) {\n            *idx = f->nptrs - 1;\n            return ZS_OK;\n        }/'

mutant "search: exact flag never set" catch \
  's/        \*exact = \(compar\(r.key, r.keylen, key, keylen\) == 0\);/        *exact = false;/'

echo
echo "private index (Task 8)"

# D-14: newest version of a key wins within a file.  The sort'"'"'s offset-descending
# tie-break is what makes that fall out of keeping the first of each run, so
# reversing it silently returns the OLDEST version of every rewritten key.
mutant "sort: offset tie-break ascending" catch \
  's/    return a > b \? -1 : 1;              \/\* higher offset first \*\//    return a < b ? -1 : 1;/'

mutant "dedup: keeps the last of a run" catch \
  's/                if \(compar\(ka, la, kb, lb\) == 0\) continue;/                if (compar(ka, la, kb, lb) == 0) { b.offs[w - 1] = b.offs[i]; continue; }/'

mutant "dedup: not done at all" catch \
  's/            if \(w\) \{\n                const char \*ka, \*kb;/            if (0) {\n                const char *ka, *kb;/'

# The delta holds newer records than the base, so consulting the base first, or
# preferring it on a tie, returns a stale record.
mutant "find: base consulted before delta" catch \
  's/    i = zsi_index_lb\(ix->delta, ix->ndelta, &ks, key, keylen\);\n    if \(zsi_index_eq\(ix->delta, ix->ndelta, i, &ks, key, keylen\)\) \{\n        \*off = ix->delta\[i\];\n        return ZS_OK;\n    \}\n\n    i = zsi_index_lb\(ix->base, ix->nbase, &ks, key, keylen\);\n    if \(zsi_index_eq\(ix->base, ix->nbase, i, &ks, key, keylen\)\) \{\n        \*off = ix->base\[i\];\n        return ZS_OK;\n    \}/    i = zsi_index_lb(ix->base, ix->nbase, \&ks, key, keylen);\n    if (zsi_index_eq(ix->base, ix->nbase, i, \&ks, key, keylen)) {\n        *off = ix->base[i];\n        return ZS_OK;\n    }\n\n    i = zsi_index_lb(ix->delta, ix->ndelta, \&ks, key, keylen);\n    if (zsi_index_eq(ix->delta, ix->ndelta, i, \&ks, key, keylen)) {\n        *off = ix->delta[i];\n        return ZS_OK;\n    }/'

mutant "get: prefers base on a tie" catch \
  's/        chosen = \(compar\(kd, ld, kb, lb\) <= 0\) \? ix->delta\[c->di\]\n                                              : ix->base\[c->bi\];/        chosen = (compar(kd, ld, kb, lb) < 0) ? ix->delta[c->di]\n                                              : ix->base[c->bi];/'

# D-14h, one level down: on a tie both sides must advance, or the base'"'"'s stale copy
# of the key surfaces on the following step and the key is yielded twice.
mutant "next: advances only delta on a tie" catch \
  's/        if \(cmp == 0\) \{\n            \/\* Advance BOTH/        if (cmp == 0) {\n            c->di++;\n            return;\n            \/* Advance BOTH/'

mutant "next: advances only base on a tie" catch \
  's/            c->di\+\+;\n            c->bi\+\+;\n        \} else if \(cmp < 0\) \{/            c->bi++;\n        } else if (cmp < 0) {/'

# D-13b at the commit site.  db->snap and txn->snap are the commit's own two
# references; counting "sole holder" as one made the incremental fold dead code
# from the first commit -- every commit fell back to a full refresh, quadratic
# over the active file when no cache directory seeds it.  This mutant IS that
# bug, preserved.
mutant "commit: incremental fold never taken" catch \
  's/    if \(act && offs && db->snap->refcount == 2/    if (act \&\& offs \&\& db->snap->refcount == 1/'

# The insert path (D-13b), and the bound that makes it amortised O(1).
mutant "insert: no replace of an existing key" catch \
  's/    if \(zsi_index_eq\(ix->delta, ix->ndelta, i, &ks, key, keylen\)\) \{\n        ix->delta\[i\] = off;\n        return ZS_OK;\n    \}/    \/* replace removed *\//'

mutant "insert: delta never merges" catch \
  's/    if \(ix->ndelta <= ZSI_DELTA_MAX\) return ZS_OK;/    if (ix->ndelta <= SIZE_MAX) return ZS_OK;/'

mutant "merge: base wins ties" catch \
  's/        if \(cmp == 0\)      \{ merged\[w\+\+\] = ix->delta\[di\+\+\]; bi\+\+; \}/        if (cmp == 0)      { merged[w++] = ix->base[bi++]; di++; }/'

mutant "merge: drops the tied base entry twice" catch \
  's/        if \(cmp == 0\)      \{ merged\[w\+\+\] = ix->delta\[di\+\+\]; bi\+\+; \}/        if (cmp == 0)      { merged[w++] = ix->delta[di++]; }/'

# Lower bound: an off-by-one here makes an exact match miss, or a seek land one
# entry early, which shows up as a key that cannot be found or one emitted twice.
mutant "lower_bound: <= instead of <" catch \
  's/        if \(ks->compar\(k, kl, key, keylen\) < 0\) lo = mid \+ 1;/        if (ks->compar(k, kl, key, keylen) <= 0) lo = mid + 1;/'

mutant "lower_bound: hi = mid - 1" catch \
  's/        else hi = mid;\n    \}\n\n    return lo;/        else hi = mid ? mid - 1 : 0;\n    }\n\n    return lo;/'

mutant "seek: delta side not sought" catch \
  's/    c->di = zsi_index_lb\(ix->delta, ix->ndelta, &ks, key, keylen\);/    c->di = 0;/'

echo
echo "span chain (Task 7)"

# F-25: visibility is per span, not a watermark.  A rolled-back span may sit
# between two live ones, so stopping at a rollback -- or treating it as a
# high-water mark -- loses committed data after it.
mutant "F-25: rollback stops the walk" catch \
  's/        if \(cb && !zsi_term_is_rollback\(&term\)\) \{/        if (zsi_term_is_rollback(\&term)) break;\n        if (cb) {/'

mutant "F-25: rollback replayed anyway" catch \
  's/        if \(cb && !zsi_term_is_rollback\(&term\)\) \{/        if (cb) {/'

# F-22 / C-4f: the terminator checksum is what makes a torn tail detectable, and
# what makes reading a live file safe with no lock.
mutant "F-22: span checksum not verified" catch \
  's/            uint32_t want = zsi_csum2\(f->csum, f->csum_id,\n                                      spandata \? spandata : "", datalen,\n                                      termbytes, term.len - 4\);\n            if \(want != term.csum\) break;/            (void)spandata; (void)termbytes;/'

mutant "F-22: checksum over span only" catch \
  's/                                      spandata \? spandata : "", datalen,\n                                      termbytes, term.len - 4\);/                                      spandata ? spandata : "", datalen,\n                                      termbytes, 0);/'

# F-23: the terminator'"'"'s span length must equal the bytes actually present.
mutant "F-23: span length not checked" catch \
  's/        if \(term.spanlen != \(uint64_t\)datalen\) break;/        \/* length check removed *\//'

# F-24: complete at the last VALID span.  Advancing the complete point after pass
# one but BEFORE the length and checksum checks makes trailing garbage part of the
# database -- which is the actual failure this rule prevents.
#
# An earlier version of this mutant inserted `f->complete = pos` at the top of the
# loop, which is a no-op: complete already equals pos there.  It was reported as an
# uncaught gap and was really a mutant that mutated nothing.
mutant "F-24: complete advanced before validation" catch \
  's/        size_t datalen = p - span_start;/        f->complete = p;\n        size_t datalen = p - span_start;/'

mutant "F-24: complete set past the terminator" catch \
  's/        f->complete = after;\n        f->last_term_off  = p;/        f->complete = f->size;\n        f->last_term_off  = p;/'

# D-9: clean requires a VALID HEADER as well as nothing after the last span.  A
# zero-length file has complete == size == 0 and would otherwise look clean, so a
# writer would append to a file with no header (R-4).
mutant "D-9: clean ignores header validity" catch \
  's/    return f->hdr_valid && f->complete == f->size;/    return f->complete == f->size;/'

# D-10: an invalid header means zero spans, not an error and not a replay attempt.
mutant "D-10: invalid header replayed anyway" catch \
  's/    if \(!f->hdr_valid\) \{\n        f->complete = 0;\n        return ZS_OK;\n    \}/    if (!f->hdr_valid) { f->complete = ZSI_HEADER_LEN; }/'

# Section 4.9: a pointer section cannot appear in an unordered file.
#
# Equivalent, because zsi_rec_decode rejects any type without HasKey too, so the
# walk's check is a fast path over a rule enforced one level down (and that level
# IS tested, by test_record_bounds).  Kept because the walk should not depend on
# the decoder's error taxonomy to know that a pointer section ends a span.
mutant "PTRS accepted mid-span" equivalent \
  's/            if \(!\(type & ZSI_HASKEY\)\) break;/            \/* family check removed *\//'

# F-29: the progress rule.
#
# All three of these are equivalent, and it took reading zsi_rec_decode to be sure
# rather than guessing.  It guarantees out->len != 0 (it rejects a saturated
# roundup8) and total <= len where len is f->size - p, so next > p and
# next <= f->size already hold for every record it accepts.
#
# The checks stay because F-29 requires the verification at the ITERATION site, not
# somewhere it happens to be implied, and because they become load-bearing the
# moment the decoder's contract changes -- which is exactly the change nobody would
# think to audit the walk for.  They are deliberately redundant, not dead.
mutant "F-29: no progress check" equivalent \
  's/            if \(r.len == 0\) break;\n            if \(!zsi_add_sz\(p, r.len, &next\)\) break;\n            if \(next <= p\) break;\n            if \(next > f->size\) break;/            next = p + r.len;/'

mutant "F-29: next not bounded by size" equivalent \
  's/            if \(next > f->size\) break;/            \/* bound removed *\//'

# ...and the mutant that shows the redundancy above really is defence and not just
# dead code: break the DECODER's bound as well, so nothing downstream of the walk
# enforces it either.  Something must object.
mutant "F-29: neither decode nor walk bounds" catch \
  's/    if \(total > len\) return ZS_BADFORMAT;/    \/* decode bound removed *\//; s/            if \(next > f->size\) break;/            \/* walk bound removed *\//'

# The walk must start after the header, not at 0.
mutant "walk starts at offset 0" catch \
  's/    size_t pos = from < ZSI_HEADER_LEN \? ZSI_HEADER_LEN : from;/    size_t pos = 0;/'

echo
echo "file object (Task 6)"

# F-30's single choke point.  Every one of these turns a bounds check into a
# bounds-check bypass, which is the whole reason the check lives in one place.
mutant "bounds: overflow unguarded" catch \
  's/    if \(!zsi_add_sz\(off, len, &end\)\) return NULL;   \/\* G-0b \*\//    end = off + len;/'

mutant "bounds: off-by-one at the end" catch \
  's/    if \(end > f->size\) return NULL;/    if (end > f->size + 1) return NULL;/'

mutant "bounds: only offset checked" catch \
  's/    if \(end > f->size\) return NULL;/    if (off > f->size) return NULL;/'

# base is NULL exactly when size is 0, so the size check already rejects every
# request with a nonzero length, and the only surviving case -- offset 0, length 0
# -- returns `base + 0`, which is NULL either way.  Confirmed unobservable: UBSan
# with -fsanitize=pointer-overflow does not flag it, and C23 made NULL + 0
# well-defined regardless.  The check stays because it states the invariant
# linking base to size, which is otherwise only implied by the open path.
mutant "bounds: NULL base not checked" equivalent \
  's/    if \(!f->base\) return NULL;                      \/\* zero-length file \*\//    \/* base check removed *\//'

# D-10: a zero-length or corrupt-header active file must be a legal state, not an
# error, or a crash leaves a database that cannot be opened at all (G-3).
mutant "D-10: zero length is an error" catch \
  's/    if \(f->size > 0\) \{/    if (f->size == 0) { zsi_file_close(\&f); return ZS_BADFORMAT; }\n    if (f->size > 0) {/'

mutant "D-10: bad header is an error" catch \
  's/    if \(!f->hdr_valid\) \{\n        \/\* Restore the name-derived generation/    if (!f->hdr_valid) { zsi_file_close(\&f); return ZS_BADFORMAT; }\n    if (!f->hdr_valid) {\n        \/* Restore the name-derived generation/'

mutant "D-10: generation not taken from name" catch \
  's/        f->hdr.start = name_start;\n        f->hdr.end = 0;\n        f->csum = zsi_csum_none;/        f->hdr.start = 0;\n        f->hdr.end = 0;\n        f->csum = zsi_csum_none;/'

# F-5a: a file'"'"'s engine comes from its own header, never the reader'"'"'s configuration.
mutant "engine: hardcoded xxhash" catch \
  's/        zs_csum \*cs = zsi_csum_for_id\(id, external_csum\);/        zs_csum *cs = zsi_csum_xxhash; (void)id; (void)external_csum;/'

mutant "engine: unknown id accepted" catch \
  's/        if \(cs && zsi_header_decode\(f->base, f->size, cs, &f->hdr\) == ZS_OK\) \{/        if (!cs) cs = zsi_csum_none;\n        if (zsi_header_decode(f->base, f->size, cs, \&f->hdr) == ZS_OK) {/'

# The kind must be readable from the header alone (section 2).
mutant "kind: invalid header reads as in-order" catch \
  's/    return !f->hdr_valid \|\| zsi_header_is_unordered\(&f->hdr\);/    return f->hdr_valid \&\& zsi_header_is_unordered(\&f->hdr);/'

mutant "open: ENOENT reported as IOERROR" catch \
  's/        int r = \(errno == ENOENT\) \? ZS_NOTFOUND : ZS_IOERROR;/        int r = ZS_IOERROR;/'

mutant "open: directory accepted as a file" catch \
  's/    if \(!S_ISREG\(sb.st_mode\)\) \{ zsi_file_close\(&f\); return ZS_BADFORMAT; \}/    \/* S_ISREG check removed *\//'

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
  's/        if \(!zsi_add3_sz\(keylen, vallen, 2 \+ 4, &body\)\) return 0;/        if (!zsi_add3_sz(keylen, vallen, 0 + 4, \&body)) return 0;/'

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
  's/        if \(!zsi_add3_sz\(keylen, vallen, 2 \+ 4, &body\)\) return ZS_BADFORMAT;/        body = keylen + vallen + 2 + 4;/'

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
echo "pointer table cache (spec section 8)"

# Every acceptance rule of P-11, one at a time.  A table that is accepted when it
# should not be does not fail loudly: it produces WRONG RECORDS, silently, which
# is why each rule needs its own mutant rather than one covering the group.
mutant "idx: drops the file's comparator check" catch \
  's/    if \(memcmp\(h\.compar_name, f->hdr\.compar_name, ZSI_COMPAR_NAME_LEN\) != 0\)\n        goto out;/    \/* P-11 file comparator check removed *\//'

mutant "idx: drops our own comparator check" catch \
  's/    if \(memcmp\(h\.compar_name, compar_name, ZSI_COMPAR_NAME_LEN\) != 0\) goto out;/    \/* P-11 handle comparator check removed *\//'

mutant "idx: drops the engine agreement check" catch \
  's/    if \(zsi_idxhdr_engine_id\(buf\) != f->csum_id\) goto out;/    \/* P-7 engine check removed *\//'

mutant "idx: drops the uuid check" catch \
  's/    if \(memcmp\(h\.uuid, f->hdr\.uuid, 16\) != 0\) goto out;/    \/* P-11 uuid check removed *\//'

mutant "idx: drops the generation check" catch \
  's/    if \(h\.start != f->hdr\.start\) goto out;/    \/* P-11 generation check removed *\//'

mutant "idx: accepts an unverified table" catch \
  's/    if \(!\(h\.flags & ZSI_IDX_FLAG_CSUM_VERIFIED\)\) goto out;/    \/* P-11 verified-flag check removed *\//'

# SUBSUMED, with the combined mutant below.  Every wrong size the suite can
# construct shifts the trailing 4 bytes, so the offset-array checksum rejects it
# first.  Isolating the size rule would need a table whose trailing bytes happen
# to be the correct checksum of a differently-sized array, which means finding a
# preimage.  The rule stays as the cheap structural check it is.
mutant "idx: drops the exact-size check" subsumed \
  's/    if \(want != len\) goto out;/    \/* P-11 exact-size check removed *\//'

mutant "idx: drops the offset-array checksum" catch \
  's/    if \(zsi_get32\(buf \+ len - 4\)\n        != f->csum\(buf \+ ZSI_IDX_HEADER_LEN, arrlen\)\)\n        goto out;/    \/* P-11 array checksum removed *\//'

mutant "idx: drops the offset range check" catch \
  's/        if \(v < ZSI_HEADER_LEN \|\| v >= h\.valid_upto\) goto out;/        \/* P-11 offset range check removed *\//'

# SUBSUMED, with the combined mutant below.  P-10's binding pins valid_upto to
# term_off + term_len, and term_off is itself bounds-checked through
# zsi_file_at, so an out-of-range valid_upto cannot survive the binding.  The
# explicit bounds stay because they run BEFORE any file access and say plainly
# what the field means.
mutant "idx: drops the valid_upto bounds" subsumed \
  's/    if \(h\.valid_upto < ZSI_HEADER_LEN \|\| h\.valid_upto > f->size\) goto out;/    \/* P-11 valid_upto bounds removed *\//'

# The combined mutants the subsumed pair require: with the sibling check gone
# too, the defence is demonstrated even though neither layer is isolated.
mutant "idx: drops BOTH size and array checksum" catch \
  's/    if \(want != len\) goto out;/    \/* size *\//;
   s/    if \(zsi_get32\(buf \+ len - 4\)\n        != f->csum\(buf \+ ZSI_IDX_HEADER_LEN, arrlen\)\)\n        goto out;/    \/* array checksum *\//'

mutant "idx: drops BOTH valid_upto bounds and binding" catch \
  's/    if \(h\.valid_upto < ZSI_HEADER_LEN \|\| h\.valid_upto > f->size\) goto out;/    \/* bounds *\//;
   s/        if \(after != \(size_t\)h\.valid_upto\) goto out;/        \/* binding *\//;
   s/        if \(h\.term_off < ZSI_HEADER_LEN \|\| h\.term_off >= h\.valid_upto\) goto out;/        \/* term_off range *\//'

# P-10.  The binding is what catches a data file whose covered prefix is not the
# one the table was built over, which is the whole defence against an out-of-band
# restore under a surviving cache directory (P-17).
mutant "idx: drops the terminator checksum binding" catch \
  's/        if \(term\.csum != h\.term_csum\) goto out;/        \/* P-10 terminator checksum binding removed *\//'

mutant "idx: drops the terminator offset binding" catch \
  's/        if \(after != \(size_t\)h\.valid_upto\) goto out;/        \/* P-10 terminator offset binding removed *\//'

# P-5/P-6.  The symmetric-layout mutant: swapping two fields in the OFFSET TABLE
# changes both the encoder and the decoder at once, so every round-trip still
# succeeds and only test_idxcache_header_byte_layout's literal can object.  This
# is the same bug class that made test_header_byte_layout necessary.
mutant "idx: header swaps valid_upto and term_off" catch \
  's/#define ZSI_IDX_OFF_VALID_UPTO    64   \/\*  8 \*\/\n#define ZSI_IDX_OFF_TERM_OFF      72   \/\*  8 \*\//#define ZSI_IDX_OFF_VALID_UPTO    72   \/*  8 *\/\n#define ZSI_IDX_OFF_TERM_OFF      64   \/*  8 *\//'

mutant "idx: magic same as a data file's" catch \
  's/    0x89, 0x7A, 0x73, 0x69, 0x6E, 0x64, 0x65, 0x78,\n    0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00\n\};/    0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,\n    0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00\n};/'

# P-7.  Using the handle's engine produces tables a conforming peer must reject:
# the failure is silent, and the cache does nothing while appearing to work.
mutant "idx: publishes under the handle's engine" catch \
  's/    h\.flags = \(uint16_t\)\(f->csum_id & ZSI_CSUM_MASK\);/    h.flags = (uint16_t)ZSI_CSUM_XXHASH;/'

# P-4.  Writing the published name directly exposes a half-written table to a
# concurrent reader, which is G-6's rule and not something checksums excuse.
mutant "idx: publishes in place, no rename" catch \
  's/    fd = open\(tmp, O_WRONLY \| O_CREAT \| O_EXCL, 0600\);/    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);/'

# P-13.  Without the threshold a table is rewritten whole on every commit, which
# makes a bulk load quadratic.  The test asserts the absence of a table below the
# threshold, so this is observable rather than merely slow.
mutant "idx: publishes on every commit" catch \
  's/    if \(f->complete - f->cached_upto < cfg->threshold\) return ZS_DONE;/    \/* P-13 threshold removed *\//'

# P-14.  A synced table is still CORRECT, so only the sync count can object --
# which is why test_idxcache_no_fsync_on_publish counts rather than times.
mutant "idx: syncs before publishing" catch \
  's/    if \(close\(fd\) < 0\) \{ fd = -1; goto out_unlink; \}/    if (ZS_FDATASYNC(fd) < 0 || close(fd) < 0) { fd = -1; goto out_unlink; }/'

# P-16.  A sweep that ignores the uuid deletes another database's tables out of a
# shared cache directory: not a correctness bug for us, entirely one for them.
mutant "idx: sweeps other databases' tables" catch \
  's/        if \(!cfg->local && strncmp\(nm, want, 36\) != 0\) continue;/        \/* P-16 uuid check removed *\//'

# P-2b's relaxation applies ONLY to zeroskip.cache.  Applying the shared-root
# uuid rule there instead strands a foreign table in a directory that serves
# exactly one database, forever.
mutant "idx: local sweep spares foreign uuids" catch \
  's/        if \(!cfg->local && strncmp\(nm, want, 36\) != 0\) continue;/        if (strncmp(nm, want, 36) != 0) continue;/'

# P-16 again, the other way: sweeping a generation that is still live throws away
# a table the very next open would have used.
mutant "idx: sweeps live generations too" catch \
  's/        if \(alive\) continue;/        \/* P-16 liveness check removed *\//'

# P-2a.  Tables published into the shared root itself defeat the per-database
# scoping: every database's sweep readdirs every other database's tables, and
# a peer resolving <root>/<uuid> never finds them.
mutant "cache: uuid subdirectory dropped" catch \
  's/        if \(\(size_t\)snprintf\(path, sizeof\(path\), "%s\/%s",\n                             setup->index_dir, uu\) < sizeof\(path\)\) \{/        if ((size_t)snprintf(path, sizeof(path), "%s", setup->index_dir) < sizeof(path)) {/'

# P-2b/R-3.  A read-only handle creating a directory inside the database is a
# visible side effect on a forensic copy or a read-only mount.
mutant "cache: read-only handle creates zeroskip.cache" catch \
  's/            if \(!db->readonly && mkdir\(path, 0700\) != 0 && errno != EEXIST\)/            if (mkdir(path, 0700) != 0 \&\& errno != EEXIST)/'

# A-8a.  The two name different locations for the same tables; accepting both
# silently picks one and hides the misconfiguration.
mutant "open: index_dir and ZS_INDEX_LOCAL together accepted" catch \
  's/    if \(db->index_local && setup->index_dir\) \{/    if (0) {/'

# P-1.  SUBSUMED, with the combined mutant below: an in-order file has no private
# index -- it uses its pointer section -- so the !f->index line answers first and
# nothing can reach the kind check with an in-order file.  The check stays because
# P-1 is a normative rule and belongs stated at the boundary, not left implied by
# an incidental property of another field, which is the same reasoning that keeps
# zsi_type_valid a switch rather than a computed predicate.
mutant "idx: publishes for in-order files too" subsumed \
  's/    if \(!zsi_file_is_unordered\(f\)\) return ZS_DONE;          \/\* P-1 \*\//    \/* P-1 kind check removed *\//'

mutant "idx: drops BOTH the kind and index checks" catch \
  's/    if \(!f->hdr_valid \|\| !f->csum \|\| !f->index\) return ZS_DONE;/    if (!f->hdr_valid || !f->csum) return ZS_DONE;/;
   s/    if \(!zsi_file_is_unordered\(f\)\) return ZS_DONE;          \/\* P-1 \*\//    \/* P-1 *\//'

# P-12.  Seeding the index but replaying from the top of the file duplicates
# every record the table already covers.
mutant "idx: seeds but replays from the header" catch \
  's/    r = zsi_index_build_from\(f, compar, base, nbase, vu\);/    r = zsi_index_build_from(f, compar, base, nbase, ZSI_HEADER_LEN);/'

# P-10.  A publisher that records the wrong terminator writes tables every reader
# then rejects -- silently, so the cache simply stops working.
mutant "idx: publishes the wrong terminator offset" catch \
  's/    h\.term_off   = \(uint64_t\)f->last_term_off;/    h.term_off   = (uint64_t)f->complete;/'

# A-8/P-2.  Without this a read-only handle publishes into the database
# directory, which is a write to the database and exactly what R-3 forbids.
mutant "idx: allows the database dir as the cache" catch \
  's/        if \(strcmp\(dir, setup->index_dir\) == 0\) \{\n            free\(db->dir\);\n            free\(db\);\n            return ZS_BADUSAGE;\n        \}/        \/* P-2 identical-path check removed *\//'

echo
echo "seal and compact (D-25..D-29)"

# D-25a.  Both reach one in-order file; only one of them burns a generation each
# time, and generations are finite (D-9c).  Only the generation assertion tells
# them apart, which is why test_seal_creates_no_new_generation exists.
mutant "seal: leaves a new active generation" catch \
  's/    if \(r == ZS_OK\) \(void\)zsi_convert_pending\(db\);/    if (r == ZS_OK) (void)zsi_convert_pending(db);\n    if (r == ZS_OK) { int fd_ = -1; uint32_t g_ = 0;\n        if (zsi_writer_active(db, \&fd_, \&g_) == ZS_OK) close(fd_); }/'

# D-25b.  Sealing an active file with no spans writes an empty in-order file and
# consumes a generation for nothing.
mutant "seal: seals a file with no spans" catch \
  's/    if \(act->complete <= ZSI_HEADER_LEN\) goto out;  \/\* no valid spans \*\//    \/* D-25b removed *\//'

# The write lock is the ONLY thing making it safe to convert the active file:
# without it another writer may be appending to the file being converted.
mutant "seal: converts without the write lock" catch \
  's/    r = zsi_lock_take\(&db->locks, ZSI_LOCK_WRITE,\n                      db->nonblocking \? ZS_NONBLOCKING : 0\);\n    if \(r != ZS_OK\) return r;/    r = ZS_OK;/'

# A-10/R-3.  A read-only handle must not write to the database at all.
mutant "seal: writes from a read-only handle" catch \
  's/static int zsi_seal\(struct zs_db \*db\)\n\{\n    struct zsi_file \*act;\n    int r = zsi_check_writable\(db\);\n    if \(r != ZS_OK\) return r;/static int zsi_seal(struct zs_db *db)\n{\n    struct zsi_file *act;\n    int r = ZS_OK;/'

# D-25d.  Without the commit-tail seal, a one-transaction bulk load leaves an
# oversized unordered file that every open must replay and whose conversion the
# NEXT writer pays for.
mutant "commit: oversized active never sealed" catch \
  's/            int sr = zsi_convert_one\(db, oversized\);/            int sr = ZS_OK;/'

# D-25d's gate inverted: every small commit pays a conversion, and the one
# commit that should seal does not.
mutant "commit: seal threshold inverted" catch \
  's/            && oversized->size >= db->rollover_size/            \&\& oversized->size < db->rollover_size/'

# D-25e.  A table published for a file the same commit seals is born stale --
# but the seal's own refresh sweeps it (P-16) before the commit returns, so the
# mutant's only effect is a whole table written and immediately unlinked: real
# wasted I/O, no observable state.  Listed so nobody writes a bogus test
# chasing it; the skip stays because a bulk-load table is megabytes.
mutant "commit: publishes a table for the file it seals" equivalent \
  's/        if \(r == ZS_OK && db->index_dir && !sealing\) \{/        if (r == ZS_OK \&\& db->index_dir) {/'

# D-26.  Skipping the seal leaves the active generation out of the result, so the
# database never reaches one file.
mutant "compact: skips the seal" catch \
  's/    r = zsi_seal\(db\);\n    if \(r != ZS_OK\) goto out;/    r = ZS_OK;/'

# D-26b.  Merging the in-order PREFIX merges nothing when an unmergeable file
# sits second -- exactly the damaged database D-28 is about.
mutant "compact: merges the prefix, not runs" catch \
  's/            if \(count >= 2\) \{ found = true; break; \}/            if (count >= 2 \&\& first == 0) { found = true; break; }\n            break;/'

# D-28.  Returning OK regardless makes the return value meaningless exactly where
# a caller most needs it.
mutant "compact: reports success regardless" catch \
  's/        r = ZS_BADFORMAT;\n    \}\n\nout:\n    zsi_lock_release\(&db->locks, ZSI_LOCK_REPACK\);/        r = ZS_OK;\n    }\n\nout:\n    zsi_lock_release(\&db->locks, ZSI_LOCK_REPACK);/'

# D-20b.  A repack input's record bodies are covered only by the records-region
# checksum, which nothing on the read path checks.  Skipping the verification
# launders a corrupt body: the output is written under a FRESH checksum computed
# over the corrupt copy, and D-23 then removes the input -- the only evidence.
mutant "repack: inputs not verified before merge" catch \
  's/        r = zsi_ptrs_verify_records\(snap->files\[first \+ i\]\);/        r = ZS_OK;/'

# D-20b again, on the conversion path.  Since replay verifies spans in every
# mode (F-5e), the old wrong version -- honouring the handle's nocsum -- is no
# longer expressible; the surviving wrong version is dropping the walk, which
# certifies in-place damage the C-4i probe cannot see (size unchanged) into an
# output that validates while D-23 removes the evidence.
mutant "convert: the D-20b re-verify removed" catch \
  's/    \{\n        struct zsi_file scratch = \*f;\n        r = zsi_unordered_replay\(&scratch, ZSI_HEADER_LEN, NULL, NULL\);\n        if \(r != ZS_OK\) return r;\n        if \(scratch\.complete < f->complete\) \{/    if (0) {\n        struct zsi_file scratch = *f;\n        r = zsi_unordered_replay(\&scratch, ZSI_HEADER_LEN, NULL, NULL);\n        if (r != ZS_OK) return r;\n        if (scratch.complete < f->complete) {/'

# F-32a.  Skipping verification at the yield is the headline gap the whole
# format change exists to close: corrupt bytes served without a word.
mutant "record: not verified at yield" catch \
  's/    if \(c->cur\[0\]\.file && !c->db->nocsum\) \{\n        int vr = zsi_rec_verify\(c->cur\[0\]\.file->csum, &rec\);\n        if \(vr != ZS_OK\) return vr;\n    \}/    \/* F-32a removed *\//'

# F-32.  Covering [0, len) instead of [0, len-4) includes the checksum field in
# its own coverage -- the off-by-pad class, wrong for every record.
mutant "record: csum covers its own field" catch \
  's/    if \(csum\(r->base, r->len - 4\) != r->csum\) return ZS_BADCHECKSUM;/    if (csum(r->base, r->len) != r->csum) return ZS_BADCHECKSUM;/'

# F-32b.  Verifying during replay is the tempting wrong version: replay
# completes a file at its first invalid record (F-24), so this turns one
# flipped value byte into the silent loss of every record after it -- caught by
# the no-truncate test, which is the G-3 half of the requirement.
mutant "record: verified during replay" catch \
  's/            struct zsi_rec r;\n            if \(zsi_rec_decode\(b, avail, f->hdr\.start, &r\) != ZS_OK\) break;/            struct zsi_rec r;\n            if (zsi_rec_decode(b, avail, f->hdr.start, \&r) != ZS_OK) break;\n            if (zsi_rec_verify(f->csum, \&r) != ZS_OK) break;/'

# F-32.  The write-side gap: a checksum never computed reads as engine 0's
# everywhere, so every engine-1 read fails -- unmissable, which is the point:
# it proves the read tests depend on the WRITTEN value, not on a round-trip.
mutant "record: checksum never written" catch \
  's/    zsi_put32\(buf \+ total - 4, csum\(buf, total - 4\)\);/    zsi_put32(buf + total - 4, 0);/'

# F-32c.  Copying verbatim across an engine boundary carries a checksum that
# validates for nobody -- the A-6 trap at one remove, silent until read.
mutant "convert: copies verbatim across engines" catch \
  's/    bool reencode = \(src->csum_id != db->create_csum_id\);/    bool reencode = false;/'

# C-4i.  The original Cyrus bug, preserved: a shared begin that reuses the
# handle's snapshot reads the world as of the handle's last write, forever.
mutant "begin: shared reuses the handle snapshot" catch \
  's/        r = zsi_db_freshen\(db\);\n        if \(r != ZS_OK\) \{ free\(txn\); return r; \}/        r = ZS_OK;/'

# C-4i.  The probe is exact only because it checks BOTH halves.  Appends grow
# the active file without changing any name...
mutant "freshen: ignores the active file size" catch \
  's/        if \(act\) \{\n            struct stat sb;/        if (act \&\& 0) {\n            struct stat sb;/'

# ...and a rollover, conversion or repack changes the name set without growing
# any file the stale snapshot knows about.
mutant "freshen: ignores the name set" catch \
  's/    stale = !db->probe_names\n         \|\| db->probe_names_len != len\n         \|\| memcmp\(db->probe_names, names, len\) != 0;/    stale = !db->probe_names;/'

# D-14j/C-4i.  A live cursor that stops looking is just a cursor: the flag's
# entire meaning is observing other processes mid-traversal.
mutant "cursor: LIVE step stops looking" catch \
  's/            int r = zsi_db_freshen\(c->db\);\n            if \(r != ZS_OK\) return r;/            int r = ZS_OK;\n            if (r != ZS_OK) return r;/'

# C-1d.  Taking WRITE outermost inverts the order against a conforming peer.  The
# in-process assertion catches it from the other side.
mutant "compact: takes the write lock outermost" catch \
  's/    r = zsi_lock_take\(&db->locks, ZSI_LOCK_REPACK,\n                      db->nonblocking \? ZS_NONBLOCKING : 0\);\n    if \(r != ZS_OK\) return r;\n\n    \/\* Steps 1 and 2/    r = zsi_lock_take(\&db->locks, ZSI_LOCK_WRITE,\n                      db->nonblocking ? ZS_NONBLOCKING : 0);\n    if (r != ZS_OK) return r;\n\n    \/* Steps 1 and 2/'

echo
echo "salvage (S-1..S-12)"

# S-7, and the most important mutant here.  Believing a candidate terminator
# without checksumming the span it implies turns salvage from a recovery into a
# guess, and everything it produced would be unverified while claiming not to be.
mutant "salvage: resync believes a candidate unchecked" catch \
  's/        if \(zsi_csum2\(cs, csum_id, spandata \? spandata : "",\n                      \(size_t\)t\.spanlen, termbytes, t\.len - 4\) != t\.csum\)\n            continue;/        \/* S-7 proof removed *\//'

# S-7 again.  SUBSUMED, and the group's demonstration is the checksum mutant
# above rather than a combined one.
#
# Reaching this check needs a candidate whose implied span starts BEFORE the last
# verified boundary and still checksums correctly -- overlapping spans, which a
# conforming writer never produces and which no test hand-builds.  Everything the
# suite does construct is rejected first by the checksum proof, or, where
# p - spanlen underflows, by zsi_file_at's bounds check (F-30).
#
# It stays because it states the invariant the `floor` parameter exists for,
# rather than leaving it as a consequence of an unsigned comparison two lines up.
mutant "salvage: resync ignores the floor" subsumed \
  's/        if \(start < floor\) continue;/        \/* floor removed *\//'

# S-9.  Recovering a rolled-back span resurrects a transaction that never
# happened and that no conforming reader has ever shown.
mutant "salvage: recovers rolled-back spans" catch \
  's/        if \(zsi_term_is_rollback\(&term\)\) \{/        if (0) {/'

# S-8.  Unverifiable records carry no checksum of their own; producing them
# without being asked makes the default output partly unproven.
mutant "salvage: recovers unverified without the flag" catch \
  's/    bool want_unverified =\n        \(ctx->setup->flags & ZS_SALVAGE_UNVERIFIED\) != 0;/    bool want_unverified = true;/'

# S-2.  Applying the tiling check makes salvage refuse the database it exists
# for -- one missing generation, every other file perfectly readable.
mutant "salvage: applies the tiling check" catch \
  's/    r = zsi_fileset_scan\(from, NULL, &fs\);/    r = zsi_fileset_scan(from, NULL, \&fs);\n    if (r == ZS_OK) { int rr_ = zsi_fileset_resolve(\&fs);\n        if (rr_ != ZS_OK) { zsi_fileset_fini(\&fs); return rr_; } }/'

# S-3.  Newest first makes an OLDER value win, silently, with no error anywhere.
mutant "salvage: processes newest first" catch \
  's/    if \(a->start != b->start\) return a->start < b->start \? -1 : 1;/    if (a->start != b->start) return a->start < b->start ? 1 : -1;/'

# S-10.  The report is the mitigation for emitting a possibly stale value, so
# dropping it leaves the value emitted and the risk unnamed.
mutant "salvage: never reports stale keys" catch \
  's/    if \(!ctx->any_loss \|\| !ctx->setup->report\) return ZS_OK;/    return ZS_OK;\n    if (!ctx->any_loss || !ctx->setup->report) return ZS_OK;/'

# S-10 the other way: reporting every key makes the report useless rather than
# wrong, which is the failure mode that would survive a careless test.
mutant "salvage: reports every key as stale" catch \
  's/            if \(d == 0\) \{ safe = true; break; \}/            if (d == 0) { break; }/'

# S-1.  Opening the source for writing is the one thing salvage must never do.
mutant "salvage: opens the source writable" catch \
  's/    outsetup\.flags = ZS_CREATE;/    outsetup.flags = ZS_CREATE;\n    { struct zs_open_data w_ = ZS_OPEN_DATA_INITIALIZER; struct zs_db *wd_ = NULL;\n      if (zs_db_open(from, \&w_, \&wd_) == ZS_OK) zs_db_close(\&wd_); }/'

echo
echo "cursor liveness (D-14j)"

# D-14j-a, the bug the Cyrus integration surfaced.  Holding an INDEX into the
# transaction's sorted pending array means a write during the traversal shifts
# the element under it and the cursor re-yields a key it already returned --
# silently, so the caller processes a record twice.
mutant "cursor: txn arm resumes from the index" catch \
  's/    if \(zsi_pend_search\(fc->txn, fc->tkey, fc->tkeylen, &pos\) == ZS_OK\n        && fc->tstarted\)\n        pos\+\+;/    (void)zsi_pend_search(fc->txn, fc->tkey, fc->tkeylen, \&pos);/'

# D-14j-b the other way: always advancing past the key means a seek that lands
# ON a key skips it, so a scan from a start key loses its first record.
mutant "cursor: txn arm always skips the seek hit" catch \
  's/        && fc->tstarted\)\n        pos\+\+;/        )\n        pos++;/'

# A-1a.  The merge caches each arm's current record, so without noticing the
# pending array changed, a write made during the traversal is never seen.
mutant "cursor: ignores writes on its own txn" catch \
  's/    if \(c->txn && zsi_txn_seq\(c->txn\) != c->txn_seq\) \{/    if (0) {/'

# D-14j.  The same for a non-transactional foreach whose callback commits --
# which is exactly what cyrusdb promises and what the integration hit.
mutant "cursor: ignores its own handle's commits" catch \
  's/        if \(c->snap != c->db->snap\) \{/        if (0) {/'

# G-4, from the other side: a cursor inside an EXPLICIT transaction must keep a
# fixed file set.  Making every cursor live breaks the transactional read.
mutant "cursor: explicit txn goes live too" catch \
  's/    if \(c->handle_live\) \{/    if (1) {/'

# D-14j-b.  A refresh re-seeks by key, and a seek lands ON that key when it is
# still present -- which has already been yielded.  Not skipping it re-emits it.
mutant "cursor: re-yields the key it resumed from" catch \
  's/    if \(!fc->exhausted\n        && c->db->compar\(fc->cur.key, fc->cur.keylen,\n                         c->last_key, c->last_keylen\) == 0\)\n        return zsi_fcur_next\(fc\);/    \/* landed-on-key skip removed *\//'

# D-14j-b, reported downstream (Cyrus aaa-db foreach_changes).  On a pending
# write, re-positioning the transaction arm from ITS OWN state -- the last key
# consumed from that arm -- instead of from the cursor's last yielded key.  The
# arm's position lags the merge (an arm exhausted at open has consumed nothing),
# so a key stored BEHIND the cursor resurfaces and is yielded out of order,
# shifting the rest of the traversal by one.
mutant "cursor: txn arm resumes from its own position" catch \
  's/(zsi_cursor_reseek_txn\(struct zs_cursor \*c\)\n\{\n.*?)int r = zsi_cursor_reseek_arm\(c, &c->cur\[i\]\);/$1int r = zsi_fcur_load(\&c->cur[i]);/s'

# The txn-only re-seek is a PERFORMANCE split, not a behavioural one: the full
# re-seek repositions every arm to the same place, it just searches every file
# again for a change that touched none of them.  Recorded so nobody writes a
# bogus test chasing it.
mutant "cursor: pending change re-seeks every arm" equivalent \
  's/    if \(txn_moved\)   return zsi_cursor_reseek_txn\(c\);/    if (txn_moved)   return zsi_cursor_reseek(c);/'

# D-14e: the merge takes from element 0, so an arm repositioned without the
# re-sort sits wherever it was and its record surfaces at the wrong point in
# the order.
mutant "cursor: txn re-seek skips the sort" catch \
  's/(zsi_cursor_reseek_txn\(struct zs_cursor \*c\)\n\{\n.*?)    zsi_cur_sort\(c\);/$1    \/* no sort *\//s'

# D-14j-b, reported downstream.  A refresh before the first record has been
# emitted has no last-yielded key to resume from, so it must fall back to the
# key the cursor was OPENED at.  Falling back to "the first key" yields records
# before the start key -- and under ZS_CURSOR_PREFIX lands outside the prefix,
# ending the scan immediately and returning nothing at all.
mutant "cursor: refresh forgets the start key" catch \
  's/    if \(!c->last_key\) return zsi_cursor_seek_arm_start\(c, fc\);/    if (!c->last_key) return zsi_fcur_seek_first(fc);/'

# A-4 under streaming.  Unmapping a superseded mapping "to save address
# space" dangles every pointer a read returned out of it -- the same bug the
# retire list fixed for the buffered writer, in its streaming shape.
mutant "txn: superseded mappings unmapped early" catch \
  's/    txn->maps\[txn->nmaps\].base = \(char \*\)m;\n    txn->maps\[txn->nmaps\].len = want;\n    txn->nmaps\+\+;/    if (txn->nmaps) munmap(txn->maps[txn->nmaps - 1].base, txn->maps[txn->nmaps - 1].len);\n    txn->maps[txn->nmaps].base = (char *)m;\n    txn->maps[txn->nmaps].len = want;\n    txn->nmaps++;/'

# C-8/F-21, the writer side.  No ROLLBACK terminator: the aborted records are
# either resurrected into the next span or invalidate its structure -- both
# ways the abort stops meaning anything.
mutant "abort: no ROLLBACK terminator" catch \
  's/    if \(txn->wfd >= 0 && txn->wsize > txn->span_base\)\n        \(void\)zsi_txn_terminate\(txn, true, NULL, NULL\);/    \/* no rollback *\//'

# F-21 the other way: a COMMIT terminator on the abort path makes the aborted
# records live outright.  Applies to the poisoned-commit path too, which
# voids its torn span the same way.
mutant "abort: writes COMMIT instead of ROLLBACK" catch \
  's/\(void\)zsi_txn_terminate\(txn, true, NULL, NULL\);/(void)zsi_txn_terminate(txn, false, NULL, NULL);/g'

# The flush-before-covering-mapping ordering: a record still in the chunk
# buffer is not in the file, and a mapping that covers its OFFSET shows the
# stale bytes there instead.  This was a real bug during bring-up.
mutant "txn: covering mapping wins over the unflushed chunk" catch \
  's/    if \(need > txn->flushed && zsi_txn_flush\(txn\) != ZS_OK\) return NULL;\n\n    if \(txn->nmaps && need <= txn->maps\[txn->nmaps - 1\].len\)\n        return txn->maps\[txn->nmaps - 1\].base \+ off;/    if (txn->nmaps \&\& need <= txn->maps[txn->nmaps - 1].len)\n        return txn->maps[txn->nmaps - 1].base + off;\n\n    if (need > txn->flushed \&\& zsi_txn_flush(txn) != ZS_OK) return NULL;/'

# C-6b: the structural syncs hold in EVERY durability mode.  Each mutant
# re-adds the nosync guard the fix removed at one site; under ZS_NOSYNC that
# site then publishes (or creates) without making the bytes durable first, and
# a crash costs converted generations rather than the active tail.  Caught by
# the crash suite's exact sync signatures.
mutant "creation: header sync skipped under NOSYNC" catch \
  's/    if \(ZS_FDATASYNC\(fd\) < 0\) \{ close\(fd\); return ZS_IOERROR; \}/    if (!db->nosync \&\& ZS_FDATASYNC(fd) < 0) { close(fd); return ZS_IOERROR; }/'

mutant "conversion: output sync skipped under NOSYNC" catch \
  's/     \* records. only other copy\. \*\/\n    if \(r == ZS_OK && ZS_FDATASYNC\(fd\) < 0\) r = ZS_IOERROR;/     * records only other copy. *\/\n    if (r == ZS_OK \&\& !db->nosync \&\& ZS_FDATASYNC(fd) < 0) r = ZS_IOERROR;/'

mutant "repack: output sync skipped under NOSYNC" catch \
  's/    \/\* Durable before the rename, in every durability mode \(C-6b\)\. \*\/\n    if \(r == ZS_OK && ZS_FDATASYNC\(fd\) < 0\) r = ZS_IOERROR;/    if (r == ZS_OK \&\& !db->nosync \&\& ZS_FDATASYNC(fd) < 0) r = ZS_IOERROR;/'

# C-4i's probe must stay stale across a FAILED refresh.  Committing the
# baseline first poisons the probe: after a transient refresh failure the
# names already match, and a snapshot with no active file has no size to
# disagree, so a peer's commits read as fresh indefinitely.
mutant "freshen: baseline committed before the refresh" catch \
  's/    r = zsi_db_refresh\(db\);\n    if \(r != ZS_OK\) \{ free\(names\); return r; \}\n\n    free\(db->probe_names\);\n    db->probe_names = names;\n    db->probe_names_len = len;\n    return ZS_OK;/    free(db->probe_names);\n    db->probe_names = names;\n    db->probe_names_len = len;\n    return zsi_db_refresh(db);/'

# C-4i at write begin.  Rebuilding unconditionally is CORRECT -- every store
# still lands and every read is fresh, so no data test can see it -- but it
# replays the active file on every begin, O(active file) per commit, the
# quadratic bulk load the D-13b fold exists to prevent.  Found downstream as
# a throughput sawtooth against rollover_size.  The anchor is the comment's
# closing line, because the shared-begin path calls zsi_db_freshen too.
mutant "write begin: rebuilds the snapshot unconditionally" catch \
  's/         \* snapped back at rollover\. \*\/\n        r = zsi_db_freshen\(db\);/         * snapped back at rollover. *\/\n        r = zsi_db_refresh(db);/'

echo
echo "same-process exclusion (C-1j)"

# C-1j: the registry must not be dropped -- on a platform without OFD locks
# that is the whole of the guarantee, and on one with them this is what T-14's
# second run exercises.
mutant "lock: registry not consulted on take" catch \
  's/    int r = zsi_lockreg_acquire\(lk, which, block\);\n    if \(r != ZS_OK\) return r;\n\n    r = zsi_lock_fcntl/    int r = zsi_lock_fcntl/'

# ... and must be released, or the first writer in a process is the only one
# that ever writes.
mutant "lock: registry not dropped on release" catch \
  's/    lk->held &= ~\(1u << which\);\n    zsi_lockreg_drop\(lk, which\);/    lk->held \&= ~(1u << which);/'

# C-1j's key is st_dev AND st_ino.  Dropping the device leaves inode numbers,
# which are unique only WITHIN a filesystem -- so two databases on different
# filesystems whose lock files happen to share an inode number would exclude
# each other, and a writer on one would block on the other.  Isolating that
# needs two filesystems with a colliding inode, which no portable test can
# construct; the combined mutant below removes the whole comparison and IS
# caught, so the key is demonstrated to be load-bearing even though this half
# of it cannot be isolated.
mutant "lock: registry ignores the device number" subsumed \
  's/        while \(e && !\(e->dev == sb.st_dev && e->ino == sb.st_ino\)\) e = e->next;/        while (e \&\& !(e->ino == sb.st_ino)) e = e->next;\n        if (0) { }/'

# The entry is per database.  One process-wide word would deadlock any caller
# holding two databases open (C-1h).
mutant "lock: registry entry shared by all databases" catch \
  's/        while \(e && !\(e->dev == sb.st_dev && e->ino == sb.st_ino\)\) e = e->next;/        \/* first entry, whatever it is *\//'

# OFD selection: falling back to F_SETLK while the registry is off leaves
# nothing excluding two handles at all.
mutant "lock: OFD commands replaced by POSIX ones" catch \
  's/    int wait_cmd = F_OFD_SETLKW, try_cmd = F_OFD_SETLK;/    int wait_cmd = F_SETLKW, try_cmd = F_SETLK;/'

echo
echo "A-4a borrow lifetime across a snapshot swap"

# A-4a: the bug as it shipped.  A transaction that reads before its first write
# holds pointers into the snapshot it began on; the first store resolves the
# active file and, when that starts a new generation, refreshes the handle --
# and releasing the outgoing snapshot there unmaps the caller's bytes.  Both
# mutants restore the plain release.
mutant "txn: first-store swap releases the borrowed snapshot" catch \
  's/    int hr = zsi_snapshot_retire\(&txn->snap, &txn->hold\);\n    if \(hr != ZS_OK\) \{ close\(fd\); return hr; \}/    zsi_snapshot_release(\&txn->snap);/'

mutant "cursor: live swap releases the borrowed snapshot" catch \
  's/                int hr = zsi_snapshot_retire\(&old, &c->hold\);\n                if \(hr != ZS_OK\) return hr;/                zsi_snapshot_release(\&old);/'

# The retention granularity itself: stealing the mapping but leaving the file
# object owning it is the same dangle with more code.
mutant "retire: detaches the mapping but still unmaps it" catch \
  's/        f->base = NULL;\n        f->maplen = 0;/        \/* left attached *\//'

echo
echo "reverse iteration (D-14k, D-14l, A-12, A-13)"

# D-14k: reverse flips the KEY order only.  Flipping the generation tie-break
# too puts the OLDEST version of every duplicated key at element 0, so the
# merge yields values that were overwritten.
mutant "cursor: reverse flips the generation tie-break" catch \
  's/    if \(a->gen == b->gen\) return 0;\n    return a->gen > b->gen \? -1 : 1;        \/\* higher generation first \*\//    if (a->gen == b->gen) return 0;\n    if (c->reverse) return a->gen > b->gen ? 1 : -1;\n    return a->gen > b->gen ? -1 : 1;/'

# D-14k: an inclusive reverse seek that never lands ON the key returns the
# predecessor of a present key -- FETCHPREV of an existing key is wrong.
# Base and delta take separate branches, hence two mutants.
mutant "index: reverse inclusive seek dropped (base)" catch \
  's/    if \(inclusive && zsi_index_eq\(ix->base, ix->nbase, c->bi, &ks, key, keylen\)\)\n        c->bi\+\+;/    \/* inclusive dropped *\//'

mutant "index: reverse inclusive seek dropped (delta)" catch \
  's/    if \(inclusive && zsi_index_eq\(ix->delta, ix->ndelta, c->di, &ks, key, keylen\)\)\n        c->di\+\+;/    \/* inclusive dropped *\//'

# D-14h one level down, in reverse: a key present in both base and delta must
# consume BOTH on the step past it, or the base's stale copy surfaces and the
# arm yields the same key twice -- the second time with the OLD value.
mutant "index: reverse tie leaves the base entry" catch \
  's/        if \(cmp == 0\) \{\n            \/\* Step past BOTH -- the D-14h reason zsi_index_cur_next gives\. \*\/\n            c->di--;\n            c->bi--;/        if (cmp == 0) {\n            c->di--;/'

# D-14j-b reversed: after a yield the txn arm must resume strictly BELOW the
# key it yielded; answering the exact hit again re-yields it forever.
mutant "cursor: reverse txn arm resumes inclusively" catch \
  's/    if \(exact && !fc->tstarted && !fc->texclusive\) \{/    if (exact \&\& !fc->texclusive) {/'

# D-14k: seeking at the prefix ITSELF instead of its byte-successor starts the
# scan below every key carrying the prefix, reporting a populated range empty.
mutant "cursor: reverse prefix seeks the prefix, not its successor" catch \
  's/        if \(c->rev_succ_none\) return zsi_fcur_seek_last\(fc\);\n        return zsi_fcur_seek_rev\(fc, c->rev_succ, c->rev_succlen, false\);/        return zsi_fcur_seek_rev(fc, c->prefix, c->prefixlen, true);/'

# A-13: LIVE composed with reverse accepted silently.
mutant "cursor: reverse accepts ZS_CURSOR_LIVE" catch \
  's/    if \(\(flags & ZS_REVERSE\) && \(flags & ZS_CURSOR_LIVE\)\) return ZS_BADUSAGE;/    \/* not rejected *\//'

# A-13: reverse foreach accepted silently -- it would even work, which is
# exactly why the reject needs a test: an untested promise is worse than none.
mutant "foreach: reverse accepted" catch \
  's/    if \(flags & ZS_REVERSE\) return ZS_BADUSAGE;/    \/* not rejected *\//'

# A-12: SKIPROOT not forwarded -- the strict variants silently become the
# inclusive ones, in both directions at once.
mutant "fetch: point forms ignore SKIPROOT" catch \
  's/                        \| \(\(uint32_t\)flags & ZS_SKIPROOT\);/                        ;/'

# A-12: the inclusive-≥ form silently made strict again -- the pre-2026-08-13
# bare FETCHNEXT, under which a present key answers with its successor and
# the point form disagrees with a walk (G-7).
mutant "fetch: FETCHNEXT skips the root" catch \
  's/        uint32_t cflags = \(\(flags & ZS_FETCHNEXT\) \? 0u : \(uint32_t\)ZS_REVERSE\)/        uint32_t cflags = ((flags \& ZS_FETCHNEXT) ? (uint32_t)ZS_SKIPROOT : (uint32_t)ZS_REVERSE)/'

# Initialising the counter at open is a PERFORMANCE fix, not a correctness one:
# with the fallback above in place a spurious refresh resumes correctly, it just
# re-seeks every arm for nothing on the first step of every cursor opened in a
# transaction that already holds a write.  Recorded so nobody writes a bogus
# test chasing it.
mutant "cursor: txn_seq not taken at open" subsumed \
  's/    c->txn_seq = zsi_txn_seq\(txn\);/    \/* not taken *\//'

echo
if [ "$ROTONLY" -eq 1 ]; then
    printf '%d patterns intact, %d ROTTED (no mutant was built or run)\n' \
        "$intact" "$broken"
else
    printf '%d caught, %d equivalent, %d NOT CAUGHT, %d inconclusive\n' \
        "$caught" "$equivalent" "$missed" "$broken"
fi

[ "$missed" -eq 0 ] && [ "$broken" -eq 0 ]
