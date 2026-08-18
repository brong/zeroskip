/* zstest.c - standalone test suite for zeroskip
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * One binary.  Run all tests, or filter by substring:
 *
 *     ./zstest              all tests
 *     ./zstest record       tests whose name contains "record"
 *
 * Each test gets a fresh temporary directory.  Tests are numbered after the
 * conformance suite in the spec's section 9 (T-1 .. T-11), and
 * doc/conformance.md maps each normative requirement to the test enforcing it.
 *
 * See LICENSE.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* The implementation is included rather than linked, so tests can reach the
 * internal zsi_* statics.
 *
 * The interoperability constants of T-2c must be asserted against literals at
 * the level where they are computed.  Checking them only through the public API
 * would let a pair of compensating bugs -- say a comparator that orders high
 * bytes wrongly and a pointer section written in that same wrong order -- pass
 * every round-trip test while producing files no other implementation can read.
 */
#include "zeroskip.c"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/*
 * ============================================================
 * Test framework
 * ============================================================
 */

static int total_tests = 0;
static int total_passed = 0;
static int total_failed = 0;
static int total_skipped = 0;
static int current_test_failed = 0;

/* assertion macros for test functions (return void) */
#define ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "\n    FAIL %s:%d: %s == %lld, expected %s == %lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        current_test_failed = 1; \
        return; \
    } \
} while (0)

/* Unsigned form, for values above LLONG_MAX (offsets, checksums, generations). */
#define ASSERT_EQU(a, b) do { \
    unsigned long long _a = (unsigned long long)(a); \
    unsigned long long _b = (unsigned long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "\n    FAIL %s:%d: %s == %llu (0x%llX), " \
                "expected %s == %llu (0x%llX)\n", \
                __FILE__, __LINE__, #a, _a, _a, #b, _b, _b); \
        current_test_failed = 1; \
        return; \
    } \
} while (0)

#define ASSERT_OK(r) ASSERT_EQ(r, ZS_OK)
#define ASSERT_NULL(p) ASSERT((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT((p) != NULL)
#define ASSERT_STR_EQ(a, b) ASSERT(strcmp(a, b) == 0)
#define ASSERT_MEM_EQ(a, b, len) ASSERT(memcmp(a, b, len) == 0)

/* Sign comparison, for comparator results: the magnitude is unspecified, so
 * asserting it would test the implementation rather than the requirement. */
#define ASSERT_SIGN(v, expected) do { \
    int _v = (v); \
    int _e = (expected); \
    int _vs = (_v > 0) - (_v < 0); \
    if (_vs != _e) { \
        fprintf(stderr, "\n    FAIL %s:%d: sign of %s is %d, expected %d\n", \
                __FILE__, __LINE__, #v, _vs, _e); \
        current_test_failed = 1; \
        return; \
    } \
} while (0)

/* assertion macros for callback functions (return int) */
static int cb_failures = 0;

#define CB_ASSERT(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "\n    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        cb_failures++; \
    } \
} while (0)

#define CB_ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        fprintf(stderr, "\n    FAIL %s:%d: %s == %lld, expected %s == %lld\n", \
                __FILE__, __LINE__, #a, _a, #b, _b); \
        cb_failures++; \
    } \
} while (0)

#define CB_ASSERT_OK(r) CB_ASSERT_EQ(r, ZS_OK)
#define CB_ASSERT_STR_EQ(a, b) CB_ASSERT(strcmp(a, b) == 0)

#define SKIP(msg) do { \
    fprintf(stderr, "SKIP: %s\n", msg); \
    total_skipped++; \
    return; \
} while (0)

/* macOS's leaks(1) analyses a process at exit, and a forked child inherits that
 * arrangement -- so each child tries to leak-check itself and the run never
 * finishes.  `make leaks` therefore sets ZS_TEST_NO_FORK, and the fork-based tests
 * skip rather than being filtered out by name, so the skip is visible in the
 * summary instead of silently absent.
 *
 * Nothing is lost: on Linux the leak target uses LeakSanitizer, which has no such
 * problem and runs the whole suite. */
/* A NON-EMPTY value disables them.  The distinction is load-bearing: the shell
 * idiom `ZS_TEST_NO_FORK= cmd` sets the variable to the empty string rather than
 * unsetting it, so a plain non-NULL test reads that as "disabled" -- the
 * opposite of what it looks like at the call site.  tests/mutate.sh used exactly
 * that idiom, and every fork test was silently skipped under mutation testing
 * until this was found by a lock mutant that no non-forking test could catch. */
#define SKIP_IF_NO_FORK() do { \
    const char *nf_ = getenv("ZS_TEST_NO_FORK"); \
    if (nf_ && *nf_) SKIP("fork tests disabled (ZS_TEST_NO_FORK)"); \
} while (0)

/* Path and command formatting, for the sites where the compiler cannot see the
 * bound.  A dbdir under $TMPDIR plus a 63-character zeroskip filename fits
 * PATH_MAX with room to spare, but the argument's DECLARED size is PATH_MAX (or
 * NAME_MAX for a `d_name`), so GCC's -Wformat-truncation flags every such join.
 * The library holds itself to a clean build because Cyrus compiles it -Werror;
 * the tests are held to the same standard, and clang does not have the warning
 * at all, so it is only ever seen on the other platform.
 *
 * USING the return value is what silences it -- level 1 deliberately spares
 * calls whose result is checked, which is the same trick zsi_ptrtable_sweep
 * uses inline.  Checking it is also better than not: a truncated path would
 * otherwise surface as a puzzling ENOENT several lines later. */
#define XSNPRINTFN(buf, len, ...) do { \
    int xn_ = snprintf((buf), (len), __VA_ARGS__); \
    if (xn_ < 0 || (size_t)xn_ >= (size_t)(len)) { \
        fprintf(stderr, "\n    FAIL %s:%d: formatted output truncated\n", \
                __FILE__, __LINE__); \
        abort(); \
    } \
} while (0)

#define XSNPRINTF(buf, ...) XSNPRINTFN((buf), sizeof(buf), __VA_ARGS__)

/*
 * ============================================================
 * Per-test scratch directory
 * ============================================================
 */

/* basedir exists; dbdir does NOT -- most tests want ZS_CREATE to make it. */
static char *basedir = NULL;
static char *dbdir = NULL;

static int setup(void)
{
    char path[PATH_MAX];
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    snprintf(path, sizeof(path), "%s/zeroskip-test.%d", tmpdir, (int)getpid());
    if (mkdir(path, 0700) && errno != EEXIST) {
        perror(path);
        return -1;
    }

    basedir = strdup(path);
    assert(basedir);

    dbdir = malloc(PATH_MAX);
    assert(dbdir);
    snprintf(dbdir, PATH_MAX, "%s/db", basedir);

    return 0;
}

static int teardown(void)
{
    if (basedir) {
        char cmd[PATH_MAX + 20];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", basedir);
        int r = system(cmd);
        (void)r;
    }

    free(basedir); basedir = NULL;
    free(dbdir); dbdir = NULL;

    return 0;
}

/*
 * ============================================================
 * Tests
 * ============================================================
 */

static void test_strerror(void)
{
    /* Every code has a string, and distinct codes have distinct strings -- so a
     * missing switch arm shows up here rather than as "unknown error" in a log
     * six months from now. */
    static const int codes[] = {
        ZS_OK, ZS_DONE, ZS_EXISTS, ZS_IOERROR, ZS_INTERNAL, ZS_LOCKED,
        ZS_NOTFOUND, ZS_READONLY, ZS_BADFORMAT, ZS_BADUSAGE, ZS_BADCHECKSUM,
        ZS_FULL, ZS_AGAIN
    };
    size_t n = sizeof(codes) / sizeof(codes[0]);

    for (size_t i = 0; i < n; i++) {
        const char *s = zs_strerror(codes[i]);
        ASSERT_NOT_NULL(s);
        ASSERT(strcmp(s, "unknown error") != 0);
        for (size_t j = i + 1; j < n; j++)
            ASSERT(strcmp(s, zs_strerror(codes[j])) != 0);
    }

    ASSERT_STR_EQ(zs_strerror(12345), "unknown error");
}

/*
 * T-2c Interoperability constants.
 *
 * The values two implementations must agree on bit for bit, each asserted
 * against a literal rather than against the implementation's own computation.
 * That distinction is the whole point: a test that compares the code to itself
 * cannot fail, and these are precisely the values where a silent disagreement
 * becomes a database one side cannot read.
 *
 * The checksum literals were generated once against the vendored xxhash.h and
 * are never to be regenerated to resolve a mismatch.  If one of them fails, the
 * implementation moved, and that is the bug.
 */
static void test_interop_constants_csum(void)
{
    /* XXH3_64bits with seed 0, low 32 bits, little-endian (F-5b). */
    ASSERT_EQU(zsi_csum_xxhash("", 0),      0x38D394C2u);
    ASSERT_EQU(zsi_csum_xxhash("a", 1),     0x1E964E1Fu);
    ASSERT_EQU(zsi_csum_xxhash("abc", 3),   0x892F3950u);

    static const unsigned char magic[16] = {
        0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
        0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00
    };
    ASSERT_EQU(zsi_csum_xxhash((const char *)magic, 16), 0x0464B4F6u);

    char big[1024];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char)(i & 0xFF);
    ASSERT_EQU(zsi_csum_xxhash(big, sizeof(big)), 0x84398D22u);

    /* The empty input F-26g needs, called out separately because it is the one
     * an implementer is most likely to short-circuit to zero -- twom's engine
     * does exactly that.  A zero-record in-order file stores this value for its
     * records region, so getting it wrong makes every empty file fail its own
     * consistency check. */
    ASSERT(zsi_csum_xxhash("", 0) != 0);

    /* Engine 0 writes zeros and never verifies (F-5). */
    ASSERT_EQU(zsi_csum_none("abc", 3), 0u);
    ASSERT_EQU(zsi_csum_none("", 0), 0u);

    /* zsi_csum2 checksums two regions as though concatenated (F-19).  Assert it
     * agrees with the one-shot form rather than trusting that XXH3's streaming
     * API does, since the whole span-terminator checksum rests on it. */
    ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_XXHASH, "ab", 2, "c", 1), 0x892F3950u);
    ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_XXHASH, "abc", 3, "", 0), 0x892F3950u);
    ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_XXHASH, "", 0, "abc", 3), 0x892F3950u);
    ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_XXHASH, "", 0, "", 0),    0x38D394C2u);
    ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_NONE, "ab", 2, "c", 1),   0u);

    /* Split a longer buffer at every offset; every split must agree. */
    for (size_t split = 0; split <= sizeof(big); split++) {
        ASSERT_EQU(zsi_csum2(NULL, ZSI_CSUM_XXHASH,
                             big, split,
                             big + split, sizeof(big) - split),
                   0x84398D22u);
    }

    /* Engine ids and the flag mapping (F-5, A-6). */
    ASSERT(zsi_csum_for_id(ZSI_CSUM_NONE, NULL) == zsi_csum_none);
    ASSERT(zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL) == zsi_csum_xxhash);
    ASSERT(zsi_csum_for_id(ZSI_CSUM_EXTERNAL, zsi_csum_none) == zsi_csum_none);
    ASSERT(zsi_csum_for_id(ZSI_CSUM_EXTERNAL, NULL) == NULL);
    ASSERT(zsi_csum_for_id(3, NULL) == NULL);
    ASSERT(zsi_csum_for_id(15, NULL) == NULL);

    ASSERT_EQ(zsi_csum_id_for_flags(0), ZSI_CSUM_XXHASH);
    ASSERT_EQ(zsi_csum_id_for_flags(ZS_CSUM_XXHASH), ZSI_CSUM_XXHASH);
    ASSERT_EQ(zsi_csum_id_for_flags(ZS_CSUM_NONE), ZSI_CSUM_NONE);
    ASSERT_EQ(zsi_csum_id_for_flags(ZS_CSUM_EXTERNAL), ZSI_CSUM_EXTERNAL);
    ASSERT_EQ(zsi_csum_id_for_flags(ZS_CREATE), ZSI_CSUM_XXHASH);
}

static void test_interop_constants_compar(void)
{
    /* F-11a's total order, over the cases that distinguish a correct
     * implementation from memcmp and from a signed-char compare. */
    struct {
        const char *a; size_t alen;
        const char *b; size_t blen;
        int sign;
    } cases[] = {
        /* a key and its own prefix: the shorter sorts first */
        { "ab", 2, "abc", 3, -1 },
        { "abc", 3, "ab", 2,  1 },

        /* keys differing only above 0x7F.  A signed-char compare gets this
         * backwards, and it is invisible to every ASCII test. */
        { "\x7f", 1, "\x80", 1, -1 },
        { "\x80", 1, "\x7f", 1,  1 },
        { "\x00", 1, "\xff", 1, -1 },
        { "\xfe", 1, "\xff", 1, -1 },
        { "a\x80", 2, "a\x7f", 2, 1 },

        /* the empty-versus-one-byte case */
        { "", 0, "a", 1, -1 },
        { "a", 1, "", 0,  1 },

        /* equal keys */
        { "", 0, "", 0, 0 },
        { "abc", 3, "abc", 3, 0 },

        /* embedded NULs are ordinary bytes: lengths are authoritative (F-13) */
        { "a\0b", 3, "a\0c", 3, -1 },
        { "a\0", 2, "a\0\0", 3, -1 },
        { "a\0b", 3, "a\0b", 3, 0 }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int r = zsi_compar_default(cases[i].a, cases[i].alen,
                                   cases[i].b, cases[i].blen);
        int rs = (r > 0) - (r < 0);
        if (rs != cases[i].sign) {
            fprintf(stderr, "\n    FAIL case %zu: got sign %d, expected %d\n",
                    i, rs, cases[i].sign);
            current_test_failed = 1;
            return;
        }
    }

    /* The order is a strict total order: antisymmetric and transitive over a
     * set that includes every boundary above. */
    static const char *keys[] = { "", "\x00", "a", "a\x00", "ab", "abc",
                                  "b", "\x7f", "\x80", "\xff" };
    static const size_t lens[] = { 0, 1, 1, 2, 2, 3, 1, 1, 1, 1 };
    size_t n = sizeof(lens) / sizeof(lens[0]);

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            int ij = zsi_compar_default(keys[i], lens[i], keys[j], lens[j]);
            int ji = zsi_compar_default(keys[j], lens[j], keys[i], lens[i]);
            ASSERT(((ij > 0) - (ij < 0)) == -((ji > 0) - (ji < 0)));
            if (i == j) ASSERT_EQ(ij, 0);
        }
    }

    /* F-11b: a comparator name is 1..16 bytes, and an empty name is invalid. */
    ASSERT(zsi_compar_name_valid("memcmp"));
    ASSERT(zsi_compar_name_valid("a"));
    ASSERT(zsi_compar_name_valid("0123456789abcdef"));       /* exactly 16 */
    ASSERT(!zsi_compar_name_valid("0123456789abcdefg"));      /* 17 */
    ASSERT(!zsi_compar_name_valid(""));
    ASSERT(!zsi_compar_name_valid(NULL));
}

static void test_interop_constants_uuid(void)
{
    /* D-0: the 36-character lowercase hyphenated RFC 4122 form. */
    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    char str[ZSI_UUID_STR_LEN];

    zsi_uuid_unparse(u, str);
    ASSERT_STR_EQ(str, "4941da54-9406-4faa-a457-c4b65beae3eb");

    zsi_uuid_t back;
    ASSERT_EQ(zsi_uuid_parse(str, back), 0);
    ASSERT_MEM_EQ(back, u, 16);

    /* Strictly the canonical form and nothing else.  A lenient parser lets two
     * implementations disagree about which files belong to a database. */
    ASSERT(zsi_uuid_parse("4941DA54-9406-4FAA-A457-C4B65BEAE3EB", back) != 0);
    ASSERT(zsi_uuid_parse("4941da5494064faaa457c4b65beae3eb", back) != 0);
    ASSERT(zsi_uuid_parse("4941da54-9406-4faa-a457-c4b65beae3e", back) != 0);
    ASSERT(zsi_uuid_parse("4941da54_9406_4faa_a457_c4b65beae3eb", back) != 0);
    ASSERT(zsi_uuid_parse("4941da54-9406-4faa-a457-c4b65beae3eg", back) != 0);
    ASSERT(zsi_uuid_parse("{4941da54-9406-4faa-a457-c4b65beae3eb}", back) != 0);
    ASSERT(zsi_uuid_parse("-941da54-9406-4faa-a457-c4b65beae3eb", back) != 0);

    /* All-zero and all-ones round-trip, since neither is special-cased. */
    static const zsi_uuid_t zero = { 0 };
    zsi_uuid_unparse(zero, str);
    ASSERT_STR_EQ(str, "00000000-0000-0000-0000-000000000000");
    ASSERT_EQ(zsi_uuid_parse(str, back), 0);
    ASSERT_MEM_EQ(back, zero, 16);

    /* A generated UUID is version 4, RFC 4122 variant, and round-trips. */
    for (int i = 0; i < 32; i++) {
        zsi_uuid_t g;
        zsi_uuid_generate(g);
        ASSERT_EQ(g[6] & 0xf0, 0x40);
        ASSERT_EQ(g[8] & 0xc0, 0x80);
        zsi_uuid_unparse(g, str);
        ASSERT_EQ(zsi_uuid_parse(str, back), 0);
        ASSERT_MEM_EQ(back, g, 16);
    }
}

/*
 * G-0a / G-0b: the accessors and guards every later section relies on.
 */
static void test_le_accessors(void)
{
    /* Little-endian, and independent of host byte order (F-1). */
    static const unsigned char buf[8] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08
    };
    const char *p = (const char *)buf;

    ASSERT_EQU(zsi_get16(p), 0x0201u);
    ASSERT_EQU(zsi_get24(p), 0x030201u);
    ASSERT_EQU(zsi_get32(p), 0x04030201u);
    ASSERT_EQU(zsi_get64(p), 0x0807060504030201ull);

    /* Round-trip at every alignment within a buffer, so an accessor that
     * assumed alignment fails here rather than on a stricter platform. */
    char out[24];
    for (size_t off = 0; off < 8; off++) {
        memset(out, 0xAA, sizeof(out));
        zsi_put16(out + off, 0xBEEF);
        ASSERT_EQU(zsi_get16(out + off), 0xBEEFu);

        zsi_put24(out + off, 0xABCDEF);
        ASSERT_EQU(zsi_get24(out + off), 0xABCDEFu);

        zsi_put32(out + off, 0xDEADBEEF);
        ASSERT_EQU(zsi_get32(out + off), 0xDEADBEEFu);

        zsi_put64(out + off, 0x0123456789ABCDEFull);
        ASSERT_EQU(zsi_get64(out + off), 0x0123456789ABCDEFull);
    }

    /* Extremes, and that a put writes exactly its own width and no more. */
    memset(out, 0x5A, sizeof(out));
    zsi_put32(out + 4, 0xFFFFFFFF);
    ASSERT_EQU(zsi_get32(out + 4), 0xFFFFFFFFu);
    ASSERT_EQ((unsigned char)out[3], 0x5A);
    ASSERT_EQ((unsigned char)out[8], 0x5A);

    memset(out, 0x5A, sizeof(out));
    zsi_put24(out + 4, 0xFFFFFF);
    ASSERT_EQU(zsi_get24(out + 4), 0xFFFFFFu);
    ASSERT_EQ((unsigned char)out[3], 0x5A);
    ASSERT_EQ((unsigned char)out[7], 0x5A);
}

static void test_overflow_guards(void)
{
    size_t out;

    /* roundup8 saturates rather than wrapping (F-2, G-0b).  A wrapping version
     * returns a small number for a huge length, which turns a bounds check into
     * a bounds-check bypass. */
    ASSERT_EQU(zsi_roundup8(0), 0u);
    ASSERT_EQU(zsi_roundup8(1), 8u);
    ASSERT_EQU(zsi_roundup8(7), 8u);
    ASSERT_EQU(zsi_roundup8(8), 8u);
    ASSERT_EQU(zsi_roundup8(9), 16u);
    ASSERT_EQU(zsi_roundup8(SIZE_MAX), 0u);
    ASSERT_EQU(zsi_roundup8(SIZE_MAX - 6), 0u);

    /* SIZE_MAX - 7 is already a multiple of 8, so it is the largest input that
     * rounds without overflowing, and it must round to itself.  The boundary is
     * asserted from both sides: one below saturates, this one does not. */
    ASSERT_EQU(zsi_roundup8(SIZE_MAX - 7), SIZE_MAX - 7);

    /* Addition reports overflow and leaves the output untouched, so a caller
     * that ignores the return value still cannot read a plausible-looking
     * length out of it. */
    out = 0xDEAD;
    ASSERT(!zsi_add_sz(SIZE_MAX, 1, &out));
    ASSERT_EQU(out, 0xDEADu);

    out = 0xDEAD;
    ASSERT(!zsi_add_sz(SIZE_MAX - 3, 4, &out));
    ASSERT_EQU(out, 0xDEADu);

    ASSERT(zsi_add_sz(SIZE_MAX - 4, 4, &out));
    ASSERT_EQU(out, SIZE_MAX);
    ASSERT(zsi_add_sz(0, 0, &out));
    ASSERT_EQU(out, 0u);
    ASSERT(zsi_add_sz(100, 200, &out));
    ASSERT_EQU(out, 300u);

    /* The three-term form is the shape lengths actually arrive in:
     * keylen + vallen + 2 (F-13's two NUL terminators). */
    out = 0xDEAD;
    ASSERT(!zsi_add3_sz(SIZE_MAX, 1, 1, &out));
    ASSERT_EQU(out, 0xDEADu);

    out = 0xDEAD;
    ASSERT(!zsi_add3_sz(SIZE_MAX - 1, 1, 1, &out));
    ASSERT_EQU(out, 0xDEADu);

    ASSERT(zsi_add3_sz(4, 8, 2, &out));
    ASSERT_EQU(out, 14u);

    /* The composite a caller would write inline: a corrupt keylen must not
     * produce a small total. */
    ASSERT(!zsi_add3_sz(SIZE_MAX - 100, 200, 2, &out));
}

/* The engine a test uses to stand in for a caller-supplied one (engine 2).
 * zsi_csum_none is a legitimate choice: engine 2 is whatever the caller supplies,
 * and the point of these tests is which function gets selected, not what it
 * computes. */
#define TEST_EXTERNAL_CSUM zsi_csum_none

/* Silence the error callback, and count how many reports it received. */
static int report_count = 0;
static void counting_error(const char *msg, const char *fmt, ...)
{
    (void)msg; (void)fmt;
    report_count++;
}

static struct zs_db *open_db_reporting(uint32_t flags)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    setup.flags = flags;
    setup.csum = TEST_EXTERNAL_CSUM;
    setup.error = counting_error;
    report_count = 0;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/*
 * ============================================================
 * Building files by hand
 * ============================================================
 *
 * Much of the conformance suite needs files this implementation would never
 * write: a truncated pointer section, a back pointer that is not 8-aligned, a
 * PTRS64 section without 4GB of data behind it.  These helpers write bytes, not
 * databases, so a test can construct exactly the damage it wants to assert about.
 */

static int fexists(const char *fname)
{
    struct stat sb;
    int r = stat(fname, &sb);
    if (r < 0) r = -errno;
    return r;
}

/* Create dbdir if absent.  Most tests want ZS_CREATE to make it, so setup()
 * deliberately does not. */
static int mkdbdir(void)
{
    if (mkdir(dbdir, 0700) && errno != EEXIST) return -1;
    return 0;
}

static char *dbpath(const char *name)
{
    static char path[PATH_MAX];
    XSNPRINTF(path, "%s/%s", dbdir, name);
    return path;
}

/* Write buf to dbdir/name, replacing anything there. */
static int writefile(const char *name, const void *buf, size_t len)
{
    char *path = dbpath(name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    ssize_t n = len ? write(fd, buf, len) : 0;
    close(fd);
    return (n == (ssize_t)len) ? 0 : -1;
}

static long filesize(const char *name)
{
    struct stat sb;
    if (stat(dbpath(name), &sb) < 0) return -1;
    return (long)sb.st_size;
}

/*
 * ============================================================
 * Filenames (T-2c, part of T-9)
 * ============================================================
 */

static const zsi_uuid_t test_uuid = {
    0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
    0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
};
#define TEST_UUID_STR "4941da54-9406-4faa-a457-c4b65beae3eb"

static void test_filenames(void)
{
    char name[ZSI_NAME_MAX];
    zsi_uuid_t u;
    uint32_t s, e;

    /* T-2c: a generated filename for a known UUID and generation range,
     * character for character.  Lowercase UUID, uppercase 8-digit hex
     * generations, no extension. */
    zsi_name_format(name, test_uuid, 1, 10);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-00000001-0000000A");

    zsi_name_format(name, test_uuid, 5, 5);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-00000005-00000005");

    /* The full 32-bit range has a name, which is what 8 digits buys (D-1). */
    zsi_name_format(name, test_uuid, 0xABCDEF01u, 0xFEDCBA98u);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-ABCDEF01-FEDCBA98");

    /* D-1b: the active file's name carries no generation at all, and there is
     * exactly one of it.  A DOT, so that "zeroskip-<uuid>-*" matches the
     * generation-named files and only those. */
    zsi_name_current(name, test_uuid);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR ".current");

    /* Round-trip, both kinds.  The active file parses with start == 0: its
     * generation is in the header, and F-9 makes 0 unambiguous as "unknown". */
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(zsi_name_parse(name, u, &s, &e), ZSI_NAME_UNORDERED);
    ASSERT_MEM_EQ(u, test_uuid, 16);
    ASSERT_EQU(s, 0u);
    ASSERT_EQU(e, 0u);

    /* The OLD active-file spelling -- a bare generation -- is not a name this
     * format produces, and F-7a forbids reading it as a fallback. */
    ASSERT_EQ(zsi_name_parse("zeroskip-" TEST_UUID_STR "-00000005", u, &s, &e),
              ZSI_NAME_OTHER);

    zsi_name_format(name, test_uuid, 3, 9);
    ASSERT_EQ(zsi_name_parse(name, u, &s, &e), ZSI_NAME_INORDER);
    ASSERT_MEM_EQ(u, test_uuid, 16);
    ASSERT_EQU(s, 3u);
    ASSERT_EQU(e, 9u);

    zsi_name_format(name, test_uuid, 0xABCDEF01u, 0xFEDCBA98u);
    ASSERT_EQ(zsi_name_parse(name, u, &s, &e), ZSI_NAME_INORDER);
    ASSERT_EQU(s, 0xABCDEF01u);
    ASSERT_EQU(e, 0xFEDCBA98u);
}

static void test_filename_rejections(void)
{
    zsi_uuid_t u;
    uint32_t s, e;

    /* Every one of these must be ignored rather than half-accepted (D-4). */
    static const char *bad[] = {
        /* lowercase hex generations: D-1 says uppercase */
        "zeroskip-" TEST_UUID_STR "-0000000a",
        "zeroskip-" TEST_UUID_STR "-0000000a-0000000b",
        /* wrong digit count */
        "zeroskip-" TEST_UUID_STR "-0000001",
        "zeroskip-" TEST_UUID_STR "-000000001",
        "zeroskip-" TEST_UUID_STR "-1",
        "zeroskip-" TEST_UUID_STR "-00000001-000000",
        /* an extension: only ".current" is a legal suffix (D-1a, D-1b) */
        "zeroskip-" TEST_UUID_STR "-00000001.zs",
        "zeroskip-" TEST_UUID_STR "-00000001-00000004.zs",
        "zeroskip-" TEST_UUID_STR "-00000001.tmp",
        /* the OLD active-file spelling: a bare generation.  D-1b replaced it
         * and F-7a forbids reading it as a fallback, so it is not ours. */
        "zeroskip-" TEST_UUID_STR "-00000001",
        "zeroskip-" TEST_UUID_STR "-0000000A",
        "zeroskip-" TEST_UUID_STR "-FFFFFFFF",
        /* near misses on the active file's name (D-1b) */
        "zeroskip-" TEST_UUID_STR "-current",
        "zeroskip-" TEST_UUID_STR ".CURRENT",
        "zeroskip-" TEST_UUID_STR ".current2",
        "zeroskip-" TEST_UUID_STR ".curren",
        "zeroskip-" TEST_UUID_STR "..current",
        "zeroskip-" TEST_UUID_STR ".current-00000001",
        /* trailing junk */
        "zeroskip-" TEST_UUID_STR "-00000001-",
        "zeroskip-" TEST_UUID_STR "-00000001-00000004-",
        "zeroskip-" TEST_UUID_STR "-00000001-00000004-00000009",
        /* generation 0 is never legitimate (F-9) */
        "zeroskip-" TEST_UUID_STR "-00000000",
        "zeroskip-" TEST_UUID_STR "-00000000-00000004",
        /* end == 0 in the in-order form would collide with unordered */
        "zeroskip-" TEST_UUID_STR "-00000001-00000000",
        /* a range running backwards */
        "zeroskip-" TEST_UUID_STR "-00000009-00000003",
        /* malformed or uppercase UUID */
        "zeroskip-4941DA54-9406-4FAA-A457-C4B65BEAE3EB-00000001",
        "zeroskip-4941da54-9406-4faa-a457-c4b65beae3e-00000001",
        "zeroskip-4941da5494064faaa457c4b65beae3eb-00000001",
        /* missing separator */
        "zeroskip-" TEST_UUID_STR "00000001",
        "zeroskip-" TEST_UUID_STR,
        /* metadata, not data (D-2) */
        "zeroskip.lock",
        "zeroskip.tmp.1234.0",
        "zeroskip.tmp.1234.17",
        /* no separator at all: the prefix is "zeroskip-", not "zeroskip" */
        "zeroskip" TEST_UUID_STR ".current",
        "zeroskip" TEST_UUID_STR "-00000001-00000001",
        /* not ours at all */
        "zeroskip",
        "zeroskip-",
        "",
        "README",
        ".",
        "..",
        "zeroskip_" TEST_UUID_STR "-00000001",
        "Zeroskip-" TEST_UUID_STR "-00000001",
        /* 0x prefixes and signs */
        "zeroskip-" TEST_UUID_STR "-0x000001",
        "zeroskip-" TEST_UUID_STR "-+0000001"
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (zsi_name_parse(bad[i], u, &s, &e) != ZSI_NAME_OTHER) {
            fprintf(stderr, "\n    FAIL accepted '%s'\n", bad[i]);
            current_test_failed = 1;
            return;
        }
    }

    /* Another database's UUID parses fine as a name -- rejecting by UUID is the
     * file set's job (D-4), not the parser's.  Asserted so the division of
     * responsibility is explicit. */
    ASSERT_EQ(zsi_name_parse("zeroskip-00000000-0000-4000-8000-000000000000"
                             "-00000001-00000001", u, &s, &e), ZSI_NAME_INORDER);
    ASSERT_EQ(zsi_name_parse("zeroskip-00000000-0000-4000-8000-000000000000"
                             ".current", u, &s, &e), ZSI_NAME_UNORDERED);
}

static void test_filename_sort_property(void)
{
    /* D-1b, asserted directly on generated names.
     *
     * D-5 resolves an overlap by taking the LAST file whose start matches, so
     * where the active file's name sorts is load-bearing.  It has to sort AFTER
     * every generation name, because its generation is above them all.  That
     * holds because '.' (0x2E) is above '-' (0x2D) at the position where the
     * names diverge.
     *
     * T-9 requires this be a test, so that changing the separator breaks a test
     * rather than the database. */
    char cur[ZSI_NAME_MAX];
    zsi_name_current(cur, test_uuid);

    for (uint32_t g = 1; g <= 300; g++) {
        char in[ZSI_NAME_MAX];
        zsi_name_format(in, test_uuid, g, g);

        if (strcmp(cur, in) <= 0) {
            fprintf(stderr, "\n    FAIL gen %u: '%s' does not sort after '%s'\n",
                    g, cur, in);
            current_test_failed = 1;
            return;
        }
    }

    /* Including the very top of the range, which is the case a separator
     * sorting below the hex digits would get wrong. */
    char top[ZSI_NAME_MAX];
    zsi_name_format(top, test_uuid, 0xFFFFFFFFu, 0xFFFFFFFFu);
    ASSERT(strcmp(cur, top) > 0);

    /* D-5a's table: N-N and a wider N-M sort in that order, so "last" is the
     * widest -- and the active file is last of all. */
    char b[ZSI_NAME_MAX], c[ZSI_NAME_MAX];
    zsi_name_format(b, test_uuid, 5, 5);
    zsi_name_format(c, test_uuid, 5, 9);
    ASSERT(strcmp(b, c) < 0);
    ASSERT(strcmp(c, cur) < 0);

    /* And the point of the dot: the generation-named files are exactly what
     * "zeroskip-<uuid>-" prefixes, with nothing to grep back out. */
    char pfx[ZSI_NAME_MAX];
    snprintf(pfx, sizeof(pfx), "zeroskip-%s-", TEST_UUID_STR);
    ASSERT(strncmp(b, pfx, strlen(pfx)) == 0);
    ASSERT(strncmp(cur, pfx, strlen(pfx)) != 0);
}

static void test_staging_names(void)
{
    char a[ZSI_NAME_MAX], b[ZSI_NAME_MAX];
    zsi_uuid_t u;
    uint32_t s, e;

    zsi_staging_name(a, 0);
    zsi_staging_name(b, 1);
    ASSERT(strcmp(a, b) != 0);

    /* Staging names begin "zeroskip." and so can never match the data-file
     * pattern (D-2).  A crashed repack leaves one behind, and it must be ignored
     * rather than mistaken for a generation (R-5). */
    ASSERT_EQ(zsi_name_parse(a, u, &s, &e), ZSI_NAME_OTHER);
    ASSERT_EQ(zsi_name_parse(b, u, &s, &e), ZSI_NAME_OTHER);
    ASSERT_MEM_EQ(a, ZSI_STAGING_PREFIX, strlen(ZSI_STAGING_PREFIX));

    /* The lock file likewise (D-3). */
    ASSERT_EQ(zsi_name_parse(ZSI_LOCK_NAME, u, &s, &e), ZSI_NAME_OTHER);

    /* Long enough not to truncate at a large pid and counter. */
    zsi_staging_name(a, 4294967295u);
    ASSERT(strlen(a) < ZSI_NAME_MAX - 1);
}

/*
 * ============================================================
 * Header and magic (T-2)
 * ============================================================
 */

/* A minimal UTF-8 validator, so F-6a's property is asserted rather than merely
 * believed.  Deliberately written here rather than borrowed from the library:
 * the point is to check the magic against an independent notion of UTF-8. */
static bool valid_utf8(const unsigned char *p, size_t len)
{
    size_t i = 0;
    while (i < len) {
        unsigned char c = p[i];
        size_t need;
        if (c < 0x80) { i++; continue; }
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else return false;              /* continuation byte or 0xF8+ leads */
        if (i + need >= len + 0 && i + need > len - 1) return false;
        for (size_t k = 1; k <= need; k++)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += need + 1;
    }
    return true;
}

/* Build a valid header into buf, for a test to then damage.
 *
 * Resolves engine 2 to TEST_EXTERNAL_CSUM rather than passing NULL through.  An
 * earlier version passed zsi_csum_for_id(engine, NULL), which is NULL for engine
 * 2, and then called it -- crashing the suite inside a helper rather than
 * failing an assertion. */
static void make_header(char *buf, uint32_t start, uint32_t end, unsigned engine)
{
    struct zsi_header h;
    memset(&h, 0, sizeof(h));
    h.version_read  = ZSI_VERSION_READ;
    h.version_write = ZSI_VERSION_WRITE;
    h.flags         = (uint16_t)engine;
    memcpy(h.uuid, test_uuid, 16);
    h.start = start;
    h.end   = end;
    memcpy(h.compar_name, "memcmp", 6);

    zs_csum *cs = zsi_csum_for_id(engine, TEST_EXTERNAL_CSUM);
    if (!cs) cs = zsi_csum_none;        /* an unknown engine: write zeros */
    zsi_header_encode(buf, &h, cs);
}

static void test_magic(void)
{
    char buf[ZSI_HEADER_LEN];

    /* The 16 bytes, as literals.  Any change here is a format change. */
    static const unsigned char expect[16] = {
        0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
        0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00
    };
    ASSERT_MEM_EQ(zsi_magic, expect, 16);

    /* F-6a: not valid UTF-8, and it is byte 0 alone that carries the property --
     * every byte after it is ASCII.  Asserted both ways so a future change to
     * byte 0 that kept the file "text-ish" fails here. */
    ASSERT(!valid_utf8(zsi_magic, 16));
    ASSERT(valid_utf8(zsi_magic + 1, 15));
    ASSERT(zsi_magic[0] >= 0x80 && zsi_magic[0] <= 0xBF);

    /* Sanity-check the validator itself, or the assertion above proves nothing. */
    ASSERT(valid_utf8((const unsigned char *)"hello", 5));
    ASSERT(valid_utf8((const unsigned char *)"\xC3\xA9", 2));
    ASSERT(!valid_utf8((const unsigned char *)"\xC3", 1));
    ASSERT(!valid_utf8((const unsigned char *)"\x80", 1));

    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    struct zsi_header h;
    ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h));

    /* F-6: all 16 bytes are validated, not a prefix.  Every single-byte mutation
     * at every one of the 16 positions is rejected. */
    for (size_t pos = 0; pos < ZSI_MAGIC_LEN; pos++) {
        for (unsigned v = 0; v < 256; v++) {
            if ((unsigned char)v == zsi_magic[pos]) continue;
            char t[ZSI_HEADER_LEN];
            make_header(t, 1, 0, ZSI_CSUM_XXHASH);
            t[pos] = (char)v;
            /* Recompute the checksum, so this tests the magic check rather than
             * incidentally tripping the checksum.  Without this the test would
             * pass even if the magic were never examined. */
            zsi_put32(t + ZSI_HDR_OFF_CSUM, zsi_csum_xxhash(t, ZSI_HDR_OFF_CSUM));
            if (zsi_header_decode(t, sizeof(t), zsi_csum_xxhash, &h) == ZS_OK) {
                fprintf(stderr, "\n    FAIL magic byte %zu = 0x%02X accepted\n",
                        pos, v);
                current_test_failed = 1;
                return;
            }
        }
    }
}

static void test_magic_designed_corruptions(void)
{
    /* The specific corruptions the magic is designed to catch (T-2).  Each is
     * built as the 16 bytes a real transfer would produce, then given a valid
     * checksum so only the magic check can reject it. */
    struct zsi_header h;
    char buf[ZSI_HEADER_LEN];

    struct { const char *what; unsigned char m[16]; } cases[] = {
        /* eighth bit stripped from byte 0 */
        { "8th bit stripped",
          { 0x09, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
            0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00 } },
        /* 0D 0A collapsed to 0A: the tail shifts left and a byte enters at the end */
        { "CRLF -> LF",
          { 0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
            0x70, 0x31, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00 } },
        /* bare 0A expanded to 0D 0A: the tail shifts right */
        { "LF -> CRLF",
          { 0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
            0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0D, 0x0A, 0x00 } },
        /* byte 0 replaced by the UTF-8 substitution character's encoding */
        { "byte 0 -> EF BF BD",
          { 0xEF, 0xBF, 0xBD, 0x7A, 0x65, 0x72, 0x6F, 0x73,
            0x6B, 0x69, 0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A } }
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
        memcpy(buf, cases[i].m, 16);
        zsi_put32(buf + ZSI_HDR_OFF_CSUM,
                  zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
        if (zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h) == ZS_OK) {
            fprintf(stderr, "\n    FAIL corruption '%s' accepted\n",
                    cases[i].what);
            current_test_failed = 1;
            return;
        }
    }
}

static void test_header_roundtrip(void)
{
    char buf[ZSI_HEADER_LEN];
    struct zsi_header h;

    static const unsigned engines[] = { ZSI_CSUM_NONE, ZSI_CSUM_XXHASH };

    for (size_t e = 0; e < 2; e++) {
        unsigned eng = engines[e];
        zs_csum *cs = zsi_csum_for_id(eng, NULL);

        /* unordered: end == 0 (F-10) */
        make_header(buf, 7, 0, eng);
        ASSERT_OK(zsi_header_decode(buf, sizeof(buf), cs, &h));
        ASSERT_EQ(h.version_read, ZSI_VERSION_READ);
        ASSERT_EQ(h.version_write, ZSI_VERSION_WRITE);
        ASSERT_EQU(h.start, 7u);
        ASSERT_EQU(h.end, 0u);
        ASSERT(zsi_header_is_unordered(&h));
        ASSERT_EQU(h.flags & ZSI_CSUM_MASK, eng);
        ASSERT_EQU(zsi_header_engine_id(buf), eng);
        ASSERT_MEM_EQ(h.compar_name, "memcmp\0\0\0\0\0\0\0\0\0\0", 16);

        /* in-order: a range (F-10) */
        make_header(buf, 3, 9, eng);
        ASSERT_OK(zsi_header_decode(buf, sizeof(buf), cs, &h));
        ASSERT_EQU(h.start, 3u);
        ASSERT_EQU(h.end, 9u);
        ASSERT(!zsi_header_is_unordered(&h));

        /* single-generation in-order, the shape a conversion produces */
        make_header(buf, 5, 5, eng);
        ASSERT_OK(zsi_header_decode(buf, sizeof(buf), cs, &h));
        ASSERT_EQU(h.start, 5u);
        ASSERT_EQU(h.end, 5u);
        ASSERT(!zsi_header_is_unordered(&h));

        /* the extremes of the generation space (D-9c) */
        make_header(buf, 1, 0xFFFFFFFFu, eng);
        ASSERT_OK(zsi_header_decode(buf, sizeof(buf), cs, &h));
        ASSERT_EQU(h.start, 1u);
        ASSERT_EQU(h.end, 0xFFFFFFFFu);
    }

    /* The encoder is deterministic: same input, identical bytes.  Canonical
     * encoding (F-15) is what makes T-12a's byte-for-byte agreement possible. */
    char a[ZSI_HEADER_LEN], b[ZSI_HEADER_LEN];
    memset(a, 0xAA, sizeof(a));
    memset(b, 0x55, sizeof(b));
    make_header(a, 4, 8, ZSI_CSUM_XXHASH);
    make_header(b, 4, 8, ZSI_CSUM_XXHASH);
    ASSERT_MEM_EQ(a, b, ZSI_HEADER_LEN);

    /* The comparator name is NUL-padded to 16, not NUL-terminated at its own
     * length, and the padding is part of the compared bytes (F-11b). */
    ASSERT_MEM_EQ(a + ZSI_HDR_OFF_COMPAR, "memcmp\0\0\0\0\0\0\0\0\0\0", 16);
}

/* The header's byte layout, pinned against literals.
 *
 * Every other header test round-trips through this implementation's own encoder
 * and decoder, so it passes unchanged if the layout table moves as a whole --
 * swapping the start and end offsets, or getting ZSI_HEADER_LEN wrong, is
 * invisible to a symmetric encode/decode pair.  Mutation testing confirmed
 * exactly that.  Only a literal catches it, and a wrong layout is precisely the
 * class of bug that makes another implementation unable to read our files.
 *
 * The golden buffer was generated once and is not to be regenerated to resolve a
 * mismatch: if this fails, the format moved. */
static void test_header_byte_layout(void)
{
    static const unsigned char golden[ZSI_HEADER_LEN] = {
        /* 0  magic, all 16 bytes */
        0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
        0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00,
        /* 16 vread, 17 vwrite, 18 flags (LE), 20 reserved */
        0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 24 uuid */
        0x49, 0x41, 0xDA, 0x54, 0x94, 0x06, 0x4F, 0xAA,
        0xA4, 0x57, 0xC4, 0xB6, 0x5B, 0xEA, 0xE3, 0xEB,
        /* 40 start = 0x01020304 LE, 44 end = 0x05060708 LE */
        0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
        /* 48 comparator name, NUL-padded to 16 */
        0x6D, 0x65, 0x6D, 0x63, 0x6D, 0x70, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 64 reserved, 68 checksum of [0, 68) */
        0x00, 0x00, 0x00, 0x00, 0x8F, 0x2D, 0xF0, 0xE0
    };

    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    struct zsi_header h;
    char buf[ZSI_HEADER_LEN];

    ASSERT_EQ(ZSI_HEADER_LEN, 72);
    ASSERT_EQ(ZSI_MAGIC_LEN, 16);

    /* version 2, the format-flip minimum (F-32): version 1 was never
     * released and zsi_header_decode now refuses it outright, so the golden
     * buffer this test decodes back below must carry a version it accepts. */
    memset(&h, 0, sizeof(h));
    h.version_read  = 2;
    h.version_write = 2;
    h.flags         = 1;
    memcpy(h.uuid, u, 16);
    h.start = 0x01020304;
    h.end   = 0x05060708;
    memcpy(h.compar_name, "memcmp", 6);

    memset(buf, 0xAA, sizeof(buf));
    zsi_header_encode(buf, &h, zsi_csum_xxhash);

    for (size_t i = 0; i < ZSI_HEADER_LEN; i++) {
        if ((unsigned char)buf[i] != golden[i]) {
            fprintf(stderr, "\n    FAIL byte %zu: got 0x%02X, expected 0x%02X\n",
                    i, (unsigned char)buf[i], golden[i]);
            current_test_failed = 1;
            return;
        }
    }

    /* Each field at its literal offset, so a failure names the field rather than
     * just an offset. */
    ASSERT_MEM_EQ(buf + 0, zsi_magic, 16);
    ASSERT_EQ((unsigned char)buf[16], 2);
    ASSERT_EQ((unsigned char)buf[17], 2);
    ASSERT_EQU(zsi_get16(buf + 18), 1u);
    ASSERT_EQU(zsi_get32(buf + 20), 0u);
    ASSERT_MEM_EQ(buf + 24, u, 16);
    ASSERT_EQU(zsi_get32(buf + 40), 0x01020304u);
    ASSERT_EQU(zsi_get32(buf + 44), 0x05060708u);
    ASSERT_MEM_EQ(buf + 48, "memcmp\0\0\0\0\0\0\0\0\0\0", 16);
    ASSERT_EQU(zsi_get32(buf + 64), 0u);
    ASSERT_EQU(zsi_get32(buf + 68), 0xE0F02D8Fu);

    /* And it decodes back to what it came from. */
    ASSERT_OK(zsi_header_decode((const char *)golden, ZSI_HEADER_LEN,
                                zsi_csum_xxhash, &h));
    ASSERT_EQU(h.start, 0x01020304u);
    ASSERT_EQU(h.end, 0x05060708u);

    /* A full 16-byte comparator name, which has no NUL padding at all: the copy
     * must be the field width, not the string length. */
    memset(&h, 0, sizeof(h));
    h.version_read = 2;
    h.version_write = 2;
    h.flags = 1;
    memcpy(h.uuid, u, 16);
    h.start = 1;
    memcpy(h.compar_name, "0123456789abcdef", 16);
    memset(buf, 0xAA, sizeof(buf));
    zsi_header_encode(buf, &h, zsi_csum_xxhash);
    ASSERT_MEM_EQ(buf + 48, "0123456789abcdef", 16);
    ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h));
    ASSERT_MEM_EQ(h.compar_name, "0123456789abcdef", 16);
}

static void test_header_versions(void)
{
    char buf[ZSI_HEADER_LEN];
    struct zsi_header h;

    /* A read version above ours is refused (F-7). */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_VREAD] = (char)(ZSI_VERSION_READ + 1);
    zsi_put32(buf + ZSI_HDR_OFF_CSUM, zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADFORMAT);

    /* A read version below 2 is refused too: version 1 was never released,
     * and its records carry no trailing checksum for F-32 to verify. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_VREAD] = 1;
    zsi_put32(buf + ZSI_HDR_OFF_CSUM, zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADFORMAT);

    /* A write version above ours with a readable read version decodes, and
     * reports the write version so the caller can open it read-only.  This split
     * is the entire reason there are two version fields. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_VWRITE] = (char)(ZSI_VERSION_WRITE + 5);
    zsi_put32(buf + ZSI_HDR_OFF_CSUM, zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
    ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h));
    ASSERT_EQ(h.version_write, ZSI_VERSION_WRITE + 5);
    ASSERT_EQ(h.version_read, ZSI_VERSION_READ);
}

static void test_header_checksum(void)
{
    char buf[ZSI_HEADER_LEN];
    struct zsi_header h;

    /* Under engine 1, flipping any byte in [0, 68) is caught.  Bytes 68..71 are
     * the checksum field itself, which F-4 excludes from its own coverage. */
    for (size_t pos = 0; pos < ZSI_HDR_OFF_CSUM; pos++) {
        make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
        buf[pos] ^= 0x01;
        int r = zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h);
        if (r == ZS_OK) {
            fprintf(stderr, "\n    FAIL flip at %zu not caught\n", pos);
            current_test_failed = 1;
            return;
        }
    }

    /* Corrupting the checksum field itself is also caught. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_CSUM] ^= 0x01;
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADCHECKSUM);

    /* Under engine 0 the field is zero and nothing is verified (F-5c).  A torn
     * header is undetectable, which is the documented cost of that engine.
     *
     * Corrupt the UUID rather than a generation: a damaged generation could be
     * caught by a structural rule (F-9 rejects start == 0) and then this would
     * be asserting the wrong thing.  A UUID is opaque, so no rule can save it --
     * which is precisely the exposure engine 0 accepts. */
    make_header(buf, 1, 0, ZSI_CSUM_NONE);
    ASSERT_EQU(zsi_get32(buf + ZSI_HDR_OFF_CSUM), 0u);
    buf[ZSI_HDR_OFF_UUID + 3] ^= 0x01;
    ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_none, &h));
    ASSERT_EQ(h.uuid[3], 0x54 ^ 0x01);

    /* The same corruption under engine 1 is caught, which is the contrast that
     * makes the assertion above meaningful rather than just a passing call. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_UUID + 3] ^= 0x01;
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADCHECKSUM);
}

static void test_header_reserved(void)
{
    char buf[ZSI_HEADER_LEN];
    struct zsi_header h;

    /* The encoder writes zeros (F-8). */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    ASSERT_EQU(zsi_get32(buf + ZSI_HDR_OFF_RESERVED1), 0u);
    ASSERT_EQU(zsi_get32(buf + ZSI_HDR_OFF_RESERVED2), 0u);

    /* The decoder ignores them rather than rejecting.  Rejecting would make a
     * future extension unreadable by this version, which is what the version
     * fields are for -- compatibility decisions do not live in reserved bytes. */
    for (int which = 0; which < 2; which++) {
        size_t off = which ? ZSI_HDR_OFF_RESERVED2 : ZSI_HDR_OFF_RESERVED1;
        make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
        zsi_put32(buf + off, 0xDEADBEEF);
        zsi_put32(buf + ZSI_HDR_OFF_CSUM,
                  zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
        ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h));
    }
}

static void test_header_bounds_and_ranges(void)
{
    char buf[ZSI_HEADER_LEN];
    struct zsi_header h;

    /* Short buffers are refused at every length below the header. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    for (size_t len = 0; len < ZSI_HEADER_LEN; len++)
        ASSERT_EQ(zsi_header_decode(buf, len, zsi_csum_xxhash, &h),
                  ZS_BADFORMAT);
    ASSERT_OK(zsi_header_decode(buf, ZSI_HEADER_LEN, zsi_csum_xxhash, &h));

    /* F-9: generations start at 1, so start == 0 is never legitimate. */
    make_header(buf, 1, 0, ZSI_CSUM_XXHASH);
    zsi_put32(buf + ZSI_HDR_OFF_START, 0);
    zsi_put32(buf + ZSI_HDR_OFF_CSUM, zsi_csum_xxhash(buf, ZSI_HDR_OFF_CSUM));
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADFORMAT);

    /* An in-order range must not run backwards. */
    make_header(buf, 9, 3, ZSI_CSUM_XXHASH);
    ASSERT_EQ(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h),
              ZS_BADFORMAT);

    /* end == start is fine: that is what a conversion produces. */
    make_header(buf, 9, 9, ZSI_CSUM_XXHASH);
    ASSERT_OK(zsi_header_decode(buf, sizeof(buf), zsi_csum_xxhash, &h));
}

/*
 * ============================================================
 * Records and terminators (T-2b, T-4 encoding boundaries)
 * ============================================================
 */

static void test_type_byte_validity(void)
{
    /* T-2b: all 256 byte values fed as a record type, asserting exactly the
     * ten in F-12's table are accepted and the other 246 rejected. */
    static const uint8_t legal[] = {
        0x01, 0x03, 0x05, 0x07,
        0x10, 0x12, 0x14, 0x16, 0x20, 0x24
    };
    bool is_legal[256];
    memset(is_legal, 0, sizeof(is_legal));
    for (size_t i = 0; i < sizeof(legal); i++) is_legal[legal[i]] = true;

    int naccepted = 0;
    for (unsigned v = 0; v < 256; v++) {
        bool got = zsi_type_valid((uint8_t)v);
        if (got) naccepted++;
        if (got != is_legal[v]) {
            fprintf(stderr, "\n    FAIL type 0x%02X: %s, expected %s\n", v,
                    got ? "accepted" : "rejected",
                    is_legal[v] ? "accepted" : "rejected");
            current_test_failed = 1;
            return;
        }
    }
    ASSERT_EQ(naccepted, 10);

    /* A bitfield admits far more values than it defines, so the near-misses
     * matter most: each of these is a plausible single flipped bit away from a
     * valid type, and each must be rejected rather than half-interpreted. */

    /* two family bits set at once */
    ASSERT(!zsi_type_valid(ZSI_HASKEY | ZSI_SPANTERM));       /* 0x11 */
    ASSERT(!zsi_type_valid(ZSI_HASKEY | ZSI_POINTERS));       /* 0x21 */
    ASSERT(!zsi_type_valid(ZSI_SPANTERM | ZSI_POINTERS));     /* 0x30 */
    ASSERT(!zsi_type_valid(0x31));

    /* The reserved 0x08, alone and on top of each shape it can combine with.
     * F-12c reserves the bit rather than reusing it, so a peer writing any of
     * these is rejected record by record rather than silently misread. */
    ASSERT(!zsi_type_valid(0x08));
    ASSERT(!zsi_type_valid(0x09));                            /* KEYVALUE_ANC */
    ASSERT(!zsi_type_valid(0x0B));                            /* DELETION_ANC */
    ASSERT(!zsi_type_valid(0x0D));                         /* BIGKEYVALUE_ANC */
    ASSERT(!zsi_type_valid(0x0F));                         /* BIGDELETION_ANC */
    ASSERT(!zsi_type_valid(0x18));
    ASSERT(!zsi_type_valid(0x28));

    /* IsDelete with Pointers */
    ASSERT(!zsi_type_valid(ZSI_POINTERS | ZSI_ISDELETE));     /* 0x22 */
    ASSERT(!zsi_type_valid(0x26));

    /* either reserved bit set, on an otherwise valid type */
    for (size_t i = 0; i < sizeof(legal); i++) {
        ASSERT(!zsi_type_valid((uint8_t)(legal[i] | 0x40)));
        ASSERT(!zsi_type_valid((uint8_t)(legal[i] | 0x80)));
    }

    /* and zero, which is what a hole in a sparse file reads as */
    ASSERT(!zsi_type_valid(0x00));

    /* The bits mean what F-12a says in isolation, checked against the table
     * rather than assumed. */
    ASSERT(ZSI_DELETION == (ZSI_KEYVALUE | ZSI_ISDELETE));
    ASSERT(ZSI_BIGKEYVALUE == (ZSI_KEYVALUE | ZSI_ISBIG));
    ASSERT(ZSI_ROLLBACK == (ZSI_COMMIT | ZSI_ISDELETE));
    ASSERT(ZSI_COMMIT_LONG == (ZSI_COMMIT | ZSI_ISBIG));
    ASSERT(ZSI_PTRS64 == (ZSI_PTRS32 | ZSI_ISBIG));
}

/* The record layouts, pinned against literals derived by hand from section 4.5's
 * diagrams rather than from this encoder.
 *
 * Task 3 established why this is necessary: a round-trip through a matched
 * encoder and decoder cannot detect a layout that is wrong in the same way at
 * both ends, and that is exactly the bug that makes another implementation unable
 * to read our files. */
static void test_record_byte_layout(void)
{
    char buf[64];
    /* Engine 0 writes a ZERO checksum field (F-32), which is what keeps these
     * literals simple: the trailing 4 bytes are zero, exactly where the old
     * (pre-F-32) layout already had zero padding. test_record_byte_layout_v2
     * is what pins a real, nonzero engine's checksum against a literal. */
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_NONE, NULL);

    /* KEYVALUE (0x01): key "ab", value "xy".
     *   +0 type, +1 keylen, +2 vallen(LE16), +4 key NUL value NUL,
     *   pad, last 4 bytes checksum (F-32)
     *   len = roundup8(4 + 2 + 1 + 2 + 1 + 4) = roundup8(14) = 16 */
    static const unsigned char kv[16] = {
        0x01, 0x02, 0x02, 0x00, 'a', 'b', 0x00, 'x',
        'y',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 2, false), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, cs, "ab", 2, "xy", 2);
    ASSERT_MEM_EQ(buf, kv, 16);

    /* DELETION (0x03): key "ab", no value field at all.
     *   +0 type, +1 keylen, +2 pad(2), +4 key NUL, then padding, then checksum
     *   len = roundup8(4 + 2 + 1 + 4) = roundup8(11) = 16 */
    static const unsigned char del[16] = {
        0x03, 0x02, 0x00, 0x00, 'a', 'b', 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 0, true), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, cs, "ab", 2, NULL, 0);
    ASSERT_MEM_EQ(buf, del, 16);

    /* An empty value is legal and distinct from an absent key (F-14, A-1).
     *   len = roundup8(4 + 2 + 1 + 0 + 1 + 4) = roundup8(12) = 16 */
    static const unsigned char kv_empty[16] = {
        0x01, 0x02, 0x00, 0x00, 'a', 'b', 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 0, false), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, cs, "ab", 2, "", 0);
    ASSERT_MEM_EQ(buf, kv_empty, 16);

    /* Note this is byte-distinct from DELETION above at exactly one place -- the
     * type byte -- and identical everywhere else.  Which is the whole reason an
     * empty value and an absent key cannot be told apart by length. */
    ASSERT_EQ(kv_empty[0], 0x01);
    ASSERT_EQ(del[0], 0x03);
    ASSERT_MEM_EQ(kv_empty + 1, del + 1, 7);
}

static void test_record_byte_layout_big(void)
{
    /* The big forms' fixed headers, asserted byte for byte.  The bodies are too
     * large for a literal, so the header and the total length are pinned here and
     * the body placement is checked by decoding.
     *
     * The +4 for F-32's trailing checksum lands inside padding these big forms
     * already carried (same roundup8 step, or absorbed exactly), so none of
     * the totals below change from the pre-F-32 layout -- verified by hand
     * for each below, not assumed. */
    size_t keylen = 256;                 /* one past the short form's limit */
    char *key = malloc(keylen);
    ASSERT_NOT_NULL(key);
    for (size_t i = 0; i < keylen; i++) key[i] = (char)('A' + (i % 26));
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_NONE, NULL);

    /* BIGKEYVALUE (0x05): +0 type, +1 pad(7), +8 keylen(LE64), +16 vallen(LE64),
     *                     +24 key NUL value NUL, then checksum (F-32)
     *   len = roundup8(24 + 256 + 1 + 2 + 1 + 4) = roundup8(288) = 288 */
    size_t want = 288;
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 2, false), want);
    char *buf = malloc(want + 16);
    ASSERT_NOT_NULL(buf);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, cs, key, keylen, "xy", 2);

    static const unsigned char bkv_hdr[24] = {
        0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   /* keylen = 256 */
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00    /* vallen = 2   */
    };
    ASSERT_MEM_EQ(buf, bkv_hdr, 24);
    ASSERT_MEM_EQ(buf + 24, key, keylen);
    ASSERT_EQ(buf[24 + keylen], 0);
    ASSERT_MEM_EQ(buf + 24 + keylen + 1, "xy", 2);
    ASSERT_EQ(buf[24 + keylen + 1 + 2], 0);

    /* BIGDELETION (0x07): +0 type, +1 pad(7), +8 keylen, +16 key NUL, checksum
     *   len = roundup8(16 + 256 + 1 + 4) = roundup8(277) = 280 */
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 0, true), 280u);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, cs, key, keylen, NULL, 0);
    static const unsigned char bdel_hdr[16] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_MEM_EQ(buf, bdel_hdr, 16);
    ASSERT_MEM_EQ(buf + 16, key, keylen);

    free(buf);
    free(key);
}

/* F-32: the checksum is the LAST 4 bytes of the padded record, covering
 * [0, len-4).  Asserted against literals for the structure, and against a
 * direct call to the engine function for the checksum itself -- never
 * against zsi_rec_encode's own output for the same field, which is exactly
 * the round-trip trap test_header_byte_layout's mutation history warns
 * about (a symmetric encode/decode bug would pass that check too). */
static void test_record_byte_layout_v2(void)
{
    char buf[64];
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL);

    /* KEYVALUE "ab" -> "xy": fixed 4 + 2+1+2+1 + csum 4 = 14 -> len 16. */
    zsi_rec_encode(buf, cs, "ab", 2, "xy", 2);
    ASSERT_EQU((size_t)zsi_rec_encoded_len(2, 2, false), 16u);
    ASSERT_EQ(buf[0], 0x01);
    ASSERT_EQ(buf[1], 2);                       /* keylen */
    ASSERT_EQU(zsi_get16(buf + 2), 2u);         /* vallen */
    ASSERT_MEM_EQ(buf + 4, "ab\0xy\0", 6);
    ASSERT_EQ(buf[10], 0); ASSERT_EQ(buf[11], 0);   /* pad, covered */
    ASSERT_EQU(zsi_get32(buf + 12), cs(buf, 12));   /* trailing csum */

    /* DELETION "ab": fixed 4 + 2+1 + csum 4 = 11 -> len 16.  The bytes between
     * the key's NUL and the checksum are padding and are covered by it. */
    zsi_rec_encode(buf, cs, "ab", 2, NULL, 0);
    ASSERT_EQ(buf[0], 0x03);
    ASSERT_EQU((size_t)zsi_rec_encoded_len(2, 0, true), 16u);
    ASSERT_MEM_EQ(buf + 4, "ab\0", 3);
    ASSERT_EQU(zsi_get32(buf + 12), cs(buf, 12));

    /* Engine 0 writes a ZERO field. */
    zs_csum *cs0 = zsi_csum_for_id(ZSI_CSUM_NONE, NULL);
    zsi_rec_encode(buf, cs0, "ab", 2, "xy", 2);
    ASSERT_EQU(zsi_get32(buf + 12), 0u);
}

static void test_record_roundtrip(void)
{
    /* Every one of the four data shapes encodes and decodes back unchanged, with
     * a total length that is a multiple of 8 and all padding zero (F-2). */
    struct {
        const char *what;
        size_t keylen, vallen;
        bool isdelete;
        uint8_t type;
    } shapes[] = {
        { "KEYVALUE",      2,   2, false, ZSI_KEYVALUE    },
        { "DELETION",      2,   0, true,  ZSI_DELETION    },
        { "BIGKEYVALUE", 300,   2, false, ZSI_BIGKEYVALUE },
        { "BIGDELETION", 300,   0, true,  ZSI_BIGDELETION }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t kl = shapes[i].keylen, vl = shapes[i].vallen;
        char *key = malloc(kl), *val = malloc(vl ? vl : 1);
        ASSERT_NOT_NULL(key);
        ASSERT_NOT_NULL(val);
        for (size_t j = 0; j < kl; j++) key[j] = (char)(1 + (j % 255));
        for (size_t j = 0; j < vl; j++) val[j] = (char)(255 - (j % 255));

        size_t len = zsi_rec_encoded_len(kl, vl, shapes[i].isdelete);
        ASSERT(len > 0);
        ASSERT_EQU(len % 8, 0u);

        char *buf = malloc(len);
        ASSERT_NOT_NULL(buf);
        memset(buf, 0xAA, len);
        zsi_rec_encode(buf, zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL), key, kl,
                       shapes[i].isdelete ? NULL : val, vl);

        ASSERT_EQ((unsigned char)buf[0], shapes[i].type);

        struct zsi_rec r;
        int rc = zsi_rec_decode(buf, len, &r);
        if (rc != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s: decode returned %d\n",
                    shapes[i].what, rc);
            current_test_failed = 1;
            free(buf); free(key); free(val);
            return;
        }

        ASSERT_EQ(r.type, shapes[i].type);
        ASSERT_EQU(r.len, len);
        ASSERT_EQU(r.keylen, kl);
        ASSERT_MEM_EQ(r.key, key, kl);
        ASSERT_EQ(zsi_rec_is_delete(&r), shapes[i].isdelete);

        if (shapes[i].isdelete) {
            ASSERT_NULL(r.val);
            ASSERT_EQU(r.vallen, 0u);
        } else {
            ASSERT_EQU(r.vallen, vl);
            ASSERT_MEM_EQ(r.val, val, vl);
        }

        /* Key and value are usable in place as C strings, since a NUL follows
         * each -- while the lengths remain authoritative (F-13). */
        ASSERT_EQ(r.key[r.keylen], 0);
        if (!shapes[i].isdelete) ASSERT_EQ(r.val[r.vallen], 0);

        free(buf); free(key); free(val);
    }
}

static void test_record_canonical(void)
{
    char buf[512];
    char key[300], val[70000];
    memset(key, 'k', sizeof(key));
    memset(val, 'v', sizeof(val));

    /* F-15: the short form is mandatory whenever the lengths fit.  Asserting the
     * exact type byte at each boundary means a wrong threshold fails here rather
     * than surfacing much later as a cross-implementation byte diff. */
    ASSERT_EQ(zsi_rec_type_for(255, 0, false), ZSI_KEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(256, 0, false), ZSI_BIGKEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(1, 65535, false), ZSI_KEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(1, 65536, false), ZSI_BIGKEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(255, 0, true), ZSI_DELETION);
    ASSERT_EQ(zsi_rec_type_for(256, 0, true), ZSI_BIGDELETION);

    /* A deletion has no value field, so a value length cannot promote it. */
    ASSERT_EQ(zsi_rec_type_for(1, 70000, true), ZSI_DELETION);

    /* Encoding at each boundary produces the type the table says. */
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL);
    memset(buf, 0, sizeof(buf));
    zsi_rec_encode(buf, cs, key, 255, "", 0);
    ASSERT_EQ((unsigned char)buf[0], ZSI_KEYVALUE);

    char *big = malloc(zsi_rec_encoded_len(256, 0, false));
    ASSERT_NOT_NULL(big);
    zsi_rec_encode(big, cs, key, 256, "", 0);
    ASSERT_EQ((unsigned char)big[0], ZSI_BIGKEYVALUE);
    free(big);

    /* A big record whose lengths would have fitted the short form is
     * non-canonical -- and MUST still decode.
     *
     * Rejecting it would be a data-loss bug: a record that fails to validate
     * makes an unordered file complete at that point (F-24), discarding
     * everything after, and G-3 forbids that costing committed data.  A peer with
     * a canonicalisation bug would silently cost us every record after its first
     * non-canonical one.  The divergence is reported instead (T-6's precedent),
     * which is what zsi_rec_is_canonical is for. */
    struct zsi_rec r;
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, 2);              /* keylen 2: fits the short form */
    zsi_put64(buf + 16, 2);             /* vallen 2: fits too */
    memcpy(buf + 24, "ab\0xy", 5);
    ASSERT_OK(zsi_rec_decode(buf, 64, &r));
    ASSERT_EQU(r.keylen, 2u);
    ASSERT_MEM_EQ(r.key, "ab", 2);      /* the data survives, which is the point */
    ASSERT_EQU(r.vallen, 2u);
    ASSERT_MEM_EQ(r.val, "xy", 2);
    ASSERT(!zsi_rec_is_canonical(&r));

    /* A big record that genuinely needs the big form decodes and is canonical. */
    size_t n = zsi_rec_encoded_len(300, 2, false);
    char *ok = malloc(n);
    ASSERT_NOT_NULL(ok);
    zsi_rec_encode(ok, cs, key, 300, "xy", 2);
    ASSERT_OK(zsi_rec_decode(ok, n, &r));
    ASSERT_EQU(r.keylen, 300u);
    ASSERT(zsi_rec_is_canonical(&r));
    free(ok);

    /* And the same for a big deletion, where only keylen can justify the form. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGDELETION;
    zsi_put64(buf + 8, 2);
    memcpy(buf + 16, "ab", 2);
    ASSERT_OK(zsi_rec_decode(buf, 64, &r));
    ASSERT_MEM_EQ(r.key, "ab", 2);
    ASSERT(!zsi_rec_is_canonical(&r));

    /* Everything this implementation writes is canonical, across all four
     * shapes -- which is the writer-side half of F-15. */
    struct { size_t kl, vl; bool del; } shapes[] = {
        { 2, 2, false }, { 2, 0, true }, { 300, 2, false }, { 300, 0, true }
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t len = zsi_rec_encoded_len(shapes[i].kl, shapes[i].vl,
                                         shapes[i].del);
        char *b = malloc(len);
        ASSERT_NOT_NULL(b);
        zsi_rec_encode(b, cs, key, shapes[i].kl,
                       shapes[i].del ? NULL : val, shapes[i].vl);
        ASSERT_OK(zsi_rec_decode(b, len, &r));
        if (!zsi_rec_is_canonical(&r)) {
            fprintf(stderr, "\n    FAIL shape %zu encoded non-canonically\n", i);
            current_test_failed = 1;
            free(b);
            return;
        }
        free(b);
    }

    /* F-18: a record's bytes are a function of its own key and value, and of
     * nothing else -- so canonicality is a property of the record alone and
     * needs no containing file to judge it against.  The same key and value
     * encode identically however the database around them got there, which is
     * what makes T-12a's byte-for-byte agreement checkable at all. */
    size_t l2 = zsi_rec_encoded_len(2, 2, false);
    char *b2 = malloc(l2), *b3 = malloc(l2);
    ASSERT_NOT_NULL(b2);
    ASSERT_NOT_NULL(b3);
    memset(b2, 0xAA, l2);
    memset(b3, 0x55, l2);
    zsi_rec_encode(b2, cs, "ab", 2, "xy", 2);
    zsi_rec_encode(b3, cs, "ab", 2, "xy", 2);
    ASSERT_MEM_EQ(b2, b3, l2);
    ASSERT_OK(zsi_rec_decode(b2, l2, &r));
    ASSERT(zsi_rec_is_canonical(&r));
    free(b2);
    free(b3);
}

static void test_record_embedded_nul(void)
{
    /* F-13: lengths are authoritative; keys and values MAY contain NUL bytes,
     * and stored lengths MUST NOT include the terminators. */
    char buf[64];
    struct zsi_rec r;

    const char key[] = { 'a', '\0', 'b' };
    const char val[] = { '\0', 'x', '\0' };
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL);

    size_t len = zsi_rec_encoded_len(3, 3, false);
    /* roundup8(4 + 3 + 1 + 3 + 1 + 4) = roundup8(16) = 16 -- the checksum's
     * +4 does not push this over another roundup8 step. */
    ASSERT_EQU(len, 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, cs, key, 3, val, 3);
    ASSERT_OK(zsi_rec_decode(buf, len, &r));
    ASSERT_EQU(r.keylen, 3u);
    ASSERT_MEM_EQ(r.key, key, 3);
    ASSERT_EQU(r.vallen, 3u);
    ASSERT_MEM_EQ(r.val, val, 3);

    /* The stored length is 3, not the 1 that strlen would report. */
    ASSERT_EQ((unsigned char)buf[1], 3);
    ASSERT_EQU(zsi_get16(buf + 2), 3u);

    /* A key that is entirely NULs. */
    const char nuls[] = { '\0', '\0', '\0', '\0' };
    len = zsi_rec_encoded_len(4, 0, false);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, cs, nuls, 4, "", 0);
    ASSERT_OK(zsi_rec_decode(buf, len, &r));
    ASSERT_EQU(r.keylen, 4u);
    ASSERT_MEM_EQ(r.key, nuls, 4);
    ASSERT_EQU(r.vallen, 0u);
    ASSERT_NOT_NULL(r.val);             /* empty, not absent */
}

static void test_record_bounds(void)
{
    char buf[512];
    struct zsi_rec r;
    zs_csum *cs = zsi_csum_for_id(ZSI_CSUM_XXHASH, NULL);

    /* For each shape, decoding with len one byte short of the true length is
     * rejected rather than reading past the end. */
    struct { size_t kl, vl; bool del, anc; } shapes[] = {
        { 2, 2, false, false }, { 2, 2, false, true },
        { 2, 0, true,  false }, { 2, 0, true,  true },
        { 300, 2, false, false }, { 300, 0, true, false }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t len = zsi_rec_encoded_len(shapes[i].kl, shapes[i].vl,
                                         shapes[i].del);
        char *b = malloc(len);
        ASSERT_NOT_NULL(b);
        char *k = malloc(shapes[i].kl);
        ASSERT_NOT_NULL(k);
        memset(k, 'k', shapes[i].kl);
        zsi_rec_encode(b, cs, k, shapes[i].kl,
                       shapes[i].del ? NULL : "xy", shapes[i].vl);

        ASSERT_OK(zsi_rec_decode(b, len, &r));
        /* every length below the true one fails */
        for (size_t l = 0; l < len; l++) {
            if (zsi_rec_decode(b, l, &r) == ZS_OK) {
                fprintf(stderr, "\n    FAIL shape %zu accepted at len %zu of %zu\n",
                        i, l, len);
                current_test_failed = 1;
                free(b); free(k);
                return;
            }
        }
        free(b); free(k);
    }

    /* A record claiming an enormous keylen is rejected rather than overflowing.
     * This is G-0b's whole point: keylen + vallen + 2 must not wrap into a small
     * total that then passes a bounds check. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, 0xFFFFFFFFFFFFFFFFull);
    zsi_put64(buf + 16, 0xFFFFFFFFFFFFFFFFull);
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);

    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, (uint64_t)SIZE_MAX - 1);
    zsi_put64(buf + 16, 4);
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);

    /* F-14: a key must be at least 1 byte, in every shape. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_KEYVALUE;
    buf[1] = 0;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);

    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, 0);
    zsi_put64(buf + 16, 70000);
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);

    /* An invalid type byte, and a valid non-data type, are both refused by the
     * data-record decoder.
     *
     * The buffer is filled with a plausible *record* body first, so these assert
     * the family check specifically.  With a zeroed buffer they would pass even
     * with no family check at all, because the zero at +1 trips F-14's minimum
     * key length instead -- which is a different rule and would leave a
     * terminator decodable as a record the moment its second byte was nonzero. */
    memset(buf, 0, sizeof(buf));
    buf[1] = 2;                          /* a keylen that would be valid */
    zsi_put16(buf + 2, 2);               /* a vallen that would be valid */
    memcpy(buf + 4, "ab\0xy", 5);

    buf[0] = 0x11;                       /* HasKey|SpanTerminator: not legal */
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_COMMIT;           /* a terminator is not a data record */
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_ROLLBACK;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_COMMIT_LONG;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS32;           /* nor is a pointer section */
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS64;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);
    buf[0] = 0x00;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), &r), ZS_BADFORMAT);

    /* Symmetrically, a data type is not a terminator -- with a plausible
     * terminator body present, for the same reason. */
    memset(buf, 0, sizeof(buf));
    zsi_put24(buf + 1, 64);
    struct zsi_term t;
    buf[0] = (char)ZSI_KEYVALUE;
    ASSERT_EQ(zsi_term_decode(buf, sizeof(buf), &t), ZS_BADFORMAT);
    buf[0] = (char)ZSI_DELETION;
    ASSERT_EQ(zsi_term_decode(buf, sizeof(buf), &t), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS32;
    ASSERT_EQ(zsi_term_decode(buf, sizeof(buf), &t), ZS_BADFORMAT);

    /* Zero-length buffer. */
    ASSERT_EQ(zsi_rec_decode(buf, 0, &r), ZS_BADFORMAT);
}

static void test_terminator(void)
{
    char buf[32];
    struct zsi_term t;
    char span[64];
    for (size_t i = 0; i < sizeof(span); i++) span[i] = (char)i;

    /* COMMIT (0x10): +0 type, +1 span length (3 bytes), +4 checksum. */
    ASSERT_EQU(zsi_term_encoded_len(0), 8u);
    ASSERT_EQU(zsi_term_encoded_len(ZSI_SHORT_SPANLEN_MAX), 8u);
    ASSERT_EQU(zsi_term_encoded_len(ZSI_SHORT_SPANLEN_MAX + 1), 24u);

    memset(buf, 0xAA, sizeof(buf));
    zsi_term_encode(buf, 64, false, span, zsi_csum_xxhash, ZSI_CSUM_XXHASH);
    ASSERT_EQ((unsigned char)buf[0], ZSI_COMMIT);
    ASSERT_EQU(zsi_get24(buf + 1), 64u);
    ASSERT_OK(zsi_term_decode(buf, 8, &t));
    ASSERT_EQ(t.type, ZSI_COMMIT);
    ASSERT_EQU(t.spanlen, 64u);
    ASSERT_EQU(t.len, 8u);
    ASSERT(!zsi_term_is_rollback(&t));

    /* F-19: the checksum covers the span's data followed by the terminator's own
     * bytes up to the checksum field.  Verified by recomputing it the way a
     * reader would. */
    ASSERT_EQU(t.csum, zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH,
                                 span, 64, buf, 4));

    /* ROLLBACK (0x12) differs from COMMIT in exactly the IsDelete bit. */
    memset(buf, 0xAA, sizeof(buf));
    zsi_term_encode(buf, 64, true, span, zsi_csum_xxhash, ZSI_CSUM_XXHASH);
    ASSERT_EQ((unsigned char)buf[0], ZSI_ROLLBACK);
    ASSERT_OK(zsi_term_decode(buf, 8, &t));
    ASSERT(zsi_term_is_rollback(&t));

    /* COMMIT_LONG (0x14): +0 type, +1 pad(7), +8 span length, +16 pad(4),
     * +20 checksum. */
    uint64_t bigspan = (uint64_t)ZSI_SHORT_SPANLEN_MAX + 1;   /* 0x1000000 */
    memset(buf, 0xAA, sizeof(buf));
    zsi_term_encode(buf, bigspan, false, span, zsi_csum_none, ZSI_CSUM_NONE);
    static const unsigned char clong[20] = {
        0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00,   /* spanlen 0x1000000 */
        0x00, 0x00, 0x00, 0x00                            /* pad             */
    };
    ASSERT_MEM_EQ(buf, clong, 20);
    ASSERT_OK(zsi_term_decode(buf, 24, &t));
    ASSERT_EQ(t.type, ZSI_COMMIT_LONG);
    ASSERT_EQU(t.spanlen, bigspan);
    ASSERT_EQU(t.len, 24u);

    memset(buf, 0xAA, sizeof(buf));
    zsi_term_encode(buf, bigspan, true, span, zsi_csum_none, ZSI_CSUM_NONE);
    ASSERT_EQ((unsigned char)buf[0], ZSI_ROLLBACK_LONG);
    ASSERT_OK(zsi_term_decode(buf, 24, &t));
    ASSERT(zsi_term_is_rollback(&t));

    /* F-15: the short terminator is mandatory whenever the span fits, so a long
     * terminator over a small span is non-canonical -- and MUST still decode, for
     * the same reason as a non-canonical record.  Rejecting it would truncate the
     * file at that span (F-24) and lose the committed data after it (G-3). */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_COMMIT_LONG;
    zsi_put64(buf + 8, 64);
    ASSERT_OK(zsi_term_decode(buf, 24, &t));
    ASSERT_EQU(t.spanlen, 64u);
    ASSERT_EQU(t.len, 24u);
    ASSERT(!zsi_term_is_canonical(&t));

    /* Everything this implementation writes is canonical, on both sides of the
     * width boundary. */
    zsi_term_encode(buf, ZSI_SHORT_SPANLEN_MAX, false, span,
                    zsi_csum_none, ZSI_CSUM_NONE);
    ASSERT_OK(zsi_term_decode(buf, 8, &t));
    ASSERT(zsi_term_is_canonical(&t));

    zsi_term_encode(buf, (uint64_t)ZSI_SHORT_SPANLEN_MAX + 1, false, span,
                    zsi_csum_none, ZSI_CSUM_NONE);
    ASSERT_OK(zsi_term_decode(buf, 24, &t));
    ASSERT(zsi_term_is_canonical(&t));

    /* Short buffers rejected at every length below the true one. */
    memset(buf, 0, sizeof(buf));
    zsi_term_encode(buf, 64, false, span, zsi_csum_none, ZSI_CSUM_NONE);
    for (size_t l = 0; l < 8; l++)
        ASSERT_EQ(zsi_term_decode(buf, l, &t), ZS_BADFORMAT);

    memset(buf, 0, sizeof(buf));
    zsi_term_encode(buf, bigspan, false, span, zsi_csum_none, ZSI_CSUM_NONE);
    for (size_t l = 0; l < 24; l++)
        ASSERT_EQ(zsi_term_decode(buf, l, &t), ZS_BADFORMAT);

    /* A data type or a pointers type is not a terminator. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_KEYVALUE;
    ASSERT_EQ(zsi_term_decode(buf, 8, &t), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS32;
    ASSERT_EQ(zsi_term_decode(buf, 8, &t), ZS_BADFORMAT);
    buf[0] = 0x00;
    ASSERT_EQ(zsi_term_decode(buf, 8, &t), ZS_BADFORMAT);
}

/*
 * ============================================================
 * Building unordered files by hand
 * ============================================================
 *
 * A growable buffer with span semantics, so a test can lay down exactly the
 * bytes it wants -- including the shapes a conforming writer never produces: a
 * span whose terminator claims the wrong length, a terminator whose data was
 * never written, trailing garbage after a valid span.
 */

struct sb {
    char  *buf;
    size_t len, alloc;
    size_t span_start;      /* offset where the current span's data began */
    unsigned engine;
};

static void sb_init(struct sb *s, uint32_t start, unsigned engine)
{
    s->alloc = 4096;
    s->buf = malloc(s->alloc);
    assert(s->buf);
    s->len = 0;
    s->engine = engine;

    char hdr[ZSI_HEADER_LEN];
    make_header(hdr, start, 0, engine);
    memcpy(s->buf, hdr, ZSI_HEADER_LEN);
    s->len = ZSI_HEADER_LEN;
    s->span_start = s->len;
}

static void sb_free(struct sb *s) { free(s->buf); s->buf = NULL; }

static void sb_reserve(struct sb *s, size_t extra)
{
    while (s->len + extra > s->alloc) {
        s->alloc *= 2;
        s->buf = realloc(s->buf, s->alloc);
        assert(s->buf);
    }
}

/* Append a data record to the current span. */
static void sb_rec(struct sb *s, const char *key, size_t keylen,
                   const char *val, size_t vallen)
{
    size_t n = zsi_rec_encoded_len(keylen, vallen, val == NULL);
    assert(n > 0);
    sb_reserve(s, n);
    zs_csum *cs = zsi_csum_for_id(s->engine, TEST_EXTERNAL_CSUM);
    zsi_rec_encode(s->buf + s->len, cs, key, keylen, val, vallen);
    s->len += n;
}

/* Close the current span with a terminator, and begin a new one. */
static void sb_term(struct sb *s, bool rollback)
{
    size_t datalen = s->len - s->span_start;
    size_t n = zsi_term_encoded_len(datalen);
    sb_reserve(s, n);
    zsi_term_encode(s->buf + s->len, datalen, rollback,
                    s->buf + s->span_start,
                    zsi_csum_for_id(s->engine, TEST_EXTERNAL_CSUM), s->engine);
    s->len += n;
    s->span_start = s->len;
}

/* Close the current span with a terminator that LIES about its length, so the
 * span-length check can be exercised independently of the checksum. */
static void sb_term_badlen(struct sb *s, uint64_t claim)
{
    size_t datalen = s->len - s->span_start;
    size_t n = zsi_term_encoded_len(claim);
    sb_reserve(s, n);
    /* checksum still computed over the real data, so only the length is wrong */
    zsi_term_encode(s->buf + s->len, claim, false, s->buf + s->span_start,
                    zsi_csum_for_id(s->engine, TEST_EXTERNAL_CSUM), s->engine);
    (void)datalen;
    s->len += n;
    s->span_start = s->len;
}

static void sb_raw(struct sb *s, const void *bytes, size_t n)
{
    sb_reserve(s, n);
    memcpy(s->buf + s->len, bytes, n);
    s->len += n;
}

static int sb_write(struct sb *s, const char *name)
{
    return writefile(name, s->buf, s->len);
}

/*
 * Building in-order files by hand.
 *
 * ib_rec appends records in the order given -- the caller is responsible for
 * supplying them in key order, exactly as a repack would, so a test can also
 * build a file that is deliberately out of order.
 */
struct ib {
    char    *buf;
    size_t   len, alloc;
    uint64_t offs[256];
    size_t   n;
    unsigned engine;
};

static void ib_init(struct ib *b, uint32_t start, uint32_t end, unsigned engine)
{
    b->alloc = 8192;
    b->buf = malloc(b->alloc);
    assert(b->buf);
    b->n = 0;
    b->engine = engine;

    char hdr[ZSI_HEADER_LEN];
    make_header(hdr, start, end, engine);
    memcpy(b->buf, hdr, ZSI_HEADER_LEN);
    b->len = ZSI_HEADER_LEN;
}

static void ib_free(struct ib *b) { free(b->buf); b->buf = NULL; }

static void ib_rec(struct ib *b, const char *key, size_t keylen,
                   const char *val, size_t vallen)
{
    size_t n = zsi_rec_encoded_len(keylen, vallen, val == NULL);
    assert(n > 0);
    while (b->len + n > b->alloc) {
        b->alloc *= 2;
        b->buf = realloc(b->buf, b->alloc);
        assert(b->buf);
    }
    assert(b->n < 256);
    b->offs[b->n++] = b->len;
    zs_csum *cs = zsi_csum_for_id(b->engine, TEST_EXTERNAL_CSUM);
    zsi_rec_encode(b->buf + b->len, cs, key, keylen, val, vallen);
    b->len += n;
}

/* Close the file: append the pointer section and trailer. */
static void ib_finish(struct ib *b)
{
    zs_csum *cs = zsi_csum_for_id(b->engine, TEST_EXTERNAL_CSUM);
    uint32_t rc = cs(b->buf + ZSI_HEADER_LEN, b->len - ZSI_HEADER_LEN);
    char *sec = NULL;
    size_t seclen = 0;

    assert(zsi_ptrs_build(b->offs, b->n, b->len, rc, cs, &sec, &seclen) == ZS_OK);
    while (b->len + seclen > b->alloc) {
        b->alloc *= 2;
        b->buf = realloc(b->buf, b->alloc);
        assert(b->buf);
    }
    memcpy(b->buf + b->len, sec, seclen);
    b->len += seclen;
    free(sec);
}

/* Write, open and load.  Leaves the open file in *fp. */
static int ib_load(struct ib *b, uint32_t start, uint32_t end,
                   struct zsi_file **fp)
{
    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, start, end);
    if (mkdbdir() != 0) return -1;
    if (writefile(name, b->buf, b->len) != 0) return -1;
    if (zsi_file_open(dbdir, name, start, TEST_EXTERNAL_CSUM, fp) != ZS_OK)
        return -1;
    return zsi_ptrs_load(*fp);
}

/* Replay collector: records the keys a replay presented, in order. */
struct collected {
    char   key[64][64];
    size_t keylen[64];
    size_t off[64];
    bool   isdel[64];
    size_t n;
};

/* For spans larger than the collector holds: count only. */
static int count_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    (void)rec;
    (void)off;
    (*(size_t *)rock)++;
    return 0;
}

static int collect_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct collected *c = rock;
    if (c->n >= 64) return 1;
    size_t kl = rec->keylen < 63 ? rec->keylen : 63;
    memcpy(c->key[c->n], rec->key, kl);
    c->key[c->n][kl] = '\0';
    c->keylen[c->n] = rec->keylen;
    c->off[c->n] = off;
    c->isdel[c->n] = (rec->val == NULL);
    c->n++;
    return 0;
}

/* Write a hand-built file, open it, replay it.  Leaves the open file in *fp for
 * the caller to inspect and close. */
static int replay_file(struct sb *s, uint32_t gen, struct collected *c,
                       struct zsi_file **fp)
{
    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    if (mkdbdir() != 0) return -1;
    if (sb_write(s, name) != 0) return -1;
    memset(c, 0, sizeof(*c));
    if (zsi_file_open(dbdir, name, gen, TEST_EXTERNAL_CSUM, fp) != ZS_OK)
        return -1;
    return zsi_unordered_replay(*fp, ZSI_HEADER_LEN, collect_cb, c);
}

/*
 * ============================================================
 * File object (part of T-6)
 * ============================================================
 */

static void test_file_bounds(void)
{
    char hdr[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    make_header(hdr, 1, 0, ZSI_CSUM_XXHASH);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));

    /* In range. */
    ASSERT_NOT_NULL(zsi_file_at(f, 0, 72));
    ASSERT_NOT_NULL(zsi_file_at(f, 71, 1));
    ASSERT_NOT_NULL(zsi_file_at(f, 0, 0));
    ASSERT_NOT_NULL(zsi_file_at(f, 72, 0));   /* the end itself, zero bytes */

    /* One byte past the end, and every larger overrun. */
    ASSERT_NULL(zsi_file_at(f, 72, 1));
    ASSERT_NULL(zsi_file_at(f, 71, 2));
    ASSERT_NULL(zsi_file_at(f, 0, 73));
    ASSERT_NULL(zsi_file_at(f, 73, 0));

    /* Offsets and lengths that would overflow rather than merely exceed (G-0b).
     * This is the case that turns a bounds check into a bypass when unguarded. */
    ASSERT_NULL(zsi_file_at(f, SIZE_MAX, 1));
    ASSERT_NULL(zsi_file_at(f, 1, SIZE_MAX));
    ASSERT_NULL(zsi_file_at(f, SIZE_MAX, SIZE_MAX));
    ASSERT_NULL(zsi_file_at(f, SIZE_MAX - 4, 8));

    /* And that a returned pointer really addresses the requested offset. */
    const char *p = zsi_file_at(f, 16, 2);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ((unsigned char)p[0], ZSI_VERSION_READ);
    ASSERT_EQ((unsigned char)p[1], ZSI_VERSION_WRITE);

    zsi_file_release(&f);
}

static void test_file_zero_length(void)
{
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_EQ(filesize(name), 0);

    /* D-10: an active file of zero length is treated as a complete file with zero
     * spans.  It is not an error, and its generation comes from its filename --
     * which is the only place it could come from. */
    ASSERT_OK(zsi_file_open(dbdir, name, 3, NULL, &f));
    ASSERT_NOT_NULL(f);
    ASSERT(!f->hdr_valid);
    ASSERT_EQU(f->size, 0u);
    ASSERT_EQU(f->hdr.start, 3u);
    ASSERT(zsi_file_is_unordered(f));

    /* Nothing can be read from it, at any offset, including zero bytes at zero. */
    ASSERT_NULL(zsi_file_at(f, 0, 0));
    ASSERT_NULL(zsi_file_at(f, 0, 1));
    zsi_file_release(&f);
}

static void test_file_bad_header(void)
{
    char buf[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_current(name, test_uuid);

    /* Garbage where a header should be (D-10).  Opens, reports the header as
     * invalid, and takes its generation from the name. */
    memset(buf, 0xFF, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    ASSERT_EQU(f->hdr.start, 4u);
    ASSERT_EQU(f->hdr.end, 0u);
    ASSERT(zsi_file_is_unordered(f));
    zsi_file_release(&f);

    /* All zeroes: no magic, and byte 0 of 0x00 is also an invalid type byte. */
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_release(&f);

    /* Shorter than a header: valid magic but truncated.  Still not an error at
     * this level -- the caller's position in the file set decides (D-10/D-10a). */
    make_header(buf, 4, 0, ZSI_CSUM_XXHASH);
    for (size_t len = 1; len < ZSI_HEADER_LEN; len += 7) {
        ASSERT_EQ(writefile(name, buf, len), 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
        ASSERT(!f->hdr_valid);
        ASSERT_EQU(f->size, len);
        ASSERT_EQU(f->hdr.start, 4u);
        /* Bounds still hold over the short file. */
        ASSERT_NOT_NULL(zsi_file_at(f, 0, len));
        ASSERT_NULL(zsi_file_at(f, 0, len + 1));
        zsi_file_release(&f);
    }

    /* A header whose checksum does not match: unverifiable, so invalid. */
    make_header(buf, 4, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_UUID] ^= 0xFF;
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_release(&f);
}

static void test_file_engine_from_header(void)
{
    char hdr[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_current(name, test_uuid);

    /* F-5a: a file's engine comes from its OWN header, so files written under
     * different engines coexist and a reader's configuration never overrides
     * what a file records. */
    make_header(hdr, 1, 0, ZSI_CSUM_NONE);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(f->hdr_valid);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_NONE);
    ASSERT(f->csum == zsi_csum_none);
    zsi_file_release(&f);

    make_header(hdr, 1, 0, ZSI_CSUM_XXHASH);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT_EQ(f->csum_id, ZSI_CSUM_XXHASH);
    ASSERT(f->csum == zsi_csum_xxhash);
    zsi_file_release(&f);

    /* Engine 2 with the caller's function supplied: readable, and the engine is
     * reported as external rather than as whatever function happened to match. */
    make_header(hdr, 1, 0, ZSI_CSUM_EXTERNAL);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT(f->hdr_valid);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_EXTERNAL);
    ASSERT(f->csum == TEST_EXTERNAL_CSUM);
    zsi_file_release(&f);

    /* The same file with NO function supplied: unverifiable, so the header is not
     * accepted.  A-6 makes this an error at the database level; here it comes back
     * as an invalid header for the caller to judge, because a tool must still be
     * able to inspect the file. */
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_release(&f);

    /* An unknown engine id, likewise. */
    make_header(hdr, 1, 0, 3);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_release(&f);
}

static void test_file_open_failures(void)
{
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];

    ASSERT_EQ(mkdbdir(), 0);

    /* A missing file is ZS_NOTFOUND specifically, not a generic I/O error: the
     * snapshot protocol distinguishes them, restarting its scan on ENOENT because
     * a file may legitimately be unlinked mid-scan (C-4 step 3). */
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(zsi_file_open(dbdir, name, 99, NULL, &f), ZS_NOTFOUND);
    ASSERT_NULL(f);

    /* A directory where a data file should be is malformed, not missing. */
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/notafile", dbdir);
    ASSERT_EQ(mkdir(sub, 0700), 0);
    ASSERT_EQ(zsi_file_open(dbdir, "notafile", 1, NULL, &f), ZS_BADFORMAT);
    ASSERT_NULL(f);

    /* Closing a NULL handle is a no-op, so cleanup paths need no guard. */
    zsi_file_release(&f);
    ASSERT_NULL(f);
}

/*
 * ============================================================
 * The span chain (section 4.8)
 * ============================================================
 */

static void test_span_basic(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* One commit span of three records replays all three, in file order. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_rec(&s, "b", 1, "2", 1);
    sb_rec(&s, "c", 1, "3", 1);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_STR_EQ(c.key[1], "b");
    ASSERT_STR_EQ(c.key[2], "c");
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    zsi_file_release(&f);
    sb_free(&s);

    /* Several spans, all committed, replay in order across span boundaries. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    sb_rec(&s, "b", 1, "2", 1);
    sb_rec(&s, "c", 1, "3", 1);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_STR_EQ(c.key[2], "c");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* An empty span -- a terminator with no records -- is legal (F-23 says zero
     * or more), and contributes nothing. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_term(&s, false);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* Deletions are presented as records with a NULL value (A-1); the replay does
     * not filter them, because resolving a deletion into "absent" is the read
     * path's job, and an index must know the tombstone exists. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_rec(&s, "a", 1, NULL, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 2u);
    ASSERT(!c.isdel[0]);
    ASSERT(c.isdel[1]);
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_span_rollback(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* A rolled-back span replays nothing (F-21, F-25). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_rec(&s, "b", 1, "2", 1);
    sb_term(&s, true);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    /* ...but the file is still clean: a rollback is a valid span terminator, so
     * the writer may keep appending to it (F-26h). */
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    zsi_file_release(&f);
    sb_free(&s);

    /* F-25 directly: visibility is per span, NOT a watermark.  A rolled-back span
     * sits between two live ones, and both live spans must survive it -- a
     * high-water-mark implementation would lose the third span or keep the
     * second. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "first", 5, "1", 1);
    sb_term(&s, false);
    sb_rec(&s, "aborted", 7, "x", 1);
    sb_term(&s, true);
    sb_rec(&s, "third", 5, "3", 1);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 2u);
    ASSERT_STR_EQ(c.key[0], "first");
    ASSERT_STR_EQ(c.key[1], "third");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* Every span rolled back: clean, zero records, not an error (F-26h). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, true);
    sb_rec(&s, "b", 1, "2", 1);
    sb_term(&s, true);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* Interleaved, several of each, to catch an implementation that skips one
     * rollback correctly but loses its place afterwards. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    for (int i = 0; i < 6; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%d", i);
        sb_rec(&s, k, strlen(k), "v", 1);
        sb_term(&s, (i % 2) == 1);       /* odd spans rolled back */
    }
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "k0");
    ASSERT_STR_EQ(c.key[1], "k2");
    ASSERT_STR_EQ(c.key[2], "k4");
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_span_empty_file(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* A file that is only a header: complete at 72, clean, zero records.  This is
     * what creating a database produces (D-8a) and F-26h makes it legal. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    ASSERT_EQU(s.len, (size_t)ZSI_HEADER_LEN);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_span_torn_tail(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;
    size_t good_end;

    /* A span whose terminator never landed.  The earlier spans still replay, the
     * complete point is before the unterminated span, and the file is NOT clean --
     * so a writer moves to a new generation rather than appending (D-9, R-4). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    good_end = s.len;
    sb_rec(&s, "b", 1, "2", 1);       /* no terminator */
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* The same file with the terminator present but a data byte flipped.  F-22:
     * because the checksum covers the span AND the terminator, the outcome is
     * identical -- the span reads as absent. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    good_end = s.len;
    size_t victim = s.len;
    sb_rec(&s, "b", 1, "2", 1);
    sb_term(&s, false);
    s.buf[victim + 5] ^= 0x01;                  /* damage the span's data */
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* Trailing garbage after a valid span. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    good_end = s.len;
    sb_raw(&s, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);
    sb_free(&s);

    /* Truncated at every byte offset past the first valid span.  Whatever the
     * truncation point, the answer is the committed prefix and never a crash. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    good_end = s.len;
    sb_rec(&s, "bb", 2, "22", 2);
    sb_term(&s, false);
    size_t full = s.len;

    for (size_t cut = good_end; cut < full; cut++) {
        char name[ZSI_NAME_MAX];
        zsi_name_current(name, test_uuid);
        ASSERT_EQ(mkdbdir(), 0);
        ASSERT_EQ(writefile(name, s.buf, cut), 0);
        memset(&c, 0, sizeof(c));
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, collect_cb, &c));
        if (c.n != 1 || f->complete != good_end) {
            fprintf(stderr, "\n    FAIL cut at %zu: n=%zu complete=%zu (want 1, %zu)\n",
                    cut, c.n, f->complete, good_end);
            current_test_failed = 1;
            zsi_file_release(&f);
            sb_free(&s);
            return;
        }

        /* Cutting exactly at the span boundary leaves a file that is legitimately
         * clean -- complete == size, nothing after the last valid span -- so a
         * writer may append to it.  Every cut past that leaves a partial span, and
         * the file must NOT be clean, or a writer would build a chain on top of a
         * boundary that failed to validate (D-9, R-4).  The boundary case is the
         * one a truncating write that happened to land on a span end produces. */
        if (cut == good_end)
            ASSERT(zsi_unordered_is_clean(f));
        else
            ASSERT(!zsi_unordered_is_clean(f));

        zsi_file_release(&f);
    }
    sb_free(&s);
}

static void test_span_terminator_without_data(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* The exact shape F-22 exists for, and the one C-4f depends on: a terminator
     * reached disk but the span's data did not.
     *
     * Built by writing a span, then a terminator over it, then overwriting the
     * span's data region with zeros -- which is what a filesystem that reordered
     * the writes would leave behind.  The terminator is structurally perfect and
     * its length field matches; only the checksum can tell. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    size_t good_end = s.len;

    size_t data_at = s.len;
    sb_rec(&s, "ghost", 5, "value", 5);
    size_t data_len = s.len - data_at;
    sb_term(&s, false);
    memset(s.buf + data_at, 0, data_len);       /* data never landed */

    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);

    sb_free(&s);

    /* A structurally valid span whose contents were altered: only the checksum
     * distinguishes it.  Verified, it is rejected; unverified, the altered data is
     * returned as though committed. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    size_t at = s.len;
    sb_rec(&s, "key", 3, "aaa", 3);
    sb_term(&s, false);
    s.buf[at + 4 + 3 + 1] = 'b';                /* first value byte: aaa -> baa */

    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);                        /* checksum catches it */
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    zsi_file_release(&f);

    sb_free(&s);
}

/* F-5e: verification rides indexing.  ZS_NOCSUM skips the per-record verify
 * at materialization and NOTHING else -- the span checksum is checked by
 * whoever adds the span to an index, in every mode, because a post-crash
 * reopen under relaxed durability can meet a terminator whose data never
 * landed (C-7c), and a NOCSUM handle accepting it would surface garbage as
 * committed records. */
static void test_nocsum_still_rejects_bad_span(void)
{
    struct sb s;
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    const char *v = NULL;
    size_t vl = 0;
    char name[ZSI_NAME_MAX];

    /* One good committed span, then a structurally valid span whose data was
     * altered after its terminator was computed -- only the checksum can
     * tell. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    size_t at = s.len;
    sb_rec(&s, "key", 3, "aaa", 3);
    sb_term(&s, false);
    s.buf[at + 4 + 3 + 1] = 'b';                /* first value byte: aaa -> baa */

    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    setup.flags = ZS_SHARED | ZS_NOCSUM;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* The good prefix survives; the altered span reads as absent, exactly as
     * it does for a verifying handle. */
    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_MEM_EQ(v, "1", 1);
    ASSERT_EQ(zs_db_fetch(db, "key", 3, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    ASSERT_OK(zs_db_close(&db));
}

static void test_span_progress(void)
{
    /* F-29: iteration computes the next offset from the current record's own
     * length fields and must verify it is strictly greater and in bounds.
     * Non-termination must be impossible by construction.
     *
     * Under an alarm, so a regression hangs this test rather than the suite --
     * which is the detector T-3 relies on. */
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    alarm(20);

    /* A record whose encoded length would be zero.  zsi_rec_encoded_len refuses
     * to produce one, so this is hand-built: a KEYVALUE claiming keylen 0, which
     * F-14 rejects -- and if it did not, roundup8(4+0+1+0+1) would still be 8 and
     * so still advance.  The point is that no length field can produce a
     * non-advancing step. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    char bad[8];
    memset(bad, 0, sizeof(bad));
    bad[0] = (char)ZSI_KEYVALUE;
    bad[1] = 0;                                  /* keylen 0 */
    sb_raw(&s, bad, sizeof(bad));
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    zsi_file_release(&f);
    sb_free(&s);

    /* A file that is entirely 0xFF: no valid type byte anywhere, so the walk stops
     * immediately rather than scanning to the end repeatedly. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    char junk[4096];
    memset(junk, 0xFF, sizeof(junk));
    sb_raw(&s, junk, sizeof(junk));
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    zsi_file_release(&f);
    sb_free(&s);

    /* A file of zero bytes throughout: 0x00 is an invalid type byte (F-12), which
     * is what a sparse hole reads as. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    memset(junk, 0x00, sizeof(junk));
    sb_raw(&s, junk, sizeof(junk));
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    zsi_file_release(&f);
    sb_free(&s);

    alarm(0);
}

static void test_span_bad_header_and_kind(void)
{
    struct collected c;
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];
    char buf[ZSI_HEADER_LEN];

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_current(name, test_uuid);

    /* D-10: an invalid header means zero spans, complete at 0, and NOT clean --
     * so a writer moves on rather than appending past a boundary that failed to
     * validate (R-4). */
    memset(buf, 0xFF, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, NULL, &f));
    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, collect_cb, &c));
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, 0u);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);

    /* A zero-length file, likewise: complete == size == 0, and yet NOT clean,
     * because D-9 requires a valid header.  This is the case where a
     * complete == size test alone would wrongly report clean and let a writer
     * append to a file with no header. */
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, NULL, &f));
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, collect_cb, &c));
    ASSERT_EQU(f->complete, 0u);
    ASSERT_EQU(f->size, 0u);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_release(&f);

    /* Replaying an in-order file is a usage error, not a data condition: it has no
     * spans at all (section 4.9), so there is no answer to invent. */
    char io[96];
    memset(io, 0, sizeof(io));
    make_header(io, 5, 5, ZSI_CSUM_XXHASH);
    io[72] = (char)ZSI_PTRS32;
    zsi_put64(io + 80, 72);
    zsi_put32(io + 88, zsi_csum_xxhash(io + 72, 0));
    zsi_put32(io + 92, zsi_csum_xxhash(io + 72, 20));
    zsi_name_format(name, test_uuid, 5, 5);
    ASSERT_EQ(writefile(name, io, sizeof(io)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 5, NULL, &f));
    ASSERT_EQ(zsi_unordered_replay(f, ZSI_HEADER_LEN, collect_cb, &c), ZS_BADUSAGE);
    zsi_file_release(&f);
}

static void test_span_pointers_rejected(void)
{
    /* A pointer section cannot appear in an unordered file (section 4.9).  Both
     * PTRS types are valid type bytes, so this is not caught by type validation --
     * the walk has to know that a valid type which is neither a data record nor a
     * terminator ends the file. */
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    for (int wide = 0; wide < 2; wide++) {
        sb_init(&s, 1, ZSI_CSUM_XXHASH);
        sb_rec(&s, "a", 1, "1", 1);
        sb_term(&s, false);
        size_t good_end = s.len;

        char ptrs[16];
        memset(ptrs, 0, sizeof(ptrs));
        ptrs[0] = (char)(wide ? ZSI_PTRS64 : ZSI_PTRS32);
        sb_raw(&s, ptrs, wide ? 16 : 8);

        ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
        ASSERT_EQU(c.n, 1u);
        ASSERT_EQU(f->complete, good_end);
        ASSERT(!zsi_unordered_is_clean(f));
        zsi_file_release(&f);
        sb_free(&s);
    }
}

static void test_span_engine_zero(void)
{
    /* Engine 0 writes zeros and never verifies (F-5c), so a torn tail is
     * undetectable.  The span-length check (F-23) still applies, which is why a
     * length-only corruption is caught even here -- but altered data is not.
     * This is also the only place F-23 can be ISOLATED: under a real engine a
     * lying spanlen moves the checksum window, so the checksum rejects the span
     * whether or not the length check exists. */
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    sb_init(&s, 1, ZSI_CSUM_NONE);
    size_t at = s.len;
    sb_rec(&s, "key", 3, "aaa", 3);
    sb_term(&s, false);
    s.buf[at + 4 + 3 + 1] = 'b';
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);                        /* undetected: engine 0 */
    ASSERT_EQ(f->csum_id, ZSI_CSUM_NONE);
    zsi_file_release(&f);
    sb_free(&s);

    /* But a length disagreement is structural and still caught. */
    sb_init(&s, 1, ZSI_CSUM_NONE);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term_badlen(&s, 999);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_span_long_terminator(void)
{
    /* A span over 0xFFFFFF bytes forces a long terminator (F-15, and the case
     * T-4 names), and the walk must handle the 24-byte form -- including that
     * its checksum lives at +20, not +4: under engine 0 a checksum read from
     * the wrong offset is indistinguishable from the right one, so only this
     * XXH3 replay tells them apart. */
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    char val[8192];
    memset(val, 'v', sizeof(val));
    size_t n = 0;
    while (s.len - s.span_start <= ZSI_SHORT_SPANLEN_MAX) {
        char k[32];
        snprintf(k, sizeof(k), "key%08zu", n++);
        sb_rec(&s, k, strlen(k), val, sizeof(val));
    }
    ASSERT(s.len - s.span_start > ZSI_SHORT_SPANLEN_MAX);
    size_t nrecs = n;
    sb_term(&s, false);

    /* The terminator really is the long form. */
    ASSERT_EQ((unsigned char)s.buf[s.span_start - ZSI_TERMLEN_LONG],
              ZSI_COMMIT_LONG);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));

    /* A counting callback, since the span holds far more than the 64-entry
     * collector holds -- and counting is the assertion that matters: every record
     * of a large span must be presented, not just the ones before some limit. */
    size_t count = 0;
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN, count_cb, &count));
    ASSERT_EQU(count, nrecs);
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    ASSERT(nrecs > 2000);       /* enough records that the span really is large */

    (void)c;
    zsi_file_release(&f);
    sb_free(&s);
}

/*
 * ============================================================
 * The private index (section 5.4)
 * ============================================================
 */

/* Build a hand-made unordered file and index it.  Leaves the open file in *fp. */
static int index_file(struct sb *s, uint32_t gen, struct zsi_file **fp)
{
    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    if (mkdbdir() != 0) return -1;
    if (sb_write(s, name) != 0) return -1;
    if (zsi_file_open(dbdir, name, gen, TEST_EXTERNAL_CSUM, fp) != ZS_OK)
        return -1;
    return zsi_index_build(*fp, zsi_compar_default);
}

/* Fold ONE record into an index.
 *
 * The fold takes a sorted run of records with one entry per key (D-13b), and a
 * single record is a run of one.  The delta tests below predate the run and go a
 * record at a time, which is still the same path -- and still the shape a
 * one-record transaction commits. */
static int index_insert1(struct zsi_index *ix, zs_compar *compar, size_t off)
{
    return zsi_index_fold_run(ix, compar, &off, 1);
}

/* Walk the index and join its keys with '|', so an ordering assertion reads as
 * one string comparison rather than a loop. */
static void index_keys(struct zsi_file *f, char *out, size_t outlen)
{
    struct zsi_index_cur c;
    struct zsi_rec r;
    size_t used = 0;

    out[0] = '\0';
    zsi_index_cur_seek_first(&c);
    while (zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
        size_t need = r.keylen + 2;
        if (used + need >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, r.key, r.keylen);
        used += r.keylen;
        out[used] = '\0';
        zsi_index_cur_next(f->index, zsi_compar_default, &c);
    }
}

/* Walk from a seek point, same joining. */
static void index_keys_from(struct zsi_file *f, const char *key, size_t keylen,
                            char *out, size_t outlen)
{
    struct zsi_index_cur c;
    struct zsi_rec r;
    size_t used = 0;

    out[0] = '\0';
    zsi_index_cur_seek(f->index, zsi_compar_default, key, keylen, &c);
    while (zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
        size_t need = r.keylen + 2;
        if (used + need >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, r.key, r.keylen);
        used += r.keylen;
        out[used] = '\0';
        zsi_index_cur_next(f->index, zsi_compar_default, &c);
    }
}

static void test_index_committed_only(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;

    /* D-13a: the index reflects COMMITTED spans only.  A key whose only record is
     * in a rolled-back span must not be in the index.
     *
     * This is the test that catches "walk every record" as an implementation
     * shortcut -- which is a tempting simplification, and which resurrects aborted
     * writes. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "live", 4, "1", 1);
    sb_term(&s, false);
    sb_rec(&s, "aborted", 7, "x", 1);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "live", 4, &off));
    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "aborted", 7, &off),
              ZS_NOTFOUND);

    char keys[256];
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "live");
    zsi_file_release(&f);
    sb_free(&s);

    /* A key committed, then rewritten in a rolled-back span: the committed
     * version survives and the aborted rewrite is invisible.  An implementation
     * that walked every record would return the aborted value. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "good", 4);
    sb_term(&s, false);
    sb_rec(&s, "k", 1, "bad", 3);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    struct zsi_rec r;
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             &r));
    ASSERT_EQU(r.vallen, 4u);
    ASSERT_MEM_EQ(r.val, "good", 4);
    zsi_file_release(&f);
    sb_free(&s);

    /* A key deleted in a rolled-back span stays present. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "v", 1);
    sb_term(&s, false);
    sb_rec(&s, "k", 1, NULL, 0);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             &r));
    ASSERT_NOT_NULL(r.val);
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_index_ordered_traversal(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    char keys[512];

    /* Keys inserted in a scrambled order traverse in comparator order (D-13). */
    static const char *ins[] = { "m", "d", "z", "a", "q", "b", "y", "c" };
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < sizeof(ins) / sizeof(ins[0]); i++)
        sb_rec(&s, ins[i], strlen(ins[i]), "v", 1);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c|d|m|q|y|z");

    /* Lower-bound seek: to a key present, to one absent between two present, to
     * one before all, and to one after all. */
    index_keys_from(f, "m", 1, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "m|q|y|z");

    index_keys_from(f, "e", 1, keys, sizeof(keys));     /* absent, between d and m */
    ASSERT_STR_EQ(keys, "m|q|y|z");

    index_keys_from(f, "", 0, keys, sizeof(keys));      /* before all */
    ASSERT_STR_EQ(keys, "a|b|c|d|m|q|y|z");

    index_keys_from(f, "zz", 2, keys, sizeof(keys));    /* after all */
    ASSERT_STR_EQ(keys, "");

    index_keys_from(f, "z", 1, keys, sizeof(keys));     /* exactly the last */
    ASSERT_STR_EQ(keys, "z");

    /* The comparator's own ordering is used, including the shorter-key-first rule
     * (F-11a), not byte order alone. */
    zsi_file_release(&f);
    sb_free(&s);

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "abc", 3, "v", 1);
    sb_rec(&s, "ab", 2, "v", 1);
    sb_rec(&s, "a", 1, "v", 1);
    sb_rec(&s, "b", 1, "v", 1);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|ab|abc|b");
    zsi_file_release(&f);
    sb_free(&s);
}

static void test_index_delta(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;
    struct zsi_rec r;

    /* Build a base, then insert past the merge boundary one record at a time,
     * asserting lookup and traversal stay correct throughout -- including across
     * the merge, which is the step most likely to lose or duplicate an entry.
     *
     * The records are all really present in the file; only the index's knowledge
     * of them arrives incrementally, which is what a writer folding in its own
     * commits looks like (D-13b). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "base0", 5, "b", 1);
    sb_rec(&s, "base1", 5, "b", 1);
    sb_term(&s, false);

    /* Record the offsets of everything appended after the base span. */
    size_t offs[ZSI_DELTA_MAX + 40];
    size_t n = 0;
    for (n = 0; n < ZSI_DELTA_MAX + 32; n++) {
        char k[32];
        snprintf(k, sizeof(k), "d%06zu", n);
        offs[n] = s.len;
        sb_rec(&s, k, strlen(k), "v", 1);
    }
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));

    /* Build the index over the FIRST span only, by replaying a truncated view.
     * Simpler: build over everything, then reset to just the base entries and
     * re-insert.  Instead of contriving that, build normally and then assert the
     * insert path against a fresh index containing only the base span's keys. */
    ASSERT_OK(zsi_index_build(f, zsi_compar_default));
    size_t total_keys = f->index->nbase;
    ASSERT_EQU(total_keys, 2u + n);

    /* Now exercise insert directly: empty the index down to nothing and add the
     * records back one at a time. */
    free(f->index->base);
    f->index->base = NULL;
    f->index->nbase = 0;
    f->index->ndelta = 0;

    bool merged_at_least_once = false;
    for (size_t i = 0; i < n; i++) {
        ASSERT_OK(index_insert1(f->index, zsi_compar_default, offs[i]));
        if (f->index->nbase > 0) merged_at_least_once = true;

        /* Every key inserted so far is findable, and none that follow are. */
        char k[32];
        snprintf(k, sizeof(k), "d%06zu", i);
        ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, k, strlen(k), &off));
        ASSERT_EQU(off, offs[i]);

        snprintf(k, sizeof(k), "d%06zu", i + 1);
        if (i + 1 < n)
            ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, k, strlen(k),
                                     &off), ZS_NOTFOUND);
    }

    /* The delta really did overflow into the base at least once. */
    ASSERT(merged_at_least_once);
    ASSERT(f->index->ndelta <= ZSI_DELTA_MAX);

    /* And traversal yields every key exactly once, in order, across the merge
     * boundary. */
    struct zsi_index_cur c;
    zsi_index_cur_seek_first(&c);
    size_t seen = 0;
    const char *prev = NULL;
    size_t prevlen = 0;
    char prevbuf[32];
    while (zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
        if (prev) {
            if (zsi_compar_default(prev, prevlen, r.key, r.keylen) >= 0) {
                fprintf(stderr, "\n    FAIL out of order at %zu\n", seen);
                current_test_failed = 1;
                zsi_file_release(&f);
                sb_free(&s);
                return;
            }
        }
        memcpy(prevbuf, r.key, r.keylen);
        prev = prevbuf;
        prevlen = r.keylen;
        seen++;
        zsi_index_cur_next(f->index, zsi_compar_default, &c);
    }
    ASSERT_EQU(seen, n);

    zsi_file_release(&f);
    sb_free(&s);
}

static void test_index_delta_shadows_base(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;
    struct zsi_rec r;
    char keys[256];

    /* A key present in BOTH base and delta must yield the delta's record in
     * lookup AND in traversal, and must be traversed exactly once.
     *
     * Yielding it twice is the failure mode: the delta's copy, then the base's
     * stale one on the following step.  That breaks the guarantee a per-file
     * cursor never yields the same key twice (D-14h), one level below where that
     * rule is stated. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "old", 3);
    sb_rec(&s, "b", 1, "bee", 3);
    sb_rec(&s, "c", 1, "see", 3);
    sb_term(&s, false);
    size_t newer_a = s.len;
    sb_rec(&s, "a", 1, "new", 3);
    size_t newer_c = s.len;
    sb_rec(&s, "c", 1, "cee", 3);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default));

    /* Reduce the index to the first span's three records, then add the two newer
     * ones through the insert path, so they land in the delta over a base that
     * already holds those keys. */
    struct zsi_index *ix = f->index;
    ASSERT_EQU(ix->nbase, 3u);
    /* rebuild base as the three ORIGINAL offsets, in key order */
    ix->nbase = 0;
    free(ix->base);
    ix->base = malloc(3 * sizeof(size_t));
    ASSERT_NOT_NULL(ix->base);
    ix->base[0] = ZSI_HEADER_LEN;                                   /* a */
    ix->base[1] = ZSI_HEADER_LEN + zsi_rec_encoded_len(1, 3, false);
    ix->base[2] = ix->base[1] + zsi_rec_encoded_len(1, 3, false);
    ix->nbase = 3;
    ix->ndelta = 0;

    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c");

    ASSERT_OK(index_insert1(ix, zsi_compar_default, newer_a));
    ASSERT_OK(index_insert1(ix, zsi_compar_default, newer_c));
    ASSERT_EQU(ix->ndelta, 2u);

    /* Lookup prefers the delta. */
    ASSERT_OK(zsi_index_find(ix, zsi_compar_default, "a", 1, &off));
    ASSERT_EQU(off, newer_a);
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             &r));
    ASSERT_MEM_EQ(r.val, "new", 3);

    ASSERT_OK(zsi_index_find(ix, zsi_compar_default, "c", 1, &off));
    ASSERT_EQU(off, newer_c);

    /* b is only in the base, and still findable. */
    ASSERT_OK(zsi_index_find(ix, zsi_compar_default, "b", 1, &off));
    ASSERT_EQU(off, ix->base[1]);

    /* Traversal yields each key ONCE -- "a|b|c", not "a|a|b|c|c". */
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c");

    /* And the values traversed are the newer ones. */
    struct zsi_index_cur c;
    zsi_index_cur_seek_first(&c);
    ASSERT_OK(zsi_index_cur_get(ix, zsi_compar_default, &c, &r, &off));
    ASSERT_EQU(off, newer_a);
    ASSERT_MEM_EQ(r.val, "new", 3);
    zsi_index_cur_next(ix, zsi_compar_default, &c);
    ASSERT_OK(zsi_index_cur_get(ix, zsi_compar_default, &c, &r, &off));
    ASSERT_MEM_EQ(r.key, "b", 1);
    zsi_index_cur_next(ix, zsi_compar_default, &c);
    ASSERT_OK(zsi_index_cur_get(ix, zsi_compar_default, &c, &r, &off));
    ASSERT_EQU(off, newer_c);
    ASSERT_MEM_EQ(r.val, "cee", 3);
    zsi_index_cur_next(ix, zsi_compar_default, &c);
    ASSERT_EQ(zsi_index_cur_get(ix, zsi_compar_default, &c, &r, &off), ZS_DONE);

    /* Seeking into the middle still merges correctly. */
    index_keys_from(f, "b", 1, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "b|c");
    index_keys_from(f, "a", 1, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c");

    /* Re-inserting the same key replaces its delta entry rather than adding one,
     * which is what keeps the delta at one entry per key. */
    ASSERT_OK(index_insert1(ix, zsi_compar_default, newer_a));
    ASSERT_EQU(ix->ndelta, 2u);

    zsi_file_release(&f);
    sb_free(&s);
}

static void test_index_delta_merge_with_duplicates(void)
{
    /* The merge, with every key present in BOTH base and delta.
     *
     * This is the case the other two delta tests each miss half of:
     * test_index_delta overflows the delta but with all-distinct keys, so the
     * merge's tie arm never runs; test_index_delta_shadows_base has duplicates but
     * only two delta entries, so no merge fires.  Mutation testing found the hole
     * -- a merge that keeps the base's stale record, or that fails to consume the
     * tied base entry, passed both.
     *
     * The failure it guards against is silent and delayed: correct results until
     * enough writes accumulate to trigger a merge, then stale values for every key
     * rewritten before it. */
    enum { N = ZSI_DELTA_MAX + 100 };
    struct sb s;
    struct zsi_file *f = NULL;
    size_t *old_off = malloc(N * sizeof(size_t));
    size_t *new_off = malloc(N * sizeof(size_t));
    ASSERT_NOT_NULL(old_off);
    ASSERT_NOT_NULL(new_off);

    /* Span one: every key with value "old".  Keys are generated in ascending
     * order, so these offsets are already in comparator order. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k%06zu", i);
        old_off[i] = s.len;
        sb_rec(&s, k, strlen(k), "old", 3);
    }
    sb_term(&s, false);

    /* Span two: the same keys with value "new". */
    for (size_t i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k%06zu", i);
        new_off[i] = s.len;
        sb_rec(&s, k, strlen(k), "new", 3);
    }
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default));

    /* Reset the index to hold only span one, then feed span two through the
     * insert path -- which is what a writer folding in its own commits does. */
    struct zsi_index *ix = f->index;
    free(ix->base);
    ix->base = malloc(N * sizeof(size_t));
    ASSERT_NOT_NULL(ix->base);
    memcpy(ix->base, old_off, N * sizeof(size_t));
    ix->nbase = N;
    ix->ndelta = 0;

    bool merged = false;
    for (size_t i = 0; i < N; i++) {
        size_t before = ix->nbase;
        ASSERT_OK(index_insert1(ix, zsi_compar_default, new_off[i]));
        if (ix->ndelta == 0 && before == N) merged = true;

        /* Whatever side it currently lives on, the newer record is what resolves. */
        char k[32];
        size_t off;
        snprintf(k, sizeof(k), "k%06zu", i);
        ASSERT_OK(zsi_index_find(ix, zsi_compar_default, k, strlen(k), &off));
        ASSERT_EQU(off, new_off[i]);
    }

    ASSERT(merged);                     /* the merge really did run */

    /* The merge collapsed each key to one base entry rather than accumulating
     * both versions -- nbase is N, not 2N.  Note that nbase + ndelta is
     * deliberately NOT asserted to be N: after the merge the remaining inserts
     * land in a now-empty delta for keys the base already holds, so a key living
     * on both sides is the ordinary steady state and summing the arrays does not
     * count keys.  The merged view is what must hold N, asserted by the traversal
     * below. */
    ASSERT_EQU(ix->nbase, (size_t)N);
    ASSERT(ix->ndelta <= ZSI_DELTA_MAX);

    /* Every key resolves to its newer record, and the base did not grow to 2N. */
    for (size_t i = 0; i < N; i++) {
        char k[32];
        size_t off;
        snprintf(k, sizeof(k), "k%06zu", i);
        if (zsi_index_find(ix, zsi_compar_default, k, strlen(k), &off) != ZS_OK
            || off != new_off[i]) {
            fprintf(stderr, "\n    FAIL %s resolves to %zu, expected %zu\n",
                    k, off, new_off[i]);
            current_test_failed = 1;
            goto done;
        }
    }

    /* Traversal yields exactly N keys, each once, in order, all with "new". */
    {
        struct zsi_index_cur c;
        struct zsi_rec r;
        size_t seen = 0;
        char prev[32];
        size_t prevlen = 0;

        zsi_index_cur_seek_first(&c);
        while (zsi_index_cur_get(ix, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
            if (prevlen &&
                zsi_compar_default(prev, prevlen, r.key, r.keylen) >= 0) {
                fprintf(stderr, "\n    FAIL duplicate or misordered key at %zu\n",
                        seen);
                current_test_failed = 1;
                goto done;
            }
            if (r.vallen != 3 || memcmp(r.val, "new", 3) != 0) {
                fprintf(stderr, "\n    FAIL stale value at %zu\n", seen);
                current_test_failed = 1;
                goto done;
            }
            memcpy(prev, r.key, r.keylen);
            prevlen = r.keylen;
            seen++;
            zsi_index_cur_next(ix, zsi_compar_default, &c);
        }
        ASSERT_EQU(seen, (size_t)N);
    }

done:
    zsi_file_release(&f);
    sb_free(&s);
    free(old_off);
    free(new_off);
}

static void test_index_binary_keys(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;
    char keys[256];

    /* Keys containing NUL bytes order and look up by length-authoritative
     * comparison (F-13), not by C-string semantics -- an index that used strcmp
     * would collapse all of these to "a". */
    const char k1[] = { 'a', '\0', '1' };
    const char k2[] = { 'a', '\0', '2' };
    const char k3[] = { 'a' };
    const char k4[] = { 'a', '\0' };

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, k2, 3, "v2", 2);
    sb_rec(&s, k1, 3, "v1", 2);
    sb_rec(&s, k4, 2, "v4", 2);
    sb_rec(&s, k3, 1, "v3", 2);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    /* Four distinct keys, ordered shortest-prefix first (F-11a). */
    struct zsi_index_cur c;
    struct zsi_rec r;
    zsi_index_cur_seek_first(&c);
    size_t n = 0;
    size_t lens[8];
    while (zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
        lens[n++] = r.keylen;
        zsi_index_cur_next(f->index, zsi_compar_default, &c);
    }
    ASSERT_EQU(n, 4u);
    ASSERT_EQU(lens[0], 1u);            /* "a"      */
    ASSERT_EQU(lens[1], 2u);            /* "a\0"    */
    ASSERT_EQU(lens[2], 3u);            /* "a\0 1"  */
    ASSERT_EQU(lens[3], 3u);            /* "a\0 2"  */

    /* Each is findable by its own exact bytes and length. */
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, k1, 3, &off));
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, k2, 3, &off));
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, k3, 1, &off));
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, k4, 2, &off));

    (void)keys;
    zsi_file_release(&f);
    sb_free(&s);
}

/*
 * ============================================================
 * Pointer section and trailer (T-2a, part of T-6)
 * ============================================================
 */

static void test_inorder_empty(void)
{
    /* F-26g: a zero-record in-order file is legal and expected -- a repack that
     * drops every key produces one (D-22).  Three properties are pinned:
     *
     *   - it is EXACTLY 96 bytes: 72 header, 8-byte PTRS32 with count 0, 16-byte
     *     trailer.  Asserted against the literal, so a layout change fails here;
     *   - it is PTRS32, since F-26c's condition holds vacuously with no offsets,
     *     making the file byte-identical every time it is produced;
     *   - its records checksum is the engine's value for EMPTY INPUT, not zero.
     */
    struct ib b;
    struct zsi_file *f = NULL;

    ib_init(&b, 5, 5, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    ASSERT_EQU(b.len, 96u);

    ASSERT_EQ(ib_load(&b, 5, 5, &f), ZS_OK);
    ASSERT_EQU(f->size, 96u);
    ASSERT_EQU(f->nptrs, 0u);
    ASSERT(!f->ptr_wide);
    ASSERT_EQU(f->ptr_off, (size_t)ZSI_HEADER_LEN);
    ASSERT_EQ((unsigned char)f->base[ZSI_HEADER_LEN], ZSI_PTRS32);

    /* Not zero: 0x38D394C2 is XXH3-64's low half over empty input. */
    ASSERT_EQU(f->records_csum, 0x38D394C2u);
    ASSERT_OK(zsi_ptrs_verify_records(f));

    /* Searching it is an ordinary case, not a special one (D-14b). */
    uint64_t idx;
    bool exact;
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "any", 3, &idx, &exact));
    ASSERT_EQU(idx, 0u);
    ASSERT(!exact);
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "", 0, &idx, &exact));
    ASSERT_EQU(idx, 0u);
    ASSERT(!exact);
    zsi_file_release(&f);

    /* Byte-identical every time it is produced. */
    struct ib b2;
    ib_init(&b2, 5, 5, ZSI_CSUM_XXHASH);
    ib_finish(&b2);
    ASSERT_EQU(b2.len, b.len);
    ASSERT_MEM_EQ(b2.buf, b.buf, b.len);
    ib_free(&b2);

    /* Under engine 0 the records checksum is zero, because that engine writes
     * zeros for everything -- which is a different thing from the empty-input
     * value above, and the contrast is the point. */
    struct ib b0;
    ib_init(&b0, 5, 5, ZSI_CSUM_NONE);
    ib_finish(&b0);
    ASSERT_EQU(b0.len, 96u);
    ASSERT_EQ(ib_load(&b0, 5, 5, &f), ZS_OK);
    ASSERT_EQU(f->records_csum, 0u);
    ASSERT_OK(zsi_ptrs_verify_records(f));
    zsi_file_release(&f);
    ib_free(&b0);

    ib_free(&b);
}

static void test_inorder_search(void)
{
    /* The general exact/miss/end cases are covered exhaustively by
     * test_inorder_probe_ends_agrees' differential bisection; what nothing else
     * reaches is the smallest files, where D-14d's first/last probe degenerates
     * (at n == 1 first IS last). */
    struct ib b;
    struct zsi_file *f = NULL;
    uint64_t idx;
    bool exact;
    static const char *keys[] = { "b", "d", "f", "h", "j" };

    /* One record, and two records: the sizes where an off-by-one in the probe or
     * the bisection shows up. */
    for (size_t n = 1; n <= 2; n++) {
        ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
        for (size_t i = 0; i < n; i++)
            ib_rec(&b, keys[i * 2], 1, "v", 1);
        ib_finish(&b);
        ASSERT_EQ(ib_load(&b, 1, 1, &f), ZS_OK);
        ASSERT_EQU(f->nptrs, n);

        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "b", 1, &idx, &exact));
        ASSERT(exact); ASSERT_EQU(idx, 0u);
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "a", 1, &idx, &exact));
        ASSERT(!exact); ASSERT_EQU(idx, 0u);
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "z", 1, &idx, &exact));
        ASSERT(!exact); ASSERT_EQU(idx, n);
        zsi_file_release(&f);
        ib_free(&b);
    }
}

static void test_inorder_trailer_negatives(void)
{
    /* T-2a.  Opening an in-order file depends entirely on the trailer, so each of
     * these must be rejected rather than read.  Every case rewrites the section
     * checksum where the damage would otherwise be caught by it, so each asserts
     * the structural rule it names rather than incidentally tripping the
     * checksum. */
    struct ib b;
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];

    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "1", 1);
    ib_rec(&b, "b", 1, "2", 1);
    ib_finish(&b);

    size_t full = b.len;
    char *orig = malloc(full);
    ASSERT_NOT_NULL(orig);
    memcpy(orig, b.buf, full);

    /* Re-checksum the section after damaging it, so only the structural rule can
     * reject.  cover = [ptr_off, size-4). */
    size_t ptr_off = zsi_get64(orig + full - 16);

    struct { const char *what; long long backptr; } bad[] = {
        { "past the end of the file",   (long long)full + 8 },
        { "before the header",          8 },
        { "at the header",              ZSI_HEADER_LEN - 8 },
        { "not 8-aligned",              (long long)ptr_off + 4 },
        { "inside the trailer",         (long long)full - 8 },
        { "overlapping the trailer",    (long long)full - 16 }
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        memcpy(b.buf, orig, full);
        zsi_put64(b.buf + full - 16, (uint64_t)bad[i].backptr);
        /* recompute the section checksum over whatever the back pointer now
         * designates, where that is even inside the file */
        if (bad[i].backptr >= 0 && (size_t)bad[i].backptr < full - 4) {
            size_t cov = (full - 4) - (size_t)bad[i].backptr;
            zsi_put32(b.buf + full - 4,
                      zsi_csum_xxhash(b.buf + bad[i].backptr, cov));
        }
        ASSERT_EQ(writefile(name, b.buf, full), 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        if (zsi_ptrs_load(f) == ZS_OK) {
            fprintf(stderr, "\n    FAIL back pointer %s accepted\n", bad[i].what);
            current_test_failed = 1;
            zsi_file_release(&f);
            goto done;
        }
        zsi_file_release(&f);
    }

    /* A back pointer to a byte that is neither PTRS32 nor PTRS64. */
    memcpy(b.buf, orig, full);
    zsi_put64(b.buf + full - 16, ZSI_HEADER_LEN);   /* points at a data record */
    {
        size_t cov = (full - 4) - ZSI_HEADER_LEN;
        zsi_put32(b.buf + full - 4,
                  zsi_csum_xxhash(b.buf + ZSI_HEADER_LEN, cov));
    }
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADFORMAT);
    zsi_file_release(&f);

    /* A file shorter than header plus trailer, at every length. */
    for (size_t len = 0; len < ZSI_HEADER_LEN + ZSI_TRAILER_LEN; len += 8) {
        ASSERT_EQ(writefile(name, orig, len), 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        ASSERT(zsi_ptrs_load(f) != ZS_OK);
        zsi_file_release(&f);
    }

    /* A corrupted pad byte inside the section is caught by the section checksum
     * (F-26d says the checksum covers the padding). */
    memcpy(b.buf, orig, full);
    ASSERT(full - ZSI_TRAILER_LEN > ptr_off + 8);
    b.buf[full - ZSI_TRAILER_LEN - 1] ^= 0x01;
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADCHECKSUM);
    zsi_file_release(&f);

    /* A corrupted count: caught by the section checksum, and if the checksum is
     * recomputed, by the section-length equality. */
    memcpy(b.buf, orig, full);
    zsi_put32(b.buf + ptr_off + 4, 9999);
    {
        size_t cov = (full - 4) - ptr_off;
        zsi_put32(b.buf + full - 4, zsi_csum_xxhash(b.buf + ptr_off, cov));
    }
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADFORMAT);
    zsi_file_release(&f);

    /* A pointer outside the records region (F-27), with the checksum fixed up. */
    memcpy(b.buf, orig, full);
    zsi_put32(b.buf + ptr_off + 8, (uint32_t)(full - 8));   /* into the trailer */
    {
        size_t cov = (full - 4) - ptr_off;
        zsi_put32(b.buf + full - 4, zsi_csum_xxhash(b.buf + ptr_off, cov));
    }
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADFORMAT);
    zsi_file_release(&f);

    /* A pointer that is not 8-aligned (F-27). */
    memcpy(b.buf, orig, full);
    zsi_put32(b.buf + ptr_off + 8, ZSI_HEADER_LEN + 4);
    {
        size_t cov = (full - 4) - ptr_off;
        zsi_put32(b.buf + full - 4, zsi_csum_xxhash(b.buf + ptr_off, cov));
    }
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADFORMAT);
    zsi_file_release(&f);

done:
    free(orig);
    ib_free(&b);
}

static void test_inorder_records_checksum(void)
{
    /* F-26e/F-26f: a record body corrupted IN PLACE is detectable, but only on
     * demand.  Nothing else in an in-order file would notice -- there are no span
     * terminators here -- which is why this checksum exists at all. */
    struct ib b;
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];
    struct zsi_rec r;

    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    ib_finish(&b);

    /* Damage a value byte, leaving every length and pointer intact. */
    size_t voff = ZSI_HEADER_LEN + 4 + 1 + 1;       /* first value byte */
    b.buf[voff] = 'V';

    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));

    /* Opening succeeds and stays O(1): the records region is never read. */
    ASSERT_OK(zsi_ptrs_load(f));
    ASSERT_EQU(f->nptrs, 2u);

    /* Records still read -- with the corrupted value, undetected so far. */
    ASSERT_OK(zsi_ptrs_rec(f, 0, &r));
    ASSERT_MEM_EQ(r.val, "Value", 5);

    /* And the on-demand check reports it. */
    ASSERT_EQ(zsi_ptrs_verify_records(f), ZS_BADCHECKSUM);
    zsi_file_release(&f);

    /* Undamaged, the same check passes. */
    ib_free(&b);
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    ib_finish(&b);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_ptrs_load(f));
    ASSERT_OK(zsi_ptrs_verify_records(f));
    zsi_file_release(&f);
    ib_free(&b);
}

static void test_inorder_widths_and_padding(void)
{
    struct ib b;
    struct zsi_file *f = NULL;

    /* F-26c: a file whose offsets all fit is written as PTRS32.  F-26d: the
     * narrow section pads to a multiple of 8, so the pad is 4 bytes for an odd
     * count and 0 for an even one.  Both cases round-trip. */
    for (size_t n = 1; n <= 4; n++) {
        ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
        for (size_t i = 0; i < n; i++) {
            char k[8];
            snprintf(k, sizeof(k), "k%zu", i);
            ib_rec(&b, k, strlen(k), "v", 1);
        }
        size_t records_end = b.len;
        ib_finish(&b);

        /* section = 8 + 4n, rounded up to 8 */
        size_t expect_sec = ((8 + 4 * n) + 7) & ~(size_t)7;
        ASSERT_EQU(b.len, records_end + expect_sec + ZSI_TRAILER_LEN);
        ASSERT_EQ((unsigned char)b.buf[records_end], ZSI_PTRS32);

        /* the pad, where there is one, is zero */
        if ((8 + 4 * n) % 8 != 0) {
            ASSERT_EQ((unsigned char)b.buf[records_end + 8 + 4 * n], 0);
            ASSERT_EQ((unsigned char)b.buf[records_end + 8 + 4 * n + 3], 0);
        }

        ASSERT_EQ(ib_load(&b, 1, 1, &f), ZS_OK);
        ASSERT_EQU(f->nptrs, n);
        ASSERT(!f->ptr_wide);
        ASSERT_OK(zsi_ptrs_verify_records(f));
        zsi_file_release(&f);
        ib_free(&b);
    }
}

static void test_inorder_ptrs64(void)
{
    /* A hand-constructed PTRS64 file, so the wide form is covered without writing
     * 4GB of real data (T-6).  The section states its own width, so a reader must
     * honour what it says rather than inferring from the file size. */
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];
    struct zsi_rec r;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_format(name, test_uuid, 1, 1);

    /* Two records, then a wide section by hand. */
    size_t reclen = zsi_rec_encoded_len(1, 1, false);
    size_t records_end = ZSI_HEADER_LEN + 2 * reclen;
    size_t seclen = 16 + 2 * 8;
    size_t total = records_end + seclen + ZSI_TRAILER_LEN;

    char *buf = calloc(1, total);
    ASSERT_NOT_NULL(buf);
    make_header(buf, 1, 1, ZSI_CSUM_XXHASH);
    zsi_rec_encode(buf + ZSI_HEADER_LEN, zsi_csum_xxhash, "a", 1, "1", 1);
    zsi_rec_encode(buf + ZSI_HEADER_LEN + reclen, zsi_csum_xxhash, "b", 1,
                   "2", 1);

    buf[records_end] = (char)ZSI_PTRS64;
    zsi_put64(buf + records_end + 8, 2);
    zsi_put64(buf + records_end + 16, ZSI_HEADER_LEN);
    zsi_put64(buf + records_end + 24, ZSI_HEADER_LEN + reclen);

    zsi_put64(buf + records_end + seclen, (uint64_t)records_end);
    zsi_put32(buf + records_end + seclen + 8,
              zsi_csum_xxhash(buf + ZSI_HEADER_LEN, records_end - ZSI_HEADER_LEN));
    zsi_put32(buf + total - 4, zsi_csum_xxhash(buf + records_end, seclen + 12));

    ASSERT_EQ(writefile(name, buf, total), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_ptrs_load(f));
    ASSERT(f->ptr_wide);
    ASSERT_EQU(f->nptrs, 2u);
    ASSERT_EQU(zsi_ptrs_at(f, 0), (uint64_t)ZSI_HEADER_LEN);
    ASSERT_EQU(zsi_ptrs_at(f, 1), (uint64_t)(ZSI_HEADER_LEN + reclen));

    ASSERT_OK(zsi_ptrs_rec(f, 0, &r));
    ASSERT_MEM_EQ(r.key, "a", 1);
    ASSERT_OK(zsi_ptrs_rec(f, 1, &r));
    ASSERT_MEM_EQ(r.key, "b", 1);
    ASSERT_OK(zsi_ptrs_verify_records(f));

    uint64_t idx;
    bool exact;
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "b", 1, &idx, &exact));
    ASSERT(exact);
    ASSERT_EQU(idx, 1u);

    zsi_file_release(&f);
    free(buf);
}

static void test_inorder_kind_rules(void)
{
    /* Loading a pointer section from an UNORDERED file is a usage error: it has
     * none, and the kind is knowable from the header alone (section 2). */
    struct sb s;
    struct zsi_file *f = NULL;

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT(zsi_file_is_unordered(f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADUSAGE);
    zsi_file_release(&f);
    sb_free(&s);

    /* And a pointers block is present exactly when end != 0 (T-6): an in-order
     * file loads one, an unordered file has none. */
    struct ib b;
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "1", 1);
    ib_finish(&b);
    ASSERT_EQ(ib_load(&b, 3, 3, &f), ZS_OK);
    ASSERT(!zsi_file_is_unordered(f));
    ASSERT_EQU(f->nptrs, 1u);
    zsi_file_release(&f);
    ib_free(&b);
}

static void test_inorder_probe_ends_agrees(void)
{
    /* D-14d's first-and-last probe is a search strategy and cannot change the
     * answer.  Here the probe path is compared against a plain bisection over the
     * same file, for keys below the first, above the last, equal to each end, and
     * in between -- which is what T-5a asks for at the read-path level and is
     * cheaper to assert directly. */
    struct ib b;
    struct zsi_file *f = NULL;

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    static const char *keys[] = { "c", "e", "g", "i", "k", "m" };
    for (size_t i = 0; i < 6; i++)
        ib_rec(&b, keys[i], 1, "v", 1);
    ib_finish(&b);
    ASSERT_EQ(ib_load(&b, 1, 1, &f), ZS_OK);

    for (unsigned ch = 'a'; ch <= 'o'; ch++) {
        char k = (char)ch;
        uint64_t got_idx;
        bool got_exact;
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, &k, 1,
                                  &got_idx, &got_exact));

        /* An independent plain bisection, written here rather than reused, so it
         * cannot share a bug with the implementation. */
        uint64_t lo = 0, hi = f->nptrs;
        while (lo < hi) {
            uint64_t mid = lo + (hi - lo) / 2;
            struct zsi_rec r;
            ASSERT_OK(zsi_ptrs_rec(f, mid, &r));
            if (zsi_compar_default(r.key, r.keylen, &k, 1) < 0) lo = mid + 1;
            else hi = mid;
        }
        bool want_exact = false;
        if (lo < f->nptrs) {
            struct zsi_rec r;
            ASSERT_OK(zsi_ptrs_rec(f, lo, &r));
            want_exact = (zsi_compar_default(r.key, r.keylen, &k, 1) == 0);
        }

        if (got_idx != lo || got_exact != want_exact) {
            fprintf(stderr, "\n    FAIL key '%c': probe gave (%llu,%d), "
                    "bisection (%llu,%d)\n", (char)ch,
                    (unsigned long long)got_idx, (int)got_exact,
                    (unsigned long long)lo, (int)want_exact);
            current_test_failed = 1;
            zsi_file_release(&f);
            ib_free(&b);
            return;
        }
    }

    zsi_file_release(&f);
    ib_free(&b);
}

/*
 * ============================================================
 * The per-file cursor
 * ============================================================
 */

/* Walk a per-file cursor from a seek point, joining keys with '|'. */
static void fcur_keys_from(struct zsi_fcur *fc, const char *key, size_t keylen,
                           char *out, size_t outlen)
{
    size_t used = 0;

    out[0] = '\0';
    if (key) assert(zsi_fcur_seek(fc, key, keylen) == ZS_OK);
    else     assert(zsi_fcur_seek_first(fc) == ZS_OK);

    while (!fc->exhausted) {
        if (used + fc->cur.keylen + 2 >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, fc->cur.key, fc->cur.keylen);
        used += fc->cur.keylen;
        out[used] = '\0';
        assert(zsi_fcur_next(fc) == ZS_OK);
    }
}

/* The same six keys, presented as an unordered file and as an in-order file, so
 * every assertion below can be run against both and compared. */
static void build_both_kinds(struct sb *s, struct ib *b,
                             struct zsi_file **uf, struct zsi_file **inf,
                             const char *const *keys, size_t n)
{
    ASSERT_EQ(mkdbdir(), 0);

    sb_init(s, 1, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < n; i++)
        sb_rec(s, keys[i], strlen(keys[i]), "v", 1);
    sb_term(s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(sb_write(s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, uf));
    ASSERT_OK(zsi_index_build(*uf, zsi_compar_default));

    /* the in-order file needs its records in key order */
    char sorted[16][32];
    ASSERT(n <= 16);
    for (size_t i = 0; i < n; i++) snprintf(sorted[i], 32, "%s", keys[i]);
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (zsi_compar_default(sorted[i], strlen(sorted[i]),
                                   sorted[j], strlen(sorted[j])) > 0) {
                char t[32];
                memcpy(t, sorted[i], 32);
                memcpy(sorted[i], sorted[j], 32);
                memcpy(sorted[j], t, 32);
            }

    ib_init(b, 2, 2, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < n; i++)
        ib_rec(b, sorted[i], strlen(sorted[i]), "v", 1);
    ib_finish(b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b->buf, b->len), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, TEST_EXTERNAL_CSUM, inf));
    ASSERT_OK(zsi_ptrs_load(*inf));
}

static void test_fcur_uniform(void)
{
    /* The same assertions driven against both file kinds, asserting they agree.
     *
     * This is the property that lets the read path be written once: if the two
     * kinds disagree about seek or traversal, the merge above them cannot be
     * correct for both, and D-14a's guarantee that all read paths resolve
     * visibility identically (G-7) fails at its foundation. */
    static const char *keys[] = { "c", "a", "e", "b", "d" };
    struct sb s;
    struct ib b;
    struct zsi_file *uf = NULL, *inf = NULL;
    struct zsi_fcur fu, fi;
    char ku[256], ki[256];

    build_both_kinds(&s, &b, &uf, &inf, keys, 5);
    zsi_fcur_init_file(&fu, uf, zsi_compar_default);
    zsi_fcur_init_file(&fi, inf, zsi_compar_default);

    ASSERT_EQ(fu.kind, ZSI_SRC_UNORDERED);
    ASSERT_EQ(fi.kind, ZSI_SRC_INORDER);
    ASSERT_EQU(fu.gen, 1u);
    ASSERT_EQU(fi.gen, 2u);

    /* Full traversal. */
    fcur_keys_from(&fu, NULL, 0, ku, sizeof(ku));
    fcur_keys_from(&fi, NULL, 0, ki, sizeof(ki));
    ASSERT_STR_EQ(ku, "a|b|c|d|e");
    ASSERT_STR_EQ(ki, ku);

    /* Seek to a key present, absent-between-two, before all, after all, and
     * exactly the last -- the two kinds must agree on every one. */
    static const char *probes[] = { "a", "c", "e", "b", "", "aa", "cc", "f", "z" };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        fcur_keys_from(&fu, probes[i], strlen(probes[i]), ku, sizeof(ku));
        fcur_keys_from(&fi, probes[i], strlen(probes[i]), ki, sizeof(ki));
        if (strcmp(ku, ki) != 0) {
            fprintf(stderr, "\n    FAIL seek '%s': unordered '%s', in-order '%s'\n",
                    probes[i], ku, ki);
            current_test_failed = 1;
            goto done;
        }
    }

    /* Seeking past every key exhausts immediately, and advancing an exhausted
     * cursor stays exhausted rather than wrapping or overrunning. */
    ASSERT_OK(zsi_fcur_seek(&fu, "z", 1));
    ASSERT(fu.exhausted);
    ASSERT_OK(zsi_fcur_next(&fu));
    ASSERT(fu.exhausted);
    ASSERT_OK(zsi_fcur_seek(&fi, "z", 1));
    ASSERT(fi.exhausted);
    ASSERT_OK(zsi_fcur_next(&fi));
    ASSERT(fi.exhausted);

    /* Single-key search agrees too, hit and miss. */
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        struct zsi_rec ru, ri;
        int a = zsi_fcur_find(&fu, probes[i], strlen(probes[i]), &ru);
        int c = zsi_fcur_find(&fi, probes[i], strlen(probes[i]), &ri);
        if (a != c) {
            fprintf(stderr, "\n    FAIL find '%s': unordered %d, in-order %d\n",
                    probes[i], a, c);
            current_test_failed = 1;
            goto done;
        }
        if (a == ZS_OK)
            ASSERT_MEM_EQ(ru.key, ri.key, ru.keylen);
    }

done:
    zsi_file_release(&uf);
    zsi_file_release(&inf);
    sb_free(&s);
    ib_free(&b);
}

static void test_fcur_empty_sources(void)
{
    /* An empty source exhausts at seek rather than misbehaving -- D-14e step 1
     * calls this "immediately the case for a source holding no records", and
     * D-14b requires both kinds treat it as ordinary. */
    struct sb s;
    struct ib b;
    struct zsi_file *uf = NULL, *inf = NULL;
    struct zsi_fcur fc;
    struct zsi_rec r;
    char name[ZSI_NAME_MAX];

    ASSERT_EQ(mkdbdir(), 0);

    /* An unordered file with no committed records (F-26h). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &uf));
    ASSERT_OK(zsi_index_build(uf, zsi_compar_default));

    zsi_fcur_init_file(&fc, uf, zsi_compar_default);
    ASSERT_OK(zsi_fcur_seek_first(&fc));
    ASSERT(fc.exhausted);
    ASSERT_OK(zsi_fcur_seek(&fc, "k", 1));
    ASSERT(fc.exhausted);
    ASSERT_OK(zsi_fcur_next(&fc));
    ASSERT(fc.exhausted);
    ASSERT_EQ(zsi_fcur_find(&fc, "k", 1, &r), ZS_NOTFOUND);

    /* An in-order file with zero records (F-26g). */
    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, TEST_EXTERNAL_CSUM, &inf));
    ASSERT_OK(zsi_ptrs_load(inf));

    zsi_fcur_init_file(&fc, inf, zsi_compar_default);
    ASSERT_OK(zsi_fcur_seek_first(&fc));
    ASSERT(fc.exhausted);
    ASSERT_OK(zsi_fcur_seek(&fc, "k", 1));
    ASSERT(fc.exhausted);
    ASSERT_OK(zsi_fcur_next(&fc));
    ASSERT(fc.exhausted);
    ASSERT_EQ(zsi_fcur_find(&fc, "k", 1, &r), ZS_NOTFOUND);
    ASSERT_EQ(zsi_fcur_find(&fc, "", 0, &r), ZS_NOTFOUND);

    /* A transaction source with no transaction is exhausted too, which is what a
     * read transaction looks like. */
    memset(&fc, 0, sizeof(fc));
    fc.kind = ZSI_SRC_TXN;
    fc.compar = zsi_compar_default;
    fc.gen = ZSI_GEN_TXN;
    ASSERT_OK(zsi_fcur_seek_first(&fc));
    ASSERT(fc.exhausted);
    ASSERT_EQ(zsi_fcur_find(&fc, "k", 1, &r), ZS_NOTFOUND);
    ASSERT_EQU(fc.gen, (uint32_t)UINT32_MAX);

    zsi_file_release(&uf);
    zsi_file_release(&inf);
    sb_free(&s);
    ib_free(&b);
}

static void test_fcur_no_duplicate_keys(void)
{
    /* D-14h: a per-file cursor never yields the same key twice.
     *
     * For an unordered file that comes from the private index exposing only the
     * newest committed record per key; for an in-order file it is structural, one
     * record per key (D-17).  The merge above relies on this -- its step 3 handles
     * duplicates ACROSS sources only, so a source that duplicated internally would
     * emit a key twice with no rule to catch it. */
    struct sb s;
    struct zsi_file *f = NULL;
    struct zsi_fcur fc;
    char keys[256];

    /* A key written three times in one file, plus neighbours either side. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_rec(&s, "k", 1, "v1", 2);
    sb_rec(&s, "k", 1, "v2", 2);
    sb_rec(&s, "z", 1, "9", 1);
    sb_rec(&s, "k", 1, "v3", 2);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default));

    zsi_fcur_init_file(&fc, f, zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|k|z");            /* k once, not three times */

    /* And it is the newest version. */
    ASSERT_OK(zsi_fcur_seek(&fc, "k", 1));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.val, "v3", 2);

    zsi_file_release(&f);
    sb_free(&s);
}

static void test_fcur_deletions_visible(void)
{
    /* A per-file cursor presents deletions as records with a NULL value.  It does
     * NOT filter them: resolving a deletion into "absent" is the merge's job
     * (D-14e step 4), and it can only do that if the tombstone reaches it.  A
     * cursor that hid tombstones would let an older file's value resurface. */
    struct sb s;
    struct zsi_file *f = NULL;
    struct zsi_fcur fc;
    char keys[256];

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_rec(&s, "b", 1, "2", 1);
    sb_rec(&s, "b", 1, NULL, 0);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default));

    zsi_fcur_init_file(&fc, f, zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b");

    ASSERT_OK(zsi_fcur_seek(&fc, "b", 1));
    ASSERT(!fc.exhausted);
    ASSERT_NULL(fc.cur.val);                 /* the tombstone reaches the merge */

    struct zsi_rec r;
    ASSERT_OK(zsi_fcur_find(&fc, "b", 1, &r));
    ASSERT_NULL(r.val);

    zsi_file_release(&f);
    sb_free(&s);
}

/*
 * ============================================================
 * The file set (T-9)
 * ============================================================
 */

/* Seed a directory with names only.  The files are EMPTY -- if anything opened
 * them, every one of these tests would fail, which is how T-9's "derived from
 * filenames alone, without opening a file" is asserted rather than assumed. */
static void seed_names(const char *const *names)
{
    /* Clear first.  Without this, successive calls within one test accumulate,
     * and a case silently inherits the previous case's directory -- which is
     * exactly the bug that made two of these tests fail on first run, with
     * symptoms (a spurious ZS_FULL, a wrong resolved set) that pointed at the
     * implementation rather than at the fixture. */
    ASSERT_EQ(mkdbdir(), 0);

    DIR *d = opendir(dbdir);
    ASSERT_NOT_NULL(d);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        unlink(dbpath(de->d_name));
    }
    closedir(d);

    for (size_t i = 0; names[i]; i++)
        ASSERT_EQ(writefile(names[i], "", 0), 0);
}

/* D-1b: the active file's generation is in its HEADER, so a fixture that wants
 * one in the set has to write a real header -- an empty placeholder parses as
 * D-10's "no discoverable generation" and is dropped from the scan, which is
 * correct behaviour and useless as a fixture.  cur_gen == 0 means no active
 * file is expected among `names`. */
static void seed_names_cur(const char *const *names, uint32_t cur_gen)
{
    char cur[ZSI_NAME_MAX];

    seed_names(names);
    if (!cur_gen) return;

    zsi_name_current(cur, test_uuid);
    for (size_t i = 0; names[i]; i++) {
        if (strcmp(names[i], cur) != 0) continue;
        char hdr[ZSI_HEADER_LEN];
        make_header(hdr, cur_gen, 0, ZSI_CSUM_XXHASH);
        ASSERT_EQ(writefile(cur, hdr, sizeof(hdr)), 0);
        return;
    }
}

/* Join the resolved set's names, stripping the common prefix, so an assertion
 * reads as one string. */
static void resolved_gens(struct zsi_fileset *fs, char *out, size_t outlen)
{
    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < fs->nresolved; i++) {
        char buf[32];
        if (fs->resolved[i].end)
            snprintf(buf, sizeof(buf), "%u-%u",
                     fs->resolved[i].start, fs->resolved[i].end);
        else
            snprintf(buf, sizeof(buf), "%u", fs->resolved[i].start);
        size_t n = strlen(buf);
        if (used + n + 2 >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, buf, n);
        used += n;
        out[used] = '\0';
    }
}

/* Format a data-file name for the test UUID into a static rotating buffer.
 * end == 0 means the active file, which under D-1b has no generation in its
 * name -- so `start` is ignored, and there is only ever one such name. */
static const char *dn(uint32_t start, uint32_t end)
{
    static char bufs[8][ZSI_NAME_MAX];
    static int which = 0;
    char *b = bufs[which = (which + 1) % 8];
    if (end == 0) zsi_name_current(b, test_uuid);
    else          zsi_name_format(b, test_uuid, start, end);
    return b;
}

static void test_fileset_overlap_table(void)
{
    /* Each row of D-5a's table.  An overlap is resolved, not rejected: an output
     * is renamed into place before its inputs are removed, so a scan legitimately
     * sees both. */
    struct zsi_fileset fs;
    char got[128];

    /* A repack output [1-4] present with its inputs [1-1]..[4-4].  The widest
     * reach wins, and D-5a orders by reach so it is last. */
    {
        const char *names[] = { dn(1,1), dn(2,2), dn(3,3), dn(4,4), dn(1,4),
                                dn(5,0), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5");
        zsi_fileset_fini(&fs);
    }

    /* The same with some inputs already unlinked: the set still tiles. */
    {
        const char *names[] = { dn(2,2), dn(1,4), dn(5,0), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5");
        zsi_fileset_fini(&fs);
    }

    /* A conversion output present with its input: the IN-ORDER file wins.  Both
     * cover generation 5, and D-5a breaks that tie by kind -- unordered first,
     * so the published form is last.  NOT by name: `.current` collates after
     * every in-order name and would win the sweep (D-5b). */
    {
        const char *names[] = { dn(5,0), dn(5,5), dn(1,4), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5-5");
        zsi_fileset_fini(&fs);
    }

    /* All three at once: unordered N, N-N, and a wider N-M.  The widest wins. */
    {
        const char *names[] = { dn(1,4), dn(5,0), dn(5,5), dn(5,9), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5-9");
        zsi_fileset_fini(&fs);
    }
}

static void test_fileset_first_vs_last(void)
{
    /* D-5b, asserted rather than assumed: taking the FIRST file at each step
     * gives a different -- and wrong -- answer.  T-9 asks for this so the rule is
     * caught by a test rather than rediscovered by a corrupted database. */
    struct zsi_fileset fs;

    const char *names[] = { dn(1,1), dn(1,4), dn(5,0), NULL };
    seed_names_cur(names, 5);
    ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
    ASSERT_OK(zsi_fileset_resolve(&fs));

    /* The implementation took the last: [1-4], then 5. */
    ASSERT_EQU(fs.nresolved, 2u);
    ASSERT_EQU(fs.resolved[0].start, 1u);
    ASSERT_EQU(fs.resolved[0].end, 4u);

    /* Taking the first would have chosen [1-1], leaving generations 2..4
     * unaccounted for -- an incomplete set that discards committed data. */
    ssize_t first = -1;
    for (size_t i = 0; i < fs.nall; i++)
        if (fs.all[i].start == 1) { first = (ssize_t)i; break; }
    ASSERT(first >= 0);
    ASSERT_EQU(fs.all[first].end, 1u);      /* the narrow one sorts first */
    ASSERT(fs.all[first].end != fs.resolved[0].end);

    zsi_fileset_fini(&fs);
}

static void test_fileset_gaps(void)
{
    struct zsi_fileset fs;

    /* D-7: a set that does not tile is not an error to report to the caller -- it
     * is a torn readdir, and the answer is ZS_AGAIN so the snapshot protocol
     * retries (C-4 step 2). */

    /* A missing middle generation. */
    {
        const char *names[] = { dn(1,1), dn(3,3), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_resolve(&fs), ZS_AGAIN);
        zsi_fileset_fini(&fs);
    }

    /* A gap at the bottom is NOT a gap: the set simply starts higher, because
     * older files are removed once repacked (D-6 says "from the oldest surviving
     * generation").  This is the case an implementation that assumed generation 1
     * would wrongly reject. */
    {
        const char *names[] = { dn(5,7), dn(8,0), NULL };
        seed_names_cur(names, 8);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        ASSERT_EQU(fs.nresolved, 2u);
        zsi_fileset_fini(&fs);
    }

    /* A gap immediately after the first file. */
    {
        const char *names[] = { dn(1,2), dn(4,0), NULL };
        seed_names_cur(names, 4);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_resolve(&fs), ZS_AGAIN);
        zsi_fileset_fini(&fs);
    }

    /* D-5c: a PARTIAL overlap -- ranges that intersect where neither contains the
     * other -- cannot arise from any legal sequence and is corruption, reported
     * rather than resolved. */
    {
        const char *names[] = { dn(1,5), dn(3,8), NULL };
        seed_names_cur(names, 4);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_resolve(&fs), ZS_BADFORMAT);
        zsi_fileset_fini(&fs);
    }
    {
        const char *names[] = { dn(1,1), dn(2,6), dn(4,9), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_resolve(&fs), ZS_BADFORMAT);
        zsi_fileset_fini(&fs);
    }

    /* Nesting is NOT partial and must still resolve. */
    {
        const char *names[] = { dn(1,9), dn(3,5), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        ASSERT_EQU(fs.nresolved, 1u);
        ASSERT_EQU(fs.resolved[0].end, 9u);
        zsi_fileset_fini(&fs);
    }
}

static void test_fileset_uuid_discovery(void)
{
    struct zsi_fileset fs;

    /* D-4a: files disagreeing on UUID are rejected, NOT resolved by majority.
     * Adopting one would read half a database and call it whole. */
    static const zsi_uuid_t other = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    /* In-order files throughout: discovery is about the UUID in the name, and
     * D-1b would otherwise make "two files of one database" unbuildable from
     * unordered ones -- there is only one such name per UUID. */
    char othername[ZSI_NAME_MAX];
    zsi_name_format(othername, other, 1, 1);

    const char *names[] = { dn(1, 1), othername, NULL };
    seed_names(names);
    ASSERT_EQ(zsi_fileset_scan(dbdir, NULL, &fs), ZS_BADFORMAT);

    /* Two of one and one of the other: still an error, not a vote. */
    const char *names2[] = { dn(1, 1), dn(2, 2), othername, NULL };
    seed_names(names2);
    ASSERT_EQ(zsi_fileset_scan(dbdir, NULL, &fs), ZS_BADFORMAT);

    /* But when the caller NAMES a uuid, the other database's file is simply not
     * ours and is ignored -- which is what lets two databases share a directory
     * once both are known. */
    ASSERT_OK(zsi_fileset_scan(dbdir, &test_uuid, &fs));
    ASSERT_EQU(fs.nall, 2u);
    ASSERT_OK(zsi_fileset_resolve(&fs));
    zsi_fileset_fini(&fs);

    ASSERT_OK(zsi_fileset_scan(dbdir, &other, &fs));
    ASSERT_EQU(fs.nall, 1u);
    zsi_fileset_fini(&fs);
}

static void test_fileset_ignores_foreign(void)
{
    struct zsi_fileset fs;

    /* D-2/D-4: staging names, the lock file, and unrelated files are ignored by
     * construction, because zeroskip.* is metadata and zeroskip-* is data. */
    const char *names[] = {
        dn(1, 0),
        "zeroskip.lock",
        "zeroskip.tmp.1234.0",
        "zeroskip.tmp.99999.17",
        "README",
        ".hidden",
        "zeroskip-not-a-uuid-00000001",
        NULL
    };
    seed_names_cur(names, 1);

    ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
    ASSERT_EQU(fs.nall, 1u);
    ASSERT_OK(zsi_fileset_resolve(&fs));
    ASSERT_EQU(fs.nresolved, 1u);
    zsi_fileset_fini(&fs);

    /* An empty directory has no UUID, which is the case D-8a handles by creating
     * one -- not an error.  seed_names clears first, so this really is empty. */
    const char *none[] = { NULL };
    seed_names(none);
    ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
    ASSERT_EQU(fs.nall, 0u);
    ASSERT(!fs.have_uuid);
    ASSERT_OK(zsi_fileset_resolve(&fs));
    ASSERT_EQU(fs.nresolved, 0u);
    zsi_fileset_fini(&fs);

    /* A directory that does not exist at all. */
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/nope", basedir);
    ASSERT_EQ(zsi_fileset_scan(missing, NULL, &fs), ZS_NOTFOUND);
}

static void test_fileset_next_gen(void)
{
    struct zsi_fileset fs;
    uint32_t next;

    /* D-9b: one above the highest generation PRESENT, computed over every file
     * rather than the resolved set.  A superseded file still pins its generation,
     * so the highest never regresses and a generation is never reissued. */
    {
        const char *names[] = { dn(1,4), dn(5,0), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
        ASSERT_EQU(next, 6u);
        zsi_fileset_fini(&fs);
    }

    /* With the repack inputs still present alongside the output, the answer is
     * unchanged -- the output already covers them. */
    {
        const char *names[] = { dn(1,1), dn(2,2), dn(3,3), dn(4,4), dn(1,4),
                                dn(5,0), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
        ASSERT_EQU(next, 6u);
        zsi_fileset_fini(&fs);
    }

    /* And after files have been removed: the highest present still decides. */
    {
        const char *names[] = { dn(1,4), dn(5,5), NULL };
        seed_names_cur(names, 5);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
        ASSERT_EQU(next, 6u);
        zsi_fileset_fini(&fs);
    }

    /* D-9c: allocating past 0xFFFFFFFF fails rather than wrapping.  Wrapping
     * would reissue generation 1 while a file bearing that name still existed. */
    {
        const char *names[] = { dn(0xFFFFFFF0u, 0xFFFFFFFFu), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_next_gen(&fs, &next), ZS_FULL);
        zsi_fileset_fini(&fs);
    }
    {
        const char *names[] = { dn(0xFFFFFFFFu, 0), NULL };
        seed_names_cur(names, 0xFFFFFFFFu);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_next_gen(&fs, &next), ZS_FULL);
        zsi_fileset_fini(&fs);
    }

    /* One below the ceiling still allocates. */
    {
        const char *names[] = { dn(0xFFFFFFFEu, 0), NULL };
        /* header carries the generation now (D-1b) */
        seed_names_cur(names, 0xFFFFFFFEu);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
        ASSERT_EQU(next, 0xFFFFFFFFu);
        zsi_fileset_fini(&fs);
    }
}

static void test_fileset_mid_conversion_stable(void)
{
    /* A directory left mid-conversion -- the unordered input and the in-order
     * output both present -- is judged COMPLETE, and stays that way.
     *
     * T-9 asks specifically that leaving it indefinitely, as a writer death would,
     * does not make readers retry forever.  A resolution that treated the overlap
     * as a gap would spin: every scan would see the same directory and reach the
     * same wrong conclusion. */
    struct zsi_fileset fs;
    char got[128];

    const char *names[] = { dn(1,4), dn(5,0), dn(5,5), NULL };
    seed_names_cur(names, 5);

    for (int attempt = 0; attempt < 5; attempt++) {
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5-5");
        zsi_fileset_fini(&fs);
    }

    /* The same for an interrupted repack: output plus every input. */
    const char *names2[] = { dn(1,1), dn(2,2), dn(1,2), NULL };
    seed_names(names2);
    for (int attempt = 0; attempt < 5; attempt++) {
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-2");
        zsi_fileset_fini(&fs);
    }
}

/*
 * ============================================================
 * Snapshots (C-4)
 * ============================================================
 */

/* Write a real (non-empty) unordered file with the given keys. */
static void put_unordered(uint32_t gen, const char *const *keys)
{
    struct sb s;
    char name[ZSI_NAME_MAX];

    sb_init(&s, gen, ZSI_CSUM_XXHASH);
    for (size_t i = 0; keys && keys[i]; i++)
        sb_rec(&s, keys[i], strlen(keys[i]), "v", 1);
    sb_term(&s, false);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);
}

/* Write a real in-order file with the given keys, which must be in order. */
static void put_inorder(uint32_t start, uint32_t end, const char *const *keys)
{
    struct ib b;
    char name[ZSI_NAME_MAX];
    const char *sorted[64];
    size_t n = 0;

    for (size_t i = 0; keys && keys[i]; i++) { assert(n < 64); sorted[n++] = keys[i]; }
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (zsi_compar_default(sorted[i], strlen(sorted[i]),
                                   sorted[j], strlen(sorted[j])) > 0) {
                const char *t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }

    ib_init(&b, start, end, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < n; i++)
        ib_rec(&b, sorted[i], strlen(sorted[i]), "v", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, start, end);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
}

static void clear_db(void)
{
    const char *none[] = { NULL };
    seed_names(none);
}

static void test_snapshot_basic(void)
{
    struct zsi_snapshot *s = NULL;

    clear_db();
    static const char *k1[] = { "a", "b", NULL };
    static const char *k2[] = { "c", NULL };
    put_inorder(1, 4, k1);
    put_unordered(5, k2);

    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 2u);

    /* Sorted by start ascending -- reads walk it descending (D-14). */
    ASSERT_EQU(s->files[0]->hdr.start, 1u);
    ASSERT_EQU(s->files[1]->hdr.start, 5u);

    /* The active file is the highest-generation unordered one. */
    struct zsi_file *act = zsi_snapshot_active(s);
    ASSERT_NOT_NULL(act);
    ASSERT_EQU(act->hdr.start, 5u);
    ASSERT(zsi_file_is_unordered(act));

    /* Step 4 built an index for the unordered file, and loaded pointers for the
     * in-order one. */
    ASSERT_NOT_NULL(s->files[1]->index);
    ASSERT_EQU(s->files[1]->index->nbase, 1u);
    ASSERT_EQU(s->files[0]->nptrs, 2u);

    zsi_snapshot_release(&s);
    ASSERT_NULL(s);

    /* When the newest file is in-order there is no active file: a writer must
     * create one rather than append (D-9). */
    clear_db();
    put_inorder(1, 4, k1);
    put_inorder(5, 5, k2);
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT_NULL(zsi_snapshot_active(s));
    zsi_snapshot_release(&s);

    /* An empty directory snapshots to zero files rather than failing: that is the
     * state D-8a turns into a new database. */
    clear_db();
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 0u);
    ASSERT_NULL(zsi_snapshot_active(s));
    zsi_snapshot_release(&s);
}

static void test_snapshot_resolves_overlap(void)
{
    /* A snapshot taken mid-conversion opens the winner and ignores the input,
     * without needing to know a conversion happened (D-5, R-5). */
    struct zsi_snapshot *s = NULL;
    static const char *k[] = { "a", NULL };

    clear_db();
    put_unordered(5, k);
    put_inorder(5, 5, k);
    put_inorder(1, 4, k);

    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT_EQU(s->files[1]->hdr.start, 5u);
    ASSERT_EQU(s->files[1]->hdr.end, 5u);       /* the in-order one won */
    ASSERT_NULL(zsi_snapshot_active(s));
    zsi_snapshot_release(&s);

    /* And mid-repack: the output wins over every input. */
    clear_db();
    put_inorder(1, 1, k);
    put_inorder(2, 2, k);
    put_inorder(1, 2, k);
    put_unordered(3, k);
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT_EQU(s->files[0]->hdr.start, 1u);
    ASSERT_EQU(s->files[0]->hdr.end, 2u);
    zsi_snapshot_release(&s);
}

static void test_snapshot_retries_and_bounds(void)
{
    /* A set that never tiles must be reported in bounded time rather than
     * spinning (C-4h).  Under an alarm, because the failure mode this guards
     * against is a livelock, not a wrong answer. */
    struct zsi_snapshot *s = NULL;
    static const char *k[] = { "a", NULL };

    alarm(30);

    clear_db();
    put_inorder(1, 1, k);
    put_inorder(3, 3, k);       /* generation 2 missing: never tiles */

    ASSERT_EQ(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s), ZS_AGAIN);
    ASSERT_NULL(s);

    /* A partial overlap is corruption rather than a stale scan, so it is
     * reported immediately rather than retried to exhaustion. */
    clear_db();
    put_inorder(1, 5, k);
    put_inorder(3, 8, k);
    ASSERT_EQ(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s), ZS_BADFORMAT);

    alarm(0);
}

static void test_snapshot_boundary(void)
{
    /* Step 4 takes each unordered file's boundary to be the end of its last valid
     * span, so anything past it is invisible (C-4c).  Modelled here with trailing
     * garbage, which is what a torn append looks like to a reader. */
    struct sb s;
    struct zsi_snapshot *snap = NULL;
    char name[ZSI_NAME_MAX];

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "visible", 7, "1", 1);
    sb_term(&s, false);
    size_t boundary = s.len;
    sb_rec(&s, "invisible", 9, "2", 1);   /* no terminator */
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(sb_write(&s, name), 0);

    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &snap));
    ASSERT_EQU(snap->nfiles, 1u);
    ASSERT_EQU(snap->files[0]->complete, boundary);
    ASSERT(snap->files[0]->size > boundary);

    /* The index reflects the boundary, not the file size. */
    size_t off;
    ASSERT_OK(zsi_index_find(snap->files[0]->index, zsi_compar_default,
                             "visible", 7, &off));
    ASSERT_EQ(zsi_index_find(snap->files[0]->index, zsi_compar_default,
                             "invisible", 9, &off), ZS_NOTFOUND);

    zsi_snapshot_release(&snap);
    sb_free(&s);
}

static void test_snapshot_bad_nonactive(void)
{
    /* D-10a: a NON-ACTIVE file with an invalid header is an error, because its
     * records cannot be recovered and skipping the generation would lose
     * committed data.  D-10: the ACTIVE file in the same state is fine. */
    struct zsi_snapshot *s = NULL;
    static const char *k[] = { "a", NULL };
    char name[ZSI_NAME_MAX];
    char junk[ZSI_HEADER_LEN];
    memset(junk, 0xFF, sizeof(junk));

    /* Corrupt the ACTIVE (highest, unordered) file: tolerated, and NOT reported.
     *
     * D-10 makes this an ordinary post-crash state, so reporting it would cry wolf
     * after every crash -- which is how a real report comes to be ignored.  The
     * contrast with the non-active case below is the whole point. */
    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);
    report_count = 0;
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                counting_error, NULL, &s));
    ASSERT_EQ(report_count, 0);

    /* D-1b/D-10: the active file's generation lives in its header, so a header
     * that does not validate leaves it with none -- and a file whose generation
     * is unknown cannot take part in D-5's scan.  It is therefore absent from
     * the set rather than present-and-empty, which is what the old name-carried
     * generation allowed.  Nothing is lost: no record in it was recoverable
     * either way, and the set without it still tiles, because it would have
     * been the generation above the highest. */
    ASSERT_EQU(s->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(s->files[0]));
    ASSERT_EQU(s->files[0]->hdr.start, 1u);
    zsi_snapshot_release(&s);

    /* And it remains resolvable rather than fatal: the set without it tiles,
     * which is what D-10 promises (G-3). */
    {
        struct zsi_fileset fs2;
        ASSERT_OK(zsi_fileset_scan(dbdir, &test_uuid, &fs2));
        ASSERT_OK(zsi_fileset_resolve(&fs2));
        ASSERT_EQU(fs2.nresolved, 1u);
        zsi_fileset_fini(&fs2);
    }

    /* Corrupt a NON-ACTIVE file: REPORTED, not fatal (D-10a, D-10b).
     *
     * Fatal would contradict D-10, which tells a writer to move on from an unclean
     * active file -- the instant it does, that file is non-active, so the first
     * write after an ordinary crash would make the database permanently
     * unopenable.  G-3 forbids that. */
    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);
    report_count = 0;
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                counting_error, NULL, &s));
    ASSERT_NOT_NULL(s);
    ASSERT(report_count > 0);       /* not silent */
    zsi_snapshot_release(&s);

    /* A zero-length active file, likewise tolerated (D-10) -- and likewise
     * absent from the set rather than present-and-empty, since a file with no
     * header has no generation to place it at (D-1b). */
    clear_db();
    put_inorder(1, 1, k);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(s->files[0]));
    zsi_snapshot_release(&s);
}

static void test_snapshot_refcount(void)
{
    /* A snapshot outlives the files being unlinked underneath it (C-4g): the
     * kernel keeps each inode alive until the last descriptor and mapping is
     * gone, so a reader holding one keeps reading while a packer retires its
     * inputs.  There is no reference table and nothing to clean up on death. */
    struct zsi_snapshot *s = NULL;
    static const char *k[] = { "alpha", "beta", NULL };
    char name[ZSI_NAME_MAX];

    clear_db();
    put_inorder(1, 1, k);

    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                "memcmp", TEST_EXTERNAL_CSUM, NULL,
                                NULL, NULL, &s));
    ASSERT_EQU(s->nfiles, 1u);

    /* Unlink the file out from under the open snapshot. */
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(unlink(dbpath(name)), 0);

    /* Still readable, in full. */
    struct zsi_fcur fc;
    char keys[128];
    zsi_fcur_init_file(&fc, s->files[0], zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "alpha|beta");
    ASSERT_OK(zsi_ptrs_verify_records(s->files[0]));

    /* Refcounting: an extra reference keeps it alive across a release. */
    s->refcount++;
    struct zsi_snapshot *alias = s;
    zsi_snapshot_release(&alias);
    ASSERT_NULL(alias);
    ASSERT_EQU(s->nfiles, 1u);           /* still valid */
    zsi_fcur_init_file(&fc, s->files[0], zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "alpha|beta");

    zsi_snapshot_release(&s);
    ASSERT_NULL(s);
}

/*
 * ============================================================
 * File locking (section 6, T-14)
 * ============================================================
 */

static void test_lock_basic(void)
{
    struct zsi_locks lk;

    ASSERT_EQ(mkdbdir(), 0);

    /* D-3a: created with O_CREAT if absent, so a database missing its lock file
     * is never unopenable. */
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), -ENOENT);
    ASSERT_OK(zsi_lock_open(&lk, dbdir));
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), 0);
    ASSERT(lk.fd >= 0);

    /* D-3c: the lock file is empty and its contents are never read.  Nothing
     * about the database is stored in it, so it carries no state to become
     * stale. */
    ASSERT_EQ(filesize(ZSI_LOCK_NAME), 0);

    /* Each lock in turn. */
    for (int i = 0; i < ZSI_NLOCKS; i++) {
        ASSERT_OK(zsi_lock_take(&lk, (enum zsi_lock)i, 0));
        ASSERT_EQU(lk.held, 1u << i);
        ASSERT_OK(zsi_lock_release(&lk, (enum zsi_lock)i));
        ASSERT_EQU(lk.held, 0u);
    }

    /* C-1d's two legal orderings -- write -> remove and repack -> remove -- and
     * only those.  Nothing holds both write and repack, and nothing takes either
     * while holding remove, so no cycle exists.
     *
     * Not asserted at runtime -- see the note at zsi_lock_take about why a
     * per-handle bitmask cannot express a per-thread rule -- so this test is the
     * record that the two orderings are the intended ones. */
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_WRITE, 0));
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_REMOVE, 0));
    ASSERT_EQU(lk.held, (1u << ZSI_LOCK_WRITE) | (1u << ZSI_LOCK_REMOVE));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_REMOVE));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_WRITE));

    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_REPACK, 0));
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_REMOVE, 0));
    ASSERT_EQU(lk.held, (1u << ZSI_LOCK_REPACK) | (1u << ZSI_LOCK_REMOVE));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_REMOVE));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_REPACK));
    ASSERT_EQU(lk.held, 0u);

    /* Releasing an unheld lock is a no-op, so cleanup paths need no guard. */
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_WRITE));

    zsi_lock_close(&lk);
    ASSERT_EQ(lk.fd, -1);

    /* Reopening an existing lock file works, and does not truncate it. */
    ASSERT_OK(zsi_lock_open(&lk, dbdir));
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_WRITE, 0));
    zsi_lock_close(&lk);
}

static void test_lock_byte_offsets(void)
{
    SKIP_IF_NO_FORK();

    /* C-1e: the byte offsets are normative, because implementations in different
     * languages must exclude each other.  Asserted against literals, and verified
     * with a raw fcntl call rather than through this library's own wrapper -- a
     * third observer, exactly as T-13 requires of the interop runner. */
    struct zsi_locks lk;

    ASSERT_EQ(ZSI_LOCK_WRITE, 0);
    ASSERT_EQ(ZSI_LOCK_REPACK, 1);
    ASSERT_EQ(ZSI_LOCK_REMOVE, 2);
    ASSERT_STR_EQ(ZSI_LOCK_NAME, "zeroskip.lock");

    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_OK(zsi_lock_open(&lk, dbdir));
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_REPACK, 0));

    /* A separate descriptor in a CHILD process, since fcntl locks do not conflict
     * within one process (C-1f).  The child asks the kernel which byte is locked
     * and by what. */
    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        int fd = open(dbpath(ZSI_LOCK_NAME), O_RDWR);
        int bad = 0;
        for (int b = 0; b < 3; b++) {
            struct flock fl;
            memset(&fl, 0, sizeof(fl));
            fl.l_type = F_WRLCK;
            fl.l_whence = SEEK_SET;
            fl.l_start = b;
            fl.l_len = 1;
            if (fcntl(fd, F_GETLK, &fl) < 0) { bad = 10; break; }
            bool locked = (fl.l_type != F_UNLCK);
            if (locked != (b == ZSI_LOCK_REPACK)) bad = 20 + b;
        }
        close(fd);
        _exit(bad);
    }

    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    zsi_lock_close(&lk);
}

static void test_lock_excludes_other_process(void)
{
    SKIP_IF_NO_FORK();

    /* Two processes, exactly one proceeding.  The child holds the write lock for
     * a while; the parent's non-blocking take must report ZS_LOCKED and its
     * blocking take must wait. */
    struct zsi_locks lk;
    int pipefd[2];

    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zsi_locks clk;
        close(pipefd[0]);
        if (zsi_lock_open(&clk, dbdir) != ZS_OK) _exit(1);
        if (zsi_lock_take(&clk, ZSI_LOCK_WRITE, 0) != ZS_OK) _exit(2);
        /* tell the parent the lock is held, then hold it briefly */
        if (write(pipefd[1], "x", 1) != 1) _exit(3);
        close(pipefd[1]);
        usleep(300000);
        zsi_lock_close(&clk);
        _exit(0);
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);       /* the child holds it now */
    close(pipefd[0]);

    ASSERT_OK(zsi_lock_open(&lk, dbdir));

    /* Non-blocking: refused rather than waiting. */
    ASSERT_EQ(zsi_lock_take(&lk, ZSI_LOCK_WRITE, ZS_NONBLOCKING), ZS_LOCKED);

    /* A different byte is unaffected -- the locks are independent (C-1a: write
     * and repack never contend, because the two jobs consume disjoint files). */
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_REPACK, ZS_NONBLOCKING));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_REPACK));

    /* Blocking: waits for the child, then succeeds. */
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_WRITE, 0));
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_WRITE));

    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid);
    ASSERT(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);

    zsi_lock_close(&lk);
}

static void test_lock_dies_with_process(void)
{
    SKIP_IF_NO_FORK();

    /* G-5: a writer SIGKILLed holding the lock never blocks the next one.
     *
     * The kernel releases fcntl locks on process death, so no lock state can
     * outlive a process and there is nothing to clean up or time out.  This is
     * the property that makes the whole design free of stale-lock recovery. */
    struct zsi_locks lk;
    int pipefd[2];

    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zsi_locks clk;
        close(pipefd[0]);
        if (zsi_lock_open(&clk, dbdir) != ZS_OK) _exit(1);
        if (zsi_lock_take(&clk, ZSI_LOCK_WRITE, 0) != ZS_OK) _exit(2);
        if (write(pipefd[1], "x", 1) != 1) _exit(3);
        for (;;) pause();                       /* hold it until killed */
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);
    close(pipefd[0]);

    ASSERT_OK(zsi_lock_open(&lk, dbdir));
    ASSERT_EQ(zsi_lock_take(&lk, ZSI_LOCK_WRITE, ZS_NONBLOCKING), ZS_LOCKED);

    /* Kill it dead -- no cleanup, no unwinding, no chance to release. */
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    int status = 0;
    ASSERT(waitpid(pid, &status, 0) == pid);
    ASSERT(WIFSIGNALED(status));

    /* The next writer proceeds with NO manual intervention. */
    alarm(20);
    ASSERT_OK(zsi_lock_take(&lk, ZSI_LOCK_WRITE, 0));
    alarm(0);
    ASSERT_OK(zsi_lock_release(&lk, ZSI_LOCK_WRITE));

    zsi_lock_close(&lk);
}

/* T-14: two write handles on one database, from one process.
 *
 * This test exists to stop the implementation quietly acquiring a mutex and
 * appearing to offer a guarantee the format cannot enforce.  fcntl locks are
 * per-process (C-1f): the kernel sees one owner and grants both, and a per-handle
 * mutex would be two different objects, so it would exclude only threads sharing a
 * handle -- which is not what anyone reads G-5 as promising.
 *
 * An earlier version of this library did acquire such a mutex.  It passed a test
 * of two threads sharing one handle, and a direct measurement of two threads with
 * SEPARATE handles found 398 overlapping entries into the write section.  The
 * guarantee was never there; the mutex only hid its absence.
 *
 * So the asserted behaviour is the honest one: the second take succeeds, and that
 * is documented rather than defended against. */
/* One run of T-14 against whichever C-1j mechanism is currently selected. */
static void two_handles_are_excluded(void)
{
    struct zsi_locks a, b;

    ASSERT_OK(zsi_lock_open(&a, dbdir));
    ASSERT_OK(zsi_lock_open(&b, dbdir));

    ASSERT_OK(zsi_lock_take(&a, ZSI_LOCK_WRITE, 0));

    /* The second handle is refused, exactly as a second process would be. */
    ASSERT_EQ(zsi_lock_take(&b, ZSI_LOCK_WRITE, ZS_NONBLOCKING), ZS_LOCKED);

    /* And it is a lock, not a permanent refusal: releasing hands it over.  A
     * mechanism that never released would pass the assertion above. */
    ASSERT_OK(zsi_lock_release(&a, ZSI_LOCK_WRITE));
    ASSERT_OK(zsi_lock_take(&b, ZSI_LOCK_WRITE, ZS_NONBLOCKING));

    /* The other two locks are tracked separately, not as one flag. */
    ASSERT_OK(zsi_lock_take(&a, ZSI_LOCK_REPACK, ZS_NONBLOCKING));

    zsi_lock_close(&a);
    zsi_lock_close(&b);
}

/* T-14, C-1j: two handles on one database within one process exclude each
 * other.  Plain fcntl locks are per-process and would not, so a caller that
 * cared would have to build the exclusion itself.
 *
 * Run against BOTH mechanisms.  F_OFD_SETLK exists on every platform anyone
 * develops on, so the registry is dead code here and would rot unexercised
 * until the one platform that needs it found out. */
static void test_lock_two_handles_one_process(void)
{
    bool saved = zsi_lock_registry;

    ASSERT_EQ(mkdbdir(), 0);

#if ZSI_HAVE_OFD_LOCKS
    zsi_lock_registry = false;          /* mechanism 1: the kernel does it */
    two_handles_are_excluded();
#endif

    zsi_lock_registry = true;           /* mechanism 2: the registry does it */
    two_handles_are_excluded();

    zsi_lock_registry = saved;
}

/* C-1j: the registry keys on the lock file's inode, not its path, so two
 * handles that reached one database by different paths still exclude each
 * other.  A path-keyed registry passes every test that opens `dbdir` twice. */
static void test_lock_registry_keys_on_inode(void)
{
    struct zsi_locks a, b;
    bool saved = zsi_lock_registry;
    char viadot[PATH_MAX];

    ASSERT_EQ(mkdbdir(), 0);
    zsi_lock_registry = true;

    XSNPRINTF(viadot, "%s/.", dbdir);

    ASSERT_OK(zsi_lock_open(&a, dbdir));
    ASSERT_OK(zsi_lock_open(&b, viadot));

    ASSERT_OK(zsi_lock_take(&a, ZSI_LOCK_WRITE, 0));
    ASSERT_EQ(zsi_lock_take(&b, ZSI_LOCK_WRITE, ZS_NONBLOCKING), ZS_LOCKED);

    zsi_lock_close(&a);
    zsi_lock_close(&b);
    zsi_lock_registry = saved;
}

/* C-1j: the registry is per DATABASE.  Keying it on anything coarser -- a
 * single process-wide flag, say -- would make one database's writer exclude
 * another's, which is a deadlock in any caller holding two open (C-1h). */
static void test_lock_registry_is_per_database(void)
{
    struct zsi_locks a, b;
    bool saved = zsi_lock_registry;
    char other[PATH_MAX];

    ASSERT_EQ(mkdbdir(), 0);
    zsi_lock_registry = true;

    XSNPRINTF(other, "%s/other", basedir);
    ASSERT_EQ(mkdir(other, 0700), 0);

    ASSERT_OK(zsi_lock_open(&a, dbdir));
    ASSERT_OK(zsi_lock_open(&b, other));

    ASSERT_OK(zsi_lock_take(&a, ZSI_LOCK_WRITE, 0));
    ASSERT_OK(zsi_lock_take(&b, ZSI_LOCK_WRITE, ZS_NONBLOCKING));

    zsi_lock_close(&a);
    zsi_lock_close(&b);
    zsi_lock_registry = saved;
}

/* And the corollary: the library holds no thread machinery at all, so nobody can
 * mistake it for thread-safe.  Structural, like the flock check -- weak evidence
 * on its own, but it fails loudly if a mutex is reintroduced as a "fix". */
static void test_lock_no_thread_machinery(void)
{
    FILE *fp = fopen("zeroskip.c", "r");
    if (!fp) SKIP("zeroskip.c not readable from the test's cwd");

    char line[1024];
    int found = 0, lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        if (strstr(line, "pthread_")) {
            fprintf(stderr, "\n    FAIL zeroskip.c:%d uses pthread\n", lineno);
            found = 1;
        }
    }
    fclose(fp);
    ASSERT_EQ(found, 0);
}

static void test_lock_never_uses_flock(void)
{
    /* C-1e forbids flock, and the hazard is that a flock-based implementation
     * looks correct to itself: on Linux the two lock spaces do not intersect, so
     * every single-implementation test passes while a conforming peer is not
     * excluded at all.
     *
     * Asserted structurally -- the source must not call flock -- because the
     * behavioural test needs a second implementation and belongs to T-13.  A
     * source-level check is weak evidence, but it is the strongest available here
     * and it fails loudly if someone reaches for flock as a "simplification". */
    FILE *fp = fopen("zeroskip.c", "r");
    if (!fp) SKIP("zeroskip.c not readable from the test's cwd");

    char line[1024];
    int found = 0, lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        /* skip the comments that explain why flock is forbidden */
        char *p = strstr(line, "flock(");
        if (p && !strstr(line, "*") && !strstr(line, "//")) {
            fprintf(stderr, "\n    FAIL zeroskip.c:%d calls flock\n", lineno);
            found = 1;
        }
    }
    fclose(fp);
    ASSERT_EQ(found, 0);
}

/*
 * ============================================================
 * Open, create and close (section 7)
 * ============================================================
 */

static void test_open_create(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    /* Without ZS_CREATE, a nonexistent database is ZS_NOTFOUND (D-8a). */
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_NOTFOUND);
    ASSERT_NULL(db);

    /* With it: the directory, the lock file, and generation 1 as the active file
     * -- a 72-byte header and no spans, which F-26h makes legal. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), 0);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, db->uuid);
    ASSERT_EQ(filesize(name), 72);
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT(zsi_unordered_is_clean(db->snap->files[0]));

    /* The comparator name went into the header (F-11b). */
    ASSERT_MEM_EQ(db->snap->files[0]->hdr.compar_name,
                  "memcmp\0\0\0\0\0\0\0\0\0\0", 16);

    zsi_uuid_t created;
    memcpy(created, db->uuid, 16);
    ASSERT_OK(zs_db_close(&db));
    ASSERT_NULL(db);

    /* Reopening finds the same UUID, discovered from the filenames (D-4a). */
    setup.flags = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_MEM_EQ(db->uuid, created, 16);
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_close(&db));

    /* ZS_CREATE on an existing database is not an error and does not re-create. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_MEM_EQ(db->uuid, created, 16);
    ASSERT_OK(zs_db_close(&db));

    /* Closing NULL, and double close, are no-ops. */
    ASSERT_OK(zs_db_close(&db));
}

static void test_open_with_uuid(void)
{
    /* zs_db_open_with_uuid exists so corpus generation is reproducible (T-1).
     * It applies only when CREATING; opening an existing database ignores it. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open_with_uuid(dbdir, &setup, TEST_UUID_STR, &db));
    ASSERT_MEM_EQ(db->uuid, test_uuid, 16);

    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(filesize(name), 72);
    ASSERT_OK(zs_db_close(&db));

    /* Reopening with a DIFFERENT uuid string ignores it rather than renaming. */
    ASSERT_OK(zs_db_open_with_uuid(dbdir, &setup,
                                   "00000000-0000-4000-8000-000000000000", &db));
    ASSERT_MEM_EQ(db->uuid, test_uuid, 16);
    ASSERT_OK(zs_db_close(&db));

    /* A malformed uuid string is rejected rather than silently generated. */
    clear_db();
    ASSERT_EQ(zs_db_open_with_uuid(dbdir, &setup, "not-a-uuid", &db),
              ZS_BADUSAGE);
    ASSERT_NULL(db);
}

static int alt_compar(const char *a, size_t alen, const char *b, size_t blen)
{
    /* reverse byte order, so it is genuinely a different total order */
    return -zsi_compar_default(a, alen, b, blen);
}

static void test_open_comparator_agreement(void)
{
    /* F-11: every file carries the comparator name, and opening a database whose
     * comparator differs from the caller's is an error.  It has to be, because
     * the comparator determines the meaning of a pointer section: reading one
     * built under a different order returns wrong answers silently. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_close(&db));

    /* Reopening with a custom comparator: mismatch. */
    setup.flags = 0;
    setup.compar = alt_compar;
    setup.compar_name = "reverse";
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADFORMAT);
    ASSERT_NULL(db);

    /* A caller comparator with no name is a usage error (F-11b). */
    setup.compar_name = NULL;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    setup.compar_name = "";
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    setup.compar_name = "seventeen chars!!";      /* 17 */
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);

    /* A database created with a custom comparator round-trips, and then rejects
     * the default one. */
    clear_db();
    setup.flags = ZS_CREATE;
    setup.compar = alt_compar;
    setup.compar_name = "reverse";
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_MEM_EQ(db->snap->files[0]->hdr.compar_name,
                  "reverse\0\0\0\0\0\0\0\0\0", 16);
    ASSERT_OK(zs_db_close(&db));

    setup.compar = NULL;
    setup.compar_name = NULL;
    setup.flags = 0;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADFORMAT);
}

static void test_open_engine_selection(void)
{
    /* A-6: a ZS_CSUM_* flag chooses the engine for files this handle CREATES; it
     * never overrides what an existing file records, since each file's engine
     * comes from its own header (F-5a). */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(db->create_csum_id, ZSI_CSUM_NONE);
    ASSERT_EQ(db->snap->files[0]->csum_id, ZSI_CSUM_NONE);
    ASSERT_OK(zs_db_close(&db));

    /* Reopening with the xxhash flag reads the engine-0 file as engine 0. */
    setup.flags = ZS_CSUM_XXHASH;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(db->create_csum_id, ZSI_CSUM_XXHASH);
    ASSERT_EQ(db->snap->files[0]->csum_id, ZSI_CSUM_NONE);   /* from the file */
    ASSERT_OK(zs_db_close(&db));

    /* Engine 2 without a function is a usage error (A-6). */
    clear_db();
    setup.flags = ZS_CREATE | ZS_CSUM_EXTERNAL;
    setup.csum = NULL;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    ASSERT_NULL(db);

    /* With one, it works, and the file records engine 2. */
    setup.csum = TEST_EXTERNAL_CSUM;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(db->snap->files[0]->csum_id, ZSI_CSUM_EXTERNAL);
    ASSERT_OK(zs_db_close(&db));

    /* Reopening those files WITHOUT the function cannot verify them. */
    setup.flags = 0;
    setup.csum = NULL;
    int r = zs_db_open(dbdir, &setup, &db);
    ASSERT(r != ZS_OK);
    ASSERT_NULL(db);
}

static void test_open_readonly_no_side_effects(void)
{
    /* R-3: opening a damaged database read-only is side-effect-free -- no
     * conversion, no repack, no new active file, no removal.  Asserted by
     * comparing the directory listing before and after, which is the only way to
     * check "did nothing" rather than "did nothing I thought to look for". */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    static const char *k[] = { "a", NULL };

    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);

    /* Make the active file unclean, which is what a crash leaves (D-10). */
    char name[ZSI_NAME_MAX];
    zsi_name_current(name, test_uuid);
    int fd = open(dbpath(name), O_WRONLY | O_APPEND);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8), 8);
    close(fd);

    /* Snapshot the directory. */
    char before[4096] = "";
    DIR *d = opendir(dbdir);
    ASSERT_NOT_NULL(d);
    struct dirent *de;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        strncat(before, de->d_name, sizeof(before) - strlen(before) - 2);
        strncat(before, "\n", 2);
    }
    closedir(d);

    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT(db->readonly);
    ASSERT_EQ(zsi_check_writable(db), ZS_READONLY);
    ASSERT_OK(zs_db_close(&db));

    char after[4096] = "";
    d = opendir(dbdir);
    ASSERT_NOT_NULL(d);
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        strncat(after, de->d_name, sizeof(after) - strlen(after) - 2);
        strncat(after, "\n", 2);
    }
    closedir(d);

    /* Same set of names, and the unclean file still unclean. */
    ASSERT_EQ(strlen(before), strlen(after));

    /* ZS_SHARED on a nonexistent database will not create one either -- not the
     * directory, not the lock file, not generation 1 (R-3, A-5). */
    clear_db();
    setup.flags = ZS_SHARED | ZS_CREATE;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_READONLY);
    ASSERT_NULL(db);
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), -ENOENT);
}

static void test_open_bad_nonactive(void)
{
    /* D-10a at the database level: a non-active file with an invalid header
     * cannot be recovered, and skipping the generation would lose committed data,
     * so the open fails rather than succeeding with less data than the caller
     * asked for. */
    struct zs_db *db = NULL;
    static const char *k[] = { "a", NULL };
    char junk[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    memset(junk, 0xFF, sizeof(junk));

    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);

    /* Reported, not fatal (D-10a as amended by D-10b): the file's records are
     * unrecoverable either way, so refusing to open would cost every OTHER file
     * too -- and after an ordinary crash this is the state D-10 creates. */
    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT(report_count > 0);
    zs_db_close(&db);

    /* The same corruptions in the ACTIVE file are the snapshot layer's cases:
     * test_snapshot_bad_nonactive opens them one call down, and the crash suite
     * produces them for real. */
}

static void test_open_lock_file_recreated(void)
{
    /* D-3a: a database whose lock file is absent opens successfully, recreating
     * it.  T-9 asks for this specifically -- an empty file named *.lock is
     * exactly what a cleanup script deletes, and a database that then refused to
     * open would be a support incident rather than a recovery. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_close(&db));

    ASSERT_EQ(unlink(dbpath(ZSI_LOCK_NAME)), 0);
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), -ENOENT);

    setup.flags = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), 0);
    ASSERT_OK(zs_db_close(&db));
}

static void test_open_uuid_mismatch(void)
{
    /* D-4a: files disagreeing on UUID are an error, not a majority vote. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    static const zsi_uuid_t other = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x46, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
    };
    char othername[ZSI_NAME_MAX];
    static const char *k[] = { "a", NULL };

    clear_db();
    put_unordered(1, k);
    zsi_name_current(othername, other);
    ASSERT_EQ(writefile(othername, "", 0), 0);

    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADFORMAT);
    ASSERT_NULL(db);
}

/*
 * ============================================================
 * The read path (T-5, T-5a, T-5b)
 * ============================================================
 */

/* Open a database over whatever is currently in dbdir. */
static struct zs_db *open_db(uint32_t flags)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    setup.flags = flags;
    setup.csum = TEST_EXTERNAL_CSUM;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/* A-16's cap, at a size a test can actually build files around.  The default is
 * 512MB and so a no-op for every test in this file, which is the point of the
 * default and the reason this helper exists. */
static struct zs_db *open_db_repack_max(uint32_t flags, size_t max)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    setup.flags = flags;
    setup.csum = TEST_EXTERNAL_CSUM;
    setup.repack_max_size = max;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/* Scan the whole database, joining "key=value" with '|'.  Deletions do not
 * appear, because the merge consumes them (D-14e step 4). */
static void db_scan(struct zs_db *db, uint32_t flags,
                    const char *start, size_t startlen,
                    char *out, size_t outlen)
{
    struct zs_cursor *c = NULL;
    struct zsi_rec r;
    size_t used = 0;

    out[0] = '\0';
    if (zsi_cursor_open(db, NULL, db->snap, start, startlen, flags, &c) != ZS_OK)
        return;

    while (zsi_cursor_next(c, &r) == ZS_OK) {
        if (used + r.keylen + r.vallen + 4 >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, r.key, r.keylen);   used += r.keylen;
        out[used++] = '=';
        memcpy(out + used, r.val, r.vallen);   used += r.vallen;
        out[used] = '\0';
    }

    zsi_cursor_free(c);
}

/* Look up one key, formatting the answer as "value" or "-". */
static void db_get(struct zs_db *db, const char *key, size_t keylen,
                   char *out, size_t outlen)
{
    struct zsi_rec r;
    int rc = zsi_lookup(db, db->snap, NULL, key, keylen, false, &r);

    if (rc != ZS_OK) { snprintf(out, outlen, "-"); return; }
    size_t n = r.vallen < outlen - 1 ? r.vallen : outlen - 1;
    memcpy(out, r.val, n);
    out[n] = '\0';
}

/* A file with explicit key/value pairs, so tests can place specific versions in
 * specific generations.  A NULL value writes a deletion. */
struct kv { const char *k; const char *v; };

static void put_unordered_kv(uint32_t gen, const struct kv *kvs)
{
    struct sb s;
    char name[ZSI_NAME_MAX];

    sb_init(&s, gen, ZSI_CSUM_XXHASH);
    for (size_t i = 0; kvs[i].k; i++)
        sb_rec(&s, kvs[i].k, strlen(kvs[i].k),
               kvs[i].v, kvs[i].v ? strlen(kvs[i].v) : 0);
    sb_term(&s, false);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);
}

/* Sorts, because an in-order file's records ARE in key order by definition and
 * every caller here wants a well-formed file.  A test wanting a deliberately
 * misordered one builds it with ib_rec directly.
 *
 * Learned the hard way: supplying unsorted pairs produced a file whose pointer
 * array was not a strict ordering, so the binary search silently missed keys --
 * and the symptom (a scan in the wrong order, a lookup returning "absent") looked
 * exactly like a merge bug. */
static void put_inorder_kv(uint32_t start, uint32_t end, const struct kv *kvs)
{
    struct ib b;
    char name[ZSI_NAME_MAX];
    struct kv sorted[64];
    size_t n = 0;

    for (size_t i = 0; kvs[i].k; i++) {
        assert(n < 64);
        sorted[n++] = kvs[i];
    }
    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (zsi_compar_default(sorted[i].k, strlen(sorted[i].k),
                                   sorted[j].k, strlen(sorted[j].k)) > 0) {
                struct kv t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t;
            }

    ib_init(&b, start, end, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < n; i++)
        ib_rec(&b, sorted[i].k, strlen(sorted[i].k),
               sorted[i].v, sorted[i].v ? strlen(sorted[i].v) : 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, start, end);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
}

/* An in-order file of n records with 30-byte values, so a test controls file
 * SIZE by record count -- which is what repack selection reads. */
static void put_inorder_n(uint32_t start, uint32_t end, const char *prefix,
                          int n)
{
    struct kv kvs[64];
    char keys[64][16], vals[64][32];

    assert(n > 0 && n < 63);
    for (int i = 0; i < n; i++) {
        snprintf(keys[i], sizeof keys[i], "%s%03d", prefix, i);
        memset(vals[i], 'v', 30); vals[i][30] = '\0';
        kvs[i].k = keys[i]; kvs[i].v = vals[i];
    }
    kvs[n].k = NULL; kvs[n].v = NULL;
    put_inorder_kv(start, end, kvs);
}

static void test_read_d14f_duplicate_across_three_files(void)
{
    /* T-5b's central case, constructed directly: the same key live in THREE
     * files at once.
     *
     * D-14f: advancing only element 0 would leave the same key at another
     * cursor's head, to be emitted again from an older version.  So the failure
     * is not merely a duplicate -- it is the key appearing three times with
     * DESCENDING freshness, newest first then progressively staler values.
     *
     * The test asserts both halves: emitted once, and from the newest. */
    struct zs_db *db;
    char got[256];

    clear_db();
    static const struct kv g1[] = { {"dup","oldest"}, {NULL,NULL} };
    static const struct kv g2[] = { {"dup","middle"}, {NULL,NULL} };
    static const struct kv g3[] = { {"dup","newest"}, {NULL,NULL} };
    put_inorder_kv(1, 1, g1);
    put_inorder_kv(2, 2, g2);
    put_unordered_kv(3, g3);

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "dup=newest");       /* once, and the newest */

    db_get(db, "dup", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "newest");

    /* With neighbours either side, so a bug cannot hide behind a one-key scan. */
    zs_db_close(&db);
    clear_db();
    static const struct kv h1[] = { {"aaa","a1"}, {"dup","oldest"}, {"zzz","z1"},
                                    {NULL,NULL} };
    static const struct kv h2[] = { {"dup","middle"}, {NULL,NULL} };
    static const struct kv h3[] = { {"dup","newest"}, {NULL,NULL} };
    put_inorder_kv(1, 1, h1);
    put_inorder_kv(2, 2, h2);
    put_unordered_kv(3, h3);

    db = open_db(0);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "aaa=a1|dup=newest|zzz=z1");
    zs_db_close(&db);
}

static void test_read_cursor_invariant(void)
{
    /* T-5b: the sorted-array invariant asserted after EVERY step --
     *
     *   - ordered by key ascending, then generation descending;
     *   - cursors on equal keys contiguous from the front, newest first;
     *   - element 0 is the correct next record.
     *
     * Checked directly against the cursor's internals, because the invariant is
     * what makes step 3 correct; verifying only the output would pass for an
     * implementation that got the right answer by a different, fragile route. */
    struct zs_db *db;
    struct zs_cursor *c = NULL;
    struct zsi_rec r;

    clear_db();
    static const struct kv g1[] = { {"a","1"}, {"c","1"}, {"e","1"}, {NULL,NULL} };
    static const struct kv g2[] = { {"b","2"}, {"c","2"}, {NULL,NULL} };
    static const struct kv g3[] = { {"c","3"}, {"d","3"}, {"e","3"}, {NULL,NULL} };
    put_inorder_kv(1, 1, g1);
    put_inorder_kv(2, 2, g2);
    put_unordered_kv(3, g3);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_cursor_open(db, NULL, db->snap, NULL, 0, 0, &c));
    ASSERT_EQU(c->ncur, 3u);

    int steps = 0;
    for (;;) {
        /* Invariant 1: ordered by key ascending then generation descending, with
         * exhausted cursors last. */
        for (size_t i = 1; i < c->ncur; i++) {
            if (zsi_cur_order(c, &c->cur[i - 1], &c->cur[i]) > 0) {
                fprintf(stderr, "\n    FAIL step %d: array out of order at %zu\n",
                        steps, i);
                current_test_failed = 1;
                goto done;
            }
        }

        /* Invariant 2: cursors on the head key are contiguous from the front and
         * ordered newest first. */
        if (!c->cur[0].exhausted) {
            size_t run = 1;
            while (run < c->ncur && !c->cur[run].exhausted
                   && zsi_compar_default(c->cur[run].cur.key,
                                         c->cur[run].cur.keylen,
                                         c->cur[0].cur.key,
                                         c->cur[0].cur.keylen) == 0)
                run++;
            for (size_t i = 1; i < run; i++) {
                if (c->cur[i - 1].gen <= c->cur[i].gen) {
                    fprintf(stderr, "\n    FAIL step %d: equal keys not "
                            "newest-first at %zu\n", steps, i);
                    current_test_failed = 1;
                    goto done;
                }
            }
            /* and nothing beyond the run shares the key */
            for (size_t i = run; i < c->ncur; i++) {
                if (c->cur[i].exhausted) break;
                if (zsi_compar_default(c->cur[i].cur.key, c->cur[i].cur.keylen,
                                       c->cur[0].cur.key,
                                       c->cur[0].cur.keylen) == 0) {
                    fprintf(stderr, "\n    FAIL step %d: key run not contiguous\n",
                            steps);
                    current_test_failed = 1;
                    goto done;
                }
            }

            /* Invariant 3: element 0 really is the smallest key present. */
            for (size_t i = 1; i < c->ncur; i++) {
                if (c->cur[i].exhausted) break;
                if (zsi_compar_default(c->cur[i].cur.key, c->cur[i].cur.keylen,
                                       c->cur[0].cur.key,
                                       c->cur[0].cur.keylen) < 0) {
                    fprintf(stderr, "\n    FAIL step %d: element 0 not least\n",
                            steps);
                    current_test_failed = 1;
                    goto done;
                }
            }
        }

        if (zsi_cursor_next(c, &r) != ZS_OK) break;
        steps++;
        ASSERT(steps < 50);
    }

    ASSERT_EQ(steps, 5);        /* a, b, c, d, e */

done:
    zsi_cursor_free(c);
    zs_db_close(&db);
}

static void test_read_arrangements(void)
{
    /* T-5a: the same assertions against every structural arrangement.  Each
     * exercises a different combination of D-14b's search primitives, and the
     * answers MUST be identical throughout -- which is the concrete form of
     * G-7. */
    static const struct kv all[] = {
        {"a","A"}, {"b","B"}, {"c","C"}, {"d","D"}, {"e","E"}, {NULL,NULL}
    };
    static const char *expect = "a=A|b=B|c=C|d=D|e=E";
    struct zs_db *db;
    char got[256];

    /* 1. one unordered file only */
    clear_db();
    put_unordered_kv(1, all);
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);
    zs_db_close(&db);

    /* 2. several generations, the newest of them unordered.  D-1b allows only
     * one active-file name, so at most one unordered file can exist (D-12a):
     * a prefix of in-order files with the active one above them is the only
     * arrangement a real database is in, and it exercises the merge. */
    clear_db();
    { static const struct kv p[] = {{"a","A"},{"b","B"},{NULL,NULL}};
      put_inorder_kv(1, 1, p); }
    { static const struct kv p[] = {{"c","C"},{NULL,NULL}};
      put_inorder_kv(2, 2, p); }
    { static const struct kv p[] = {{"d","D"},{"e","E"},{NULL,NULL}};
      put_unordered_kv(3, p); }
    db = open_db(0);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);
    zs_db_close(&db);

    /* 3. one in-order file only */
    clear_db();
    put_inorder_kv(1, 1, all);
    db = open_db(0);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);
    zs_db_close(&db);

    /* 4. a mixture */
    clear_db();
    { static const struct kv p[] = {{"a","A"},{"c","C"},{NULL,NULL}};
      put_inorder_kv(1, 2, p); }
    { static const struct kv p[] = {{"b","B"},{"e","E"},{NULL,NULL}};
      put_inorder_kv(3, 3, p); }
    { static const struct kv p[] = {{"d","D"},{NULL,NULL}};
      put_unordered_kv(4, p); }
    db = open_db(0);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);
    zs_db_close(&db);

    /* 5. after a repack that collapsed some but not all files */
    clear_db();
    { static const struct kv p[] = {{"a","A"},{"b","B"},{"c","C"},{NULL,NULL}};
      put_inorder_kv(1, 3, p); }
    { static const struct kv p[] = {{"d","D"},{NULL,NULL}};
      put_inorder_kv(4, 4, p); }
    { static const struct kv p[] = {{"e","E"},{NULL,NULL}};
      put_unordered_kv(5, p); }
    db = open_db(0);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);
    zs_db_close(&db);

    /* 6. WITH AN EMPTY IN-ORDER FILE among populated ones -- where a binary
     * search or a cursor seek is most likely to go wrong (F-26g). */
    clear_db();
    { static const struct kv p[] = {{"a","A"},{"b","B"},{NULL,NULL}};
      put_inorder_kv(1, 1, p); }
    { static const struct kv p[] = {{NULL,NULL}};
      put_inorder_kv(2, 2, p); }                    /* empty, 96 bytes */
    { static const struct kv p[] = {{"c","C"},{"d","D"},{NULL,NULL}};
      put_inorder_kv(3, 3, p); }
    { static const struct kv p[] = {{NULL,NULL}};
      put_inorder_kv(4, 4, p); }                    /* another empty */
    { static const struct kv p[] = {{"e","E"},{NULL,NULL}};
      put_unordered_kv(5, p); }
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->snap->nfiles, 5u);
    ASSERT_EQU(db->snap->files[1]->nptrs, 0u);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, expect);

    /* point lookups agree, including for keys either side of everything */
    for (const struct kv *p = all; p->k; p++) {
        db_get(db, p->k, strlen(p->k), got, sizeof(got));
        if (strcmp(got, p->v) != 0) {
            fprintf(stderr, "\n    FAIL get %s: '%s'\n", p->k, got);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
    }
    db_get(db, "", 0, got, sizeof(got));    ASSERT_STR_EQ(got, "-");
    db_get(db, "zz", 2, got, sizeof(got));  ASSERT_STR_EQ(got, "-");
    db_get(db, "bb", 2, got, sizeof(got));  ASSERT_STR_EQ(got, "-");

    /* 7. a database that is ONLY an empty in-order file reads as empty and
     * iterates to zero records (F-26g). */
    zs_db_close(&db);
    clear_db();
    { static const struct kv p[] = {{NULL,NULL}};
      put_inorder_kv(1, 1, p); }
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "");
    db_get(db, "a", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "-");
    zs_db_close(&db);
}

static void test_read_seek_and_flags(void)
{
    struct zs_db *db;
    char got[256];

    clear_db();
    static const struct kv g[] = {
        {"ab","1"}, {"abc","2"}, {"abd","3"}, {"b","4"}, {"bc","5"}, {NULL,NULL}
    };
    put_inorder_kv(1, 1, g);
    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Seek to a present key, an absent one between two present, before all, and
     * after all. */
    db_scan(db, 0, "abc", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "abc=2|abd=3|b=4|bc=5");
    db_scan(db, 0, "abcz", 4, got, sizeof(got));
    ASSERT_STR_EQ(got, "abd=3|b=4|bc=5");
    db_scan(db, 0, "", 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "ab=1|abc=2|abd=3|b=4|bc=5");
    db_scan(db, 0, "zzz", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "");

    /* ZS_SKIPROOT: skip the first record if it matches exactly.  Only an exact
     * match, and only the first. */
    db_scan(db, ZS_SKIPROOT, "abc", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "abd=3|b=4|bc=5");
    db_scan(db, ZS_SKIPROOT, "abcz", 4, got, sizeof(got));
    ASSERT_STR_EQ(got, "abd=3|b=4|bc=5");   /* no exact match: nothing skipped */

    /* ZS_CURSOR_PREFIX: stop when the key leaves the prefix (D-14e step 6). */
    db_scan(db, ZS_CURSOR_PREFIX, "ab", 2, got, sizeof(got));
    ASSERT_STR_EQ(got, "ab=1|abc=2|abd=3");
    db_scan(db, ZS_CURSOR_PREFIX, "b", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "b=4|bc=5");
    db_scan(db, ZS_CURSOR_PREFIX, "abc", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "abc=2");
    db_scan(db, ZS_CURSOR_PREFIX, "zz", 2, got, sizeof(got));
    ASSERT_STR_EQ(got, "");
    db_scan(db, ZS_CURSOR_PREFIX, "", 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "ab=1|abc=2|abd=3|b=4|bc=5");   /* empty prefix: all */

    /* The two flags compose. */
    db_scan(db, ZS_CURSOR_PREFIX | ZS_SKIPROOT, "ab", 2, got, sizeof(got));
    ASSERT_STR_EQ(got, "abc=2|abd=3");

    zs_db_close(&db);
}

static void test_read_prefix_across_files(void)
{
    /* A prefix scan whose members are spread across files, with a deletion inside
     * the prefix -- so the bound, the merge and the tombstone filter all act at
     * once. */
    struct zs_db *db;
    char got[256];

    clear_db();
    { static const struct kv p[] = {{"pre1","a"},{"pre2","b"},{"pre3","c"},
                                    {"zzz","z"},{NULL,NULL}};
      put_inorder_kv(1, 1, p); }
    { static const struct kv p[] = {{"pre2",NULL},{"pre4","d"},{NULL,NULL}};
      put_unordered_kv(2, p); }

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    db_scan(db, ZS_CURSOR_PREFIX, "pre", 3, got, sizeof(got));
    ASSERT_STR_EQ(got, "pre1=a|pre3=c|pre4=d");
    zs_db_close(&db);
}

/*
 * T-5 model-based: randomised sequences against an in-memory reference, checked
 * after every step by BOTH a point lookup and a full scan, cross-checked against
 * each other.  That cross-check is the direct test for G-7 and D-14f.
 */
struct model {
    char key[64][16];
    char val[64][16];
    bool live[64];
    size_t n;
};

static int model_find(struct model *m, const char *k)
{
    for (size_t i = 0; i < m->n; i++)
        if (strcmp(m->key[i], k) == 0) return (int)i;
    return -1;
}

static void model_set(struct model *m, const char *k, const char *v)
{
    int i = model_find(m, k);
    if (i < 0) {
        assert(m->n < 64);
        i = (int)m->n++;
        snprintf(m->key[i], 16, "%s", k);
    }
    m->live[i] = (v != NULL);
    if (v) snprintf(m->val[i], 16, "%s", v);
}

static int model_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

static void model_scan(struct model *m, char *out, size_t outlen)
{
    char keys[64][16];
    size_t n = 0;
    for (size_t i = 0; i < m->n; i++)
        if (m->live[i]) snprintf(keys[n++], 16, "%s", m->key[i]);
    qsort(keys, n, 16, model_cmp);

    size_t used = 0;
    out[0] = '\0';
    for (size_t i = 0; i < n; i++) {
        int j = model_find(m, keys[i]);
        size_t need = strlen(keys[i]) + strlen(m->val[j]) + 3;
        if (used + need >= outlen) break;
        if (used) out[used++] = '|';
        used += (size_t)snprintf(out + used, outlen - used, "%s=%s",
                                 keys[i], m->val[j]);
    }
}

static void test_read_model(void)
{
    /* The generator MUST produce the shapes that stress the merge (T-5): the same
     * key live in several files at once, a key deleted in a newer file and present
     * in older ones, a key whose only version is in the oldest file, and keys
     * adjacent in comparator order but split across files.  A small key space over
     * several generations produces all four by construction. */
    struct model m;
    struct zs_db *db;
    char got[1024], want[1024];
    unsigned seed = 12345;
    const char *env = getenv("ZS_TEST_SEED");
    if (env) seed = (unsigned)strtoul(env, NULL, 10);

    memset(&m, 0, sizeof(m));
    clear_db();

    /* Eight generations of overlapping writes over a twelve-key space. */
    for (uint32_t gen = 1; gen <= 8; gen++) {
        struct kv kvs[8];
        char kbuf[8][16], vbuf[8][16];
        size_t n = 0;

        size_t count = 1 + (seed % 5);
        for (size_t i = 0; i < count && n < 7; i++) {
            seed = seed * 1103515245u + 12345u;
            unsigned which = (seed >> 16) % 12;
            seed = seed * 1103515245u + 12345u;
            bool del = ((seed >> 16) % 4) == 0;

            snprintf(kbuf[n], 16, "k%02u", which);
            if (del) {
                kvs[n].k = kbuf[n];
                kvs[n].v = NULL;
                model_set(&m, kbuf[n], NULL);
            } else {
                snprintf(vbuf[n], 16, "g%ur%zu", gen, i);
                kvs[n].k = kbuf[n];
                kvs[n].v = vbuf[n];
                model_set(&m, kbuf[n], vbuf[n]);
            }
            n++;
        }
        kvs[n].k = NULL;
        kvs[n].v = NULL;

        /* Only the NEWEST generation may be unordered: D-1b gives the active
         * file one name, so an older unordered generation would have been
         * overwritten rather than sitting beside this one -- leaving a hole in
         * the set.  Every earlier generation is therefore written in-order,
         * which is the state a writer leaves behind anyway (D-12b), and the
         * last one exercises the unordered primitive. */
        if (gen < 8) {
            /* in-order files need key order and one record per key */
            struct kv sorted[8];
            size_t sn = 0;
            for (size_t i = 0; i < n; i++) {
                bool dup = false;
                for (size_t j = i + 1; j < n; j++)
                    if (strcmp(kvs[i].k, kvs[j].k) == 0) dup = true;
                if (!dup) sorted[sn++] = kvs[i];
            }
            for (size_t i = 0; i < sn; i++)
                for (size_t j = i + 1; j < sn; j++)
                    if (strcmp(sorted[i].k, sorted[j].k) > 0) {
                        struct kv t = sorted[i];
                        sorted[i] = sorted[j];
                        sorted[j] = t;
                    }
            sorted[sn].k = NULL;
            sorted[sn].v = NULL;
            put_inorder_kv(gen, gen, sorted);
        } else {
            put_unordered_kv(gen, kvs);
        }

        db = open_db(0);
        ASSERT_NOT_NULL(db);

        /* The scan must match the model exactly. */
        db_scan(db, 0, NULL, 0, got, sizeof(got));
        model_scan(&m, want, sizeof(want));
        if (strcmp(got, want) != 0) {
            fprintf(stderr, "\n    FAIL gen %u scan\n      got  %s\n      want %s\n",
                    gen, got, want);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }

        /* And every key in the space must agree between the point lookup and the
         * scan -- the cross-check that is the direct test for G-7. */
        for (unsigned which = 0; which < 12; which++) {
            char k[16], expect[32];
            snprintf(k, sizeof(k), "k%02u", which);
            int i = model_find(&m, k);
            if (i >= 0 && m.live[i]) snprintf(expect, sizeof(expect), "%s", m.val[i]);
            else                     snprintf(expect, sizeof(expect), "-");

            db_get(db, k, strlen(k), got, sizeof(got));
            if (strcmp(got, expect) != 0) {
                fprintf(stderr, "\n    FAIL gen %u get %s: got '%s' want '%s'\n",
                        gen, k, got, expect);
                current_test_failed = 1;
                zs_db_close(&db);
                return;
            }

            /* the scan agrees with the lookup about presence */
            char needle[32];
            snprintf(needle, sizeof(needle), "%s=", k);
            db_scan(db, 0, NULL, 0, want, sizeof(want));
            bool in_scan = strstr(want, needle) != NULL;
            bool in_get = strcmp(got, "-") != 0;
            if (in_scan != in_get) {
                fprintf(stderr, "\n    FAIL gen %u %s: scan=%d get=%d\n",
                        gen, k, in_scan, in_get);
                current_test_failed = 1;
                zs_db_close(&db);
                return;
            }
        }

        zs_db_close(&db);
    }
}

/*
 * ============================================================
 * The write path and the public API (T-4)
 * ============================================================
 */

static struct zs_db *fresh_db(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    clear_db();
    setup.flags = ZS_CREATE;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/* For tests that need to BUILD a particular file layout.  D-16e merges one
 * away as fast as store+seal can make it, which is the point of D-16e and the
 * ruin of any test whose subject is the layout itself (A-14). */
static struct zs_db *fresh_db_noautorepack(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    clear_db();
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/* Collect a whole database through the PUBLIC scan, so these tests exercise the
 * same path a caller would. */
static int api_collect_cb(void *rock, const char *key, size_t keylen,
                          const char *val, size_t vallen)
{
    char *out = rock;
    size_t used = strlen(out);
    if (used) out[used++] = '|';
    memcpy(out + used, key, keylen); used += keylen;
    out[used++] = '=';
    memcpy(out + used, val, vallen); used += vallen;
    out[used] = '\0';
    return 0;
}

/* Keys only, for cases whose values are too large to join. */
static int api_keys_cb(void *rock, const char *key, size_t keylen,
                       const char *val, size_t vallen)
{
    char *out = rock;
    size_t used = strlen(out);
    (void)val; (void)vallen;
    if (used) out[used++] = '|';
    memcpy(out + used, key, keylen); used += keylen;
    out[used] = '\0';
    return 0;
}

static void api_scan(struct zs_db *db, char *out, size_t outlen)
{
    (void)outlen;
    out[0] = '\0';
    ASSERT_OK(zs_db_foreach(db, NULL, 0, NULL, api_collect_cb, out, 0));
}

static void test_write_basic(void)
{
    struct zs_db *db = fresh_db();
    char got[512];
    const char *v;
    size_t vl;

    ASSERT_NOT_NULL(db);

    ASSERT_OK(zs_db_store(db, "b", 1, "two", 3, 0));
    ASSERT_OK(zs_db_store(db, "a", 1, "one", 3, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "three", 5, 0));

    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 3u);
    ASSERT_MEM_EQ(v, "one", 3);

    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=one|b=two|c=three");

    /* Overwrite. */
    ASSERT_OK(zs_db_store(db, "b", 1, "TWO", 3, 0));
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "TWO", 3);

    /* Delete, via the macro, which is a store of NULL (A-1b). */
    ASSERT_OK(zs_db_delete(db, "b", 1, 0));
    ASSERT_EQ(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=one|c=three");

    /* A-1: an empty value is legal and DISTINCT from an absent key. */
    ASSERT_OK(zs_db_store(db, "empty", 5, "", 0, 0));
    ASSERT_OK(zs_db_fetch(db, "empty", 5, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 0u);
    ASSERT_NOT_NULL(v);
    ASSERT_EQ(zs_db_fetch(db, "absent", 6, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* F-14: a zero-length key is invalid. */
    ASSERT_EQ(zs_db_store(db, "", 0, "x", 1, 0), ZS_BADUSAGE);

    /* Reopening sees everything. */
    ASSERT_OK(zs_db_close(&db));
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=one|c=three|empty=");
    zs_db_close(&db);
}

static void test_write_txn_isolation(void)
{
    /* A-1a: a write inside a transaction is visible to subsequent reads on that
     * same transaction, and to nothing else until commit. */
    struct zs_db *db = fresh_db();
    struct zs_txn *txn = NULL;
    const char *v;
    size_t vl;
    char got[512];

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "before", 6, "x", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "inside", 6, "y", 1, 0));

    /* Read-your-own-writes. */
    ASSERT_OK(zs_txn_fetch(txn, "inside", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "y", 1);

    /* And the transaction's scan sees it, ordered among the committed records. */
    got[0] = '\0';
    ASSERT_OK(zs_txn_foreach(txn, NULL, 0, NULL, api_collect_cb, got, 0));
    ASSERT_STR_EQ(got, "before=x|inside=y");

    /* A second handle does not, until commit. */
    struct zs_db *other = open_db(0);
    ASSERT_NOT_NULL(other);
    ASSERT_EQ(zs_db_fetch(other, "inside", 6, NULL, NULL, &v, &vl, 0),
              ZS_NOTFOUND);

    ASSERT_OK(zs_txn_commit(&txn));
    ASSERT_NULL(txn);

    /* C-4i: `other` sees the commit on its NEXT read -- freshness belongs to
     * the begin, not the open.  Otherwise a handle held open across another
     * process's commit answers from the world as of its own last write. */
    ASSERT_OK(zs_db_fetch(other, "inside", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "y", 1);
    zs_db_close(&other);
    zs_db_close(&db);
}

static int reap(pid_t pid);     /* defined with the multi-process tests */

static void test_mp_read_sees_other_process_commit(void)
{
    /* C-4i in its sharpest shape: a long-lived READONLY handle in one process,
     * a commit in another, then a read on the old handle.  Answering
     * ZS_NOTFOUND here is a key that exists being reported absent. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL, *rdb = NULL;
    const char *v; size_t vl;
    pid_t pid;

    /* Not optional: leaks(1) tracking a forked child hangs, which is the
     * whole reason make leaks sets the variable -- this test lacking the
     * guard hung two full verification runs at exactly this line. */
    SKIP_IF_NO_FORK();

    clear_db();
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "base", 4, "b", 1, 0));
    zs_db_close(&db);

    /* The long-lived handle, opened readonly BEFORE the other process's
     * commit, and warmed so it holds a snapshot. */
    {
        struct zs_open_data rs = ZS_OPEN_DATA_INITIALIZER;
        rs.flags = ZS_SHARED;
        ASSERT_OK(zs_db_open(dbdir, &rs, &rdb));
        ASSERT_OK(zs_db_fetch(rdb, "base", 4, NULL, NULL, &v, &vl, 0));
    }

    pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zs_open_data ws = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *wdb = NULL;
        if (zs_db_open(dbdir, &ws, &wdb) != ZS_OK) _exit(1);
        if (zs_db_store(wdb, "mailbox", 7, "m", 1, 0) != ZS_OK) _exit(2);
        zs_db_close(&wdb);
        _exit(0);
    }
    ASSERT_EQ(reap(pid), 0);

    /* The commit completed before this read began, so it MUST be seen --
     * non-transactionally, and from a new read transaction. */
    ASSERT_OK(zs_db_fetch(rdb, "mailbox", 7, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "m", 1);
    {
        struct zs_txn *txn = NULL;
        ASSERT_OK(zs_db_begin_txn(rdb, 1, &txn));
        ASSERT_OK(zs_txn_fetch(txn, "mailbox", 7, NULL, NULL, &v, &vl, 0));
        ASSERT_OK(zs_txn_abort(&txn));
    }
    zs_db_close(&rdb);
}

static void test_read_freshens_after_rollover(void)
{
    /* C-4i's name-set half, isolated: the stale handle's snapshot holds NO
     * unordered file (the database was sealed), so the probe's active-size
     * check has nothing to stat and the changed NAME SET is the only thing
     * that can announce the commit -- which arrives in a brand-new
     * generation whose creation changes no existing file. */
    struct zs_db *a, *b;
    const char *v; size_t vl;

    clear_db();
    b = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(b);
    ASSERT_OK(zs_db_store(b, "old", 3, "o", 1, 0));
    ASSERT_OK(zs_db_seal(b));

    a = open_db(0);
    ASSERT_NOT_NULL(a);
    ASSERT_OK(zs_db_fetch(a, "old", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_NULL(zsi_snapshot_active(a->snap));      /* the premise */

    ASSERT_OK(zs_db_store(b, "new", 3, "n", 1, 0)); /* creates a generation */

    ASSERT_OK(zs_db_fetch(a, "new", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "n", 1);
    zs_db_close(&a);
    zs_db_close(&b);
}

/* C-4i under D-1b: the active file has a FIXED name, so "has anything
 * committed" is answered by that one file's identity and size.  These two cases
 * are the ones size alone cannot answer. */
static void test_freshen_notices_a_rollover_by_inode(void)
{
    /* A peer converts the active file and creates a replacement at the SAME
     * name (D-12b).  The replacement can be any size, including the same size,
     * so only the inode distinguishes it from "nothing happened". */
    struct zs_db *a, *b;
    const char *v; size_t vl;
    ino_t before, after;
    struct stat sb;
    char path[PATH_MAX], name[ZSI_NAME_MAX];

    clear_db();
    b = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(b);
    ASSERT_OK(zs_db_store(b, "old", 3, "o", 1, 0));

    a = open_db(0);
    ASSERT_NOT_NULL(a);
    ASSERT_OK(zs_db_fetch(a, "old", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(zsi_snapshot_active(a->snap));  /* the premise */

    zsi_name_current(name, a->uuid);
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
    ASSERT_EQ(stat(path, &sb), 0);
    before = sb.st_ino;

    /* Seal, then write again: the name now holds a different inode. */
    ASSERT_OK(zs_db_seal(b));
    ASSERT_OK(zs_db_store(b, "new", 3, "n", 1, 0));
    ASSERT_EQ(stat(path, &sb), 0);
    after = sb.st_ino;
    ASSERT(before != after);                        /* the premise */

    /* The stale handle must see it. */
    ASSERT_OK(zs_db_fetch(a, "new", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "n", 1);
    zs_db_close(&a);
    zs_db_close(&b);
}

static void test_freshen_notices_the_active_file_going_away(void)
{
    /* The other direction: we hold a snapshot WITH an active file, and a peer
     * seals it away.  Nothing is created, nothing grows -- the file simply
     * stops existing, and a probe that only compared sizes would never look. */
    struct zs_db *a, *b;
    const char *v; size_t vl;

    clear_db();
    b = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(b);
    ASSERT_OK(zs_db_store(b, "old", 3, "o", 1, 0));
    ASSERT_OK(zs_db_store(b, "two", 3, "t", 1, 0));

    a = open_db(0);
    ASSERT_NOT_NULL(a);
    ASSERT_OK(zs_db_fetch(a, "old", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(zsi_snapshot_active(a->snap));  /* the premise */

    /* Seal: the active file is converted and its name is gone. */
    ASSERT_OK(zs_db_seal(b));

    /* The stale handle's snapshot still names a file that no longer exists, so
     * it must rebuild rather than keep reading a set that has moved. */
    ASSERT_OK(zs_db_fetch(a, "two", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "t", 1);
    ASSERT_NULL(zsi_snapshot_active(a->snap));
    zs_db_close(&a);
    zs_db_close(&b);
}

static void test_cursor_live_sees_other_handle_commit(void)
{
    /* ZS_CURSOR_LIVE (D-14j): a live cursor observes commits from OTHER
     * handles mid-traversal, resuming strictly after its last key (D-14j-b).
     * Without the flag the same traversal keeps its snapshot -- both halves
     * asserted, since the boundary is the flag's whole meaning. */
    struct zs_db *db, *other;
    struct zs_cursor *c = NULL;
    const char *k, *v; size_t kl, vl;

    clear_db();
    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "3", 1, 0));

    other = open_db(0);
    ASSERT_NOT_NULL(other);

    /* ZS_SHARED matters here and is not decoration: a cursor from a handle
     * takes an implicit WRITE transaction unless told otherwise, and that
     * holds the write lock for the cursor's whole life -- so `other` could
     * never commit underneath it.  This test passed before C-1j only because
     * two handles in one process excluded each other by nothing; against a
     * second PROCESS it would always have deadlocked. */
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, ZS_CURSOR_LIVE | ZS_SHARED));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "a", 1);

    ASSERT_OK(zs_db_store(other, "b", 1, "2", 1, 0));

    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "b", 1);                   /* the flag, working */
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "c", 1);
    zs_cursor_fini(&c);

    /* Without the flag, the traversal's view stays put. */
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "a", 1);
    ASSERT_OK(zs_db_store(other, "aa", 2, "x", 1, 0));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "b", 1);                   /* not aa: snapshot held */
    zs_cursor_fini(&c);

    zs_db_close(&other);
    zs_db_close(&db);
}

static void test_probe_no_change_reuses_snapshot(void)
{
    /* C-4i's other edge: freshness must not mean rebuilding.  With nothing
     * committed in between, consecutive reads keep the SAME snapshot object
     * -- the probe is a few syscalls, not a snapshot take.  A commit through
     * another handle then swaps it. */
    struct zs_db *db, *other;
    struct zsi_snapshot *s1;
    const char *v; size_t vl;

    clear_db();
    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));

    /* The FIRST read on a handle refreshes once regardless: the probe has no
     * baseline until a freshen has run, and "no baseline" must read as stale
     * -- guessing fresh there would be the bug this whole test guards. */
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));

    s1 = db->snap;
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT(db->snap == s1);                         /* reused, not rebuilt */
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT(db->snap == s1);

    other = open_db(0);
    ASSERT_NOT_NULL(other);
    ASSERT_OK(zs_db_store(other, "k2", 2, "w", 1, 0));
    zs_db_close(&other);

    ASSERT_OK(zs_db_fetch(db, "k2", 2, NULL, NULL, &v, &vl, 0));
    ASSERT(db->snap != s1);                         /* the probe fired */
    zs_db_close(&db);
}

static void test_failed_refresh_keeps_probe_stale(void)
{
    /* C-4i promises the probe is EXACT, and that holds only if the baseline
     * is committed by a refresh that succeeded.  Committing it first leaves
     * a poisoned baseline behind a transient refresh failure: the next
     * probe's names already match, and a snapshot holding NO active file has
     * no size left to disagree, so a peer's commit into a file this handle
     * has never opened reads as fresh -- indefinitely, since every commit
     * lands in the file the baseline already names.  The seal is what makes
     * the shape harmful: it is the act == NULL case. */
    struct zs_db *db, *other;
    const char *v = NULL; size_t vl = 0;
    char newest[PATH_MAX];

    if (geteuid() == 0) SKIP("chmod 000 cannot fail an open for root");

    clear_db();
    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "old", 3, "1", 1, 0));
    ASSERT_OK(zs_db_seal(db));          /* the snapshot holds no active file */
    ASSERT_OK(zs_db_fetch(db, "old", 3, NULL, NULL, &v, &vl, 0)); /* baseline */

    /* A peer creates a new active file and commits into it. */
    other = open_db(0);
    ASSERT_NOT_NULL(other);
    ASSERT_OK(zs_db_store(other, "new", 3, "2", 1, 0));
    snprintf(newest, sizeof(newest), "%s",
             zsi_snapshot_active(other->snap)->fname);
    zs_db_close(&other);

    /* The probe fires -- the name set changed -- and the refresh it triggers
     * fails.  The failure is the point. */
    ASSERT_EQ(chmod(newest, 0), 0);
    ASSERT_EQ(zs_db_fetch(db, "new", 3, NULL, NULL, &v, &vl, 0), ZS_IOERROR);
    ASSERT_EQ(chmod(newest, 0644), 0);

    /* The failed refresh must not have committed the baseline: this read
     * must probe stale AGAIN, refresh for real, and see the peer's commit. */
    ASSERT_OK(zs_db_fetch(db, "new", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_MEM_EQ(v, "2", 1);

    zs_db_close(&db);
}

static void test_write_begin_reuses_snapshot(void)
{
    /* C-4i says "shared or exclusive": a WRITE begin may reuse the handle's
     * snapshot after the same exact probe a read uses.  The commit-site fold
     * (D-13b) keeps db->snap current across a sole writer's commits, so its
     * next begin has nothing to rebuild -- and rebuilding anyway replays the
     * active file, an O(active file) cost per commit that made a
     * one-store-per-transaction load quadratic.  Found downstream as a
     * throughput sawtooth against rollover_size. */
    struct zs_db *db, *other;
    struct zsi_snapshot *s1;
    struct zs_txn *txn = NULL;
    const char *v; size_t vl;

    clear_db();
    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);

    /* Two stores to reach the steady state: the first write begin has no
     * probe baseline and must refresh regardless, and its commit's fold
     * leaves db->snap current for the second. */
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

    /* Pinned so it survives a rebuild: two live snapshots cannot share an
     * address, so pointer equality means reuse rather than a recycled
     * allocation. */
    s1 = db->snap;
    s1->refcount++;

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT(db->snap == s1);                         /* reused, not rebuilt */
    ASSERT_OK(zs_txn_store(txn, "c", 1, "3", 1, 0));
    ASSERT_OK(zs_txn_commit(&txn));

    /* The probe is what keeps reuse honest: another handle's commit grew the
     * active file, so the next write begin must see it -- appending against
     * a stale view is the one direction reuse must never err in. */
    other = open_db(0);
    ASSERT_NOT_NULL(other);
    ASSERT_OK(zs_db_store(other, "d", 1, "4", 1, 0));
    zs_db_close(&other);

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT(db->snap != s1);                         /* the probe fired */
    ASSERT_OK(zs_txn_fetch(txn, "d", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_OK(zs_txn_abort(&txn));

    zsi_snapshot_release(&s1);
    zs_db_close(&db);
}

static void test_write_abort(void)
{
    /* An aborted transaction leaves no visible records, and the ROLLBACK it
     * appends is what stops a LATER commit's span enclosing them (F-21). */
    struct zs_db *db = fresh_db();
    struct zs_txn *txn = NULL;
    char got[512];

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "keep", 4, "1", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "gone", 4, "2", 1, 0));
    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_NULL(txn);

    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "keep=1");

    /* A commit after the abort: its records are live, the aborted ones are not.
     * Without the ROLLBACK record this span would begin where the aborted records
     * begin, and enclose them. */
    ASSERT_OK(zs_db_store(db, "after", 5, "3", 1, 0));
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "after=3|keep=1");

    /* And after reopening, which replays the file from scratch. */
    ASSERT_OK(zs_db_close(&db));
    db = open_db(0);
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "after=3|keep=1");

    /* An abort leaves NO trace at all in the file, because this writer buffers
     * until commit and so has nothing on disk to void.  A streaming writer would
     * leave a ROLLBACK span here (C-8, F-21); both are conforming, and the reader
     * handles either. */
    /* An empty transaction commits and aborts cleanly, leaving no trace. */
    long before = 0;
    char name[ZSI_NAME_MAX];
    /* The only place a test reaches into the handle after a REOPEN, which is why
     * this assertion is here and not everywhere: gcc cannot prove open_db
     * returned non-null, so `db->uuid` alone is "reading 16 bytes from a region
     * of size 0" under -Wstringop-overread, and Cyrus builds -Werror. */
    ASSERT_NOT_NULL(db);
    zsi_name_current(name, db->uuid);
    before = filesize(name);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_commit(&txn));
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_EQ(filesize(name), before);

    zs_db_close(&db);
}

/* D-13b: the folded index is the index a REPLAY would have built.
 *
 * The two paths reach the same structure by different routes -- the fold merges
 * the committed span as a sorted run, taking the pending skiplist's key order,
 * while a rebuild sorts what a replay collected in offset order -- so reading one
 * database through both is what tells them apart.  The writing handle's index was
 * folded; a second handle opened on the same directory replays.
 *
 * The workload has to reach both halves of the fold, and both sides of each tie:
 *
 *   - a transaction bigger than ZSI_DELTA_MAX, so the fold overflows the delta
 *     into the base merge, and one smaller, so it does not;
 *   - keys arriving out of key order, or the run's order proves nothing;
 *   - keys rewritten in a LATER transaction, where the run must beat the delta
 *     and (after a flush) the base;
 *   - a key rewritten and a key deleted WITHIN one transaction, which the pending
 *     set resolves to one entry before the fold ever sees it (D-17b);
 *   - a flush whose delta and base share keys, which is the merge's tie arm.
 *
 * The expected content is modelled rather than compared only between the two
 * handles: a differential assertion alone would pass if both paths were wrong. */
#define FOLD_KEYS 1200

static void fold_key(char *out, size_t outlen, int i)
{
    snprintf(out, outlen, "k%04d", i);
}

static void test_index_fold_run_matches_replay(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    struct zs_txn *txn = NULL;
    char *folded = malloc(1 << 17), *replayed = malloc(1 << 17);
    char *want = malloc(1 << 17);
    char *live = calloc(FOLD_KEYS + 200, 1);       /* 0 absent, else value tag */
    char key[16], val[16];
    size_t used = 0;

    ASSERT_NOT_NULL(db);
    ASSERT_NOT_NULL(folded);
    ASSERT_NOT_NULL(replayed);
    ASSERT_NOT_NULL(want);
    ASSERT_NOT_NULL(live);

    /* One transaction past ZSI_DELTA_MAX, keys strided so the run is not simply
     * ascending.  A stride coprime with the count visits every key once. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int n = 0, i = 0; n < FOLD_KEYS; n++, i = (i + 457) % FOLD_KEYS) {
        fold_key(key, sizeof(key), i);
        snprintf(val, sizeof(val), "a%04d", i);
        ASSERT_OK(zs_txn_store(txn, key, strlen(key), val, strlen(val), 0));
        live[i] = 'a';
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* Smaller than the delta bound, so this fold stays in the delta: rewrites of
     * existing keys (the run beats the base), new keys above the range, and
     * deletions of keys the first transaction created. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int i = 0; i < FOLD_KEYS; i += 4) {
        fold_key(key, sizeof(key), i);
        snprintf(val, sizeof(val), "b%04d", i);
        ASSERT_OK(zs_txn_store(txn, key, strlen(key), val, strlen(val), 0));
        live[i] = 'b';
    }
    for (int i = 1; i < FOLD_KEYS; i += 40) {
        fold_key(key, sizeof(key), i);
        ASSERT_OK(zs_txn_store(txn, key, strlen(key), NULL, 0, 0));
        live[i] = 0;
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* Rewritten and deleted WITHIN one transaction: the pending set resolves each
     * to a single entry, so the run the fold sees has one offset per key even
     * though the span holds two records for it. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    fold_key(key, sizeof(key), 7);
    ASSERT_OK(zs_txn_store(txn, key, strlen(key), "first", 5, 0));
    ASSERT_OK(zs_txn_store(txn, key, strlen(key), "c0007", 5, 0));
    live[7] = 'c';
    fold_key(key, sizeof(key), 11);
    ASSERT_OK(zs_txn_store(txn, key, strlen(key), "gone", 4, 0));
    ASSERT_OK(zs_txn_store(txn, key, strlen(key), NULL, 0, 0));
    live[11] = 0;
    ASSERT_OK(zs_txn_commit(&txn));

    /* And a fold that overflows a delta already holding keys the base holds too,
     * which is the flush's tie arm. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int i = 0; i < FOLD_KEYS; i += 2) {
        fold_key(key, sizeof(key), i);
        snprintf(val, sizeof(val), "d%04d", i);
        ASSERT_OK(zs_txn_store(txn, key, strlen(key), val, strlen(val), 0));
        live[i] = 'd';
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* What the database should now hold, in key order. */
    want[0] = '\0';
    for (int i = 0; i < FOLD_KEYS; i++) {
        if (!live[i]) continue;
        if (used) want[used++] = '|';
        used += (size_t)snprintf(want + used, 32, "k%04d=%c%04d", i, live[i], i);
    }

    api_scan(db, folded, 1 << 17);
    ASSERT_STR_EQ(folded, want);

    /* The same directory through a handle that never folded anything. */
    ASSERT_OK(zs_db_close(&db));
    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    api_scan(db, replayed, 1 << 17);
    ASSERT_STR_EQ(replayed, want);

    ASSERT_OK(zs_db_close(&db));
    free(folded);
    free(replayed);
    free(want);
    free(live);
}

/* D-13b: a writer folds the records it appended into the active file's index
 * rather than rebuilding the snapshot -- observable as the SNAPSHOT OBJECT a
 * write transaction's begin installed surviving that transaction's commit.
 * (Identity must be captured AFTER begin: a write begin performs its own full
 * refresh under the lock, so the handle's snapshot always changes there.)
 *
 * The handle and the committing transaction are the snapshot's two expected
 * holders at the fold; miscounting "sole holder" as one made the fold dead
 * code from the first commit, so every commit paid a SECOND full rebuild on
 * top of begin's -- and the writer-side P-13 publish, which lives in the same
 * branch, never ran at all. */
static void test_commit_folds_index_incrementally(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "seed", 4, "x", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    struct zsi_snapshot *during = db->snap;
    struct zsi_index *idx_during = zsi_snapshot_active(db->snap)->index;
    ASSERT_OK(zs_txn_store(txn, "next", 4, "y", 1, 0));
    ASSERT_OK(zs_txn_commit(&txn));

    ASSERT(db->snap == during);
    ASSERT(zsi_snapshot_active(db->snap)->index == idx_during);

    /* And the folded record is really in the index. */
    const char *v = NULL;
    size_t vl = 0;
    ASSERT_OK(zs_db_fetch(db, "next", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "y", 1);

    ASSERT_OK(zs_db_close(&db));
}

static void test_write_rollover(void)
{
    /* D-9a: a writer moves to a new file when the active file exceeds
     * rollover_size.  Rollover is cheap -- a new header and nothing else, since
     * the writer never appends a pointer section to an unordered file (D-11). */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 2048;         /* small, so a few writes cross it */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char val[256];
    memset(val, 'v', sizeof(val));
    for (int i = 0; i < 40; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), val, sizeof(val), 0));
    }

    /* Several generations now exist, and the steady state holds: AT MOST ONE
     * unordered file, the active one, with everything older converted.  The
     * writer converts before it finishes (D-12), and a commit that grows the
     * active file past rollover_size seals it too (D-25d), so this is true
     * after any commit rather than only after an explicit maintenance pass --
     * and it is zero, not one, when the last commit was the one that crossed.
     *
     * The active file has no pointer section, because a writer never appends one
     * to an unordered file (D-11). */
    ASSERT(db->snap->nfiles > 1);
    size_t nunordered = 0;
    for (size_t i = 0; i < db->snap->nfiles; i++) {
        if (zsi_file_is_unordered(db->snap->files[i])) {
            nunordered++;
            ASSERT_EQU(db->snap->files[i]->nptrs, 0u);    /* D-11 */
        } else {
            ASSERT(db->snap->files[i]->nptrs > 0
                   || db->snap->files[i]->hdr.end != 0);
        }
    }
    ASSERT(nunordered <= 1);
    if (nunordered)
        ASSERT(zsi_file_is_unordered(db->snap->files[db->snap->nfiles - 1]));

    /* Every record still reads back, across the file boundaries. */
    for (int i = 0; i < 40; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "k%03d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s missing after rollover\n", k);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
        ASSERT_EQU(vl, sizeof(val));
    }

    /* And the scan yields exactly 40, in order. */
    size_t count = 0;
    struct zs_cursor *c = NULL;
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED));
    const char *k, *v;
    size_t kl, vl;
    char prev[16] = "";
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        char cur[16];
        memcpy(cur, k, kl); cur[kl] = '\0';
        if (prev[0]) ASSERT(strcmp(prev, cur) < 0);
        snprintf(prev, sizeof(prev), "%s", cur);
        count++;
    }
    zs_cursor_abort(&c);
    ASSERT_EQU(count, 40u);

    zs_db_close(&db);
}

static void test_write_unclean_rollover(void)
{
    /* D-9/R-4: an active file that is not clean is never appended to.  The writer
     * moves to a new generation instead, so no chain is built on a boundary that
     * failed to validate. */
    struct zs_db *db = fresh_db();
    char name[ZSI_NAME_MAX];
    char got[512];

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    /* Append garbage behind the library's back, as a crash would leave. */
    int fd = open(dbpath(name), O_WRONLY | O_APPEND);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8), 8);
    close(fd);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT(!zsi_unordered_is_clean(db->snap->files[0]));

    /* The next write creates generation 2 rather than appending. */
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_EQU(db->snap->nfiles, 2u);
    ASSERT_EQU(db->snap->files[1]->hdr.start, 2u);

    /* Generation 1's committed record survives; the garbage is invisible. */
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=1|b=2");

    zs_db_close(&db);
}

/* F-18: what a record encodes to depends on its key and value, and on nothing
 * else -- not on the file it lands in, and not on what the key held before.
 *
 * This replaces test_write_ancestors, which pinned every row of the retired
 * F-17 table.  The property that matters now is the absence of those rows: the
 * same store into three databases with completely different histories must
 * produce identical bytes, which is what makes T-12a's byte-for-byte agreement
 * checkable and what removed the point lookup from the write path.
 *
 * Asserted on the bytes rather than through a round-trip, for the same reason
 * the old test was: a symmetric encode/decode bug survives a round-trip. */
/* A-1d's actual claim: the write is SKIPPED, not merely redundant.
 *
 * Every assertion in test_read_seek_and_flags would hold just as well if the
 * flag did nothing and the store went through, because storing the same value
 * twice leaves the same visible state.  So this one watches the bytes and the
 * change counter instead: the active file must not grow, and a cursor
 * traversing the transaction must not be told anything happened. */
/* The transaction arm's step hint (D-14j-a), tested at the ARM rather than
 * through a cursor -- which is the only place its two guards are separable.
 *
 * The hint caches where tkey resolved to, and is trusted only while the
 * transaction's pend_seq is unchanged.  A seek clears it outright.  Through a
 * cursor those two guards mask each other completely: any write bumps
 * pend_seq, which makes the cursor refresh and re-seek every txn arm, which
 * clears the hint -- so the stale-hint path is unreachable from above.
 *
 * Stepping the arm directly separates them: a write with no seek exercises the
 * sequence check, and a seek with no write exercises the clear. */
/* Walk a handle's whole key range, joining the keys with '|'. */
static void resort_walk(struct zs_db *db, char *out, size_t outlen)
{
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl, used = 0;

    out[0] = '\0';
    if (zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED) != ZS_OK) return;
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        if (used + kl + 2 >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, k, kl);
        used += kl;
        out[used] = '\0';
    }
    zs_cursor_abort(&c);
}

/* D-14i-a: the re-sort may take a shortcut, and the yields must be identical
 * either way.
 *
 * The shortcut is one comparison deciding that the advanced arm is still the
 * smallest, so nothing moves and the array is not touched.  Its failure mode is
 * returning early when the arm SHOULD have moved -- which leaves the array
 * mis-sorted and the merge yielding out of key order, silently.
 *
 * So both shapes are here.  Interleaved files make the head change on EVERY
 * step, so the shortcut must decline every time; a dominant file makes it
 * apply every time.  The naive insertion sort passes both, which is the point:
 * this pins the yields, not the effort. */
/* The platform assumption the active file's mapping rests on: an over-sized
 * MAP_SHARED mapping sees bytes appended through a SEPARATE descriptor,
 * without being remapped.
 *
 * zsi_file_remap maps the active file with headroom and then only moves the
 * `size` bound as it grows, which turns an munmap plus an mmap per commit into
 * no syscall at all.  That is sound wherever the page cache is shared between
 * a mapping and a descriptor -- Linux, macOS and the BSDs all are -- but it is
 * an assumption about the SYSTEM rather than about this code, so it is checked
 * here.  A platform that fails it would otherwise read zeros where committed
 * records should be, and would find out in production.
 *
 * Deliberately at the syscall level rather than through the library: the point
 * is to test the kernel's behaviour, and going through zeroskip would let a
 * bug in zeroskip mask it. */
static void test_file_grows_under_an_oversized_map(void)
{
    const size_t initial = 4096, maplen = 1u << 20, appended = 8192;
    char *path = dbpath("oversized-map");
    char buf[8192];
    struct stat sb;
    int fd, wfd;
    const char *m;

    ASSERT_EQ(mkdbdir(), 0);
    unlink(path);
    fd = open(path, O_RDWR | O_CREAT, 0600);
    ASSERT(fd >= 0);
    memset(buf, 'A', initial);
    ASSERT_EQ((size_t)write(fd, buf, initial), initial);

    /* Map far past EOF.  A platform may refuse this outright. */
    m = mmap(NULL, maplen, PROT_READ, MAP_SHARED, fd, 0);
    ASSERT(m != MAP_FAILED);
    ASSERT_EQ(m[0], 'A');

    /* Append through a second descriptor, which is what the writer does -- it
     * never writes through the mapping. */
    wfd = open(path, O_WRONLY | O_APPEND);
    ASSERT(wfd >= 0);
    memset(buf, 'B', appended);
    ASSERT_EQ((size_t)write(wfd, buf, appended), appended);
    ASSERT_EQ(fstat(fd, &sb), 0);
    ASSERT_EQU((size_t)sb.st_size, initial + appended);

    /* And the appended bytes are visible through the mapping made BEFORE them,
     * with no remap in between.  This is the whole assumption. */
    for (size_t off = initial; off < initial + appended; off++) {
        if (m[off] != 'B') {
            fprintf(stderr, "\n    FAIL byte %zu reads 0x%02x, expected 'B' -- "
                            "this platform does not show appends through an "
                            "existing mapping\n", off, (unsigned char)m[off]);
            current_test_failed = 1;
            break;
        }
    }

    ASSERT_EQ(munmap((void *)m, maplen), 0);
    close(wfd);
    close(fd);
    unlink(path);
}

/* The active file is mapped with HEADROOM, and access is bounded by the file
 * rather than by the mapping.
 *
 * These are two halves of one thing.  The headroom is what removes the munmap
 * and mmap from every commit; bounding by `size` is the only reason the
 * headroom is safe, since reading into it past EOF is SIGBUS.  Everywhere else
 * in the suite maplen == size, so this is the only place the difference between
 * the two bounds is observable at all. */
static void test_active_file_headroom_is_bounded_by_size(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    struct zsi_file *act;

    ASSERT_NOT_NULL(db);
    /* A few commits, so the commit-site fold has remapped with headroom. */
    for (int i = 0; i < 5; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "v", 1, 0));
    }

    act = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(act);
    ASSERT(act->size > 0);
    ASSERT(act->maplen > act->size);            /* headroom really is applied */

    /* The last real byte is reachable... */
    ASSERT_NOT_NULL(zsi_file_at(act, act->size - 1, 1));
    /* ...and the first byte of headroom is NOT, though the mapping covers it.
     * Bounding by maplen here would hand out a pointer past EOF, which reads
     * as SIGBUS rather than as data. */
    ASSERT_NULL(zsi_file_at(act, act->size, 1));
    ASSERT_NULL(zsi_file_at(act, act->size, 64));
    ASSERT_NULL(zsi_file_at(act, act->maplen - 1, 1));

    zs_db_close(&db);
}

static void test_cursor_resort_no_move(void)
{
    struct zs_db *db;
    char seen[64];

    /* Interleaved: a,c,e in one generation and b,d,f in the next, so the
     * smallest arm alternates and the head moves at every single step. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){
        {"a","1"}, {"c","3"}, {"e","5"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){
        {"b","2"}, {"d","4"}, {"f","6"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });
    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    resort_walk(db, seen, sizeof(seen));
    ASSERT_STR_EQ(seen, "a|b|c|d|e|f");
    zs_db_close(&db);

    /* Dominant: one generation holds everything below the other's single key,
     * so the head never moves until it is exhausted and the shortcut applies
     * on every step. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){
        {"a","1"}, {"b","2"}, {"c","3"}, {"d","4"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"z","9"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });
    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    resort_walk(db, seen, sizeof(seen));
    ASSERT_STR_EQ(seen, "a|b|c|d|z");
    zs_db_close(&db);
}

static void test_txn_arm_step_hint(void)
{
    struct zs_db *db = fresh_db();
    struct zs_txn *txn = NULL;
    struct zsi_fcur fc;

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (const char *p = "abcd"; *p; p++)
        ASSERT_OK(zs_txn_store(txn, p, 1, "v", 1, 0));

    memset(&fc, 0, sizeof(fc));
    fc.kind = ZSI_SRC_TXN;
    fc.txn = txn;
    fc.compar = db->compar;
    fc.gen = ZSI_GEN_TXN;

    ASSERT_OK(zsi_fcur_seek_first(&fc));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.key, "a", 1);

    ASSERT_OK(zsi_fcur_next(&fc));              /* arms the hint at index 1 */
    ASSERT_MEM_EQ(fc.cur.key, "b", 1);

    /* A write BELOW the position, with no seek in between.  "aa" inserts at
     * index 1, so every entry from there up shifts and the arm's record moves
     * from index 1 to index 2 -- a hint that survived the write would name
     * index 2 and re-yield "b". */
    ASSERT_OK(zs_txn_store(txn, "aa", 2, "!", 1, 0));
    ASSERT_OK(zsi_fcur_next(&fc));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.key, "c", 1);          /* NOT "b" */

    /* And a seek with NO write, so pend_seq still matches whatever the hint
     * was armed at.  Only clearing the hint gets this right; the sequence
     * check cannot, because nothing changed. */
    ASSERT_OK(zsi_fcur_next(&fc));              /* re-arms, now at "d" */
    ASSERT_MEM_EQ(fc.cur.key, "d", 1);
    ASSERT_OK(zsi_fcur_seek(&fc, "aa", 2));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.key, "aa", 2);         /* not wherever the hint said */

    zsi_fcur_fini(&fc);

    /* The same, travelling the other way (D-14k).  Reverse has its own index
     * resolution and so its own hint check, and a write below the position
     * moves the arm's record UP the array rather than leaving it put -- so a
     * stale hint lands short of where it should. */
    memset(&fc, 0, sizeof(fc));
    fc.kind = ZSI_SRC_TXN;
    fc.txn = txn;
    fc.compar = db->compar;
    fc.gen = ZSI_GEN_TXN;
    fc.reverse = true;

    ASSERT_OK(zsi_fcur_seek_last(&fc));         /* a aa b c d -> "d" */
    ASSERT_MEM_EQ(fc.cur.key, "d", 1);
    ASSERT_OK(zsi_fcur_next(&fc));              /* arms the hint below "c" */
    ASSERT_MEM_EQ(fc.cur.key, "c", 1);

    /* "a0" sorts between "a" and "aa", so it inserts at index 1 and shifts
     * "c" from index 3 to index 4.  A surviving hint names index 2 -- "aa" --
     * skipping "b" entirely. */
    ASSERT_OK(zs_txn_store(txn, "a0", 2, "!", 1, 0));
    ASSERT_OK(zsi_fcur_next(&fc));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.key, "b", 1);          /* NOT "aa" */

    zsi_fcur_fini(&fc);
    ASSERT_OK(zs_txn_abort(&txn));
    zs_db_close(&db);
}

static void test_ifchanged_writes_nothing(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    struct zs_txn *txn = NULL;
    size_t before, after, grown;
    unsigned long seq;

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "value", 5, 0));

    before = zsi_snapshot_active(db->snap)->size;

    /* Skipped: not one byte. */
    ASSERT_OK(zs_db_store(db, "k", 1, "value", 5, ZS_IFCHANGED));
    after = zsi_snapshot_active(db->snap)->size;
    ASSERT_EQU(after, before);

    /* The same store WITHOUT the flag writes a record, which is what makes the
     * assertion above mean something -- a store that could not have grown the
     * file either way would prove nothing. */
    ASSERT_OK(zs_db_store(db, "k", 1, "value", 5, 0));
    grown = zsi_snapshot_active(db->snap)->size;
    ASSERT(grown > before);

    /* Deleting an absent key is likewise free. */
    before = grown;
    ASSERT_OK(zs_db_delete(db, "absent", 6, ZS_IFCHANGED));
    ASSERT_EQU(zsi_snapshot_active(db->snap)->size, before);
    /* ...and without the flag it writes a tombstone for a key that never
     * existed, which is exactly the record A-1d exists to avoid. */
    ASSERT_OK(zs_db_delete(db, "absent", 6, 0));
    ASSERT(zsi_snapshot_active(db->snap)->size > before);

    /* And the change counter: a skipped write MUST NOT bump pend_seq, or every
     * cursor on the transaction refreshes and re-seeks for a write that did
     * not happen (D-14j). */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "k", 1, "pending", 7, 0));
    seq = txn->pend_seq;
    ASSERT_OK(zs_txn_store(txn, "k", 1, "pending", 7, ZS_IFCHANGED));
    ASSERT_EQU(txn->pend_seq, seq);                 /* nothing to observe */
    ASSERT_OK(zs_txn_store(txn, "k", 1, "changed", 7, ZS_IFCHANGED));
    ASSERT(txn->pend_seq != seq);                   /* a real write does bump */

    /* Both skip branches, not just the value one: deleting an absent key is
     * the other way to return without writing, and it has its own early exit
     * to get wrong. */
    seq = txn->pend_seq;
    ASSERT_OK(zs_txn_delete(txn, "still-absent", 12, ZS_IFCHANGED));
    ASSERT_EQU(txn->pend_seq, seq);
    ASSERT_OK(zs_txn_delete(txn, "k", 1, ZS_IFCHANGED));   /* present: a change */
    ASSERT(txn->pend_seq != seq);
    ASSERT_OK(zs_txn_commit(&txn));

    zs_db_close(&db);
}

static void test_write_record_is_self_contained(void)
{
    struct zsi_rec r;
    size_t off;
    char first[512], again[512], older[512];
    size_t firstlen, againlen, olderlen;

    /* (a) a create, into an empty database. */
    {
        struct zs_db *db = fresh_db_noautorepack();
        ASSERT_NOT_NULL(db);
        ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
        struct zsi_file *f = db->snap->files[0];
        ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
        ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off, &r));
        ASSERT_EQ(r.type, ZSI_KEYVALUE);
        ASSERT(r.len <= sizeof(first));
        firstlen = r.len;
        memcpy(first, r.base, r.len);
        zs_db_close(&db);
    }

    /* (b) an UPDATE of that key, later in the same file.  Under the old rule
     * this was the row that "coincides" with a create; now there is no rule to
     * coincide with. */
    {
        struct zs_db *db;
        clear_db();
        db = open_db(ZS_CREATE | ZS_NOAUTOREPACK);
        ASSERT_NOT_NULL(db);
        ASSERT_OK(zs_db_store(db, "k", 1, "other", 5, 0));
        ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
        struct zsi_file *f = db->snap->files[0];
        ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
        ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off, &r));
        ASSERT(r.len <= sizeof(again));
        againlen = r.len;
        memcpy(again, r.base, r.len);
        zs_db_close(&db);
    }

    /* (c) an update of a key whose previous version is in an OLDER FILE. */
    {
        struct zs_db *db;
        clear_db();
        { static const struct kv p[] = {{"k","old"},{NULL,NULL}};
          put_inorder_kv(1, 1, p); }
        put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });
        db = open_db(ZS_NOAUTOREPACK);
        ASSERT_NOT_NULL(db);
        ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
        struct zsi_file *f = db->snap->files[db->snap->nfiles - 1];
        ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
        ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off, &r));
        ASSERT_EQ(r.type, ZSI_KEYVALUE);        /* NOT a distinct "update" form */
        ASSERT(r.len <= sizeof(older));
        olderlen = r.len;
        memcpy(older, r.base, r.len);

        /* And a deletion of that same key is likewise just a DELETION. */
        ASSERT_OK(zs_db_delete(db, "k", 1, 0));
        f = db->snap->files[db->snap->nfiles - 1];
        ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
        ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off, &r));
        ASSERT_EQ(r.type, ZSI_DELETION);
        ASSERT_NULL(r.val);
        zs_db_close(&db);
    }

    /* All three are the same bytes.  Under the old format (c) differed from
     * (a) and (b) in its type byte, its header length and its total length. */
    ASSERT_EQU(againlen, firstlen);
    ASSERT_EQU(olderlen, firstlen);
    ASSERT_MEM_EQ(again, first, firstlen);
    ASSERT_MEM_EQ(older, first, firstlen);
}

static void test_write_encoding_boundaries(void)
{
    /* T-4's encoding boundaries, driven through the public API so the writer's
     * form selection is what is being tested. */
    struct zs_db *db = fresh_db();
    const char *v;
    size_t vl;
    ASSERT_NOT_NULL(db);

    char k255[256], k256[257];
    memset(k255, 'a', 255); k255[255] = '\0';
    memset(k256, 'b', 256); k256[256] = '\0';

    char *v65535 = malloc(65535), *v65536 = malloc(65536);
    ASSERT_NOT_NULL(v65535);
    ASSERT_NOT_NULL(v65536);
    memset(v65535, 'x', 65535);
    memset(v65536, 'y', 65536);

    ASSERT_OK(zs_db_store(db, k255, 255, "s", 1, 0));
    ASSERT_OK(zs_db_store(db, k256, 256, "b", 1, 0));
    ASSERT_OK(zs_db_store(db, "v1", 2, v65535, 65535, 0));
    ASSERT_OK(zs_db_store(db, "v2", 2, v65536, 65536, 0));

    /* Keys containing embedded NULs (F-13). */
    const char knul[] = { 'n', '\0', 'u', '\0', 'l' };
    const char vnul[] = { '\0', 'V', '\0' };
    ASSERT_OK(zs_db_store(db, knul, 5, vnul, 3, 0));

    /* A record landing exactly on an 8-byte boundary: keylen 3, vallen 0 gives
     * roundup8(4 + 3 + 1 + 0 + 1) = 16, and keylen 2 vallen 0 gives exactly 8. */
    ASSERT_OK(zs_db_store(db, "ab", 2, "", 0, 0));

    /* All read back. */
    ASSERT_OK(zs_db_fetch(db, k255, 255, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_db_fetch(db, k256, 256, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_db_fetch(db, "v1", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 65535u);
    ASSERT_OK(zs_db_fetch(db, "v2", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 65536u);
    ASSERT_OK(zs_db_fetch(db, knul, 5, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 3u);
    ASSERT_MEM_EQ(v, vnul, 3);
    ASSERT_OK(zs_db_fetch(db, "ab", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 0u);

    /* And it all survives a reopen, which replays the file from scratch and so
     * re-parses every form the writer chose (F-15). */
    ASSERT_OK(zs_db_close(&db));
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, k256, 256, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_db_fetch(db, "v2", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 65536u);
    ASSERT_OK(zs_db_fetch(db, knul, 5, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, vnul, 3);

    free(v65535);
    free(v65536);
    zs_db_close(&db);
}

static void test_api_three_forms(void)
{
    /* A-0: every flag exercised through all three entry points -- database,
     * transaction and cursor -- and asserted to behave IDENTICALLY.  The zs_db_*
     * forms are literally wrappers, so a divergence would mean the wrapper had
     * grown its own logic. */
    struct zs_db *db;
    struct zs_txn *txn = NULL;
    const char *v;
    size_t vl;

    /* ZS_IFNOTEXIST */
    db = fresh_db();
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "1", 1, ZS_IFNOTEXIST));
    ASSERT_EQ(zs_db_store(db, "k", 1, "2", 1, ZS_IFNOTEXIST), ZS_EXISTS);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_EQ(zs_txn_store(txn, "k", 1, "3", 1, ZS_IFNOTEXIST), ZS_EXISTS);
    ASSERT_OK(zs_txn_store(txn, "j", 1, "3", 1, ZS_IFNOTEXIST));
    ASSERT_OK(zs_txn_commit(&txn));
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "1", 1);

    /* ZS_IFEXIST */
    ASSERT_OK(zs_db_store(db, "k", 1, "upd", 3, ZS_IFEXIST));
    ASSERT_EQ(zs_db_store(db, "nope", 4, "x", 1, ZS_IFEXIST), ZS_NOTFOUND);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_EQ(zs_txn_store(txn, "nope", 4, "x", 1, ZS_IFEXIST), ZS_NOTFOUND);
    ASSERT_OK(zs_txn_commit(&txn));

    /* ZS_IFEXIST composes with delete to mean "delete only if present" (A-1b). */
    ASSERT_OK(zs_db_delete(db, "k", 1, ZS_IFEXIST));
    ASSERT_EQ(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_EQ(zs_db_delete(db, "k", 1, ZS_IFEXIST), ZS_NOTFOUND);

    /* ZS_IFCHANGED (A-1d): the write is skipped when the stored state already
     * matches, and ZS_OK is returned either way -- the requested state holds. */
    ASSERT_OK(zs_db_store(db, "c1", 2, "same", 4, 0));
    ASSERT_OK(zs_db_store(db, "c1", 2, "same", 4, ZS_IFCHANGED));   /* skipped */
    ASSERT_OK(zs_db_store(db, "c1", 2, "diff", 4, ZS_IFCHANGED));   /* written */
    ASSERT_OK(zs_db_fetch(db, "c1", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "diff", 4);

    /* Deleting a key that is already absent is not a change, so nothing is
     * written -- which is the case where the saving is structural, since
     * otherwise a tombstone exists for a key that never did. */
    ASSERT_OK(zs_db_delete(db, "never", 5, ZS_IFCHANGED));
    ASSERT_EQ(zs_db_fetch(db, "never", 5, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* And deleting a key that IS present is a change. */
    ASSERT_OK(zs_db_delete(db, "c1", 2, ZS_IFCHANGED));
    ASSERT_EQ(zs_db_fetch(db, "c1", 2, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    /* ...and deleting it again is not. */
    ASSERT_OK(zs_db_delete(db, "c1", 2, ZS_IFCHANGED));

    /* A-1's distinction survives: an EMPTY VALUE over a deletion is a change,
     * and is not confused with the deletion it replaces.  This is the case a
     * naive "compare the value bytes" test gets wrong, since both have length
     * zero. */
    ASSERT_OK(zs_db_store(db, "c1", 2, "", 0, ZS_IFCHANGED));
    ASSERT_OK(zs_db_fetch(db, "c1", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(v);
    ASSERT_EQU(vl, 0u);
    /* Storing the empty value again IS now a no-op. */
    ASSERT_OK(zs_db_store(db, "c1", 2, "", 0, ZS_IFCHANGED));
    ASSERT_OK(zs_db_fetch(db, "c1", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 0u);
    /* But deleting it is a change, not a match for the empty value. */
    ASSERT_OK(zs_db_delete(db, "c1", 2, ZS_IFCHANGED));
    ASSERT_EQ(zs_db_fetch(db, "c1", 2, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* Judged against the TRANSACTION's own view (A-1a), so it composes with an
     * earlier write in the same transaction rather than with what is
     * committed. */
    ASSERT_OK(zs_db_store(db, "c2", 2, "committed", 9, 0));
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "c2", 2, "pending", 7, 0));
    ASSERT_OK(zs_txn_store(txn, "c2", 2, "pending", 7, ZS_IFCHANGED));
    ASSERT_OK(zs_txn_store(txn, "c2", 2, "committed", 9, ZS_IFCHANGED));
    ASSERT_OK(zs_txn_commit(&txn));
    ASSERT_OK(zs_db_fetch(db, "c2", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "committed", 9);   /* the last one was a change, and took */

    /* ZS_FETCHNEXT: the record at or after the given key; the strict bound is
     * spelled with ZS_SKIPROOT (A-12).  Through both forms. */
    ASSERT_OK(zs_db_store(db, "a", 1, "A", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "B", 1, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "C", 1, 0));

    const char *k2;
    size_t kl2;
    ASSERT_OK(zs_db_fetch(db, "a", 1, &k2, &kl2, &v, &vl,
                          ZS_FETCHNEXT | ZS_SKIPROOT));
    ASSERT_EQU(kl2, 1u);
    ASSERT_MEM_EQ(k2, "b", 1);

    ASSERT_OK(zs_db_begin_txn(db, 1, &txn));
    ASSERT_OK(zs_txn_fetch(txn, "a", 1, &k2, &kl2, &v, &vl,
                           ZS_FETCHNEXT | ZS_SKIPROOT));
    ASSERT_MEM_EQ(k2, "b", 1);
    ASSERT_OK(zs_txn_abort(&txn));

    /* past the last key */
    ASSERT_EQ(zs_db_fetch(db, "zz", 2, &k2, &kl2, &v, &vl, ZS_FETCHNEXT),
              ZS_NOTFOUND);

    zs_db_close(&db);
}

static void test_api_cursor_replace(void)
{
    /* A cursor from a database owns an implicit transaction; replace writes at
     * the cursor's current key, and commit ends the transaction. */
    struct zs_db *db = fresh_db();
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    char got[512];

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "3", 1, 0));

    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, 0));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "a", 1);
    ASSERT_OK(zs_cursor_replace(c, "ONE", 3, 0));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "b", 1);
    ASSERT_OK(zs_cursor_delete(c, 0));      /* a replace with NULL (A-1b) */
    ASSERT_OK(zs_cursor_commit(&c));
    ASSERT_NULL(c);

    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=ONE|c=3");

    /* An aborted cursor discards its writes. */
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, 0));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_OK(zs_cursor_replace(c, "zzz", 3, 0));
    ASSERT_OK(zs_cursor_abort(&c));
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=ONE|c=3");

    /* Replacing before the first next is a usage error, not a silent no-op. */
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, 0));
    ASSERT_EQ(zs_cursor_replace(c, "x", 1, 0), ZS_BADUSAGE);
    ASSERT_OK(zs_cursor_abort(&c));

    zs_db_close(&db);
}

static void test_api_readonly(void)
{
    /* A-5: ZS_SHARED is read-only and MUST NOT write. */
    struct zs_db *db = fresh_db();
    struct zs_txn *txn = NULL;

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    ASSERT_EQ(zs_db_store(db, "x", 1, "y", 1, 0), ZS_READONLY);
    ASSERT_EQ(zs_db_delete(db, "k", 1, 0), ZS_READONLY);
    ASSERT_EQ(zs_db_begin_txn(db, 0, &txn), ZS_READONLY);
    ASSERT_NULL(txn);

    /* Reads work. */
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);

    /* A read transaction works. */
    ASSERT_OK(zs_db_begin_txn(db, 1, &txn));
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQ(zs_txn_store(txn, "x", 1, "y", 1, 0), ZS_READONLY);
    ASSERT_OK(zs_txn_abort(&txn));

    zs_db_close(&db);
}

static void test_api_pointer_lifetime(void)
{
    /* A-4: a pointer from zs_db_fetch stays valid until the NEXT call on that
     * handle.  Records from a mapping are stable for the snapshot's life; a
     * record from a transaction's pending array is not, so the wrapper copies it
     * -- and this is the case that would dangle if it did not.
     *
     * Run under ASan, where a violation is caught rather than merely observed to
     * work by luck. */
    struct zs_db *db = fresh_db();
    const char *v;
    size_t vl;

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "value", 5, 0));

    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 5u);
    ASSERT_MEM_EQ(v, "value", 5);

    /* Still readable before the next call. */
    ASSERT_MEM_EQ(v, "value", 5);

    /* A transaction's pointers live as long as the transaction. */
    struct zs_txn *txn = NULL;
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "pend", 4, "fromtxn", 7, 0));
    const char *pv;
    size_t pvl;
    ASSERT_OK(zs_txn_fetch(txn, "pend", 4, NULL, NULL, &pv, &pvl, 0));
    ASSERT_MEM_EQ(pv, "fromtxn", 7);
    /* more writes, then re-read the earlier pointer */
    ASSERT_OK(zs_txn_store(txn, "other", 5, "z", 1, 0));
    ASSERT_MEM_EQ(pv, "fromtxn", 7);
    ASSERT_OK(zs_txn_commit(&txn));

    zs_db_close(&db);
}

/* A-4a: a read before the transaction's first write, where that write starts a
 * NEW generation.
 *
 * The first store resolves the active file (D-9), and after a seal there is no
 * active file at all, so it creates one -- which refreshes the handle and
 * replaces the snapshot the earlier fetch returned a pointer into.  Releasing
 * that snapshot there unmaps the value under the caller.  Read-modify-write is
 * the ordinary shape of a transaction, so this fired constantly; it was
 * reported from the sqlite integration as crashes with no library frame in the
 * backtrace.
 *
 * The value is deliberately larger than a page, so the mapping it lives in is
 * one the kernel really does tear down. */
static void test_a4_borrow_survives_new_generation(void)
{
    struct zs_db *db = fresh_db();
    char big[8192];
    const char *k, *v;
    size_t kl, vl;

    ASSERT_NOT_NULL(db);
    memset(big, 'v', sizeof(big));
    ASSERT_OK(zs_db_store(db, "key", 3, big, sizeof(big), 0));

    /* Every file in-order, so the next transaction's first store must create a
     * generation rather than append to one. */
    ASSERT_OK(zs_db_seal(db));

    struct zs_txn *txn = NULL;
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_fetch(txn, "key", 3, &k, &kl, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(big));

    struct zsi_snapshot *before = txn->snap;
    ASSERT_OK(zs_txn_store(txn, "other", 5, "x", 1, 0));

    /* The swap really happened -- otherwise this test would pass for the wrong
     * reason on any change that stopped rolling over here. */
    ASSERT(txn->snap != before);
    ASSERT(txn->hold.n > 0);

    /* A-4: both pointers are still the caller's to read. */
    ASSERT_MEM_EQ(k, "key", 3);
    ASSERT_MEM_EQ(v, big, sizeof(big));

    ASSERT_OK(zs_txn_commit(&txn));
    zs_db_close(&db);
}

/* A-4a, the cursor half: a handle-live cursor follows the handle's file set
 * (D-14j), and the snapshot it leaves behind owns the bytes of every record it
 * has already yielded.  A shared cursor takes no write lock, so a commit
 * through the same handle moves the set underneath it. */
static void test_a4_borrow_survives_cursor_swap(void)
{
    struct zs_db *db = fresh_db();
    struct zs_cursor *c = NULL;
    char big[8192];
    const char *k, *v;
    size_t kl, vl;

    ASSERT_NOT_NULL(db);
    memset(big, 'v', sizeof(big));
    ASSERT_OK(zs_db_store(db, "a", 1, big, sizeof(big), 0));
    ASSERT_OK(zs_db_store(db, "b", 1, big, sizeof(big), 0));
    ASSERT_OK(zs_db_seal(db));

    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "a", 1);
    ASSERT_EQU(vl, sizeof(big));

    const char *held_k = k, *held_v = v;
    struct zsi_snapshot *before = c->snap;

    /* A commit on the same handle: db->snap moves, and the next step brings
     * the cursor onto it. */
    ASSERT_OK(zs_db_store(db, "c", 1, "z", 1, 0));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT(c->snap != before);
    ASSERT(c->hold.n > 0);

    /* The record yielded BEFORE the swap is still readable. */
    ASSERT_MEM_EQ(held_k, "a", 1);
    ASSERT_MEM_EQ(held_v, big, sizeof(big));

    zs_cursor_fini(&c);
    zs_db_close(&db);
}

/* A-4a, the case the first fix missed: the outgoing snapshot is still SHARED.
 *
 * Taking the mappings over is only available to the last holder, so with a
 * cursor also referencing it the swap can only keep a reference.  Dropping one
 * instead assumes the remaining holder outlives us, and it does not: the cursor
 * closes, the snapshot hits zero, and the TRANSACTION's borrow is unmapped
 * underneath it.  The shape that reaches it is fetch, then cursor, then
 * store. */
static void test_a4_borrow_survives_shared_snapshot_swap(void)
{
    struct zs_db *db = fresh_db();
    struct zs_cursor *c = NULL;
    char big[8192];
    const char *k, *v, *ck, *cv;
    size_t kl, vl, ckl, cvl;

    ASSERT_NOT_NULL(db);
    memset(big, 'v', sizeof(big));
    ASSERT_OK(zs_db_store(db, "key", 3, big, sizeof(big), 0));
    ASSERT_OK(zs_db_seal(db));

    struct zs_txn *txn = NULL;
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    /* The borrow, promised for the transaction's lifetime. */
    ASSERT_OK(zs_txn_fetch(txn, "key", 3, &k, &kl, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(big));

    /* A third reference on the transaction's snapshot. */
    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, 0));
    ASSERT_OK(zs_cursor_next(c, &ck, &ckl, &cv, &cvl));

    /* The swap, with the snapshot shared. */
    struct zsi_snapshot *before = txn->snap;
    ASSERT_OK(zs_txn_store(txn, "other", 5, "x", 1, 0));
    ASSERT(txn->snap != before);

    /* Now the cursor lets go.  Before the fix this unmapped `v`, so the reads
     * below come FIRST: a regression here should fail as the use-after-unmap
     * it is, not as a bookkeeping assertion that merely correlates with one. */
    zs_cursor_fini(&c);
    ASSERT_MEM_EQ(k, "key", 3);
    ASSERT_MEM_EQ(v, big, sizeof(big));

    /* The transaction holds its own references to the files it may have
     * returned pointers into -- which is the whole point: it no longer matters
     * who else was looking at the snapshot. */
    ASSERT(txn->hold.n > 0);

    ASSERT_OK(zs_txn_commit(&txn));
    zs_db_close(&db);
}

/* A-1 on the read side: an empty value is a non-NULL pointer of length 0 on
 * EVERY path, and absent is ZS_NOTFOUND. Returning NULL would collapse it into
 * the deletion case that store is careful to keep distinct, leaving a caller
 * to track "present but empty" itself. */
static int empty_cb(void *rock, const char *key, size_t keylen,
                    const char *val, size_t vallen)
{
    int *seen = rock;

    if (keylen == 7 && memcmp(key, "a_empty", 7) == 0) {
        CB_ASSERT(val != NULL);
        CB_ASSERT_EQ(vallen, 0);
        (*seen)++;
    }
    return 0;
}

static void test_empty_value_is_not_null_on_read(void)
{
    struct zs_db *db = fresh_db();
    struct zs_cursor *c = NULL;
    struct zs_txn *txn = NULL;
    const char *k, *v;
    size_t kl, vl;

    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a_empty", 7, "", 0, 0));
    ASSERT_OK(zs_db_store(db, "b_full", 6, "x", 1, 0));

    /* 1. zs_db_fetch */
    ASSERT_OK(zs_db_fetch(db, "a_empty", 7, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(v);
    ASSERT_EQU(vl, 0u);

    /* ... and absent is a different answer, not the same one. */
    ASSERT_EQ(zs_db_fetch(db, "nope", 4, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* 2. zs_txn_fetch over committed data */
    ASSERT_OK(zs_db_begin_txn(db, 1, &txn));
    ASSERT_OK(zs_txn_fetch(txn, "a_empty", 7, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(v);
    ASSERT_EQU(vl, 0u);
    ASSERT_OK(zs_txn_commit(&txn));

    /* 3. a cursor yield */
    ASSERT_OK(zs_db_begin_cursor(db, "a_", 2, &c, ZS_SHARED | ZS_CURSOR_PREFIX));
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "a_empty", 7);
    ASSERT_NOT_NULL(v);
    ASSERT_EQU(vl, 0u);
    zs_cursor_fini(&c);

    /* 4. a foreach callback */
    int seen = 0;
    ASSERT_OK(zs_db_foreach(db, NULL, 0, NULL, empty_cb, &seen, 0));
    ASSERT_EQ(seen, 1);

    /* 5. the transaction's OWN uncommitted write, which reads back through
     *    the pending array rather than a file mapping. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "c_pend", 6, "", 0, 0));
    ASSERT_OK(zs_txn_fetch(txn, "c_pend", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_NOT_NULL(v);
    ASSERT_EQU(vl, 0u);

    /* And a deletion in the same transaction stays absent, not empty. */
    ASSERT_OK(zs_txn_delete(txn, "b_full", 6, 0));
    ASSERT_EQ(zs_txn_fetch(txn, "b_full", 6, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_OK(zs_txn_commit(&txn));

    zs_db_close(&db);
}

/* C-4c: a rebuild reuses the file objects it already holds, because everything
 * except the active file is immutable. Asserted by OBJECT IDENTITY, which is
 * the only thing that separates "reused" from "re-derived the same answer".
 *
 * The active file is the exception and must be re-opened, since its index and
 * complete boundary belong to the snapshot that built them (G-4). */
static void test_snapshot_reuses_immutable_files(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    ASSERT_NOT_NULL(db);

    /* Three in-order files plus an active one. */
    for (int i = 0; i < 3; i++) {
        char k[32];
        int n = snprintf(k, sizeof(k), "key%d", i);
        ASSERT_OK(zs_db_store(db, k, n, "v", 1, 0));
        ASSERT_OK(zs_db_seal(db));
    }
    ASSERT_OK(zs_db_store(db, "live", 4, "v", 1, 0));

    struct zsi_snapshot *before = db->snap;
    ASSERT(before->nfiles >= 4);

    struct zsi_file *was[16];
    size_t n = before->nfiles;
    ASSERT(n <= 16);
    for (size_t i = 0; i < n; i++) was[i] = before->files[i];

    /* A full rebuild, not the C-4i probe: this is the path a peer's commit or
     * a repack puts a handle through. */
    ASSERT_OK(zsi_db_refresh(db));
    ASSERT(db->snap != before);
    ASSERT_EQU(db->snap->nfiles, n);

    /* Every immutable file is the SAME object. */
    for (size_t i = 0; i + 1 < n; i++)
        ASSERT(db->snap->files[i] == was[i]);

    /* The active file is not, because it is the one that can have changed. */
    ASSERT(zsi_file_is_unordered(db->snap->files[n - 1]));
    ASSERT(db->snap->files[n - 1] != was[n - 1]);

    zs_db_close(&db);
}

/* The cache holds a descriptor and a mapping per file, so a file the set no
 * longer names has to be let go -- otherwise a long-lived handle accumulates
 * every input a repack ever superseded. */
static void test_fcache_sweeps_superseded_files(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    ASSERT_NOT_NULL(db);

    for (int i = 0; i < 6; i++) {
        char k[32];
        int n = snprintf(k, sizeof(k), "key%d", i);
        ASSERT_OK(zs_db_store(db, k, n, "v", 1, 0));
        ASSERT_OK(zs_db_seal(db));
    }

    ASSERT(db->fcache.n >= 4);          /* the in-order files are held */

    /* Merge them away; the inputs are superseded and then removed (D-23). */
    ASSERT_OK(zs_db_compact(db));

    /* The cache must not still be holding the inputs.  Bounded by what the
     * current set actually names. */
    ASSERT(db->fcache.n <= db->snap->nfiles);

    zs_db_close(&db);
}

/* D-16e: the writer runs the cascade itself, so a caller that never calls
 * zs_db_repack does not accumulate files without bound.
 *
 * The workload is the one that produced this in the field: store, seal, repeat,
 * which leaves one single-generation in-order file per transaction. Without
 * D-16e that is a file per iteration forever; with it the count stays near
 * log(n), and every read stops merging across the whole set. */
static void test_autorepack_bounds_the_file_count(void)
{
    struct zs_db *db = fresh_db();
    char val[512];
    const int n = 40;

    ASSERT_NOT_NULL(db);
    memset(val, 'v', sizeof(val));

    for (int i = 0; i < n; i++) {
        char k[32];
        int kl = snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_OK(zs_db_store(db, k, kl, val, sizeof(val), 0));
        ASSERT_OK(zs_db_seal(db));
    }

    /* Geometric, not linear.  The bound is generous on purpose -- what is
     * being asserted is that it does not track n, not an exact shape. */
    ASSERT(db->snap->nfiles < 12);

    /* And every record is still there, which is the part a merge can break. */
    for (int i = 0; i < n; i++) {
        char k[32];
        const char *v;
        size_t vl;
        int kl = snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_OK(zs_db_fetch(db, k, kl, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, sizeof(val));
    }

    zs_db_close(&db);
}

/* D-16e's TRIGGER, not just its effect: the cascade runs at a begin that is
 * about to start a new generation, and at no other begin.
 *
 * The two tests either side of this one cannot see the difference.  Both use
 * store-seal-repeat, and after a seal there is no active file at all, so every
 * begin starts a generation and the narrow trigger and "every begin" agree on
 * the whole workload.  Broadening it merges at moments the narrow rule leaves
 * alone, which is a real behaviour change (D-16b puts an unbounded operation on
 * the write path) and went untested for exactly that reason.
 *
 * So this builds the state they never do: repack work pending AND a clean
 * active file with room in it, where the narrow trigger is false.
 *
 * Deliberately does NOT set ZS_NOAUTOREPACK, though its subject is a file
 * layout and the house rule says otherwise.  Here the armed cascade is the
 * subject -- what is asserted is that it declines to fire -- so suppressing it
 * would assert nothing. */
static void test_autorepack_only_at_a_new_generation(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    char val[512];
    const int n = 40;
    size_t before;

    ASSERT_NOT_NULL(db);
    memset(val, 'v', sizeof(val));

    /* Files to merge, with the cascade disarmed so they survive to be merged. */
    for (int i = 0; i < n; i++) {
        char k[32];
        int kl = snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_OK(zs_db_store(db, k, kl, val, sizeof(val), 0));
        ASSERT_OK(zs_db_seal(db));
    }
    /* One more store WITHOUT a seal, so the database is left with a clean
     * active file rather than none -- the whole point of the fixture. */
    ASSERT_OK(zs_db_store(db, "tail", 4, val, sizeof(val), 0));
    ASSERT_OK(zs_db_close(&db));

    /* Reopen with the cascade ARMED.  Opening does not repack (D-16e is a
     * write-begin rule), so the work is still here to be declined. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT(zs_db_should_repack(db));

    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(act);
        ASSERT(zsi_unordered_is_clean(act));
        ASSERT(act->size < db->rollover_size);      /* D-9a: room to append */
        ASSERT(act->nspans < db->rollover_txns);    /* D-9d: and spans spare */
    }
    before = db->snap->nfiles;
    ASSERT(before >= (size_t)n);

    /* A begin that APPENDS to that active file. D-9a's condition is false, so
     * no generation is starting and the cascade must not run -- this
     * transaction created no repack work and must not pay for anyone else's. */
    ASSERT_OK(zs_db_store(db, "appended", 8, val, sizeof(val), 0));
    ASSERT_EQU(db->snap->nfiles, before);
    ASSERT(zs_db_should_repack(db));       /* still pending, still declined */

    /* The positive control, and not optional: without it this test also passes
     * on a build where the cascade never runs at all, which is the opposite
     * bug.  Seal leaves no active file, so the next begin IS starting a
     * generation, and there the same pending work must be taken. */
    ASSERT_OK(zs_db_seal(db));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_OK(zs_db_store(db, "newgen", 6, val, sizeof(val), 0));
    ASSERT(db->snap->nfiles < before);

    /* And nothing was lost to the merge that did happen. */
    for (int i = 0; i < n; i++) {
        char k[32];
        const char *v;
        size_t vl;
        int kl = snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_OK(zs_db_fetch(db, k, kl, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, sizeof(val));
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* A-14: and the caller can turn it off, which is what makes the latency of an
 * unbounded cascade (D-16b) something they can schedule rather than something
 * that happens to them. The same workload, one flag apart. */
static void test_noautorepack_leaves_the_files(void)
{
    struct zs_db *db = fresh_db_noautorepack();
    char val[512];
    const int n = 40;

    ASSERT_NOT_NULL(db);
    memset(val, 'v', sizeof(val));

    for (int i = 0; i < n; i++) {
        char k[32];
        int kl = snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_OK(zs_db_store(db, k, kl, val, sizeof(val), 0));
        ASSERT_OK(zs_db_seal(db));
    }

    /* Nothing merged: one file per transaction, as before D-16e. */
    ASSERT(db->snap->nfiles >= (size_t)n);

    /* The work is still REPORTED -- the flag suppresses the doing, not the
     * telling, so a caller scheduling its own cascade can still find it. */
    ASSERT(zs_db_should_repack(db));
    ASSERT_OK(zs_db_repack(db));
    ASSERT(db->snap->nfiles < 12);

    zs_db_close(&db);
}

/*
 * ============================================================
 * Conversion (D-12, T-10a)
 * ============================================================
 */

/* Count unordered and in-order files in the current snapshot. */
static void count_kinds(struct zs_db *db, size_t *unordered, size_t *inorder)
{
    *unordered = *inorder = 0;
    for (size_t i = 0; i < db->snap->nfiles; i++) {
        if (zsi_file_is_unordered(db->snap->files[i])) (*unordered)++;
        else                                           (*inorder)++;
    }
}

static void test_convert_basic(void)
{
    /* Generation 1 is grown past rollover_size and converted to 1-1 before
     * the writer finishes -- by the same commit's seal (D-25d), or by rollover
     * plus the next writer's conversion (D-9a, D-12); either way the data
     * reads back identically and the input is retired. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char got[512];

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 256;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

    char pad[300];
    memset(pad, 'p', sizeof(pad));
    ASSERT_OK(zs_db_store(db, "big", 3, pad, sizeof(pad), 0));   /* forces rollover */
    ASSERT_OK(zs_db_store(db, "c", 1, "3", 1, 0));

    size_t un, in;
    count_kinds(db, &un, &in);
    ASSERT_EQU(un, 1u);            /* D-12a: exactly one, the active file */
    ASSERT(in >= 1);

    /* The converted file really is in-order, with a pointer section. */
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, 1u);
    ASSERT_EQU(db->snap->files[0]->hdr.end, 1u);
    ASSERT(db->snap->files[0]->nptrs > 0);
    ASSERT_OK(zsi_ptrs_verify_records(db->snap->files[0]));

    /* The unordered input was retired (D-23).  Under D-1b that cannot be
     * asserted by ABSENCE of a name -- the active file's name is reused by
     * whatever generation comes next -- so it is asserted by generation: the
     * file at that name, if there is one at all, is no longer generation 1. */
    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        if (act) ASSERT(act->hdr.start > 1u);
    }

    /* The keys survived the conversion, in order.  (The join would include a
     * 300-byte value, so this checks presence and order rather than the text.) */
    got[0] = '\0';
    ASSERT_OK(zs_db_foreach(db, NULL, 0, NULL, api_keys_cb, got, 0));
    ASSERT_STR_EQ(got, "a|b|big|c");

    /* Everything still reads by key. */
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "1", 1);
    ASSERT_OK(zs_db_fetch(db, "big", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(pad));

    /* And after a reopen, which validates the converted file's structure. */
    ASSERT_OK(zs_db_close(&db));
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "2", 1);
    zs_db_close(&db);
}

static void test_convert_steady_state(void)
{
    /* T-10a: drive many rollovers, asserting after EACH commit that at most
     * one unordered file remains -- exactly one while the active file is
     * below rollover_size, zero right after the commit that crossed it and
     * sealed (D-25d) -- and the rest are in-order (D-12a). */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    size_t sealed = 0;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char val[200];
    memset(val, 'v', sizeof(val));

    for (int i = 0; i < 30; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), val, sizeof(val), 0));

        size_t un, in;
        count_kinds(db, &un, &in);
        if (un > 1) {
            fprintf(stderr, "\n    FAIL after %d writes: %zu unordered files\n",
                    i, un);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
        /* and any unordered file is the newest */
        if (un)
            ASSERT(zsi_file_is_unordered(db->snap->files[db->snap->nfiles - 1]));
        else
            sealed++;
    }

    /* The workload crosses rollover_size many times, so the sealed state must
     * actually have been observed -- otherwise the "at most one" above never
     * tested anything D-25d changed. */
    ASSERT(sealed > 0);

    /* Every record readable, across all those files. */
    for (int i = 0; i < 30; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "k%03d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s lost\n", k);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
    }

    /* And the generation range tiles, which is what makes the set complete (D-6). */
    struct zsi_fileset fs;
    ASSERT_OK(zsi_fileset_scan(dbdir, &db->uuid, &fs));
    ASSERT_OK(zsi_fileset_resolve(&fs));
    zsi_fileset_fini(&fs);

    zs_db_close(&db);
}

static void test_convert_only_one_unordered_file(void)
{
    /* T-10a's second half.
     *
     * D-12b's OLDEST FIRST ordering has nothing left to order: there is exactly
     * one
     * active-file NAME (D-1b), so at most one unordered file can exist at all
     * -- the invariant is structural now rather than a steady state kept by
     * policy (D-12a), and "a backlog of unordered files" is not a state the
     * format can represent. What is asserted instead is that stronger
     * property, and that the one file still converts. */
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX];
    size_t un, io;

    clear_db();

    /* Three crashes in a row, each leaving an unclean active file: a valid
     * span, then a torn tail. Each one lands on the SAME name, so the third
     * is all that survives -- which is the point. */
    for (uint32_t gen = 1; gen <= 3; gen++) {
        struct sb s2;
        char k[16];
        sb_init(&s2, gen, ZSI_CSUM_XXHASH);
        snprintf(k, sizeof(k), "g%u", gen);
        sb_rec(&s2, k, strlen(k), "v", 1);
        sb_term(&s2, false);
        sb_raw(&s2, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8);   /* torn tail */
        zsi_name_current(name, test_uuid);
        ASSERT_EQ(mkdbdir(), 0);
        ASSERT_EQ(sb_write(&s2, name), 0);
        sb_free(&s2);
    }

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* One unordered file, however many crashes preceded it. */
    count_kinds(db, &un, &io);
    ASSERT_EQU(un, 1u);

    /* And a writer converts it: unclean is no obstacle, because the conversion
     * reads to the complete point (F-24) and the garbage beyond is simply not
     * carried over. */
    ASSERT_OK(zs_db_store(db, "after", 5, "v", 1, 0));
    count_kinds(db, &un, &io);
    ASSERT(io >= 1);

    /* The record from the surviving crash is still readable -- conversion kept
     * the valid prefix rather than discarding the file. */
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "g3", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);

    zs_db_close(&db);
}

static void test_convert_staging_exclusive(void)
{
    /* D-20a: a staging name already taken must make O_EXCL advance <n> rather than
     * overwrite.  Two processes writing one staging file would produce an
     * interleaved output that is then renamed in as though complete -- and a pid
     * is not unique on shared storage, so this is reachable rather than
     * theoretical. */
    struct zs_db *db = fresh_db();
    char taken[ZSI_NAME_MAX];
    int fd = -1;

    ASSERT_NOT_NULL(db);

    /* Occupy the first staging name. */
    zsi_staging_name(taken, 0);
    ASSERT_EQ(writefile(taken, "squatter", 8), 0);

    ASSERT_OK(zsi_staging_open(db, taken, &fd));
    ASSERT(fd >= 0);
    close(fd);

    /* It picked a different name, and did not clobber the squatter. */
    char first[ZSI_NAME_MAX];
    zsi_staging_name(first, 0);
    ASSERT(strcmp(taken, first) != 0);
    ASSERT_EQ(filesize(first), 8);

    unlink(dbpath(taken));
    unlink(dbpath(first));
    zs_db_close(&db);
}

static void test_convert_remove_refuses_when_needed(void)
{
    /* D-23: removal happens only after verifying a complete set exists WITHOUT the
     * candidate, and verification plus removal are one unbroken hold of the remove
     * lock.  If verification fails the file is left alone -- leaking a file costs
     * disk space, removing a needed one costs the database. */
    struct zs_db *db;
    char name[ZSI_NAME_MAX];

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Generation 2 is needed: without it the set does not tile. */
    zsi_name_format(name, db->uuid, 2, 2);
    ASSERT_EQ(zsi_remove_file(db, name), ZS_AGAIN);
    ASSERT_EQ(fexists(dbpath(name)), 0);        /* still there */

    /* A file nobody needs -- a superseded repack input -- is removed. */
    put_inorder_kv(1, 2, (const struct kv[]){ {"a","1"}, {"b","2"}, {NULL,NULL} });
    ASSERT_OK(zs_db_close(&db));
    db = open_db(0);
    ASSERT_NOT_NULL(db);

    zsi_name_format(name, db->uuid, 1, 1);
    ASSERT_OK(zsi_remove_file(db, name));
    ASSERT_EQ(fexists(dbpath(name)), -ENOENT);

    /* And now generation 2's narrow file is also superseded. */
    zsi_name_format(name, db->uuid, 2, 2);
    ASSERT_OK(zsi_remove_file(db, name));
    ASSERT_EQ(fexists(dbpath(name)), -ENOENT);

    /* But 1-2 itself is needed. */
    zsi_name_format(name, db->uuid, 1, 2);
    ASSERT_EQ(zsi_remove_file(db, name), ZS_AGAIN);
    ASSERT_EQ(fexists(dbpath(name)), 0);

    /* Removing a name that is not there at all. */
    zsi_name_format(name, db->uuid, 99, 99);
    ASSERT_EQ(zsi_remove_file(db, name), ZS_NOTFOUND);

    zs_db_close(&db);
}

static void test_convert_readonly_does_nothing(void)
{
    /* R-3: a read-only handle performs no conversion, however much backlog there
     * is.  Asserted by listing the directory before and after. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();

    /* One unordered file, because D-1b makes one the maximum -- the point of
     * the test is the READ-ONLY refusal, not the size of the backlog. */
    put_unordered_kv(1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });

    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    size_t un, in;
    count_kinds(db, &un, &in);
    ASSERT_EQU(un, 1u);
    ASSERT_EQ(zsi_convert_pending(db), ZS_READONLY);

    count_kinds(db, &un, &in);
    ASSERT_EQU(un, 1u);            /* unchanged */
    ASSERT_EQU(in, 0u);

    zs_db_close(&db);
}

/*
 * ============================================================
 * Repacking (T-7)
 * ============================================================
 */

/* Read a record's ancestor and form directly from a file, so tests can assert
 * what was WRITTEN rather than what round-trips. */
static bool rec_at_key(struct zsi_file *f, const char *key, size_t keylen,
                       struct zsi_rec *out)
{
    uint64_t idx;
    bool exact;

    if (zsi_ptrs_search(f, zsi_compar_default, key, keylen, &idx, &exact)
        != ZS_OK) return false;
    if (!exact) return false;
    return zsi_ptrs_rec(f, idx, out) == ZS_OK;
}

static struct zsi_file *file_with_range(struct zs_db *db, uint32_t s, uint32_t e)
{
    for (size_t i = 0; i < db->snap->nfiles; i++)
        if (db->snap->files[i]->hdr.start == s && db->snap->files[i]->hdr.end == e)
            return db->snap->files[i];
    return NULL;
}

static void test_repack_selection(void)
{
    /* Selection merges from the oldest file the newer ones collectively
     * outweigh.  Sizes here are controlled by record counts. */
    struct zs_db *db;
    size_t first, count;

    /* TWO files of equal size wait: the sum above the first is exactly its own
     * size, and the comparison is strict. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->snap->files[0]->size, db->snap->files[1]->size);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 0u);
    ASSERT(!zs_db_should_repack(db));
    zs_db_close(&db);

    /* A THIRD collapses all three: two of them together outweigh the first. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });
    put_inorder_kv(3, 3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });
    put_unordered_kv(4, (const struct kv[]){ {"d","4"}, {NULL,NULL} });
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 3u);
    ASSERT_EQU(first, 0u);
    ASSERT(zs_db_should_repack(db));
    zs_db_close(&db);

    /* The sum is what decides, not the neighbour: a big oldest file with two
     * smaller ones above it that TOGETHER outweigh it is selected whole, even
     * though no adjacent pair would trigger on its own. */
    clear_db();
    {
        struct kv many[24];
        char keys[24][16], vals[24][32];
        for (int i = 0; i < 23; i++) {
            snprintf(keys[i], 16, "k%03d", i);
            memset(vals[i], 'v', 30); vals[i][30] = '\0';
            many[i].k = keys[i]; many[i].v = vals[i];
        }
        many[23].k = NULL; many[23].v = NULL;
        put_inorder_kv(1, 1, many);
    }
    {
        struct kv some[16];
        char keys[16][16], vals[16][32];
        for (int i = 0; i < 15; i++) {
            snprintf(keys[i], 16, "m%03d", i);
            memset(vals[i], 'v', 30); vals[i][30] = '\0';
            some[i].k = keys[i]; some[i].v = vals[i];
        }
        some[15].k = NULL; some[15].v = NULL;
        put_inorder_kv(2, 2, some);
        put_inorder_kv(3, 3, some);
    }
    put_unordered_kv(4, (const struct kv[]){ {"z","4"}, {NULL,NULL} });
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    /* Neither newer file alone reaches the oldest ... */
    ASSERT(db->snap->files[1]->size < db->snap->files[0]->size);
    ASSERT(db->snap->files[2]->size < db->snap->files[0]->size);
    /* ... but together they exceed it. */
    ASSERT(db->snap->files[1]->size + db->snap->files[2]->size
           > db->snap->files[0]->size);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 3u);
    ASSERT_EQU(first, 0u);
    zs_db_close(&db);

    /* A much larger older file is NOT absorbed by a small newer one: the stop rule
     * fires, which is what bounds the work a single repack does. */
    clear_db();
    {
        struct kv many[40];
        char keys[40][16], vals[40][32];
        for (int i = 0; i < 39; i++) {
            snprintf(keys[i], 16, "k%03d", i);
            memset(vals[i], 'v', 30); vals[i][30] = '\0';
            many[i].k = keys[i]; many[i].v = vals[i];
        }
        many[39].k = NULL; many[39].v = NULL;
        put_inorder_kv(1, 1, many);
    }
    put_inorder_kv(2, 2, (const struct kv[]){ {"z","1"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT(db->snap->files[0]->size > db->snap->files[1]->size);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT(count < 2);
    ASSERT(!zs_db_should_repack(db));
    zs_db_close(&db);

    /* A large newest file and a small older one: the older is absorbed. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    {
        struct kv many[40];
        char keys[40][16], vals[40][32];
        for (int i = 0; i < 39; i++) {
            snprintf(keys[i], 16, "k%03d", i);
            memset(vals[i], 'v', 30); vals[i][30] = '\0';
            many[i].k = keys[i]; many[i].v = vals[i];
        }
        many[39].k = NULL; many[39].v = NULL;
        put_inorder_kv(2, 2, many);
    }
    put_unordered_kv(3, (const struct kv[]){ {"z","3"}, {NULL,NULL} });
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 2u);
    ASSERT_EQU(first, 0u);
    ASSERT(zs_db_should_repack(db));

    /* The repacker never selects the active file or any unordered file (D-15). */
    for (size_t i = first; i < first + count; i++)
        ASSERT(!zsi_file_is_unordered(db->snap->files[i]));

    ASSERT_OK(zs_db_repack(db));

    /* One output covering the whole input range (D-16b, D-21). */
    ASSERT_NOT_NULL(file_with_range(db, 1, 2));
    ASSERT_NULL(file_with_range(db, 1, 1));         /* inputs retired */
    ASSERT_NULL(file_with_range(db, 2, 2));
    ASSERT(!zs_db_should_repack(db));

    zs_db_close(&db);
}

/* A-17: the counters separate what a conversion rewrote from what the cascade did.
 *
 * A single "bytes rewritten" figure would be satisfied by counting either one
 * twice, so the assertion that matters is the SEPARATION: the same workload run
 * with the cascade disarmed must still report conversions, and must report zero
 * repacks.  Anything that folded the two together, or attributed a conversion to
 * the repack bucket, passes a "did it count something" test and fails this one.
 *
 * Small generations so a few hundred records produce several conversions and
 * enough files for the cascade to select from. */
static void test_db_stats_separates_repack_from_conversion(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db_stats armed, disarmed, again;
    struct zs_db *db = NULL;
    char key[32], val[128];

    memset(val, 'v', sizeof(val));

    /* A fresh handle reports nothing, before it has done anything. */
    clear_db();
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 8 * 1024;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_stats(db, &disarmed));
    ASSERT_EQU(disarmed.repacks, 0u);
    ASSERT_EQU(disarmed.conversions, 0u);
    ASSERT_EQU(disarmed.repack_bytes, 0u);
    ASSERT_EQU(disarmed.convert_bytes, 0u);

    for (int i = 0; i < 400; i++) {
        snprintf(key, sizeof(key), "k%05d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
    }
    ASSERT_OK(zs_db_stats(db, &disarmed));
    ASSERT_OK(zs_db_close(&db));

    /* With the cascade disarmed: conversions happened, repacks did not.  This is
     * the pair that proves the attribution rather than the counting. */
    ASSERT(disarmed.conversions > 0);
    ASSERT(disarmed.convert_records > 0);
    ASSERT(disarmed.convert_bytes > disarmed.convert_records);   /* whole files */
    ASSERT_EQU(disarmed.repacks, 0u);
    ASSERT_EQU(disarmed.repack_records, 0u);
    ASSERT_EQU(disarmed.repack_bytes, 0u);
    ASSERT_EQU(disarmed.repack_ns, 0u);

    /* The same workload with the cascade armed: both halves move. */
    clear_db();
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 400; i++) {
        snprintf(key, sizeof(key), "k%05d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
    }
    ASSERT_OK(zs_db_stats(db, &armed));

    ASSERT(armed.conversions > 0);
    ASSERT(armed.repacks > 0);
    ASSERT(armed.repack_records > 0);
    ASSERT(armed.repack_bytes > armed.repack_records);
#ifdef CLOCK_MONOTONIC
    /* A merge of hundreds of records cannot take zero nanoseconds.  Guarded
     * because A-17 permits zero where there is no monotonic clock. */
    ASSERT(armed.repack_ns > 0);
    ASSERT(armed.convert_ns > 0);
#endif

    /* Monotonic: more work never lowers a counter. */
    ASSERT_OK(zs_db_compact(db));
    ASSERT_OK(zs_db_stats(db, &again));
    ASSERT(again.repacks >= armed.repacks);
    ASSERT(again.repack_bytes >= armed.repack_bytes);
    ASSERT(again.conversions >= armed.conversions);

    /* And a compaction is a merge, so it lands in the repack half (A-17). */
    ASSERT(again.repacks > armed.repacks);

    ASSERT_EQ(zs_db_stats(NULL, &again), ZS_BADUSAGE);
    ASSERT_EQ(zs_db_stats(db, NULL), ZS_BADUSAGE);
    ASSERT_OK(zs_db_close(&db));
}

static void test_repack_max_size(void)
{
    /* A-16/D-16: repack_max_size skips LEADING in-order files too big to be
     * worth re-merging, so one merge rewrites about twice the cap rather than
     * the whole database.  The skipped files never merge again, so the file
     * count would grow without bound -- which is the read path degrading
     * linearly (D-14d) -- and the cap therefore yields once it has skipped more
     * than ZSI_REPACK_MAX_FROZEN of them. */
    struct zs_db *db;
    size_t first, count, cap;

    /* Four in-order files, the oldest much the largest.  The newer three
     * together outweigh it, so the uncapped rule merges all four -- and so does
     * the DEFAULT cap, 512MB being a no-op at any size a test can build. */
    clear_db();
    put_inorder_n(1, 1, "k", 23);
    put_inorder_n(2, 2, "m", 15);
    put_inorder_n(3, 3, "p", 15);
    put_inorder_n(4, 4, "r", 15);
    put_unordered_kv(5, (const struct kv[]){ {"z","5"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT(db->snap->files[0]->size > db->snap->files[1]->size);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 4u);
    ASSERT_EQU(first, 0u);

    /* A cap above every file but the oldest. */
    cap = db->snap->files[0]->size - 1;
    ASSERT(cap > db->snap->files[1]->size);
    zs_db_close(&db);

    /* The oldest is skipped and the walk starts at the first file at or under
     * the cap, where the sum rule still fires among the three above it. */
    db = open_db_repack_max(0, cap);
    ASSERT_NOT_NULL(db);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 3u);
    ASSERT_EQU(first, 1u);
    ASSERT(zs_db_should_repack(db));
    zs_db_close(&db);

    /* An oversized file INSIDE the chosen range is merged like any other:
     * only the leading run is skipped, because excluding a file in the middle
     * would break adjacency (D-16). */
    clear_db();
    put_inorder_n(1, 1, "a", 1);
    put_inorder_n(2, 2, "k", 40);
    put_unordered_kv(3, (const struct kv[]){ {"z","3"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    cap = db->snap->files[0]->size;             /* oldest fits, newest does not */
    ASSERT(cap < db->snap->files[1]->size);
    zs_db_close(&db);

    db = open_db_repack_max(0, cap);
    ASSERT_NOT_NULL(db);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 2u);
    ASSERT_EQU(first, 0u);
    zs_db_close(&db);

    /* The sum that decides is taken over the SURVIVING range, not the whole
     * in-order prefix.  A skipped file is not merge work, so counting its bytes
     * as weight above the candidate merges files the rule should have left
     * alone: here the only file above the candidate is far smaller than it, and
     * nothing but the skipped file's bytes could make the sum win. */
    clear_db();
    put_inorder_n(1, 1, "a", 40);
    put_inorder_n(2, 2, "k", 20);
    put_inorder_n(3, 3, "p", 5);
    put_unordered_kv(4, (const struct kv[]){ {"z","4"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    cap = db->snap->files[1]->size;             /* skips the oldest, and only it */
    ASSERT(cap < db->snap->files[0]->size);
    ASSERT(db->snap->files[2]->size < cap);
    ASSERT(db->snap->files[0]->size + db->snap->files[2]->size > cap);
    zs_db_close(&db);

    db = open_db_repack_max(0, cap);
    ASSERT_NOT_NULL(db);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 0u);
    ASSERT(!zs_db_should_repack(db));
    zs_db_close(&db);

    /* The budget.  A cap of one byte makes every file oversized, so the skip
     * consumes the whole in-order prefix and nothing is selected ... */
    clear_db();
    for (uint32_t g = 1; g <= ZSI_REPACK_MAX_FROZEN; g++)
        put_inorder_n(g, g, "a", 1);

    db = open_db_repack_max(0, 1);
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->snap->nfiles, (size_t)ZSI_REPACK_MAX_FROZEN);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, 0u);
    ASSERT(!zs_db_should_repack(db));
    zs_db_close(&db);

    /* ... until one more file takes the skipped count PAST the budget, when the
     * cap yields and the uncapped walk merges the pile. */
    put_inorder_n(ZSI_REPACK_MAX_FROZEN + 1, ZSI_REPACK_MAX_FROZEN + 1, "a", 1);

    db = open_db_repack_max(0, 1);
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->snap->nfiles, (size_t)ZSI_REPACK_MAX_FROZEN + 1);
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT_EQU(count, (size_t)ZSI_REPACK_MAX_FROZEN + 1);
    ASSERT_EQU(first, 0u);
    ASSERT(zs_db_should_repack(db));
    zs_db_close(&db);
}

static void test_repack_one_record_per_key(void)
{
    /* D-17: exactly one record per key in the output, built from the live records
     * of all inputs, taking V3's value. */
    struct zs_db *db;
    char got[512];

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","a1"}, {"k","v1"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"k","v2"}, {"m","m2"}, {NULL,NULL} });
    put_inorder_kv(3, 3, (const struct kv[]){ {"k","v3"}, {"z","z3"}, {NULL,NULL} });
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Force a full merge regardless of the size rule. */
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 3));
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 1, 3);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 4u);          /* a, k, m, z -- k once */
    ASSERT_OK(zsi_ptrs_verify_records(out));

    struct zsi_rec r;
    ASSERT(rec_at_key(out, "k", 1, &r));
    ASSERT_MEM_EQ(r.val, "v3", 2);        /* V3's value */

    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=a1|k=v3|m=m2|z=z3");
    zs_db_close(&db);
}

/* D-17b: the emitted record takes V3's value under the total order -- across
 * files by increasing start generation.  Getting the order wrong silently emits
 * the value from whichever record the merge's iteration happened to touch
 * first or last, which no amount of "one record per key" checking would see. */
static void test_repack_version_order(void)
{
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    struct zsi_rec r;

    clear_db();
    ASSERT_EQ(mkdbdir(), 0);

    /* Three generations, each holding a different version of "k".  The values
     * are deliberately NOT in an order that any accidental iteration would
     * produce by luck: the newest sorts lowest as a byte string. */
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "zzz", 3);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "mmm", 3);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "aaa", 3);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 3));
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 1, 3);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 1u);                     /* D-17: one record per key */
    ASSERT(rec_at_key(out, "k", 1, &r));
    ASSERT_MEM_EQ(r.val, "aaa", 3);                 /* V3's value, generation 3 */
    ASSERT_EQ(r.type, ZSI_KEYVALUE);                /* the only value form now */
    zs_db_close(&db);
}

/* Every row of D-18, which is D-19's retention test written out.
 *
 * The two rows that DROP matter as much as the two that keep: a rule that never
 * drops is trivially safe and reclaims nothing, so a mutation that made
 * retention unconditional would pass a keep-only test. */
static void test_repack_d18_table(void)
{
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    struct zsi_rec r;
    struct zsi_file *out;

    /* Row 1: V3 is a value -> emitted, whatever lies below. */
    clear_db();
    ASSERT_EQ(mkdbdir(), 0);
    put_inorder_kv(1, 1, (const struct kv[]){ {"k","below"}, {NULL,NULL} });
    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "v2", 2);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 1, 2));       /* merge [2,3] */
    ASSERT_OK(zsi_db_refresh(db));
    out = file_with_range(db, 2, 3);
    ASSERT_NOT_NULL(out);
    ASSERT(rec_at_key(out, "k", 1, &r));
    ASSERT_MEM_EQ(r.val, "v2", 2);
    ASSERT_EQ(r.type, ZSI_KEYVALUE);
    zs_db_close(&db);

    /* Row 2: V3 is a deletion and a VALUE lies below -> the tombstone is kept.
     * This is the load-bearing row; the others only reclaim space. */
    clear_db();
    ASSERT_EQ(mkdbdir(), 0);
    put_inorder_kv(1, 1, (const struct kv[]){ {"k","below"}, {NULL,NULL} });
    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 1, 2));
    ASSERT_OK(zsi_db_refresh(db));
    out = file_with_range(db, 2, 3);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 1u);
    ASSERT(rec_at_key(out, "k", 1, &r));
    ASSERT_NULL(r.val);                             /* the tombstone survives */
    ASSERT_EQ(r.type, ZSI_DELETION);
    zs_db_close(&db);

    /* Row 3: V3 is a deletion and a DELETION lies below -> dropped.  That
     * deletion already hides everything under it, so ours adds nothing.  This
     * row is free precision: the search stops at the first file holding the key
     * either way, so it costs nothing to look at what it found. */
    clear_db();
    ASSERT_EQ(mkdbdir(), 0);
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_rec(&b, "other", 5, "o", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 1, 2));
    ASSERT_OK(zsi_db_refresh(db));
    out = file_with_range(db, 2, 3);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 0u);                     /* dropped */
    ASSERT(!rec_at_key(out, "k", 1, &r));
    zs_db_close(&db);

    /* Row 4: V3 is a deletion and NOTHING lies below -> dropped, because the
     * key's whole lifespan is inside the inputs. */
    clear_db();
    ASSERT_EQ(mkdbdir(), 0);
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "v1", 2);
    ib_rec(&b, "other", 5, "o", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 2));       /* merge [1,2] */
    ASSERT_OK(zsi_db_refresh(db));
    out = file_with_range(db, 1, 2);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 1u);                     /* only "other" */
    ASSERT(!rec_at_key(out, "k", 1, &r));
    ASSERT(rec_at_key(out, "other", 5, &r));
    zs_db_close(&db);
}

static void test_repack_d19a_resurrection(void)
{
    /* The resurrection D-19 exists to prevent, constructed directly.
     *
     * A value in an older file, a tombstone in the range being repacked,
     * nothing newer.  Dropping the tombstone -- which under the retired
     * ancestor scheme looked like a safe optimisation, and under this one looks
     * like one too if the search below is skipped -- lets generation 1's value
     * come back.
     *
     * Both halves are asserted: that the key stays absent with the tombstone,
     * AND that removing it does produce the bug.  The second half is what stops
     * the rule being deleted as dead weight later. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    const char *v;
    size_t vl;

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"k","RESURRECTED"}, {NULL,NULL} });

    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    /* Generation 3 is empty -- it exists so [2,3] is a legal merge range. */
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* Repack [2,3].  Generation 1 is below the output range and holds a VALUE
     * for "k", so the tombstone must be retained. */
    ASSERT_OK(zsi_repack_merge(db, db->snap, 1, 2));
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 2, 3);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 1u);                     /* the tombstone is there */

    /* The key stays absent -- generation 1's value did not come back. */
    ASSERT_EQ(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    zs_db_close(&db);

    /* Now the other half: hand-build the same output WITHOUT the tombstone, which
     * is what dropping it would produce, and confirm the value resurrects. */
    zsi_name_format(name, test_uuid, 2, 3);
    ASSERT_EQ(unlink(dbpath(name)), 0);
    ib_init(&b, 2, 3, ZSI_CSUM_XXHASH);
    ib_finish(&b);                                  /* zero records */
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "RESURRECTED", 11);            /* the bug, demonstrated */
    zs_db_close(&db);
}

/* D-19's counterpart, and the reason it looks only DOWNWARD.
 *
 * Same shape as the resurrection above except that a newer file re-creates the
 * key: created in the range being repacked, deleted there, re-created above it.
 * Nothing lies below, so the tombstone is dropped -- and that is correct, both
 * because the key's lifespan is contained and because the newer record would
 * shadow it anyway.
 *
 * A newer file can only ever make a retained tombstone REDUNDANT; it can never
 * make a dropped one unsafe.  That asymmetry is what lets the test consult one
 * direction only, and this is the case that would expose it if the reasoning
 * were backwards. */
static void test_repack_d19_newer_file_recreates(void)
{
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    const char *v;
    size_t vl;
    struct zsi_rec r;

    clear_db();
    ASSERT_EQ(mkdbdir(), 0);

    /* [1,2] is the range to repack: "k" created then deleted inside it. */
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "gone", 4);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    /* Generation 3, ABOVE the range, re-creates it. */
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "NEW", 3);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "NEW", 3);

    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 2));       /* merge [1,2] */
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 1, 2);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 0u);                     /* dropped: nothing below */
    ASSERT(!rec_at_key(out, "k", 1, &r));

    /* And the read is unchanged, which is the point. */
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "NEW", 3);
    zs_db_close(&db);
}

/* D-19a: a record is emitted even when a NEWER file already shadows the key.
 *
 * Being shadowed does not permit dropping it -- only D-19 does, and D-19 asks
 * about what lies BELOW.  Proving that a newer file shadows a key would cost a
 * lookup per KEY, where D-19's test costs one per surviving tombstone. */
static void test_repack_d19a_shadowed(void)
{
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    struct zsi_rec r;

    clear_db();
    ASSERT_EQ(mkdbdir(), 0);

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "old", 3);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    /* Newer, and holding a value for the same key. */
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_rec(&b, "k", 1, "newer", 5);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 3, 3);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(4, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOAUTOREPACK);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 2));       /* merge [1,2] */
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 1, 2);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 1u);                 /* written, though shadowed */
    ASSERT(rec_at_key(out, "k", 1, &r));
    ASSERT_MEM_EQ(r.val, "old", 3);
    zs_db_close(&db);
}

static void test_repack_empty_output(void)
{
    /* D-22: the output may legitimately hold zero records -- generation X creates a
     * key and X+1 deletes it, repacked together.  The file MUST still be written so
     * the generation range stays tiled (D-6). */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    char got[256];

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "only", 4, "v", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    ib_init(&b, 2, 2, ZSI_CSUM_XXHASH);
    ib_rec(&b, "only", 4, NULL, 0);        /* chain begins in 1 */
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 2, 2);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 2));
    ASSERT_OK(zsi_db_refresh(db));

    struct zsi_file *out = file_with_range(db, 1, 2);
    ASSERT_NOT_NULL(out);
    ASSERT_EQU(out->nptrs, 0u);                     /* empty, and written */
    zsi_name_format(name, test_uuid, 1, 2);
    ASSERT_EQ(filesize(name), 96);                  /* F-26g's exact form */
    ASSERT_OK(zsi_ptrs_verify_records(out));

    /* The set still tiles, and the database reads as empty. */
    struct zsi_fileset fs;
    ASSERT_OK(zsi_fileset_scan(dbdir, &db->uuid, &fs));
    ASSERT_OK(zsi_fileset_resolve(&fs));
    zsi_fileset_fini(&fs);

    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "");
    zs_db_close(&db);
}

static void test_repack_verifies_inputs(void)
{
    /* D-20b: repack verifies each input's records-region checksum before
     * copying a byte.  Without this, a record body corrupted in place is
     * LAUNDERED: the merge computes a fresh checksum over the corrupt copy,
     * D-23 removes the input, and check_consistency reports clean forever
     * after -- the only evidence F-26e could have caught is gone. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    char got[64];

    clear_db();

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_finish(&b);
    /* Damage a value byte, leaving every length and pointer intact -- the
     * corruption reads fine and only the records checksum knows. */
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    put_inorder_kv(2, 2, (const struct kv[]){ {"b","other"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Since F-32a the READ sees the damage too -- the record's own checksum
     * fails at materialization.  D-20b's merge gate remains load-bearing for
     * what reads never reach: a NOCSUM handle, and shadowed or unread bytes
     * that a merge copies anyway. */
    db_get(db, "a", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "-");

    ASSERT_EQ(zsi_repack_merge(db, db->snap, 0, 2), ZS_BADCHECKSUM);

    /* Both inputs are left in place, and nothing was written. */
    ASSERT_OK(zsi_db_refresh(db));
    ASSERT_NOT_NULL(file_with_range(db, 1, 1));
    ASSERT_NOT_NULL(file_with_range(db, 2, 2));
    ASSERT_NULL(file_with_range(db, 1, 2));
    zs_db_close(&db);
}

static void test_repack_verifies_inputs_nocsum(void)
{
    /* D-20b under ZS_NOCSUM: the flag is scoped to READS (F-5e).  A repack
     * writes a fresh checksum over whatever it copies, so it verifies its
     * inputs no matter how the handle was opened -- a NOCSUM handle that
     * repacked without checking would certify corrupt bytes as good. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];

    clear_db();

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_finish(&b);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    put_inorder_kv(2, 2, (const struct kv[]){ {"b","other"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {NULL,NULL} });

    db = open_db(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zsi_repack_merge(db, db->snap, 0, 2), ZS_BADCHECKSUM);

    ASSERT_OK(zsi_db_refresh(db));
    ASSERT_NOT_NULL(file_with_range(db, 1, 1));
    ASSERT_NOT_NULL(file_with_range(db, 2, 2));
    ASSERT_NULL(file_with_range(db, 1, 2));
    zs_db_close(&db);
}

static void test_seal_verifies_spans_nocsum(void)
{
    /* D-20b on the conversion path.  Since span verification rides indexing
     * in every mode (F-5e), no handle -- NOCSUM included -- ever ADMITS a
     * span whose checksum fails.  What IS reachable is time: in-place damage
     * AFTER the snapshot admitted the span changes neither the name set nor the
     * file size, so the C-4i probe cannot see it, and a commit-driven
     * conversion (D-25d, D-12) runs against the pre-damage snapshot.
     * D-20b's re-verify is what stands between that and an in-order file
     * that validates perfectly while D-23 removes the evidence.  Driven at
     * the unit level -- zsi_convert_one under the write lock, exactly the
     * commit path's call -- because zs_db_seal refreshes first and so
     * rejects the span before ever reaching the walk. */
    struct zs_db *db;
    struct sb s;
    char name[ZSI_NAME_MAX];
    char got[64];
    size_t voff = 0;

    clear_db();

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "value", 5);
    sb_term(&s, false);
    for (size_t i = ZSI_HEADER_LEN; i + 5 <= s.len; i++)
        if (memcmp(s.buf + i, "value", 5) == 0) { voff = i; break; }
    ASSERT(voff != 0);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    /* Admitted while intact: the replay verified this span. */
    db = open_db(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "value");

    /* Damage a value byte ON DISK, behind the admitted snapshot. */
    {
        int fd = open(dbpath(name), O_RDWR);
        ASSERT(fd >= 0);
        ASSERT_EQ(pwrite(fd, "V", 1, (off_t)voff), 1);
        close(fd);
    }

    /* The NOCSUM handle keeps reading the damaged bytes through its mapping
     * (F-5e's bargain at the record level)... */
    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "Value");

    /* ...but a conversion re-verifies the chain it is about to copy, and
     * must not certify it. */
    ASSERT_OK(zsi_lock_take(&db->locks, ZSI_LOCK_WRITE, 0));
    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(act);
        ASSERT_EQ(zsi_convert_one(db, act), ZS_BADCHECKSUM);
    }
    zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);

    /* The file is still unordered, still present, still readable. */
    ASSERT_NOT_NULL(file_with_range(db, 1, 0));
    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "Value");
    zs_db_close(&db);
}

static void test_read_verifies_record_csum(void)
{
    /* F-32a: a value byte flipped in place in an in-order file fails THAT key
     * at materialization; sibling keys still read; a ZS_NOCSUM handle still
     * reads the corrupt value (F-5e).
     *
     * The corruption lands BETWEEN ib_rec and ib_finish, so the trailer's
     * records-region checksum is computed over the corrupt bytes and
     * validates -- the record's own checksum is the ONLY witness.  Corrupting
     * after ib_finish would leave both stale and prove nothing about which
     * one fired. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    char got[64];
    const char *v; size_t vl;

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';    /* first value byte of "a" */
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0), ZS_BADCHECKSUM);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "other", 5);
    zs_db_close(&db);

    db = open_db(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    db_get(db, "a", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "Value");
    zs_db_close(&db);
}

static void test_read_verifies_record_csum_unordered(void)
{
    /* F-32a in an unordered file: corrupt ONE record inside a span BEFORE the
     * terminator is written, so the span checksum covers the corrupt bytes
     * and the span validates -- pure record-level corruption, invisible to
     * replay, caught only at materialization. */
    struct zs_db *db;
    struct sb s;
    char name[ZSI_NAME_MAX];
    const char *v; size_t vl;

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "value", 5);
    sb_rec(&s, "b", 1, "other", 5);
    s.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';    /* first value byte of "a" */
    sb_term(&s, false);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0), ZS_BADCHECKSUM);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "other", 5);
    zs_db_close(&db);
}

static void test_record_csum_replay_no_truncate(void)
{
    /* F-32b, the G-3 half: a record checksum failure must NOT make replay
     * complete the file early.  "b" was stored AFTER the corrupt "a" in the
     * SAME span; a replay that verified record checksums would discard it
     * (F-24).  A scan must also survive: the corrupt record fails its own
     * yield and only its own yield. */
    struct zs_db *db;
    struct sb s;
    char name[ZSI_NAME_MAX];
    const char *v; size_t vl;

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "value", 5);
    sb_rec(&s, "b", 1, "other", 5);
    s.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    sb_term(&s, false);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "other", 5);

    /* A cursor walk hits the corrupt record first and reports it, rather
     * than skipping it silently or truncating the traversal semantics. */
    {
        struct zs_cursor *c = NULL;
        struct zsi_rec r;
        ASSERT_OK(zsi_cursor_open(db, NULL, db->snap, NULL, 0, 0, &c));
        ASSERT_EQ(zsi_cursor_next(c, &r), ZS_BADCHECKSUM);
        zsi_cursor_free(c);
    }
    zs_db_close(&db);
}

static void test_record_csum_engine0(void)
{
    /* Engine 0: the checksum field is written as zero and the engine computes
     * zero for every input, so verification passes with no special case --
     * F-5c's bargain, unchanged by F-32. */
    struct zs_db *db;
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    const char *v; size_t vl;

    clear_db();
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);
    zs_db_close(&db);
}

static void test_repack_cascade(void)
{
    /* D-16's cascade reaching the geometric size relation after many rollovers,
     * driven through the real writer and the real repacker. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char val[200];
    memset(val, 'v', sizeof(val));

    for (int i = 0; i < 60; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), val, sizeof(val), 0));
        if (zs_db_should_repack(db)) ASSERT_OK(zs_db_repack(db));
    }

    /* Every record survives the cascade. */
    for (int i = 0; i < 60; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "k%03d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s lost in cascade\n", k);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
    }

    /* The cascade produced a geometric progression rather than a flat pile: the
     * older files are strictly larger, each roughly the sum of those above it. */
    ASSERT(!zs_db_should_repack(db));
    size_t nio = 0;
    while (nio < db->snap->nfiles && !zsi_file_is_unordered(db->snap->files[nio]))
        nio++;
    ASSERT(nio >= 2);
    for (size_t i = 1; i < nio; i++)
        ASSERT(db->snap->files[i - 1]->size > db->snap->files[i]->size);

    struct zsi_fileset fs;
    ASSERT_OK(zsi_fileset_scan(dbdir, &db->uuid, &fs));
    ASSERT_OK(zsi_fileset_resolve(&fs));
    zsi_fileset_fini(&fs);

    /* And the file count is well below the number of rollovers, which is the point
     * of the policy (D-16). */
    ASSERT(db->snap->nfiles < 20);

    zs_db_close(&db);
}

static void test_repack_never_touches_unordered(void)
{
    /* D-15: the repacker never touches the active file, and never touches an
     * unordered file at all -- selection considers only the in-order prefix
     * (D-16).
     *
     * One unordered file, since D-1b gives the active file a single name and a
     * backlog is not a state the format can represent (D-12a).  What is
     * asserted is that selection stops at the in-order prefix. */
    struct zs_db *db;

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_unordered_kv(2, (const struct kv[]){ {"c","3"}, {NULL,NULL} });

    db = open_db(ZS_SHARED);
    ASSERT_NOT_NULL(db);

    size_t first, count;
    count = zsi_repack_select(db->snap, db->repack_max_size, &first);
    ASSERT(count < 2);              /* only one in-order file: nothing to merge */
    ASSERT(!zs_db_should_repack(db));

    /* And a read-only handle refuses to repack (R-3). */
    ASSERT_EQ(zs_db_repack(db), ZS_READONLY);
    zs_db_close(&db);
}

/*
 * ============================================================
 * Consistency checking (T-6 negatives)
 * ============================================================
 */

static void test_check_clean_database(void)
{
    /* A normally produced database of every arrangement passes, with no reports. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    setup.error = counting_error;
    report_count = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char val[100];
    memset(val, 'v', sizeof(val));
    for (int i = 0; i < 20; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), val, sizeof(val), 0));
    }
    ASSERT_OK(zs_db_delete(db, "k005", 4, 0));
    if (zs_db_should_repack(db)) ASSERT_OK(zs_db_repack(db));

    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_EQ(report_count, 0);

    /* This writer's abort touches no disk (C-8: transactions are buffered), so
     * there is no state here to re-check.  The checker accepting a real
     * ROLLBACK span is test_dump_shows_rollback's hand-built file. */

    zs_db_close(&db);
}

static void test_check_out_of_order_pointers(void)
{
    /* F-28: an in-order file's pointer array must be strictly increasing by key.
     * A misordered array is invisible to a binary search -- it just returns wrong
     * answers -- so this check is the only thing that finds it. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];

    clear_db();
    /* ib_rec appends in the order given, so this builds a deliberately misordered
     * file -- which put_inorder_kv would have sorted. */
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "z", 1, "1", 1);
    ib_rec(&b, "a", 1, "2", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADFORMAT);
    ASSERT(report_count > 0);
    zs_db_close(&db);

    /* A DUPLICATE key, which is what a repack emitting a key twice produces
     * (D-17).  Sorted, so only the equality test can catch it. */
    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "dup", 3, "1", 1);
    ib_rec(&b, "dup", 3, "2", 1);
    ib_finish(&b);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADFORMAT);
    ASSERT(report_count > 0);
    zs_db_close(&db);
}

static void test_check_records_checksum(void)
{
    /* F-26e/F-26f: a record body corrupted in place is detected here and nowhere
     * else, since an in-order file has no span terminators to notice. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    ib_finish(&b);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';        /* flip a value byte */
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);

    /* Opening succeeded -- the check is on demand, so open stays O(1) (F-26f). */
    ASSERT_EQ(report_count, 0);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADCHECKSUM);
    ASSERT(report_count > 0);

    /* And ZS_NOCSUM skips it, which is what that flag means (F-5e). */
    zs_db_close(&db);
    db = open_db_reporting(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}

static void test_check_noncanonical(void)
{
    /* T-6's negative: a hand-built file whose records use the wrong ENCODING
     * for their contents is REPORTED, while still reading correctly -- because
     * rejecting it would discard committed data (F-24 plus G-3).
     *
     * The space of non-canonical records is smaller than it was.  It used to
     * include a record storing an ancestor equal to its own file's start, which
     * decoded identically either way and so could only ever be caught by a
     * canonicality check.  With F-18 a record's bytes are a function of its own
     * key and value, so the only remaining divergence is using a wider form
     * than the lengths require -- checked here for both data shapes, since a
     * deletion and a key/value pair pick their form by different rules. */
    struct zs_db *db;
    char name[ZSI_NAME_MAX];
    const char *v;
    size_t vl;

    /* A BIGDELETION whose keylen fits the short form. */
    clear_db();
    {
        char buf[512];
        size_t reclen;
        memset(buf, 0, sizeof(buf));
        make_header(buf, 5, 5, ZSI_CSUM_XXHASH);
        buf[ZSI_HEADER_LEN] = (char)ZSI_BIGDELETION;
        zsi_put64(buf + ZSI_HEADER_LEN + 8, 1);          /* keylen 1: fits */
        buf[ZSI_HEADER_LEN + 16] = 'k';
        buf[ZSI_HEADER_LEN + 17] = '\0';
        reclen = 24;                        /* roundup8(16 + 1 + 1 + 4) */
        zsi_put32(buf + ZSI_HEADER_LEN + reclen - 4,
                  zsi_csum_xxhash(buf + ZSI_HEADER_LEN, reclen - 4));

        uint64_t ptr = ZSI_HEADER_LEN;
        char *sec = NULL;
        size_t seclen = 0;
        uint32_t rc = zsi_csum_xxhash(buf + ZSI_HEADER_LEN, reclen);
        ASSERT_OK(zsi_ptrs_build(&ptr, 1, ZSI_HEADER_LEN + reclen, rc,
                                 zsi_csum_xxhash, &sec, &seclen));
        memcpy(buf + ZSI_HEADER_LEN + reclen, sec, seclen);
        free(sec);

        zsi_name_format(name, test_uuid, 5, 5);
        ASSERT_EQ(mkdbdir(), 0);
        ASSERT_EQ(writefile(name, buf, ZSI_HEADER_LEN + reclen + seclen), 0);
    }
    put_unordered_kv(6, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);

    /* It reads as a deletion: the record is not discarded. */
    ASSERT_EQ(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    /* And the divergence is reported. */
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADFORMAT);
    ASSERT(report_count > 0);
    zs_db_close(&db);

    /* A big form whose lengths would have fitted the short one: same treatment. */
    clear_db();
    {
        char buf[512];
        size_t reclen;
        memset(buf, 0, sizeof(buf));
        make_header(buf, 1, 1, ZSI_CSUM_XXHASH);
        /* BIGKEYVALUE with keylen 1, vallen 1 -- both fit the short form */
        buf[ZSI_HEADER_LEN] = (char)ZSI_BIGKEYVALUE;
        zsi_put64(buf + ZSI_HEADER_LEN + 8, 1);
        zsi_put64(buf + ZSI_HEADER_LEN + 16, 1);
        buf[ZSI_HEADER_LEN + 24] = 'k';
        buf[ZSI_HEADER_LEN + 25] = '\0';
        buf[ZSI_HEADER_LEN + 26] = 'v';
        buf[ZSI_HEADER_LEN + 27] = '\0';
        reclen = 32;                        /* roundup8(24 + 1+1+1+1 + 4) */
        /* F-32: the trailing checksum, honest even though the FORM is
         * non-canonical -- the divergence under test is the shape, and a
         * stale checksum would make the fetch below fail for the wrong
         * reason. */
        zsi_put32(buf + ZSI_HEADER_LEN + reclen - 4,
                  zsi_csum_xxhash(buf + ZSI_HEADER_LEN, reclen - 4));

        uint64_t ptr = ZSI_HEADER_LEN;
        char *sec = NULL;
        size_t seclen = 0;
        uint32_t rc = zsi_csum_xxhash(buf + ZSI_HEADER_LEN, reclen);
        ASSERT_OK(zsi_ptrs_build(&ptr, 1, ZSI_HEADER_LEN + reclen, rc,
                                 zsi_csum_xxhash, &sec, &seclen));
        memcpy(buf + ZSI_HEADER_LEN + reclen, sec, seclen);
        free(sec);

        zsi_name_format(name, test_uuid, 1, 1);
        ASSERT_EQ(mkdbdir(), 0);
        ASSERT_EQ(writefile(name, buf, ZSI_HEADER_LEN + reclen + seclen), 0);
    }
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));   /* reads */
    ASSERT_MEM_EQ(v, "v", 1);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADFORMAT);          /* reported */
    ASSERT(report_count > 0);
    zs_db_close(&db);
}

static void test_check_unclean_reported(void)
{
    /* Content after the last valid span is reported but is NOT an error: F-24
     * makes it an ordinary state and D-9 has the writer move on rather than
     * repair. */
    struct sb s;
    struct zs_db *db;
    char name[ZSI_NAME_MAX];

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1);
    sb_term(&s, false);
    sb_raw(&s, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_check_consistency(db));     /* reported, not an error */
    ASSERT(report_count > 0);
    zs_db_close(&db);
}

static void test_dump_line_format(void)
{
    /* The dump's line format is a compatibility surface: T-0a's `dump` subcommand
     * emits it and the interop runner compares it as text.  Asserted by capturing
     * stdout, so a change to the format fails here rather than in another
     * implementation's test suite. */
    struct zs_db *db;
    char buf[8192];

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {"b","2"}, {NULL,NULL} });
    put_unordered_kv(2, (const struct kv[]){ {"c","3"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Capture stdout. */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/dump.out", basedir);
    int saved = dup(1);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(fd >= 0);
    ASSERT(dup2(fd, 1) >= 0);
    close(fd);

    ASSERT_OK(zs_db_dump(db, 1));
    fflush(stdout);
    dup2(saved, 1);
    close(saved);

    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    ASSERT(n > 0);
    buf[n] = '\0';

    /* Both files, with the documented fields. */
    ASSERT(strstr(buf, "kind=inorder") != NULL);
    ASSERT(strstr(buf, "kind=unordered") != NULL);
    ASSERT(strstr(buf, "start=1 end=1") != NULL);
    ASSERT(strstr(buf, "start=2 end=0") != NULL);
    ASSERT(strstr(buf, "PTRS ") != NULL);
    ASSERT(strstr(buf, "width=32") != NULL);
    ASSERT(strstr(buf, "count=2") != NULL);
    ASSERT(strstr(buf, "SPAN ") != NULL);
    ASSERT(strstr(buf, "term=COMMIT") != NULL);
    ASSERT(strstr(buf, "REC  ") != NULL);
    /* keys are hex, so binary keys survive the format */
    ASSERT(strstr(buf, "key=61") != NULL);      /* 'a' */
    ASSERT(strstr(buf, "key=63") != NULL);      /* 'c' */

    zs_db_close(&db);
}

static void test_dump_shows_rollback(void)
{
    /* A dump must show a ROLLBACK span rather than skipping it: the point of a dump
     * is to show what is there, INCLUDING what a reader ignores.
     *
     * This writer never produces one -- it buffers until commit, so an abort has
     * nothing to void (see zsi_txn_abort) -- so the file is hand-built to the shape
     * a STREAMING peer would produce.  That makes this a test of the dump's
     * handling of a foreign file, which is the case that actually matters. */
    struct sb s;
    struct zs_db *db;
    char buf[8192];
    char path[PATH_MAX];
    char name[ZSI_NAME_MAX];

    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "live", 4, "1", 1);
    sb_term(&s, false);
    sb_rec(&s, "dead", 4, "2", 1);
    sb_term(&s, true);                          /* a rolled-back span */
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* The reader ignores the rolled-back record (F-25)... */
    const char *v;
    size_t vl;
    ASSERT_EQ(zs_db_fetch(db, "dead", 4, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_OK(zs_db_fetch(db, "live", 4, NULL, NULL, &v, &vl, 0));

    /* ...and a ROLLBACK span is legal, not an inconsistency (F-25): this is the
     * one test with a real one on disk, so this is where that is asserted. */
    ASSERT_OK(zs_db_check_consistency(db));

    snprintf(path, sizeof(path), "%s/dump2.out", basedir);
    int saved = dup(1);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ASSERT(fd >= 0);
    ASSERT(dup2(fd, 1) >= 0);
    close(fd);
    ASSERT_OK(zs_db_dump(db, 1));
    fflush(stdout);
    dup2(saved, 1);
    close(saved);

    fd = open(path, O_RDONLY);
    ASSERT(fd >= 0);
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    ASSERT(n > 0);
    buf[n] = '\0';

    /* ...but the dump shows both spans and both records. */
    ASSERT(strstr(buf, "term=COMMIT") != NULL);
    ASSERT(strstr(buf, "term=ROLLBACK") != NULL);
    ASSERT(strstr(buf, "key=6c697665") != NULL);        /* "live" */
    ASSERT(strstr(buf, "key=64656164") != NULL);        /* "dead" */

    zs_db_close(&db);
}

/*
 * ============================================================
 * The golden corpus (T-0, T-1, and T-12a's local half)
 * ============================================================
 *
 * Two directions, and the second is the sharper one.
 *
 * DECODE: open each checked-in case and check its expectations.  Catches a reader
 * that disagrees with the format.  Opened ZS_SHARED, so validating the corpus
 * cannot modify it -- a decode test that converted or repacked would rewrite the
 * contract it is checking.
 *
 * ENCODE: replay the recorded operations into an empty directory and compare the
 * result BYTE FOR BYTE.  This catches divergence in padding, in ancestor omission,
 * in the choice of short versus big form, and in checksum seeding -- before that
 * divergence becomes a compatibility rule nobody meant to make.  It is only
 * possible because encoding is canonical (F-15, F-26c) and nothing time-varying
 * enters the format.
 */

#define CORPUS_DIR "tests/corpus"

static char *slurp(const char *path, size_t *lenp)
{
    struct stat sb;
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    if (fstat(fd, &sb) < 0) { close(fd); return NULL; }

    char *buf = malloc((size_t)sb.st_size + 1);
    if (!buf) { close(fd); return NULL; }
    ssize_t n = read(fd, buf, (size_t)sb.st_size);
    close(fd);
    if (n != sb.st_size) { free(buf); return NULL; }
    buf[n] = '\0';
    if (lenp) *lenp = (size_t)n;
    return buf;
}

static size_t unhex_into(const char *in, char *out, size_t outmax)
{
    size_t n = strlen(in);
    if (n % 2 || n / 2 > outmax) return (size_t)-1;
    for (size_t i = 0; i < n / 2; i++) {
        int v = 0;
        for (int j = 0; j < 2; j++) {
            char c = in[i * 2 + j];
            int d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return (size_t)-1;
            v = (v << 4) | d;
        }
        out[i] = (char)v;
    }
    return n / 2;
}

/* Collect a scan as "<keyhex> <valhex>\n" lines, which is the corpus format.
 *
 * Bounds-checked, and the buffer is heap-allocated by the caller: the
 * encoding-boundaries case holds 65536-byte values, which is 131072 hex
 * characters, and an earlier fixed 4KB stack buffer overflowed here.  A test
 * harness that can corrupt its own stack reports the wrong thing, so the check
 * matters as much as the size. */
struct hexbuf {
    char  *buf;
    size_t len, cap;
    bool   overflow;
};

static int corpus_scan_cb(void *rock, const char *key, size_t keylen,
                          const char *val, size_t vallen)
{
    struct hexbuf *h = rock;
    size_t need = (keylen + vallen) * 2 + 3;

    if (h->len + need >= h->cap) { h->overflow = true; return 0; }

    for (size_t i = 0; i < keylen; i++)
        h->len += (size_t)sprintf(h->buf + h->len, "%02x", (unsigned char)key[i]);
    h->buf[h->len++] = ' ';
    for (size_t i = 0; i < vallen; i++)
        h->len += (size_t)sprintf(h->buf + h->len, "%02x", (unsigned char)val[i]);
    h->buf[h->len++] = '\n';
    h->buf[h->len] = '\0';
    return 0;
}

/* List a directory's zeroskip-* names, sorted, newline separated. */
static void list_data_files(const char *dir, char *out, size_t outlen)
{
    char names[64][ZSI_NAME_MAX];
    size_t n = 0;
    DIR *d = opendir(dir);
    struct dirent *de;

    out[0] = '\0';
    if (!d) return;
    while ((de = readdir(d)) && n < 64)
        if (!strncmp(de->d_name, "zeroskip-", 9))
            XSNPRINTFN(names[n++], ZSI_NAME_MAX, "%s", de->d_name);
    closedir(d);

    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[ZSI_NAME_MAX];
                memcpy(t, names[i], ZSI_NAME_MAX);
                memcpy(names[i], names[j], ZSI_NAME_MAX);
                memcpy(names[j], t, ZSI_NAME_MAX);
            }

    size_t used = 0;
    for (size_t i = 0; i < n; i++) {
        size_t need = strlen(names[i]) + 2;
        if (used + need >= outlen) break;
        used += (size_t)snprintf(out + used, outlen - used, "%s\n", names[i]);
    }
}

/* Extract a section from case.txt: everything after "expect <what>" up to the next
 * blank-line-then-directive or end. */
static void corpus_section(const char *txt, const char *what, char *out,
                           size_t outlen)
{
    char needle[64];
    snprintf(needle, sizeof(needle), "expect %s\n", what);
    out[0] = '\0';

    const char *p = strstr(txt, needle);
    if (!p) return;
    p += strlen(needle);

    size_t used = 0;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len == 0) break;                        /* blank line ends it */
        if (!strncmp(p, "expect ", 7)) break;
        if (!strncmp(p, "op ", 3)) break;
        if (used + len + 2 >= outlen) break;
        memcpy(out + used, p, len);
        used += len;
        out[used++] = '\n';
        out[used] = '\0';
        if (!eol) break;
        p = eol + 1;
    }
}

/* Every case directory name, sorted. */
static size_t corpus_cases(char names[][64], size_t max)
{
    DIR *d = opendir(CORPUS_DIR);
    struct dirent *de;
    size_t n = 0;

    if (!d) return 0;
    while ((de = readdir(d)) && n < max) {
        if (de->d_name[0] == '.') continue;
        if (!strcmp(de->d_name, "README.md")) continue;
        char path[PATH_MAX];
        XSNPRINTF(path, CORPUS_DIR "/%s/case.txt", de->d_name);
        if (fexists(path) != 0) continue;
        XSNPRINTFN(names[n++], 64, "%s", de->d_name);
    }
    closedir(d);

    for (size_t i = 0; i < n; i++)
        for (size_t j = i + 1; j < n; j++)
            if (strcmp(names[i], names[j]) > 0) {
                char t[64];
                memcpy(t, names[i], 64);
                memcpy(names[i], names[j], 64);
                memcpy(names[j], t, 64);
            }
    return n;
}

static void test_corpus_decode(void)
{
    char cases[32][64];
    size_t ncases = corpus_cases(cases, 32);

    if (!ncases) SKIP("no corpus (run make corpus)");

    for (size_t i = 0; i < ncases; i++) {
        char dir[PATH_MAX], txtpath[PATH_MAX];
        XSNPRINTF(dir, CORPUS_DIR "/%s", cases[i]);
        XSNPRINTF(txtpath, "%s/case.txt", dir);

        char *txt = slurp(txtpath, NULL);
        if (!txt) {
            fprintf(stderr, "\n    FAIL %s: no case.txt\n", cases[i]);
            current_test_failed = 1;
            return;
        }

        /* ZS_SHARED so validating cannot mutate the corpus. */
        struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *db = NULL;
        setup.flags = ZS_SHARED;
        int r = zs_db_open(dir, &setup, &db);
        if (r != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s: open returned %d (%s)\n",
                    cases[i], r, zs_strerror(r));
            current_test_failed = 1;
            free(txt);
            return;
        }

        /* Heap-allocated and generous: a case may hold 64KB values, which is
         * 128KB of hex per record. */
        size_t cap = 1u << 20;
        char *want = malloc(cap), *gotbuf = malloc(cap);
        ASSERT_NOT_NULL(want);
        ASSERT_NOT_NULL(gotbuf);

        /* expect files */
        corpus_section(txt, "files", want, cap);
        list_data_files(dir, gotbuf, cap);
        if (want[0] && strcmp(want, gotbuf) != 0) {
            fprintf(stderr, "\n    FAIL %s files\n      want %s      got  %s",
                    cases[i], want, gotbuf);
            current_test_failed = 1;
            goto casefail;
        }

        /* expect scan */
        corpus_section(txt, "scan", want, cap);
        struct hexbuf hb = { gotbuf, 0, cap, false };
        gotbuf[0] = '\0';
        r = zs_db_foreach(db, NULL, 0, NULL, corpus_scan_cb, &hb, 0);
        if (r != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s scan returned %d\n", cases[i], r);
            current_test_failed = 1;
            goto casefail;
        }
        if (hb.overflow) {
            fprintf(stderr, "\n    FAIL %s scan overflowed the buffer\n",
                    cases[i]);
            current_test_failed = 1;
            goto casefail;
        }
        if (strcmp(want, gotbuf) != 0) {
            fprintf(stderr, "\n    FAIL %s scan mismatch (%zu vs %zu bytes)\n",
                    cases[i], strlen(want), strlen(gotbuf));
            current_test_failed = 1;
            goto casefail;
        }

        /* expect check */
        const char *ck = strstr(txt, "expect check ");
        if (ck) {
            bool want_ok = !strncmp(ck + 13, "OK", 2);
            int cr = zs_db_check_consistency(db);
            if (want_ok != (cr == ZS_OK)) {
                fprintf(stderr, "\n    FAIL %s check: wanted %s, got %d\n",
                        cases[i], want_ok ? "OK" : "FAILED", cr);
                current_test_failed = 1;
                goto casefail;
            }
        }

        free(want);
        free(gotbuf);
        zs_db_close(&db);
        free(txt);
        continue;

casefail:
        free(want);
        free(gotbuf);
        zs_db_close(&db);
        free(txt);
        return;
    }
}

/* Replay a case's `op` lines into dir.  Returns false if the case uses something
 * this replayer does not implement, which is reported rather than skipped
 * silently. */
static bool corpus_replay(const char *txt, const char *dir, const char *uuid,
                          int engine)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *batch = NULL;
    char kbuf[70000], vbuf[70000];

    setup.flags = ZS_CREATE | (engine == 0 ? ZS_CSUM_NONE : ZS_CSUM_XXHASH);
    if (zs_db_open_with_uuid(dir, &setup, uuid, &db) != ZS_OK) return false;

    const char *p = txt;
    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[140000];
        if (len >= sizeof(line)) return false;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : NULL;

        char *arg = NULL;
        bool in_batch = (batch != NULL);
        char *verb = line;

        if (!strncmp(line, "op ", 3)) verb = line + 3;
        else if (!in_batch) continue;       /* a header line or an expectation */

        arg = strchr(verb, ' ');
        if (arg) *arg++ = '\0';

        if (!strcmp(verb, "batch")) {
            if (zs_db_begin_txn(db, 0, &batch) != ZS_OK) return false;
        } else if (!strcmp(verb, "end")) {
            if (zs_txn_commit(&batch) != ZS_OK) return false;
            batch = NULL;
        } else if (!strcmp(verb, "abort")) {
            /* T-0a: end the batch WITHOUT committing.  The streamed records
             * become a ROLLBACK span, which is the point of the case. */
            if (!batch || zs_txn_abort(&batch) != ZS_OK) return false;
            batch = NULL;
        } else if (!strcmp(verb, "store")) {
            char *sp = arg ? strchr(arg, ' ') : NULL;
            size_t kl, vl = 0;
            if (sp) *sp++ = '\0';
            kl = unhex_into(arg ? arg : "", kbuf, sizeof(kbuf));
            if (kl == (size_t)-1) return false;
            if (sp) {
                vl = unhex_into(sp, vbuf, sizeof(vbuf));
                if (vl == (size_t)-1) return false;
            }
            int r = batch ? zs_txn_store(batch, kbuf, kl, vbuf, vl, 0)
                          : zs_db_store(db, kbuf, kl, vbuf, vl, 0);
            if (r != ZS_OK) return false;
        } else if (!strcmp(verb, "delete")) {
            size_t kl = unhex_into(arg ? arg : "", kbuf, sizeof(kbuf));
            if (kl == (size_t)-1) return false;
            int r = batch ? zs_txn_store(batch, kbuf, kl, NULL, 0, 0)
                          : zs_db_delete(db, kbuf, kl, 0);
            if (r != ZS_OK && r != ZS_NOTFOUND) return false;
        } else if (!strcmp(verb, "garbage")) {
            /* Append raw bytes to the newest data file, as a torn write leaves. */
            size_t n = unhex_into(arg ? arg : "", vbuf, sizeof(vbuf));
            if (n == (size_t)-1) return false;
            char names[64][ZSI_NAME_MAX];
            size_t nn = 0;
            DIR *d = opendir(dir);
            struct dirent *de;
            if (!d) return false;
            while ((de = readdir(d)) && nn < 64)
                if (!strncmp(de->d_name, "zeroskip-", 9))
                    XSNPRINTFN(names[nn++], ZSI_NAME_MAX, "%s", de->d_name);
            closedir(d);
            if (!nn) return false;
            for (size_t i = 0; i < nn; i++)
                for (size_t j = i + 1; j < nn; j++)
                    if (strcmp(names[i], names[j]) > 0) {
                        char t[ZSI_NAME_MAX];
                        memcpy(t, names[i], ZSI_NAME_MAX);
                        memcpy(names[i], names[j], ZSI_NAME_MAX);
                        memcpy(names[j], t, ZSI_NAME_MAX);
                    }
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", dir, names[nn - 1]);
            /* Close and reopen around the raw append, so the handle's snapshot is
             * rebuilt from what is actually on disk. */
            zs_db_close(&db);
            int fd = open(path, O_WRONLY | O_APPEND);
            if (fd < 0) return false;
            if (write(fd, vbuf, n) != (ssize_t)n) { close(fd); return false; }
            close(fd);
            /* ZS_NOAUTOREPACK: repack selection is not normative (D-16), so a
             * replay that let the cascade fire would compare golden bytes
             * against this implementation's policy rather than against the
             * format.  Cases drive merges explicitly with `op repack`. */
            setup.flags = (engine == 0 ? ZS_CSUM_NONE : ZS_CSUM_XXHASH)
                        | ZS_NOAUTOREPACK;
            if (zs_db_open(dir, &setup, &db) != ZS_OK) return false;
        } else if (!strcmp(verb, "convert")) {
            struct zs_txn *t = NULL;
            if (zs_db_begin_txn(db, 0, &t) != ZS_OK) return false;
            if (zs_txn_commit(&t) != ZS_OK) return false;
        } else if (!strcmp(verb, "repack")) {
            if (zs_db_repack(db) != ZS_OK) return false;
        } else if (!strcmp(verb, "compact")) {
            if (zs_db_compact(db) != ZS_OK) return false;
        } else {
            return false;                   /* an op we do not implement */
        }
    }

    if (batch) return false;                /* unterminated */
    zs_db_close(&db);
    return true;
}

static void test_corpus_encode_byte_identical(void)
{
    /* T-12a's local half: the same UUID and the same operations must produce
     * IDENTICAL FILES.  Sharper than reading each other's output, because it
     * catches divergence before it becomes a compatibility rule. */
    char cases[32][64];
    size_t ncases = corpus_cases(cases, 32);

    if (!ncases) SKIP("no corpus (run make corpus)");

    for (size_t i = 0; i < ncases; i++) {
        char dir[PATH_MAX], txtpath[PATH_MAX], out[PATH_MAX];
        XSNPRINTF(dir, CORPUS_DIR "/%s", cases[i]);
        XSNPRINTF(txtpath, "%s/case.txt", dir);
        XSNPRINTF(out, "%s/replay-%s", basedir, cases[i]);

        char *txt = slurp(txtpath, NULL);
        ASSERT_NOT_NULL(txt);

        /* uuid and engine from the header. */
        char uuid[64] = "";
        int engine = 1;
        const char *u = strstr(txt, "uuid ");
        if (u) sscanf(u, "uuid %63s", uuid);
        const char *e = strstr(txt, "engine ");
        if (e) sscanf(e, "engine %d", &engine);

        if (!uuid[0]) {
            fprintf(stderr, "\n    FAIL %s: no uuid in case.txt\n", cases[i]);
            current_test_failed = 1;
            free(txt);
            return;
        }

        if (!corpus_replay(txt, out, uuid, engine)) {
            fprintf(stderr, "\n    FAIL %s: replay failed\n", cases[i]);
            current_test_failed = 1;
            free(txt);
            return;
        }
        free(txt);

        /* Compare every data file byte for byte. */
        char names[4096];
        list_data_files(dir, names, sizeof(names));
        char *save = NULL;
        for (char *nm = strtok_r(names, "\n", &save); nm;
             nm = strtok_r(NULL, "\n", &save)) {
            char a[PATH_MAX], b[PATH_MAX];
            size_t alen = 0, blen = 0;
            XSNPRINTF(a, "%s/%s", dir, nm);
            XSNPRINTF(b, "%s/%s", out, nm);

            char *abuf = slurp(a, &alen);
            char *bbuf = slurp(b, &blen);
            if (!abuf || !bbuf) {
                fprintf(stderr, "\n    FAIL %s: %s missing in replay\n",
                        cases[i], nm);
                current_test_failed = 1;
                free(abuf); free(bbuf);
                return;
            }
            if (alen != blen || memcmp(abuf, bbuf, alen) != 0) {
                fprintf(stderr, "\n    FAIL %s: %s differs (%zu vs %zu bytes)\n",
                        cases[i], nm, alen, blen);
                for (size_t k = 0; k < (alen < blen ? alen : blen); k++)
                    if (abuf[k] != bbuf[k]) {
                        fprintf(stderr, "      first difference at byte %zu: "
                                "corpus 0x%02X, replay 0x%02X\n", k,
                                (unsigned char)abuf[k], (unsigned char)bbuf[k]);
                        break;
                    }
                current_test_failed = 1;
                free(abuf); free(bbuf);
                return;
            }
            free(abuf);
            free(bbuf);
        }
    }
}

static void test_corpus_engine_from_file_not_config(void)
{
    /* F-5a/A-6: a file's engine comes from its OWN header, not the reader's
     * configuration.  The engine0 case is opened by a handle configured for
     * xxHash and must still read correctly. */
    char dir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    snprintf(dir, sizeof(dir), CORPUS_DIR "/engine0");
    if (fexists(dir) != 0) SKIP("no engine0 corpus case");

    setup.flags = ZS_SHARED | ZS_CSUM_XXHASH;
    ASSERT_OK(zs_db_open(dir, &setup, &db));

    /* The file records engine 0 regardless of the flag. */
    ASSERT_EQ(db->create_csum_id, ZSI_CSUM_XXHASH);
    ASSERT_EQ(db->snap->files[0]->csum_id, ZSI_CSUM_NONE);

    /* And its records read. */
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "01", 2);

    zs_db_close(&db);
}

/*
 * ============================================================
 * Malformed input (T-3)
 * ============================================================
 *
 * Every golden corpus file, truncated at EVERY byte offset and systematically
 * bit-flipped.  Each case asserts an error OR the committed prefix, and never a
 * crash, a hang, or an out-of-bounds read.
 *
 * "Or the committed prefix" is not a weakening: F-24 makes a truncated unordered
 * file legitimately complete at its last valid span, so both outcomes are correct
 * and the harness must accept either -- while still checking that what comes back
 * IS a prefix of what the intact file held, rather than merely non-empty.
 *
 * The per-case wall-clock alarm is THE DETECTOR FOR F-29: a progress-rule bug
 * shows up as non-termination, not as a wrong answer, so without a timeout this
 * suite would hang rather than fail.
 *
 * Run under ASan and UBSan by `make asan`, which is where an out-of-bounds read
 * becomes an observable failure rather than a value nobody notices.
 */

/* Every scanned key of a database, joined -- or NULL if it will not open. */
static bool damaged_scan(const char *dir, char *out, size_t outlen, bool *opened)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct hexbuf hb = { out, 0, outlen, false };

    out[0] = '\0';
    *opened = false;

    /* ZS_SHARED: a damaged database must be inspectable without being written to
     * (R-3), and a harness that let the library repair the file under it would be
     * testing something else on the next iteration. */
    setup.flags = ZS_SHARED;
    setup.error = counting_error;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) return true;   /* an error is fine */

    *opened = true;
    int r = zs_db_foreach(db, NULL, 0, NULL, corpus_scan_cb, &hb, 0);
    zs_db_close(&db);

    /* A scan may fail; that is an error, which is an acceptable outcome. */
    return (r == ZS_OK || r == ZS_DONE || r < 0);
}

/* The invariant a damaged database must still satisfy.
 *
 * NOT "a prefix of the intact scan": truncation removes later TRANSACTIONS, so the
 * surviving state can contain keys the final state does not -- the `deletion` case
 * truncated after its first span yields the key a later span deleted.  That is
 * correct behaviour, and asserting a scan prefix rejected it.
 *
 * What must hold is that the surviving state is INTERNALLY CONSISTENT: every key
 * the scan reports resolves by point lookup to the same value, and every key it
 * does not report resolves to absent.  That is G-7 -- the guarantee that the two
 * read paths cannot disagree -- and it is the property most likely to break when a
 * file is damaged part-way through a record or a pointer array. */
static bool damaged_is_consistent(const char *dir, const char *scan)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    bool ok = true;

    setup.flags = ZS_SHARED;
    setup.error = counting_error;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) return true;

    /* Every line is "<keyhex> <valhex>"; check each by lookup. */
    const char *p = scan;
    while (*p && ok) {
        const char *eol = strchr(p, '\n');
        if (!eol) break;

        char line[300000];
        size_t len = (size_t)(eol - p);
        if (len >= sizeof(line)) break;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol + 1;

        char *sp = strchr(line, ' ');
        if (!sp) { ok = false; break; }
        *sp++ = '\0';

        static char kbuf[70000], vbuf[70000];
        size_t kl = unhex_into(line, kbuf, sizeof(kbuf));
        size_t vl = unhex_into(sp, vbuf, sizeof(vbuf));
        if (kl == (size_t)-1 || vl == (size_t)-1) { ok = false; break; }

        const char *gv;
        size_t gvl;
        int r = zs_db_fetch(db, kbuf, kl, NULL, NULL, &gv, &gvl, 0);
        if (r != ZS_OK || gvl != vl || (vl && memcmp(gv, vbuf, vl) != 0))
            ok = false;
    }

    zs_db_close(&db);
    return ok;
}

struct damage_target {
    char   dir[PATH_MAX];       /* a scratch copy we may damage */
    char   name[ZSI_NAME_MAX];  /* the data file within it */
    char  *orig;                /* its intact bytes */
    size_t len;
    char  *intact_scan;         /* what the intact database scans to */
};

static bool damage_setup(struct damage_target *t, const char *casedir,
                         const char *name)
{
    char src[PATH_MAX], dst[PATH_MAX];
    bool opened;

    snprintf(t->dir, sizeof(t->dir), "%s/damage", basedir);
    char cmd[PATH_MAX * 2];
    XSNPRINTF(cmd, "rm -rf '%s' && mkdir -p '%s'", t->dir, t->dir);
    if (system(cmd)) return false;

    snprintf(t->name, sizeof(t->name), "%s", name);
    snprintf(src, sizeof(src), "%s/%s", casedir, name);
    XSNPRINTF(dst, "%s/%s", t->dir, name);

    t->orig = slurp(src, &t->len);
    if (!t->orig) return false;

    /* Copy every data file, so a multi-file case keeps its tiling. */
    snprintf(cmd, sizeof(cmd), "cp '%s'/zeroskip-* '%s'/", casedir, t->dir);
    if (system(cmd)) { free(t->orig); return false; }

    t->intact_scan = malloc(1u << 20);
    if (!t->intact_scan) { free(t->orig); return false; }
    if (!damaged_scan(t->dir, t->intact_scan, 1u << 20, &opened) || !opened) {
        free(t->orig);
        free(t->intact_scan);
        return false;
    }

    (void)dst;
    return true;
}

static void damage_teardown(struct damage_target *t)
{
    free(t->orig);
    free(t->intact_scan);
}

/* Write `len` bytes of the target's file, then scan.  Returns false on a
 * disallowed outcome. */
static bool damage_write_and_scan(struct damage_target *t, const char *buf,
                                 size_t len, char *scan, size_t scanlen,
                                 bool check_consistency)
{
    char path[PATH_MAX];
    bool opened;

    XSNPRINTF(path, "%s/%s", t->dir, t->name);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    if (len && write(fd, buf, len) != (ssize_t)len) { close(fd); return false; }
    close(fd);

    if (!damaged_scan(t->dir, scan, scanlen, &opened)) return false;

    /* Whatever survived must be internally consistent: the two read paths must
     * agree about it (G-7). */
    if (check_consistency && opened && !damaged_is_consistent(t->dir, scan))
        return false;

    return true;
}

static void test_malformed_truncation(void)
{
    /* Truncated at EVERY byte offset, not merely record boundaries. */
    char cases[32][64];
    size_t ncases = corpus_cases(cases, 32);
    char *scan = malloc(1u << 20);

    if (!ncases) { free(scan); SKIP("no corpus (run make corpus)"); }
    ASSERT_NOT_NULL(scan);

    for (size_t i = 0; i < ncases; i++) {
        char casedir[PATH_MAX], names[4096];
        XSNPRINTF(casedir, CORPUS_DIR "/%s", cases[i]);
        list_data_files(casedir, names, sizeof(names));

        char *save = NULL;
        for (char *nm = strtok_r(names, "\n", &save); nm;
             nm = strtok_r(NULL, "\n", &save)) {
            struct damage_target t;
            if (!damage_setup(&t, casedir, nm)) continue;

            /* Large files get a stride, so the suite stays runnable; the boundary
             * regions -- the header, the first records, and the tail -- are always
             * covered exhaustively, because that is where the interesting offsets
             * are.  The sampling is logged rather than silent (T-3). */
            size_t stride = t.len > 4096 ? 64 : 1;

            for (size_t cut = 0; cut <= t.len; cut++) {
                bool near_edge = (cut < 256) || (cut + 256 > t.len);
                if (!near_edge && (cut % stride)) continue;

                alarm(30);
                bool ok = damage_write_and_scan(&t, t.orig, cut, scan,
                                                1u << 20, true);
                alarm(0);

                if (!ok) {
                    fprintf(stderr,
                            "\n    FAIL %s/%s truncated to %zu of %zu\n",
                            cases[i], nm, cut, t.len);
                    current_test_failed = 1;
                    damage_teardown(&t);
                    free(scan);
                    return;
                }
            }

            if (stride > 1)
                fprintf(stderr, "[%s/%s: stride %zu] ", cases[i], nm, stride);

            damage_teardown(&t);
        }
    }

    free(scan);
}

static void test_malformed_bitflips(void)
{
    /* Systematically bit-flipped.  Exhaustive for small files; a deterministic
     * sample for large ones, seeded so a failure reproduces (ZS_TEST_SEED). */
    char cases[32][64];
    size_t ncases = corpus_cases(cases, 32);
    char *scan = malloc(1u << 20);
    char *tmp = NULL;
    unsigned seed = 20260807;
    const char *env = getenv("ZS_TEST_SEED");
    bool full = getenv("ZS_TEST_FUZZ_FULL") != NULL;

    if (env) seed = (unsigned)strtoul(env, NULL, 10);
    if (!ncases) { free(scan); SKIP("no corpus (run make corpus)"); }
    ASSERT_NOT_NULL(scan);

    for (size_t i = 0; i < ncases; i++) {
        char casedir[PATH_MAX], names[4096];
        XSNPRINTF(casedir, CORPUS_DIR "/%s", cases[i]);
        list_data_files(casedir, names, sizeof(names));

        char *save = NULL;
        for (char *nm = strtok_r(names, "\n", &save); nm;
             nm = strtok_r(NULL, "\n", &save)) {
            struct damage_target t;
            if (!damage_setup(&t, casedir, nm)) continue;

            tmp = realloc(tmp, t.len ? t.len : 1);
            if (!tmp) { damage_teardown(&t); free(scan); return; }

            size_t total = t.len * 8;
            size_t budget = (full || total <= 4096) ? total : 4096;
            size_t tried = 0;

            for (size_t n = 0; n < budget; n++) {
                size_t bit;
                if (budget == total) {
                    bit = n;
                } else {
                    seed = seed * 1103515245u + 12345u;
                    bit = (size_t)((seed >> 8) % total);
                }

                memcpy(tmp, t.orig, t.len);
                tmp[bit / 8] ^= (char)(1u << (bit % 8));

                alarm(30);
                bool ok = damage_write_and_scan(&t, tmp, t.len, scan,
                                                1u << 20, false);
                alarm(0);
                tried++;

                if (!ok) {
                    fprintf(stderr, "\n    FAIL %s/%s bit %zu (seed %u)\n",
                            cases[i], nm, bit, seed);
                    current_test_failed = 1;
                    damage_teardown(&t);
                    free(scan);
                    free(tmp);
                    return;
                }
            }

            /* Never let a reduced run read as full coverage (T-3). */
            if (budget < total)
                fprintf(stderr, "[%s/%s: %zu of %zu bits] ",
                        cases[i], nm, tried, total);

            damage_teardown(&t);
        }
    }

    free(scan);
    free(tmp);
}

static void test_malformed_never_hangs(void)
{
    /* F-29 head on: files built to make a naive walker loop -- a record whose
     * length would not advance, a file of repeated valid type bytes with nonsense
     * lengths, and a file that is entirely one byte value.
     *
     * The alarm is the detector.  A progress bug is non-termination rather than a
     * wrong answer, so without it this test would hang the suite instead of
     * failing it. */
    struct sb s;
    char name[ZSI_NAME_MAX];
    char scan[4096];
    bool opened;

    alarm(60);

    /* Repeated KEYVALUE headers claiming keylen 0, which F-14 rejects. */
    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    {
        char junk[4096];
        for (size_t i = 0; i < sizeof(junk); i += 8) {
            junk[i] = (char)ZSI_KEYVALUE;
            junk[i + 1] = 0;
            memset(junk + i + 2, 0, 6);
        }
        sb_raw(&s, junk, sizeof(junk));
    }
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);
    ASSERT(damaged_scan(dbdir, scan, sizeof(scan), &opened));

    /* Every byte value as the whole file body, one at a time: 256 files, each of
     * which must terminate. */
    for (unsigned v = 0; v < 256; v++) {
        clear_db();
        sb_init(&s, 1, ZSI_CSUM_XXHASH);
        char junk[512];
        memset(junk, (int)v, sizeof(junk));
        sb_raw(&s, junk, sizeof(junk));
        ASSERT_EQ(sb_write(&s, name), 0);
        sb_free(&s);
        if (!damaged_scan(dbdir, scan, sizeof(scan), &opened)) {
            fprintf(stderr, "\n    FAIL body of 0x%02X\n", v);
            current_test_failed = 1;
            alarm(0);
            return;
        }
    }

    /* A big record claiming enormous lengths, so the overflow guards are the only
     * thing between the walk and a wild pointer. */
    clear_db();
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    {
        char big[32];
        memset(big, 0, sizeof(big));
        big[0] = (char)ZSI_BIGKEYVALUE;
        zsi_put64(big + 8, 0xFFFFFFFFFFFFFFFFull);
        zsi_put64(big + 16, 0xFFFFFFFFFFFFFFFFull);
        sb_raw(&s, big, sizeof(big));
    }
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);
    ASSERT(damaged_scan(dbdir, scan, sizeof(scan), &opened));

    alarm(0);
}

/*
 * ============================================================
 * Multi-process (T-10, T-10b)
 * ============================================================
 *
 * Real forked processes, because these properties do not exist within one: fcntl
 * locks are per-process (C-1f), and the whole point of a lock-free reader is that
 * it works against a writer it cannot coordinate with.
 *
 * Where a case needs a particular interleaving it is FORCED with a pause point
 * compiled in under ZS_TEST_HOOKS, not with a sleep.  A timing-dependent test that
 * passes by luck is worse than no test, because it reads as coverage.  Where a
 * hook is impractical the case loops enough times to make a miss improbable and
 * the iteration count is printed.
 */

/* Wait for a child, returning its exit status or -1. */
static int reap(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    if (WIFSIGNALED(status)) return -(128 + WTERMSIG(status));
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_mp_writer_and_readers(void)
{
    /* A writer plus N readers: each reader's snapshot is stable across the writer's
     * commits, and a fresh open sees them (G-4).
     *
     * The stability claim is the one that matters: a reader takes no lock (C-2), so
     * nothing stops the writer appending underneath it -- only the fact that every
     * byte below its boundary is immutable (C-4c). */
    SKIP_IF_NO_FORK();

    struct zs_db *db = fresh_db();
    ASSERT_NOT_NULL(db);
    for (int i = 0; i < 5; i++) {
        char k[16];
        snprintf(k, sizeof(k), "base%d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "v", 1, 0));
    }
    zs_db_close(&db);

    enum { NREADERS = 3 };
    pid_t readers[NREADERS];

    for (int i = 0; i < NREADERS; i++) {
        readers[i] = fork();
        ASSERT(readers[i] >= 0);
        if (readers[i] == 0) {
            /* G-4 holds per TRANSACTION: a scan inside one explicit read
             * transaction must never change, however much the writer commits.
             * It does NOT hold of the handle: C-4i makes handle-level reads
             * fresh at every begin. */
            struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
            struct zs_db *rdb = NULL;
            struct zs_txn *rtxn = NULL;
            setup.flags = ZS_SHARED;
            setup.error = counting_error;
            if (zs_db_open(dbdir, &setup, &rdb) != ZS_OK) _exit(1);

            if (zs_db_begin_txn(rdb, 1, &rtxn) != ZS_OK) _exit(1);

            char first[4096] = "";
            if (zs_txn_foreach(rtxn, NULL, 0, NULL, api_collect_cb, first, 0)
                != ZS_OK) _exit(2);

            for (int n = 0; n < 200; n++) {
                char again[4096] = "";
                if (zs_txn_foreach(rtxn, NULL, 0, NULL, api_collect_cb,
                                   again, 0) != ZS_OK) _exit(3);
                if (strcmp(first, again) != 0) _exit(4);   /* snapshot moved */
                usleep(200);
            }

            /* The snapshot must be a consistent state, not a mixture: every base
             * key present, since they were all committed before the fork. */
            for (int j = 0; j < 5; j++) {
                char want[32];
                snprintf(want, sizeof(want), "base%d=v", j);
                if (!strstr(first, want)) _exit(5);
            }

            if (zs_txn_abort(&rtxn) != ZS_OK) _exit(6);

            /* And C-4i's other half: HANDLE-level reads on this old handle see
             * whatever the writer has committed by now -- at least one scan
             * must observe more than the base keys once the writer has run.
             * The parent holds reaping until its 40 commits are done, so by
             * the time anyone checks this exit code the writer HAS run; loop
             * briefly to let this reader catch it. */
            {
                bool grew = false;
                for (int n = 0; n < 2000 && !grew; n++) {
                    char now[4096] = "";
                    if (zs_db_foreach(rdb, NULL, 0, NULL, api_collect_cb,
                                      now, 0) != ZS_OK) _exit(7);
                    if (strstr(now, "new39=w")) grew = true;
                    else usleep(1000);
                }
                if (!grew) _exit(8);            /* stale forever: the bug */
            }

            zs_db_close(&rdb);
            _exit(0);
        }
    }

    /* The writer commits throughout. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    for (int i = 0; i < 40; i++) {
        char k[16];
        snprintf(k, sizeof(k), "new%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "w", 1, 0));
    }
    zs_db_close(&db);

    for (int i = 0; i < NREADERS; i++) {
        int rc = reap(readers[i]);
        if (rc != 0) {
            fprintf(stderr, "\n    FAIL reader %d exited %d "
                    "(4 = snapshot changed under it)\n", i, rc);
            current_test_failed = 1;
            return;
        }
    }

    /* A fresh open sees everything. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "new39", 5, NULL, NULL, &v, &vl, 0));
    ASSERT_OK(zs_db_fetch(db, "base0", 5, NULL, NULL, &v, &vl, 0));
    zs_db_close(&db);
}

static void test_mp_two_writers(void)
{
    /* Two writers, exactly one proceeding, and ZS_LOCKED under ZS_NONBLOCKING. */
    SKIP_IF_NO_FORK();

    struct zs_db *db = fresh_db();
    ASSERT_NOT_NULL(db);
    zs_db_close(&db);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *cdb = NULL;
        struct zs_txn *txn = NULL;
        close(pipefd[0]);
        setup.error = counting_error;
        if (zs_db_open(dbdir, &setup, &cdb) != ZS_OK) _exit(1);
        if (zs_db_begin_txn(cdb, 0, &txn) != ZS_OK) _exit(2);
        if (zs_txn_store(txn, "child", 5, "c", 1, 0) != ZS_OK) _exit(3);
        if (write(pipefd[1], "x", 1) != 1) _exit(4);
        close(pipefd[1]);
        usleep(400000);                 /* hold the write lock */
        if (zs_txn_commit(&txn) != ZS_OK) _exit(5);
        zs_db_close(&cdb);
        _exit(0);
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);       /* the child holds the lock */
    close(pipefd[0]);

    /* Non-blocking: refused. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    setup.flags = ZS_NONBLOCKING;
    setup.error = counting_error;
    struct zs_db *nb = NULL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &nb));
    struct zs_txn *txn = NULL;
    ASSERT_EQ(zs_db_begin_txn(nb, 0, &txn), ZS_LOCKED);
    ASSERT_NULL(txn);

    /* A READ transaction is not blocked at all -- readers take no lock (C-2). */
    ASSERT_OK(zs_db_begin_txn(nb, 1, &txn));
    ASSERT_OK(zs_txn_abort(&txn));
    zs_db_close(&nb);

    /* Blocking: waits, then proceeds. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    alarm(30);
    ASSERT_OK(zs_db_store(db, "parent", 6, "p", 1, 0));
    alarm(0);
    zs_db_close(&db);

    ASSERT_EQ(reap(pid), 0);

    /* Both writes survived: the lock serialised them rather than losing one. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "child", 5, NULL, NULL, &v, &vl, 0));
    ASSERT_OK(zs_db_fetch(db, "parent", 6, NULL, NULL, &v, &vl, 0));
    zs_db_close(&db);
}

static void test_mp_killed_writer(void)
{
    /* G-5: a writer SIGKILLed holding the lock, and the next writer proceeding with
     * NO manual intervention.
     *
     * This is what frees the design from stale-lock recovery entirely: the kernel
     * releases fcntl locks on process death, so no lock state can outlive a
     * process and there is nothing to time out or clean up. */
    SKIP_IF_NO_FORK();

    struct zs_db *db = fresh_db();
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "before", 6, "1", 1, 0));
    zs_db_close(&db);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *cdb = NULL;
        struct zs_txn *txn = NULL;
        close(pipefd[0]);
        setup.error = counting_error;
        if (zs_db_open(dbdir, &setup, &cdb) != ZS_OK) _exit(1);
        if (zs_db_begin_txn(cdb, 0, &txn) != ZS_OK) _exit(2);
        if (zs_txn_store(txn, "doomed", 6, "x", 1, 0) != ZS_OK) _exit(3);
        if (write(pipefd[1], "x", 1) != 1) _exit(4);
        for (;;) pause();               /* hold it until killed */
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);
    close(pipefd[0]);

    ASSERT_EQ(kill(pid, SIGKILL), 0);
    ASSERT(reap(pid) < 0);              /* died by signal */

    /* The next writer proceeds immediately. */
    alarm(30);
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "after", 5, "2", 1, 0));
    alarm(0);

    /* The killed writer's uncommitted record is not visible -- it was never
     * committed, and this writer buffered it in memory that died with it. */
    const char *v;
    size_t vl;
    ASSERT_EQ(zs_db_fetch(db, "doomed", 6, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_OK(zs_db_fetch(db, "before", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_OK(zs_db_fetch(db, "after", 5, NULL, NULL, &v, &vl, 0));
    zs_db_close(&db);
}

static void test_mp_reader_across_repack(void)
{
    /* C-4g: a reader holding a snapshot across a repack keeps reading while the
     * inputs are unlinked.  The kernel keeps each inode alive until the last
     * descriptor AND MAPPING is gone, so there is no reference table and nothing to
     * clean up when a process dies.
     *
     * C-5's accepted cost is asserted alongside: the space is held until that
     * reader exits. */
    SKIP_IF_NO_FORK();

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {"b","2"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"c","3"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {"d","4"}, {NULL,NULL} });

    int pipefd[2], donefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    ASSERT_EQ(pipe(donefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *rdb = NULL;
        char before[2048] = "", after[2048] = "";
        close(pipefd[0]);
        close(donefd[1]);
        setup.flags = ZS_SHARED;
        setup.error = counting_error;
        if (zs_db_open(dbdir, &setup, &rdb) != ZS_OK) _exit(1);
        if (zs_db_foreach(rdb, NULL, 0, NULL, api_collect_cb, before, 0) != ZS_OK)
            _exit(2);

        if (write(pipefd[1], "x", 1) != 1) _exit(3);
        close(pipefd[1]);

        /* wait for the parent to repack and unlink the inputs */
        char c;
        if (read(donefd[0], &c, 1) != 1) _exit(4);
        close(donefd[0]);

        /* Still readable, identically, from files that no longer have names. */
        if (zs_db_foreach(rdb, NULL, 0, NULL, api_collect_cb, after, 0) != ZS_OK)
            _exit(5);
        if (strcmp(before, after) != 0) _exit(6);
        zs_db_close(&rdb);
        _exit(0);
    }

    close(pipefd[1]);
    close(donefd[0]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);       /* the reader holds its snapshot */
    close(pipefd[0]);

    struct zs_db *db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_repack_merge(db, db->snap, 0, 2));
    ASSERT_OK(zsi_db_refresh(db));

    /* Retire the inputs while the reader holds them. */
    char n1[ZSI_NAME_MAX], n2[ZSI_NAME_MAX];
    zsi_name_format(n1, db->uuid, 1, 1);
    zsi_name_format(n2, db->uuid, 2, 2);
    ASSERT_OK(zsi_remove_file(db, n1));
    ASSERT_OK(zsi_remove_file(db, n2));
    ASSERT_EQ(fexists(dbpath(n1)), -ENOENT);
    ASSERT_EQ(fexists(dbpath(n2)), -ENOENT);
    zs_db_close(&db);

    ASSERT_EQ(write(donefd[1], "x", 1), 1);
    close(donefd[1]);

    int rc = reap(pid);
    if (rc != 0) {
        fprintf(stderr, "\n    FAIL reader exited %d "
                "(6 = data changed under it after its inputs were unlinked)\n", rc);
        current_test_failed = 1;
        return;
    }
}

static void test_mp_racing_removers(void)
{
    /* Two processes racing to remove debris, asserting the surviving set still
     * tiles and no needed file is removed (D-23).
     *
     * Then the case T-10 calls out specifically: the debris of TWO half-finished
     * repacks over OVERLAPPING ranges, where naive independent cleanup would remove
     * both outputs' inputs and lose a generation. */
    SKIP_IF_NO_FORK();

    /* A directory holding [1-2] and [2-3] -- two outputs whose ranges overlap --
     * plus the inputs [1-1], [2-2], [3-3] and an active file. */
    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });
    put_inorder_kv(3, 3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });
    put_inorder_kv(1, 2, (const struct kv[]){ {"a","1"}, {"b","2"}, {NULL,NULL} });
    put_unordered_kv(4, (const struct kv[]){ {"d","4"}, {NULL,NULL} });

    /* A partial overlap ([2-3] against [1-2]) is corruption (D-5c), so use a
     * NESTED second output instead: [1-3], which encloses [1-2]. */
    put_inorder_kv(1, 3, (const struct kv[]){ {"a","1"}, {"b","2"}, {"c","3"},
                                              {NULL,NULL} });

    /* Two children each try to remove every file they think is debris. */
    static const char *victims[] = { NULL };
    (void)victims;

    pid_t kids[2];
    for (int i = 0; i < 2; i++) {
        kids[i] = fork();
        ASSERT(kids[i] >= 0);
        if (kids[i] == 0) {
            struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
            struct zs_db *cdb = NULL;
            setup.error = counting_error;
            if (zs_db_open(dbdir, &setup, &cdb) != ZS_OK) _exit(1);

            /* Attempt every plausible removal, in a different order per child, so
             * they genuinely interleave. */
            uint32_t ranges[5][2] = { {1,1}, {2,2}, {3,3}, {1,2}, {1,3} };
            for (int n = 0; n < 5; n++) {
                int idx = i ? 4 - n : n;
                char nm[ZSI_NAME_MAX];
                zsi_name_format(nm, cdb->uuid, ranges[idx][0], ranges[idx][1]);
                (void)zsi_remove_file(cdb, nm);
                usleep(1000);
            }
            zs_db_close(&cdb);
            _exit(0);
        }
    }

    for (int i = 0; i < 2; i++)
        ASSERT_EQ(reap(kids[i]), 0);

    /* Whatever survived, the set must still tile and hold every record. */
    struct zs_db *db = open_db(0);
    ASSERT_NOT_NULL(db);

    struct zsi_fileset fs;
    ASSERT_OK(zsi_fileset_scan(dbdir, &db->uuid, &fs));
    int rr = zsi_fileset_resolve(&fs);
    zsi_fileset_fini(&fs);
    if (rr != ZS_OK) {
        fprintf(stderr, "\n    FAIL the surviving set does not tile (%d)\n", rr);
        current_test_failed = 1;
        zs_db_close(&db);
        return;
    }

    char got[512];
    api_scan(db, got, sizeof(got));
    if (strcmp(got, "a=1|b=2|c=3|d=4") != 0) {
        fprintf(stderr, "\n    FAIL records lost to racing removal: '%s'\n", got);
        current_test_failed = 1;
    }
    zs_db_close(&db);
}

static void test_mp_removal_needs_the_lock(void)
{
    /* D-23: removal attempted WITHOUT the remove lock, asserting refusal.
     *
     * The lock is what makes verification and unlinking one step, so the set cannot
     * change in between.  Asserted by holding the lock in a child and confirming a
     * non-blocking removal in the parent is refused rather than proceeding on a
     * stale verification. */
    SKIP_IF_NO_FORK();

    clear_db();
    put_inorder_kv(1, 1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_inorder_kv(1, 2, (const struct kv[]){ {"a","1"}, {"b","2"}, {NULL,NULL} });
    put_inorder_kv(2, 2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });
    put_unordered_kv(3, (const struct kv[]){ {"c","3"}, {NULL,NULL} });

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zsi_locks lk;
        close(pipefd[0]);
        if (zsi_lock_open(&lk, dbdir) != ZS_OK) _exit(1);
        if (zsi_lock_take(&lk, ZSI_LOCK_REMOVE, 0) != ZS_OK) _exit(2);
        if (write(pipefd[1], "x", 1) != 1) _exit(3);
        close(pipefd[1]);
        usleep(400000);
        zsi_lock_close(&lk);
        _exit(0);
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);
    close(pipefd[0]);

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    setup.flags = ZS_NONBLOCKING;
    setup.error = counting_error;
    struct zs_db *db = NULL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char nm[ZSI_NAME_MAX];
    zsi_name_format(nm, db->uuid, 1, 1);
    ASSERT_EQ(zsi_remove_file(db, nm), ZS_LOCKED);
    ASSERT_EQ(fexists(dbpath(nm)), 0);          /* untouched */
    zs_db_close(&db);

    ASSERT_EQ(reap(pid), 0);

    /* With the lock free it succeeds. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zsi_remove_file(db, nm));
    ASSERT_EQ(fexists(dbpath(nm)), -ENOENT);
    zs_db_close(&db);
}

static void test_mp_repack_and_writer_concurrent(void)
{
    /* C-1a's disjointness claim, made concrete: a repack and a writer both
     * proceeding, and the resulting set still tiling (D-6).
     *
     * The two locks never contend because the jobs consume disjoint file sets -- a
     * writer only converts files with end == 0, the repacker only merges files with
     * end != 0 -- so this must complete without either waiting on the other. */
    SKIP_IF_NO_FORK();

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 400;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char pad[150];
    memset(pad, 'p', sizeof(pad));
    for (int i = 0; i < 12; i++) {
        char k[16];
        snprintf(k, sizeof(k), "seed%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), pad, sizeof(pad), 0));
    }
    zs_db_close(&db);

    pid_t packer = fork();
    ASSERT(packer >= 0);
    if (packer == 0) {
        struct zs_open_data s2 = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *pdb = NULL;
        s2.error = counting_error;
        if (zs_db_open(dbdir, &s2, &pdb) != ZS_OK) _exit(1);
        for (int n = 0; n < 8; n++) {
            if (zs_db_repack(pdb) != ZS_OK) _exit(2);
            usleep(2000);
        }
        zs_db_close(&pdb);
        _exit(0);
    }

    /* The writer keeps going throughout. */
    alarm(60);
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    for (int i = 0; i < 30; i++) {
        char k[16];
        snprintf(k, sizeof(k), "live%02d", i);
        if (zs_db_store(db, k, strlen(k), pad, sizeof(pad), 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL writer blocked or failed at %d\n", i);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
        usleep(1000);
    }
    zs_db_close(&db);
    alarm(0);

    ASSERT_EQ(reap(packer), 0);

    /* Everything survived, and the set tiles. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    for (int i = 0; i < 12; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "seed%02d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s lost\n", k);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
    }
    for (int i = 0; i < 30; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "live%02d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) {
            fprintf(stderr, "\n    FAIL %s lost\n", k);
            current_test_failed = 1;
            zs_db_close(&db);
            return;
        }
    }
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}

static void test_mp_reader_sees_torn_span(void)
{
    /* T-10b's most valuable case: a writer killed MID-SPAN while a reader scans,
     * asserting the reader stops at the last valid terminator (C-4f).
     *
     * This is the case that shows the terminator checksum, not a lock, is what
     * makes reading a live file safe -- and it is why the checksum covers the span
     * as well as the terminator (F-19).
     *
     * The interleaving is forced rather than raced: the child writes a span's
     * records with a raw descriptor and dies before writing the terminator, so the
     * reader is guaranteed to meet a span in progress. */
    SKIP_IF_NO_FORK();

    struct zs_db *db = fresh_db();
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "committed", 9, "yes", 3, 0));
    char name[ZSI_NAME_MAX];
    zsi_name_current(name, db->uuid);
    zs_db_close(&db);

    long before = filesize(name);
    ASSERT(before > 0);

    /* Append a span's records with NO terminator, exactly as a writer killed
     * between the two durability gates would leave (C-7). */
    {
        size_t n = zsi_rec_encoded_len(7, 3, false);
        char *rec = malloc(n);
        ASSERT_NOT_NULL(rec);
        /* fresh_db() defaults to ENGINE 1 (zsi_csum_id_for_flags maps no
         * flag to XXHASH), so the record's checksum must be engine 1's --
         * the same engine the terminator below this uses.  This span is
         * never read as valid data, but salvage walks it, and a wrong-engine
         * checksum here would read as record corruption rather than the
         * missing terminator this test is about. */
        zsi_rec_encode(rec, zsi_csum_xxhash, "partial", 7, "no!", 3);
        int fd = open(dbpath(name), O_WRONLY | O_APPEND);
        ASSERT(fd >= 0);
        ASSERT_EQ(write(fd, rec, n), (ssize_t)n);
        close(fd);
        free(rec);
    }

    /* A reader must stop at the last valid terminator. */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->snap->files[0]->complete, (size_t)before);
    ASSERT(db->snap->files[0]->size > (size_t)before);

    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "committed", 9, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "yes", 3);
    ASSERT_EQ(zs_db_fetch(db, "partial", 7, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    zs_db_close(&db);

    /* Now the harder shape: the terminator IS present but its data was not fully
     * written, which is what a reordering filesystem leaves.  The checksum is the
     * only thing that can tell. */
    {
        size_t datalen = (size_t)(filesize(name) - before);
        char term[ZSI_TERMLEN_LONG];
        char *data = malloc(datalen);
        ASSERT_NOT_NULL(data);

        /* the terminator covers the data as WRITTEN... */
        int fd = open(dbpath(name), O_RDONLY);
        ASSERT(fd >= 0);
        ASSERT_EQ(lseek(fd, before, SEEK_SET), before);
        ASSERT_EQ(read(fd, data, datalen), (ssize_t)datalen);
        close(fd);

        zsi_term_encode(term, datalen, false, data, zsi_csum_xxhash,
                        ZSI_CSUM_XXHASH);

        /* ...but then the data is zeroed, as though it never landed. */
        fd = open(dbpath(name), O_WRONLY);
        ASSERT(fd >= 0);
        ASSERT_EQ(lseek(fd, before, SEEK_SET), before);
        char *zeros = calloc(1, datalen);
        ASSERT_NOT_NULL(zeros);
        ASSERT_EQ(write(fd, zeros, datalen), (ssize_t)datalen);
        ASSERT_EQ(lseek(fd, 0, SEEK_END), (off_t)filesize(name));
        ASSERT_EQ(write(fd, term, zsi_term_encoded_len(datalen)),
                  (ssize_t)zsi_term_encoded_len(datalen));
        close(fd);
        free(data);
        free(zeros);
    }

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    /* The span reads as absent: the terminator arrived, the data did not, and F-22
     * caught it.  Nothing but the checksum could have. */
    ASSERT_EQU(db->snap->files[0]->complete, (size_t)before);
    ASSERT_OK(zs_db_fetch(db, "committed", 9, NULL, NULL, &v, &vl, 0));
    ASSERT_EQ(zs_db_fetch(db, "partial", 7, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    zs_db_close(&db);
}

/*
 * ============================================================
 * Negative and structural requirements (T-11 gap closing)
 * ============================================================
 *
 * Requirements of the form "never do X" and "X is not consulted".  They are easy to
 * leave untested precisely because there is nothing to observe when they hold --
 * which is why the conformance map made them visible.
 */

static void test_never_unlinks_the_lock_file(void)
{
    /* D-3b: the lock file MUST NOT be unlinked, by this library or anything else.
     *
     * Unlinking it while processes hold locks is the one way to break mutual
     * exclusion from outside: holders keep locking the removed inode while a new
     * process creates a fresh one and locks that, so TWO WRITERS each believe they
     * hold the write lock.  Worth a test because an empty file named *.lock is
     * exactly what a cleanup script deletes.
     *
     * Asserted two ways: the source never unlinks that name, and a full workload
     * leaves it in place. */
    FILE *fp = fopen("zeroskip.c", "r");
    if (!fp) SKIP("zeroskip.c not readable from the test's cwd");

    char line[1024];
    int lineno = 0, found = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        /* any unlink whose argument mentions the lock name */
        if (strstr(line, "UNLINK") && strstr(line, "LOCK_NAME")) {
            fprintf(stderr, "\n    FAIL zeroskip.c:%d unlinks the lock file\n",
                    lineno);
            found = 1;
        }
    }
    fclose(fp);
    ASSERT_EQ(found, 0);

    /* And behaviourally: a workload including conversion and repack leaves it. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    clear_db();
    setup.flags = ZS_CREATE;
    setup.rollover_size = 400;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    char pad[150];
    memset(pad, 'p', sizeof(pad));
    for (int i = 0; i < 12; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), pad, sizeof(pad), 0));
    }
    while (zs_db_should_repack(db)) ASSERT_OK(zs_db_repack(db));
    zs_db_close(&db);

    ASSERT_EQ(fexists(dbpath(ZSI_LOCK_NAME)), 0);
}

static void test_one_lock_descriptor(void)
{
    /* C-1g: fcntl locks are released by closing ANY descriptor for the file in that
     * process, so an implementation MUST hold exactly one for the handle's lifetime
     * and MUST NOT open a second.
     *
     * The hazard is silent: a second open followed by a close drops every lock the
     * handle holds, and nothing reports it.  Asserted structurally, since the
     * behavioural version would need the bug present to observe. */
    FILE *fp = fopen("zeroskip.c", "r");
    if (!fp) SKIP("zeroskip.c not readable from the test's cwd");

    char line[1024];
    int opens = 0;
    while (fgets(line, sizeof(line), fp))
        if (strstr(line, "ZSI_LOCK_NAME") && strstr(line, "path")) opens++;
    fclose(fp);

    /* Exactly one place builds the lock file's path: zsi_lock_open. */
    ASSERT_EQ(opens, 1);

    /* And a handle holds exactly one descriptor for it. */
    struct zs_db *db = fresh_db();
    ASSERT_NOT_NULL(db);
    ASSERT(db->locks.fd >= 0);
    int fd = db->locks.fd;
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    ASSERT_EQ(db->locks.fd, fd);           /* unchanged by a transaction */
    zs_db_close(&db);
}

static void test_reads_never_consult_ancestors(void)
{
    /* D-14c: ancestors are NOT consulted by any read.  They exist solely for
     * repacking (F-16), so a lookup never follows a chain.
     *
     * Asserted by making every ancestor a lie: hand-built files whose ancestor
     * fields point at nonsense generations.  If a read consulted them it would
     * follow a chain into a file that does not exist; because it does not, the
     * answers are unchanged. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];
    char got[256];

    clear_db();

    /* Generation 5 with ancestors naming generations 1 and 99 -- one below the
     * file, one far above anything present. */
    ib_init(&b, 5, 5, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "A", 1);
    ib_rec(&b, "b", 1, "B", 1);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 5, 5);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    put_unordered_kv(6, (const struct kv[]){ {"c","C"}, {NULL,NULL} });

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* Both keys read normally, and the scan is complete. */
    const char *v;
    size_t vl;
    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "A", 1);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "B", 1);
    api_scan(db, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=A|b=B|c=C");

    /* A deletion with a nonsense ancestor still hides the key -- the tombstone is
     * honoured on its own terms, not by chasing its ancestor. */
    zs_db_close(&db);

    /* Generation 6 becomes in-order, and 7 carries the tombstone.  The active
     * file has to move out of the way first: D-1b gives it one name, so writing
     * a new unordered generation over it would drop generation 6 out of the set
     * and leave a gap where the old naming allowed both to sit side by side. */
    put_inorder_kv(6, 6, (const struct kv[]){ {"c","C"}, {NULL,NULL} });
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(unlink(dbpath(name)), 0);

    ib_init(&b, 7, 7, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, NULL, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 7, 7);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    zs_db_close(&db);
}

static void test_no_yield_and_no_mvcc(void)
{
    /* A-2: there is no yield call and no yield flags, because readers hold no lock
     * and so have nothing to yield.  A-3: there is no MVCC flag, because snapshot
     * isolation is the only read mode.
     *
     * twom has both.  Porting them here would be copying an answer to a question
     * this design does not ask, and a structural check is what stops that happening
     * by reflex during a later port. */
    FILE *fp = fopen("zeroskip.h", "r");
    if (!fp) SKIP("zeroskip.h not readable from the test's cwd");

    char line[1024];
    int bad = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "yield") && !strstr(line, "no yield")
            && line[0] != ' ' && line[0] != '*') {
            fprintf(stderr, "\n    FAIL zeroskip.h declares a yield entry point\n");
            bad = 1;
        }
        if (strstr(line, "ZS_MVCC")) {
            fprintf(stderr, "\n    FAIL zeroskip.h declares an MVCC flag\n");
            bad = 1;
        }
    }
    fclose(fp);
    ASSERT_EQ(bad, 0);

    /* The behavioural side -- that an explicit read transaction's view is fixed
     * no matter who commits -- is test_write_txn_isolation and
     * test_txn_cursor_view_is_fixed; an explicit transaction's snapshot is
     * simply never refreshed, whichever handle commits. */
}

static void test_conversion_avoids_the_repack_lock(void)
{
    /* D-12c: conversion never takes the repack lock.  It renames its output in
     * without any lock (C-1b) and takes the remove lock only momentarily, so a
     * WRITER NEVER WAITS ON A REPACK -- which is C-1a's practical consequence.
     *
     * Asserted by holding the repack lock in a child and confirming a writer's
     * conversion still completes. */
    SKIP_IF_NO_FORK();

    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX];

    clear_db();
    put_unordered_kv(1, (const struct kv[]){ {"a","1"}, {NULL,NULL} });
    put_unordered_kv(2, (const struct kv[]){ {"b","2"}, {NULL,NULL} });

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);

    pid_t pid = fork();
    ASSERT(pid >= 0);
    if (pid == 0) {
        struct zsi_locks lk;
        close(pipefd[0]);
        if (zsi_lock_open(&lk, dbdir) != ZS_OK) _exit(1);
        if (zsi_lock_take(&lk, ZSI_LOCK_REPACK, 0) != ZS_OK) _exit(2);
        if (write(pipefd[1], "x", 1) != 1) _exit(3);
        close(pipefd[1]);
        usleep(500000);
        zsi_lock_close(&lk);
        _exit(0);
    }

    close(pipefd[1]);
    char c;
    ASSERT_EQ(read(pipefd[0], &c, 1), 1);       /* the repack lock is held */
    close(pipefd[0]);

    /* A write, which converts before finishing (D-12), must not block on it. */
    alarm(20);
    db = open_db(0);
    ASSERT_NOT_NULL(db);

    /* A SEAL, because D-1b leaves no other way to make a writer convert: there
     * is no such thing as a non-active unordered file to drain (D-12a), so the
     * conversion that remains is the active file's own (D-25, D-12b). */
    ASSERT_OK(zs_db_seal(db));
    alarm(0);

    /* The conversion happened despite the repack lock being held. */
    zsi_name_format(name, db->uuid, 2, 2);
    ASSERT_EQ(fexists(dbpath(name)), 0);
    zs_db_close(&db);

    ASSERT_EQ(reap(pid), 0);
}

static void test_open_is_o1_in_records(void)
{
    /* F-31: opening an in-order file is O(1) -- validate the header, read the
     * 16-byte trailer, verify the pointer-section checksum, use the pointers.  The
     * records region is NOT touched, and its checksum is verified only on demand
     * (F-26f).
     *
     * Asserted by corrupting the records region and confirming the open succeeds:
     * an open that read the records would have to notice. */
    struct zs_db *db;
    struct ib b;
    char name[ZSI_NAME_MAX];

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    for (int i = 0; i < 50; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ib_rec(&b, k, strlen(k), "value", 5);
    }
    ib_finish(&b);

    /* Damage a value byte deep in the records region. */
    b.buf[ZSI_HEADER_LEN + 40] ^= 0xFF;

    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
    put_unordered_kv(2, (const struct kv[]){ {NULL,NULL} });

    db = open_db_reporting(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(report_count, 0);          /* open said nothing: it never looked */

    /* On demand, it is found. */
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADCHECKSUM);
    zs_db_close(&db);
}

/*
 * ============================================================
 * Pointer table cache (spec section 8)
 * ============================================================
 */

/* Make a cache directory under basedir and return its path in `out`. */
static void idxcache_mkdir(char *out, size_t outlen)
{
    snprintf(out, outlen, "%s/cache", basedir);
    if (mkdir(out, 0700) && errno != EEXIST) {
        fprintf(stderr, "\n    FAIL: mkdir %s: %s\n", out, strerror(errno));
        current_test_failed = 1;
    }
}

/* P-2a: a handle's tables live under <root>/<uuid>/, which open resolves and
 * creates.  Tests that plant or inspect tables need the same path. */
static void idxcache_dbdir(struct zs_db *db, const char *root,
                           char *out, size_t outlen)
{
    char uu[ZSI_UUID_STR_LEN];
    zsi_uuid_unparse(db->uuid, uu);
    XSNPRINTFN(out, outlen, "%s/%s", root, uu);
}

/* A-8, P-2, R-3: the cache directory must not be the database directory.
 * Allowing it would let a read-only handle write into the database, which is
 * precisely what R-3 forbids -- the amendment permits publishing only because a
 * cache directory is somewhere else. */
static void test_idxcache_rejects_db_dir(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char dotted[PATH_MAX];

    setup.flags = ZS_CREATE;

    /* Identical strings, caught BEFORE the database is created.  The "before" is
     * the point of having two checks: a call rejected as a usage error must not
     * leave a directory behind, and the resolved-path compare cannot run until
     * the directory exists.  Asserting only the return code would leave the
     * ordering untested. */
    setup.index_dir = dbdir;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    ASSERT_NULL(db);
    {
        struct stat sb;
        ASSERT_EQ(stat(dbdir, &sb), -1);
    }

    /* Create the database for real, then try to name it a second way.  A plain
     * string compare cannot catch this one. */
    setup.index_dir = NULL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_close(&db));

    snprintf(dotted, sizeof(dotted), "%s/./", dbdir);
    setup.index_dir = dotted;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    ASSERT_NULL(db);
}

/* A-9: index_threshold 0 resolves to a default derived from rollover_size, so a
 * caller that sets index_dir and nothing else still gets bounded publishing. */
static void test_idxcache_threshold_defaults(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cachedir[PATH_MAX];

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;

    /* The default is DERIVED PER FILE at the publish site, so it stays zero here:
     * it bounds the number of publications over a file's life rather than the
     * bytes between them, because each one rewrites a table proportional to the
     * file.  A fixed byte gap makes the total quadratic in generation size; a
     * fraction of rollover_size makes a small database with a large rollover
     * publish almost never and pay at every open. */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NOT_NULL(db);
    ASSERT_EQU(db->index_threshold, 0u);
    {
        /* A-8/P-2a: the handle resolves the root to its per-uuid child. */
        char want[PATH_MAX];
        struct stat sb;
        idxcache_dbdir(db, cachedir, want, sizeof(want));
        ASSERT_STR_EQ(db->index_dir, want);
        ASSERT_EQ(stat(want, &sb), 0);
        ASSERT(S_ISDIR(sb.st_mode));
    }
    ASSERT_OK(zs_db_close(&db));

    /* rollover_size does not enter into it either way, which is the point: the
     * threshold scales with the file, so the same handle publishes often while its
     * active file is small and rarely once it is large. */
    setup.rollover_size = 64 * 1024 * 1024;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQU(db->index_threshold, 0u);
    ASSERT_OK(zs_db_close(&db));
    setup.rollover_size = 0;

    /* An explicit value wins, and the cache stays off when index_dir is NULL. */
    setup.index_threshold = 4321;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQU(db->index_threshold, 4321u);
    ASSERT_OK(zs_db_close(&db));

    setup.index_dir = NULL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NULL(db->index_dir);
    ASSERT_OK(zs_db_close(&db));
}

/* P-5, P-6: all 96 bytes against a literal.
 *
 * A matched encoder and decoder round-trip perfectly under a SYMMETRIC layout
 * change -- swap two fields in both and nothing notices -- which is the exact
 * bug class that leaves a peer unable to read our tables.  Mutation testing
 * found it once already, in the data-file header, which is why
 * test_header_byte_layout exists.  Only a literal catches it. */
static void test_idxcache_header_byte_layout(void)
{
    static const unsigned char golden[ZSI_IDX_HEADER_LEN] = {
        /* 0  magic, all 16 bytes */
        0x89, 0x7A, 0x73, 0x69, 0x6E, 0x64, 0x65, 0x78,
        0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
        /* 16 vread, 17 vwrite, 18 flags (LE) = 0x0011, 20 reserved */
        0x01, 0x01, 0x11, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 24 uuid */
        0x49, 0x41, 0xDA, 0x54, 0x94, 0x06, 0x4F, 0xAA,
        0xA4, 0x57, 0xC4, 0xB6, 0x5B, 0xEA, 0xE3, 0xEB,
        /* 40 start = 0x01020304 LE, 44 reserved */
        0x04, 0x03, 0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
        /* 48 comparator name, NUL-padded to 16 */
        0x6D, 0x65, 0x6D, 0x63, 0x6D, 0x70, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 64 valid_upto = 0x1122334455667788 LE */
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
        /* 72 term_off = 200 LE */
        0xC8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 80 nptrs = 7 LE */
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 88 term_csum = 0xDEADBEEF LE, 92 checksum of [0, 92) */
        0xEF, 0xBE, 0xAD, 0xDE, 0x30, 0x10, 0x2D, 0x0D
    };

    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    struct zsi_idxhdr h;
    char buf[ZSI_IDX_HEADER_LEN];

    ASSERT_EQ(ZSI_IDX_HEADER_LEN, 96);
    ASSERT_EQ(ZSI_IDX_MAGIC_LEN, 16);

    /* Different from the data-file magic, so the two artefacts are told apart by
     * content and not only by name (P-6). */
    ASSERT(memcmp(zsi_idx_magic, zsi_magic, ZSI_MAGIC_LEN) != 0);

    memset(&h, 0, sizeof(h));
    h.version_read  = 1;
    h.version_write = 1;
    h.flags         = ZSI_CSUM_XXHASH | ZSI_IDX_FLAG_CSUM_VERIFIED;
    memcpy(h.uuid, u, 16);
    h.start      = 0x01020304;
    memcpy(h.compar_name, "memcmp", 6);
    h.valid_upto = 0x1122334455667788ULL;
    h.term_off   = 200;
    h.nptrs      = 7;
    h.term_csum  = 0xDEADBEEF;

    memset(buf, 0xAA, sizeof(buf));
    zsi_idxhdr_encode(buf, &h, zsi_csum_xxhash);

    for (size_t i = 0; i < ZSI_IDX_HEADER_LEN; i++) {
        if ((unsigned char)buf[i] != golden[i]) {
            fprintf(stderr, "\n    FAIL byte %zu: got 0x%02X, expected 0x%02X\n",
                    i, (unsigned char)buf[i], golden[i]);
            current_test_failed = 1;
            return;
        }
    }

    /* Each field at its literal offset, so a failure names the field rather than
     * just an offset. */
    ASSERT_MEM_EQ(buf + 0, zsi_idx_magic, 16);
    ASSERT_EQ((unsigned char)buf[16], 1);
    ASSERT_EQ((unsigned char)buf[17], 1);
    ASSERT_EQU(zsi_get16(buf + 18), 0x0011u);
    ASSERT_EQU(zsi_get32(buf + 20), 0u);
    ASSERT_MEM_EQ(buf + 24, u, 16);
    ASSERT_EQU(zsi_get32(buf + 40), 0x01020304u);
    ASSERT_EQU(zsi_get32(buf + 44), 0u);
    ASSERT_MEM_EQ(buf + 48, "memcmp", 6);
    ASSERT_EQU(zsi_get64(buf + 64), 0x1122334455667788ULL);
    ASSERT_EQU(zsi_get64(buf + 72), 200u);
    ASSERT_EQU(zsi_get64(buf + 80), 7u);
    ASSERT_EQU(zsi_get32(buf + 88), 0xDEADBEEFu);
    ASSERT_EQU(zsi_get32(buf + 92), zsi_csum_xxhash(buf, 92));

    /* Round-trip, including the engine read as plain data first (F-5a). */
    {
        struct zsi_idxhdr back;
        memset(&back, 0, sizeof(back));
        ASSERT_EQ(zsi_idxhdr_engine_id(buf), ZSI_CSUM_XXHASH);
        ASSERT_OK(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back));
        ASSERT_EQU(back.flags, h.flags);
        ASSERT_MEM_EQ(back.uuid, u, 16);
        ASSERT_EQU(back.start, h.start);
        ASSERT_MEM_EQ(back.compar_name, h.compar_name, ZSI_COMPAR_NAME_LEN);
        ASSERT_EQU(back.valid_upto, h.valid_upto);
        ASSERT_EQU(back.term_off, h.term_off);
        ASSERT_EQU(back.nptrs, h.nptrs);
        ASSERT_EQU(back.term_csum, h.term_csum);
    }

    /* A corrupt byte anywhere before the checksum is rejected. */
    {
        struct zsi_idxhdr back;
        buf[40] = (char)((unsigned char)buf[40] ^ 0x01);
        ASSERT_EQ(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back),
                  ZS_BADCHECKSUM);
        buf[40] = (char)((unsigned char)buf[40] ^ 0x01);
    }

    /* Wrong magic is rejected before anything else, and every byte counts. */
    for (size_t i = 0; i < ZSI_IDX_MAGIC_LEN; i++) {
        struct zsi_idxhdr back;
        buf[i] = (char)((unsigned char)buf[i] ^ 0x01);
        ASSERT_EQ(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back),
                  ZS_BADFORMAT);
        buf[i] = (char)((unsigned char)buf[i] ^ 0x01);
    }

    /* Too short to hold a header. */
    {
        struct zsi_idxhdr back;
        ASSERT_EQ(zsi_idxhdr_decode(buf, ZSI_IDX_HEADER_LEN - 1,
                                    zsi_csum_xxhash, &back), ZS_BADFORMAT);
    }

    /* A read version above ours is refused rather than guessed at (F-7). */
    {
        struct zsi_idxhdr back;
        buf[ZSI_IDX_OFF_VREAD] = (char)(ZSI_IDX_VERSION_READ + 1);
        zsi_put32(buf + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(buf, ZSI_IDX_OFF_CSUM));
        ASSERT_EQ(zsi_idxhdr_decode(buf, sizeof(buf), zsi_csum_xxhash, &back),
                  ZS_BADFORMAT);
    }
}

/* P-3: the published name.  This is interoperability surface -- a peer looks for
 * exactly this string -- so it is asserted against a literal rather than
 * round-tripped through our own parser. */
static void test_idxcache_published_name(void)
{
    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    char name[ZSI_NAME_MAX];
    zsi_uuid_t parsed;
    uint32_t s, e;

    zsi_name_format_index(name, u, 0x0000002A);
    ASSERT_STR_EQ(name,
        "zeroskip.index-4941da54-9406-4faa-a457-c4b65beae3eb-0000002A");

    /* D-2: the zeroskip. prefix is the metadata namespace, so this must never
     * parse as a data file -- even if a cache directory and a database directory
     * were somehow the same. */
    ASSERT_EQ(zsi_name_parse(name, parsed, &s, &e), ZSI_NAME_OTHER);

    /* And it fits the shared name buffer with room to spare. */
    ASSERT(strlen(name) < ZSI_NAME_MAX);
}

/* Counting callback shared by the idxcache tests' direct replay assertions. */
struct idxcache_count { size_t n; };

static int idxcache_count_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct idxcache_count *c = rock;
    (void)rec;
    (void)off;
    c->n++;
    return 0;
}

/* P-9: a seeded build agrees with a build from scratch, key for key and offset
 * for offset. */
static void test_idxcache_matches_full_build(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    size_t *seed = NULL, nseed = 0;
    size_t *want = NULL, nwant = 0;
    size_t *got = NULL, ngot = 0;
    size_t seed_upto;
    char key[32];

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 40; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "first", 5, 0));
    }

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* Snapshot the index as it stands: this is exactly what a table holds. */
    ASSERT_OK(zsi_index_flatten(f->index, db->compar, &seed, &nseed));
    seed_upto = f->complete;
    ASSERT(nseed == 40);

    /* Twenty more keys, ten of them rewrites of existing ones, so the seeded
     * path has to handle both insertion and replacement. */
    for (int i = 30; i < 60; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "second", 6, 0));
    }

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* The answer, from a full replay. */
    ASSERT_OK(zsi_index_build(f, db->compar));
    ASSERT_OK(zsi_index_flatten(f->index, db->compar, &want, &nwant));
    ASSERT_EQU(nwant, 60u);

    /* The same, seeded from the earlier snapshot plus the suffix.  seed's
     * ownership passes to the index. */
    ASSERT_OK(zsi_index_build_from(f, db->compar, seed, nseed, seed_upto));
    seed = NULL;
    ASSERT_OK(zsi_index_flatten(f->index, db->compar, &got, &ngot));

    ASSERT_EQU(ngot, nwant);
    for (size_t i = 0; i < ngot; i++)
        ASSERT_EQU(got[i], want[i]);

    free(got);
    free(want);
    ASSERT_OK(zs_db_close(&db));
}

static char *idxcache_slurp(const char *path, size_t *lenp)
{
    struct stat sb;
    char *buf;
    int fd = open(path, O_RDONLY);

    if (fd < 0) return NULL;
    if (fstat(fd, &sb) < 0) { close(fd); return NULL; }
    buf = malloc((size_t)sb.st_size ? (size_t)sb.st_size : 1);
    if (!buf) { close(fd); return NULL; }
    if (read(fd, buf, (size_t)sb.st_size) != (ssize_t)sb.st_size) {
        free(buf);
        close(fd);
        return NULL;
    }
    close(fd);
    *lenp = (size_t)sb.st_size;
    return buf;
}

static int idxcache_spew(const char *path, const char *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0) return ZS_IOERROR;
    if (write(fd, buf, len) != (ssize_t)len) { close(fd); return ZS_IOERROR; }
    close(fd);
    return ZS_OK;
}

/* Fill `out` with the path of the table for the active file's generation --
 * under the per-uuid subdirectory the open resolved (P-2a) -- and return that
 * generation. */
static uint32_t idxcache_table_path(struct zs_db *db, const char *cachedir,
                                    char *out, size_t outlen)
{
    struct zsi_file *act = zsi_snapshot_active(db->snap);
    char name[ZSI_NAME_MAX], dir[PATH_MAX];

    if (!act) return 0;
    idxcache_dbdir(db, cachedir, dir, sizeof(dir));
    zsi_name_format_index(name, act->hdr.uuid, act->hdr.start);
    XSNPRINTFN(out, outlen, "%s/%s", dir, name);
    return act->hdr.start;
}

/* P-11: every rejection rule, one at a time.
 *
 * Each must yield ZS_NOTFOUND -- "ignore it and replay" -- and never an error,
 * because a bad table in a directory the database does not depend on must not be
 * able to make a readable database look unreadable. */
static void test_idxcache_rejection_rules(void)
{
    char cachedir[PATH_MAX], resolved[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zsi_idxcfg cfg;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    char *tab = NULL;
    size_t tablen = 0;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* The loader takes the RESOLVED per-database directory (P-2a). */
    idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
    cfg.dir = resolved;
    cfg.threshold = 1;
    cfg.local = false;

    for (int i = 0; i < 30; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "value", 5, 0));
    }

    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);
    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    ASSERT(tablen > ZSI_IDX_HEADER_LEN);

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* Baseline: the pristine table loads, and describes every key. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        ASSERT_OK(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc));
        ASSERT_EQU(nbase, 30u);
        ASSERT_EQU(vu, (unsigned long long)f->complete);
        free(base);
    }

    {
        static const struct { const char *what; size_t off; } cases[] = {
            { "magic",        ZSI_IDX_OFF_MAGIC      },
            { "header csum",  ZSI_IDX_OFF_CSUM       },
            { "uuid",         ZSI_IDX_OFF_UUID       },
            { "comparator",   ZSI_IDX_OFF_COMPAR     },
            { "engine",       ZSI_IDX_OFF_FLAGS      },
            { "valid_upto",   ZSI_IDX_OFF_VALID_UPTO },
            { "term_off",     ZSI_IDX_OFF_TERM_OFF   },
            { "nptrs",        ZSI_IDX_OFF_NPTRS      },
            { "term_csum",    ZSI_IDX_OFF_TERM_CSUM  },
            { "first offset", ZSI_IDX_HEADER_LEN     }
        };

        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            size_t *base = NULL, nbase = 0, vu = 0, to = 0;
            uint32_t tc = 0;
            size_t off = cases[i].off;

            tab[off] = (char)((unsigned char)tab[off] ^ 0x01);

            /* Fields the header checksum covers need it recomputed, or every
             * case would only ever exercise the checksum rule. */
            if (off < ZSI_IDX_OFF_CSUM)
                zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                          zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));

            ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
            if (zsi_idx_load(f, &cfg, db->compar_name,
                             &base, &nbase, &vu, &to, &tc) != ZS_NOTFOUND) {
                fprintf(stderr, "\n    FAIL %s: accepted a table it should "
                        "have rejected\n", cases[i].what);
                current_test_failed = 1;
                free(base);
                free(tab);
                zs_db_close(&db);
                return;
            }
            ASSERT_NULL(base);

            tab[off] = (char)((unsigned char)tab[off] ^ 0x01);
            if (off < ZSI_IDX_OFF_CSUM)
                zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                          zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        }
    }

    /* The checksum over the offset array. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        tab[tablen - 1] = (char)((unsigned char)tab[tablen - 1] ^ 0x01);
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);
        tab[tablen - 1] = (char)((unsigned char)tab[tablen - 1] ^ 0x01);
    }

    /* A generation that is not this file's.  Set to a different NONZERO value
     * rather than flipped: flipping the low bit of generation 1 gives 0, which
     * F-9 rejects during the header decode, so the generation rule itself would
     * never run and the case would assert nothing. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        uint32_t was = zsi_get32(tab + ZSI_IDX_OFF_START);

        zsi_put32(tab + ZSI_IDX_OFF_START, was + 7);
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        zsi_put32(tab + ZSI_IDX_OFF_START, was);
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    }

    /* An offset outside [H, valid_upto), with the ARRAY checksum recomputed so
     * the range rule is the only one that can object.  Without recomputing it,
     * a corrupted offset is caught by the checksum and the range rule is never
     * reached -- which is a case that reads as coverage and provides none. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        uint64_t was = zsi_get64(tab + ZSI_IDX_HEADER_LEN);
        size_t arrlen = tablen - ZSI_IDX_HEADER_LEN - 4;

        zsi_put64(tab + ZSI_IDX_HEADER_LEN,
                  zsi_get64(tab + ZSI_IDX_OFF_VALID_UPTO) + 8);
        zsi_put32(tab + tablen - 4,
                  zsi_csum_xxhash(tab + ZSI_IDX_HEADER_LEN, arrlen));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        /* And below the data file's header length. */
        zsi_put64(tab + ZSI_IDX_HEADER_LEN, 8);
        zsi_put32(tab + tablen - 4,
                  zsi_csum_xxhash(tab + ZSI_IDX_HEADER_LEN, arrlen));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        zsi_put64(tab + ZSI_IDX_HEADER_LEN, was);
        zsi_put32(tab + tablen - 4,
                  zsi_csum_xxhash(tab + ZSI_IDX_HEADER_LEN, arrlen));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    }

    /* A comparator name the FILE does not carry, with the handle's matching.
     *
     * Constructed by poking the file's own header field, because F-11 and the
     * agreement check at open make the two names equal in every state the
     * database can reach -- so without this the file-side rule is invisible
     * behind the handle-side one. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        char saved[ZSI_COMPAR_NAME_LEN];

        memcpy(saved, f->hdr.compar_name, sizeof(saved));
        memset(f->hdr.compar_name, 0, sizeof(f->hdr.compar_name));
        memcpy(f->hdr.compar_name, "elsewise", 8);

        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        memcpy(f->hdr.compar_name, saved, sizeof(saved));
    }

    /* Truncated and padded: the size must be exactly 96 + 8n + 4. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;

        ASSERT_OK(idxcache_spew(tabpath, tab, tablen - 1));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        {
            char *padded = malloc(tablen + 8);
            ASSERT_NOT_NULL(padded);
            memcpy(padded, tab, tablen);
            memset(padded + tablen, 0, 8);
            ASSERT_OK(idxcache_spew(tabpath, padded, tablen + 8));
            free(padded);
        }
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    }

    /* A table built without checksum verification must not be handed to a
     * verifying reader, and IS acceptable to a non-verifying one. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        uint16_t fl = zsi_get16(tab + ZSI_IDX_OFF_FLAGS);

        zsi_put16(tab + ZSI_IDX_OFF_FLAGS,
                  (uint16_t)(fl & ~ZSI_IDX_FLAG_CSUM_VERIFIED));
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));

        /* Rejected by EVERY reader, ZS_NOCSUM included: span verification
         * rides indexing (F-5e), so a conforming builder always sets bit 4,
         * and a table without it indexed spans no one verified. */
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        zsi_put16(tab + ZSI_IDX_OFF_FLAGS, fl);
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    }

    /* A comparator name this handle does not implement, even though the file
     * agrees with the table.  Constructed by asking with a different name. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        char other[ZSI_COMPAR_NAME_LEN];

        memset(other, 0, sizeof(other));
        memcpy(other, "notmemcmp", 9);
        ASSERT_EQ(zsi_idx_load(f, &cfg, other,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);
    }

    /* A missing table is ZS_NOTFOUND, not an error, and no cache directory at
     * all is the same. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        struct zsi_idxcfg off = { NULL, 0, false };

        ASSERT_EQ(zsi_idx_load(f, &off, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        ASSERT_EQ(unlink(tabpath), 0);
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);
        ASSERT_EQU(vu, (unsigned long long)ZSI_HEADER_LEN);
    }

    free(tab);
    ASSERT_OK(zs_db_close(&db));
}

/* P-10, P-17: the terminator binding.  A table whose recorded terminator is not
 * the one at valid_upto in the file it is being applied to is rejected, which is
 * what catches a database directory restored from backup under a surviving cache
 * directory. */
static void test_idxcache_rejects_bad_term_binding(void)
{
    char cachedir[PATH_MAX], resolved[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zsi_idxcfg cfg;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    char *tab;
    size_t tablen;
    size_t *base = NULL, nbase = 0, vu = 0, to = 0;
    size_t first_term_off;
    uint32_t tc = 0, first_term_csum;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* The loader takes the RESOLVED per-database directory (P-2a).  Pointing
     * it at the root turned every load below into ZS_NOTFOUND for the wrong
     * reason and the whole test vacuous -- found by the full mutation run,
     * where the offset-binding mutant escaped it.  The positive baseline
     * load below is the guard against that ever happening silently again. */
    idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
    cfg.dir = resolved;
    cfg.threshold = 1;
    cfg.local = false;

    /* One record first, so an EARLIER span's terminator is on record.  It is a
     * perfectly valid terminator at a lower offset, which is the only way to
     * reach the "ends exactly at valid_upto" rule: any other term_off either
     * fails to decode or trips the range check first. */
    ASSERT_OK(zs_db_store(db, "key00", 5, "value", 5, 0));
    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    first_term_off  = f->last_term_off;
    first_term_csum = f->last_term_csum;

    for (int i = 1; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%02d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "value", 5, 0));
    }

    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);
    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    ASSERT(first_term_off < f->last_term_off);

    /* The recorded checksum is genuinely the terminator's, and the recorded
     * offset genuinely ends at valid_upto -- so the baseline is meaningful. */
    {
        struct zsi_term term;
        const char *tb;
        size_t rec_off = (size_t)zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF);
        size_t rec_vu  = (size_t)zsi_get64(tab + ZSI_IDX_OFF_VALID_UPTO);

        tb = zsi_file_at(f, rec_off, 1);
        ASSERT_NOT_NULL(tb);
        ASSERT_OK(zsi_term_decode(tb, f->size - rec_off, &term));
        ASSERT_EQU(rec_off + term.len, rec_vu);
        ASSERT_EQU(term.csum, zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM));
    }

    /* The PRISTINE table loads.  Without this baseline, a load that fails for
     * an unrelated reason -- a wrong directory, say -- makes every rejection
     * below pass without testing anything. */
    ASSERT_OK(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc));
    ASSERT_EQU(nbase, 10u);
    free(base);
    base = NULL;

    /* A term_csum that is not the terminator's. */
    zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM,
              zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM) ^ 0xFFFFFFFFu);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);
    zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM,
              zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM) ^ 0xFFFFFFFFu);

    /* A term_off naming an EARLIER span's terminator, with its real checksum.
     * Everything about it is genuine except that it does not end at valid_upto,
     * so only that rule can object -- which is exactly the stale table a
     * publisher recording the wrong boundary would leave behind. */
    {
        uint64_t was_off = zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF);
        uint32_t was_csum = zsi_get32(tab + ZSI_IDX_OFF_TERM_CSUM);

        zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF, (uint64_t)first_term_off);
        zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM, first_term_csum);
        zsi_put32(tab + ZSI_IDX_OFF_CSUM,
                  zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
        ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
        ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
        ASSERT_NULL(base);

        zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF, was_off);
        zsi_put32(tab + ZSI_IDX_OFF_TERM_CSUM, was_csum);
    }

    /* A term_off that is not a terminator at all. */
    zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF,
              zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF) - 8);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);
    zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF,
              zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF) + 8);

    /* term_off at or beyond valid_upto. */
    zsi_put64(tab + ZSI_IDX_OFF_TERM_OFF,
              zsi_get64(tab + ZSI_IDX_OFF_VALID_UPTO));
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);

    /* valid_upto past the end of the data file. */
    zsi_put64(tab + ZSI_IDX_OFF_VALID_UPTO, (uint64_t)f->size + 4096);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);

    /* And below the data file's header length. */
    zsi_put64(tab + ZSI_IDX_OFF_VALID_UPTO, 3);
    zsi_put32(tab + ZSI_IDX_OFF_CSUM, zsi_csum_xxhash(tab, ZSI_IDX_OFF_CSUM));
    ASSERT_OK(idxcache_spew(tabpath, tab, tablen));
    ASSERT_EQ(zsi_idx_load(f, &cfg, db->compar_name,
                           &base, &nbase, &vu, &to, &tc), ZS_NOTFOUND);
    ASSERT_NULL(base);

    free(tab);
    ASSERT_OK(zs_db_close(&db));
}

/* P-12 into D-13b: a table-seeded reader folds its replayed SUFFIX, and the fold
 * takes a sorted run -- but a replay collects in OFFSET order, so this is the one
 * caller that has to sort before folding.
 *
 * The commit site gets its order for free from the pending skiplist, so nothing
 * else in the suite can tell a missing sort here from a present one.  Making it
 * visible needs three things at once: a table that really loads (asserted through
 * cached_upto, or this test silently exercises the plain build), records committed
 * AFTER it by a handle with no cache configured, so they are left as a suffix
 * rather than republished, and those records arriving in DESCENDING key order,
 * which is what an offset-ordered run looks like when it is not sorted.
 *
 * The suffix also rewrites a key the table describes and deletes another, so the
 * run has to win against the seeded base and not merely sit above it. */
static void test_idxcache_seeded_suffix_folds_in_order(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    char got[4096], key[32], val[32];

    idxcache_mkdir(cachedir, sizeof(cachedir));

    /* A table over keys 00..19, published because the threshold is 1. */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        snprintf(key, sizeof(key), "k%02d", i);
        snprintf(val, sizeof(val), "a%02d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, strlen(val), 0));
    }
    ASSERT_OK(zs_db_close(&db));

    /* The suffix, through a handle with NO cache: descending keys, one rewrite of
     * a key the table holds, one deletion of another. */
    setup.flags = ZS_NOAUTOREPACK;
    setup.index_dir = NULL;
    setup.index_threshold = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    {
        struct zs_txn *txn = NULL;
        ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
        for (int i = 39; i >= 20; i--) {
            snprintf(key, sizeof(key), "k%02d", i);
            snprintf(val, sizeof(val), "b%02d", i);
            ASSERT_OK(zs_txn_store(txn, key, strlen(key), val, strlen(val), 0));
        }
        ASSERT_OK(zs_txn_store(txn, "k05", 3, "B05", 3, 0));
        ASSERT_OK(zs_txn_store(txn, "k10", 3, NULL, 0, 0));
        ASSERT_OK(zs_txn_commit(&txn));
    }
    ASSERT_OK(zs_db_close(&db));

    /* And a reader that seeds from the table and replays the suffix. */
    setup.flags = ZS_NOAUTOREPACK;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    /* The table was used: an unseeded build leaves cached_upto at the header. */
    ASSERT(f->cached_upto > ZSI_HEADER_LEN);

    api_scan(db, got, sizeof(got));
    {
        char want[4096];
        size_t used = 0;
        want[0] = '\0';
        for (int i = 0; i < 40; i++) {
            if (i == 10) continue;                  /* deleted */
            if (used) want[used++] = '|';
            used += (size_t)snprintf(want + used, 32, "k%02d=%c%02d", i,
                                     i == 5 ? 'B' : i < 20 ? 'a' : 'b', i);
        }
        ASSERT_STR_EQ(got, want);
    }

    ASSERT_OK(zs_db_close(&db));
}

/* P-13: nothing published below the threshold, something at it. */
static void test_idxcache_threshold(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat sb;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1024 * 1024;   /* far above anything written here */

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "value", 5, 0));
    }
    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);
    ASSERT_EQ(stat(tabpath, &sb), -1);
    ASSERT_OK(zs_db_close(&db));

    /* Same database, threshold of one byte: a table appears. */
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "another", 7, "value", 5, 0));
    ASSERT_EQ(stat(tabpath, &sb), 0);
    ASSERT(sb.st_size > ZSI_IDX_HEADER_LEN);
    ASSERT_OK(zs_db_close(&db));
}

/* A-9: the DEFAULT threshold is a fraction of the file it describes, and both ends
 * of that matter for a different reason.
 *
 * A small file must publish often, because every open replays from the last
 * published point and most databases are small.  A large one must not publish
 * often, because each publication rewrites the whole table: hold the byte gap
 * fixed and the total cost is quadratic in the file's size, which is how a 64MB
 * generation came to write 4.7GB of tables on a 2M-record load.
 *
 * Both halves are asserted against the rule rather than against a constant, and
 * each is the other's counter-example: an absolute threshold passes the first and
 * fails the second, a fraction of rollover_size passes the second and fails the
 * first. */
static void test_idxcache_threshold_scales_with_the_file(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    char key[32], val[512];
    size_t before, grew;
    int i;

    memset(val, 'v', sizeof(val));
    idxcache_mkdir(cachedir, sizeof(cachedir));

    /* rollover far above anything written, so this is all ONE unordered file and
     * rollover_size cannot be what the threshold tracks. */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.index_dir = cachedir;
    setup.rollover_size = 512 * 1024 * 1024;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* SMALL: the floor applies, so a few KB of growth is enough to publish. If the
     * threshold tracked rollover_size this would be 8MB away from publishing and
     * every open would replay the whole file. */
    for (i = 0; i < 40; i++) {
        snprintf(key, sizeof(key), "s%05d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
    }
    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    ASSERT(f->complete > 16u * 1024u);          /* it really did grow */
    ASSERT(f->cached_upto > ZSI_HEADER_LEN);    /* ... and it published */
    ASSERT(f->complete - f->cached_upto < 8u * 1024u);

    /* The FLOOR, which is the other half of "small": a fraction of a small file is
     * a few hundred bytes, so without a floor this publishes on nearly every
     * commit -- the churn P-13 exists to bound.  One 512-byte record must not be
     * enough to publish when the last one was this recent. */
    before = f->cached_upto;
    snprintf(key, sizeof(key), "s%05d", i + 1);
    ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    ASSERT(f->complete - before < 4u * 1024u);  /* the gap really is under the floor */
    ASSERT_EQU(f->cached_upto, (unsigned long long)before);

    /* LARGE: grow the same file past 4MB, then find the next publication. The gap
     * it waits for has to exceed the old absolute 32KB, or nothing has changed. */
    for (i = 0; f->complete < 4u * 1024u * 1024u && i < 200000; i++) {
        snprintf(key, sizeof(key), "L%08d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
        f = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(f);
    }
    ASSERT(f->complete >= 4u * 1024u * 1024u);

    before = f->cached_upto;
    for (i = 0; i < 200000; i++) {
        snprintf(key, sizeof(key), "M%08d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, sizeof(val), 0));
        f = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(f);
        if (f->cached_upto != before) break;
    }
    grew = f->cached_upto - before;

    /* It waited for more than the 32KB an absolute threshold would have used, and
     * for no more than the rule allows plus the record that crossed it. */
    ASSERT(grew > 32u * 1024u);
    ASSERT(grew < f->complete / 64u + 4096u);

    ASSERT_OK(zs_db_close(&db));
}

/* P-9, P-12: a cached open and an uncached open agree on every key.  This is the
 * test the whole feature exists for. */
static void test_idxcache_open_agrees(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data cached = ZS_OPEN_DATA_INITIALIZER;
    struct zs_open_data plain = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char key[32];

    idxcache_mkdir(cachedir, sizeof(cachedir));

    cached.flags = ZS_CREATE;
    cached.index_dir = cachedir;
    cached.index_threshold = 1;
    plain.flags = ZS_CREATE;

    /* Written in two halves with a close in between, so the second open loads a
     * table and replays only the suffix. */
    ASSERT_OK(zs_db_open(dbdir, &cached, &db));
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "first", 5, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    ASSERT_OK(zs_db_open(dbdir, &cached, &db));
    {
        /* The open really did use a table rather than replaying from the top.
         *
         * An empty DELTA is the sharp part: the table covers the whole file, so
         * P-12's replay from valid_upto has nothing to find.  A build that
         * seeded from the table and then replayed from the header anyway would
         * produce an identical ordering -- the delta wins ties, so every answer
         * is still right -- and would silently do the work the table exists to
         * avoid.  Only the delta shows it. */
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(act);
        ASSERT(act->cached_upto > ZSI_HEADER_LEN);
        ASSERT_EQU(act->cached_upto, (unsigned long long)act->complete);
        ASSERT_NOT_NULL(act->index);
        ASSERT_EQU(act->index->ndelta, 0u);
        ASSERT_EQU(act->index->nbase, 50u);
    }
    for (int i = 25; i < 75; i++) {
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "second", 6, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    for (int pass = 0; pass < 2; pass++) {
        ASSERT_OK(zs_db_open(dbdir, pass ? &plain : &cached, &db));
        for (int i = 0; i < 75; i++) {
            const char *v = NULL;
            size_t vl = 0;
            snprintf(key, sizeof(key), "key%03d", i);
            ASSERT_OK(zs_db_fetch(db, key, strlen(key), NULL, NULL, &v, &vl, 0));
            if (i < 25) { ASSERT_EQU(vl, 5u); ASSERT_MEM_EQ(v, "first", 5); }
            else        { ASSERT_EQU(vl, 6u); ASSERT_MEM_EQ(v, "second", 6); }
        }
        ASSERT_OK(zs_db_close(&db));
    }
}

/* P-4: published by rename, never written in place.  Checked by inode, because
 * writing in place would expose a half-written table to a concurrent reader --
 * which no amount of checksumming makes acceptable, since it is G-6's rule. */
static void test_idxcache_publishes_by_rename(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat a, b;
    DIR *d;
    struct dirent *de;
    int staging = 0;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);
    ASSERT_EQ(stat(tabpath, &a), 0);

    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_EQ(stat(tabpath, &b), 0);
    ASSERT(a.st_ino != b.st_ino);

    /* And no staging file survives a successful publish. */
    d = opendir(cachedir);
    ASSERT_NOT_NULL(d);
    while ((de = readdir(d)))
        if (!strncmp(de->d_name, ZSI_STAGING_PREFIX, strlen(ZSI_STAGING_PREFIX)))
            staging++;
    closedir(d);
    ASSERT_EQ(staging, 0);

    ASSERT_OK(zs_db_close(&db));
}

/* P-15: a cache directory that cannot be written to must not fail a commit, and
 * must not stop the database working. */
static void test_idxcache_publish_failure_is_not_fatal(void)
{
    char cachedir[PATH_MAX], p[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    const char *v = NULL;
    size_t vl = 0;
    DIR *d;
    struct dirent *de;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));

    /* Take the cache directory away underneath the open handle -- the
     * per-uuid child the open resolved (P-2a) first, then the root. */
    {
        char resolved[PATH_MAX];
        idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
        d = opendir(resolved);
        ASSERT_NOT_NULL(d);
        while ((de = readdir(d))) {
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
            XSNPRINTF(p, "%s/%s", resolved, de->d_name);
            unlink(p);
        }
        closedir(d);
        ASSERT_EQ(rmdir(resolved), 0);
    }
    ASSERT_EQ(rmdir(cachedir), 0);

    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_MEM_EQ(v, "2", 1);
    ASSERT_OK(zs_db_close(&db));

    /* And a handle opened against a cache directory that does not exist works
     * exactly as one with no cache at all. */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_db_close(&db));
}

/* P-1: only unordered files get a table.  An in-order file has a pointer section
 * of its own, and after a repack nothing but the active file is unordered. */
static void test_idxcache_only_unordered_files(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    DIR *d;
    struct dirent *de;
    int tables = 0;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    setup.rollover_size = 512;          /* force rollovers and conversions */

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 60; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "0123456789", 10, 0));
    }
    ASSERT_OK(zs_db_repack(db));

    /* Every surviving table must name a generation that is still an unordered
     * file, and after a repack that is at most the active one. */
    {
        char resolved[PATH_MAX];
        idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
        d = opendir(resolved);
        ASSERT_NOT_NULL(d);
        while ((de = readdir(d)))
            if (!strncmp(de->d_name, ZSI_IDX_NAME_PREFIX,
                         strlen(ZSI_IDX_NAME_PREFIX)))
                tables++;
        closedir(d);
    }

    ASSERT(tables <= 1);

    /* And publishing is refused for an in-order file outright.  Called directly,
     * because the two real call sites only ever reach unordered files -- so
     * without this the rule is unreachable and untested, which is the same thing
     * as absent the day someone adds a third caller.
     *
     * Threshold ZERO deliberately.  An in-order file has complete == 0, so any
     * nonzero threshold refuses it for a reason that has nothing to do with its
     * kind, and the assertion would hold whatever P-1 said. */
    {
        char resolved[PATH_MAX];
        idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
        struct zsi_idxcfg cfg = { resolved, 0, false };
        struct zsi_file *inorder = NULL;

        for (size_t i = 0; i < db->snap->nfiles; i++)
            if (!zsi_file_is_unordered(db->snap->files[i])) {
                inorder = db->snap->files[i];
                break;
            }

        ASSERT_NOT_NULL(inorder);
        ASSERT_EQ(zsi_idx_publish(inorder, &cfg, db->compar), ZS_DONE);

        /* And nothing appeared under its start generation. */
        {
            char name[ZSI_NAME_MAX], p[PATH_MAX];
            struct stat sb;
            zsi_name_format_index(name, inorder->hdr.uuid, inorder->hdr.start);
            XSNPRINTF(p, "%s/%s", resolved, name);
            ASSERT_EQ(stat(p, &sb), -1);
        }
    }

    /* And the data is intact. */
    for (int i = 0; i < 60; i++) {
        char key[32];
        const char *v = NULL;
        size_t vl = 0;
        snprintf(key, sizeof(key), "key%03d", i);
        ASSERT_OK(zs_db_fetch(db, key, strlen(key), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 10u);
    }

    ASSERT_OK(zs_db_close(&db));
}

/* P-16: a table whose generation is no longer an unordered file is unlinked; one
 * whose generation is still live, and one belonging to another database sharing
 * the directory, are both kept. */
static void test_idxcache_sweeps_dead_generations(void)
{
    char cachedir[PATH_MAX], live[PATH_MAX], dead[PATH_MAX], other[PATH_MAX];
    char name[ZSI_NAME_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct stat sb;
    zsi_uuid_t alien;
    uint32_t gen;
    char junk[ZSI_IDX_HEADER_LEN + 4];

    idxcache_mkdir(cachedir, sizeof(cachedir));
    memset(junk, 0, sizeof(junk));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = idxcache_table_path(db, cachedir, live, sizeof(live));
    ASSERT(gen != 0);
    ASSERT_EQ(stat(live, &sb), 0);

    char resolved[PATH_MAX];
    idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));

    /* A table for a generation that has never existed.  Its contents do not
     * matter: the sweep works on names. */
    zsi_name_format_index(name, db->uuid, gen + 100);
    XSNPRINTF(dead, "%s/%s", resolved, name);
    ASSERT_OK(idxcache_spew(dead, junk, sizeof(junk)));

    /* And one for a different database, planted in OUR resolved directory at
     * a generation that is NOT live here.  P-2a makes sharing structural, but
     * P-16's uuid rule still holds for whatever lands in the directory -- and
     * a live generation would be kept for the wrong reason, the liveness test
     * rather than the uuid test, which leaves the uuid rule unexercised. */
    memcpy(alien, db->uuid, 16);
    alien[0] = (unsigned char)(alien[0] ^ 0xFF);
    zsi_name_format_index(name, alien, gen + 100);
    XSNPRINTF(other, "%s/%s", resolved, name);
    ASSERT_OK(idxcache_spew(other, junk, sizeof(junk)));

    /* The sweep runs with a snapshot rebuild (P-16), and a steady-state
     * store no longer forces one -- C-4i's probe lets a sole writer's begin
     * reuse its snapshot -- so trigger the rebuild honestly: reopen, which is
     * the C-4 protocol from scratch (R-1). */
    ASSERT_OK(zs_db_close(&db));
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    ASSERT_EQ(stat(dead, &sb), -1);      /* dead generation gone */
    ASSERT_EQ(stat(other, &sb), 0);      /* another database untouched */
    ASSERT_EQ(stat(live, &sb), 0);       /* our own live generation kept */

    ASSERT_OK(zs_db_close(&db));
}

/* A-8a/P-2b: the flag creates zeroskip.cache inside the database and
 * publishes into it, and a reopen seeds from it. */
static void test_index_local_publishes(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cache[PATH_MAX];
    DIR *d;
    struct dirent *de;
    int ntables = 0;

    setup.flags = ZS_CREATE | ZS_INDEX_LOCAL;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

    /* Tables live DIRECTLY in zeroskip.cache -- one database per directory,
     * so P-2a's uuid level would be redundant (P-2b). */
    snprintf(cache, sizeof(cache), "%s/%s", dbdir, ZSI_CACHE_DIR_NAME);
    d = opendir(cache);
    ASSERT_NOT_NULL(d);
    while ((de = readdir(d)))
        if (!strncmp(de->d_name, ZSI_IDX_NAME_PREFIX,
                     strlen(ZSI_IDX_NAME_PREFIX))) ntables++;
    closedir(d);
    ASSERT_EQ(ntables, 1);
    ASSERT_OK(zs_db_close(&db));

    /* A reopen with the flag seeds from the published table. */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(act);
        ASSERT(act->cached_upto > ZSI_HEADER_LEN);
    }
    ASSERT_OK(zs_db_close(&db));

    /* And the database itself is untouched by the extra directory: a handle
     * WITHOUT the flag reads it as a plain file set. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NULL(db->index_dir);
    {
        const char *v = NULL;
        size_t vl = 0;
        ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* P-2b/R-3: a read-only open never creates the directory -- creating anything
 * inside the database is a visible side effect -- but uses one if present. */
static void test_index_local_readonly_creates_nothing(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cache[PATH_MAX];
    struct stat sb;

    /* Create the database with NO cache. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    /* Read-only with the flag: works, creates nothing, cache disabled. */
    setup.flags = ZS_SHARED | ZS_INDEX_LOCAL;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NULL(db->index_dir);
    {
        const char *v = NULL;
        size_t vl = 0;
        ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    snprintf(cache, sizeof(cache), "%s/%s", dbdir, ZSI_CACHE_DIR_NAME);
    ASSERT_EQ(stat(cache, &sb), -1);

    /* A writable handle creates it; a read-only one then USES it. */
    setup.flags = ZS_INDEX_LOCAL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_close(&db));
    ASSERT_EQ(stat(cache, &sb), 0);

    setup.flags = ZS_SHARED | ZS_INDEX_LOCAL;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NOT_NULL(db->index_dir);
    ASSERT_OK(zs_db_close(&db));
}

/* A-8a: naming both locations is ambiguous, not a preference order. */
static void test_index_local_and_dir_is_badusage(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cachedir[PATH_MAX];

    idxcache_mkdir(cachedir, sizeof(cachedir));
    setup.flags = ZS_CREATE | ZS_INDEX_LOCAL;
    setup.index_dir = cachedir;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADUSAGE);
    ASSERT_NULL(db);
}

/* P-2b: inside zeroskip.cache a foreign-uuid table is garbage by construction
 * and is swept; under a shared root the P-16 uuid rule still protects it
 * (test_idxcache_sweeps_dead_generations holds that side). */
static void test_index_local_sweeps_foreign_uuid(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cache[PATH_MAX], foreign[PATH_MAX], name[ZSI_NAME_MAX];
    char junk[ZSI_IDX_HEADER_LEN + 4];
    zsi_uuid_t alien;
    struct stat sb;

    memset(junk, 0, sizeof(junk));
    setup.flags = ZS_CREATE | ZS_INDEX_LOCAL;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));

    /* Plant a well-formed table name carrying another database's uuid, at a
     * generation that is not live here. */
    memcpy(alien, db->uuid, 16);
    alien[0] = (unsigned char)(alien[0] ^ 0xFF);
    zsi_name_format_index(name, alien, 101);
    snprintf(cache, sizeof(cache), "%s/%s", dbdir, ZSI_CACHE_DIR_NAME);
    XSNPRINTF(foreign, "%s/%s", cache, name);
    ASSERT_OK(idxcache_spew(foreign, junk, sizeof(junk)));

    /* The sweep runs with a snapshot rebuild (P-16), which a steady-state
     * store no longer forces (C-4i) -- reopen to trigger one honestly. */
    ASSERT_OK(zs_db_close(&db));
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(stat(foreign, &sb), -1);

    ASSERT_OK(zs_db_close(&db));
}

/* T-0/P-11: a case shipping a pointer table must have it LOADED, not merely
 * present.
 *
 * The corpus exists to prove a format is shared.  A validator that writes its own
 * table and compares nothing has proved only that it agrees with itself, which is
 * exactly the failure T-12a exists to prevent for data files -- so the shipped
 * bytes have to be the ones under test.
 *
 * The case is COPIED to scratch before being opened, and the checked-in bytes
 * are never the ones handed to a live handle.  That is not belt and braces: a
 * cache directory is one this library UNLINKS from (P-16), so pointing one at
 * the corpus would put the golden bytes within reach of any bug in the sweep --
 * and a deleted corpus file reads as a corpus that needs regenerating rather
 * than as the bug it is. */
static void test_corpus_index_table(void)
{
    char cases[32][64];
    size_t ncases = corpus_cases(cases, 32);
    size_t checked = 0;

    if (!ncases) SKIP("no corpus (run make corpus)");

    for (size_t i = 0; i < ncases; i++) {
        char src[PATH_MAX], dir[PATH_MAX], txtpath[PATH_MAX], idxdir[PATH_MAX];
        char cmd[PATH_MAX * 2 + 32];
        struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *db = NULL;
        struct zsi_file *f;
        struct zsi_idxcfg cfg;
        char *txt, *line, sub[64];
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;

        XSNPRINTF(src, CORPUS_DIR "/%s", cases[i]);
        XSNPRINTF(txtpath, "%s/case.txt", src);

        txt = slurp(txtpath, NULL);
        if (!txt) continue;

        line = strstr(txt, "\nindexdir ");
        if (!line) { free(txt); continue; }
        if (sscanf(line + 10, "%63s", sub) != 1) { free(txt); continue; }
        free(txt);

        snprintf(dir, sizeof(dir), "%s/corpuscase", basedir);
        XSNPRINTF(cmd, "rm -rf '%s' && cp -R '%s' '%s'", dir, src, dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "\n    FAIL %s: could not copy the case\n",
                    cases[i]);
            current_test_failed = 1;
            return;
        }

        XSNPRINTF(idxdir, "%s/%s", dir, sub);
        cfg.threshold = (size_t)1 << 40;
        cfg.local = false;

        setup.flags = ZS_SHARED;
        setup.index_dir = idxdir;
        setup.index_threshold = cfg.threshold;
        ASSERT_OK(zs_db_open(dir, &setup, &db));

        /* The case ships its table under the per-uuid level open resolves
         * (P-2a); the direct loads below need the same directory. */
        char resolved[PATH_MAX];
        idxcache_dbdir(db, idxdir, resolved, sizeof(resolved));
        cfg.dir = resolved;

        f = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(f);

        /* The shipped table is accepted, and describes the file it ships with. */
        ASSERT_OK(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc));
        ASSERT_EQU(vu, (unsigned long long)f->complete);
        ASSERT(nbase > 0);

        /* And the index the open built from it agrees with a full replay, which
         * is what makes "accepted" mean something. */
        {
            size_t *cached = NULL, ncached = 0;
            size_t *fresh = NULL, nfresh = 0;

            ASSERT(f->cached_upto > ZSI_HEADER_LEN);   /* it really was used */
            ASSERT_OK(zsi_index_flatten(f->index, db->compar, &cached, &ncached));
            ASSERT_EQU(ncached, nbase);

            ASSERT_OK(zsi_index_build(f, db->compar));
            ASSERT_OK(zsi_index_flatten(f->index, db->compar, &fresh, &nfresh));
            ASSERT_EQU(nfresh, ncached);
            for (size_t j = 0; j < nfresh; j++) {
                ASSERT_EQU(cached[j], fresh[j]);
                ASSERT_EQU(base[j], fresh[j]);
            }

            free(cached);
            free(fresh);
        }

        free(base);
        ASSERT_OK(zs_db_close(&db));
        checked++;
    }

    /* A corpus with no such case would make this test read as coverage while
     * exercising nothing. */
    ASSERT(checked > 0);
}

/* P-7: the table records the engine the DATA FILE names, not the one this handle
 * would choose for a file it creates.
 *
 * Built by two opens with different engine defaults, which is the arrangement
 * that caught the equivalent bug for appending: the second handle's default is
 * XXHASH, the file says engine 0, and the table must follow the file.  A table
 * checksummed under the handle's engine validates for nobody, so every reader
 * silently rejects it and the cache does nothing while appearing to work. */
static void test_idxcache_uses_file_engine(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    struct zsi_idxcfg cfg;
    char *tab;
    size_t tablen;

    char resolved[PATH_MAX];

    idxcache_mkdir(cachedir, sizeof(cachedir));

    /* Create under engine 0. */
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    /* Reopen with the DEFAULT engine -- XXHASH -- and append. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(db->create_csum_id, ZSI_CSUM_XXHASH);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));

    idxcache_dbdir(db, cachedir, resolved, sizeof(resolved));
    cfg.dir = resolved;
    cfg.threshold = 1;
    cfg.local = false;

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_NONE);
    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);

    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    ASSERT_EQ(zsi_idxhdr_engine_id(tab), ZSI_CSUM_NONE);
    free(tab);

    /* And it loads, which it would not if it had been written under the
     * handle's engine: the engine check compares against the file's. */
    {
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;
        ASSERT_OK(zsi_idx_load(f, &cfg, db->compar_name,
                               &base, &nbase, &vu, &to, &tc));
        ASSERT_EQU(nbase, 2u);
        free(base);
    }

    ASSERT_OK(zs_db_close(&db));

    /* The data survives a cached reopen under either engine default. */
    {
        const char *v = NULL;
        size_t vl = 0;
        ASSERT_OK(zs_db_open(dbdir, &setup, &db));
        ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 1u);
        ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 1u);
        ASSERT_OK(zs_db_close(&db));
    }
}

/* P-8: valid_upto is always a span boundary, and term_off names the terminator
 * that ends there.  Checked against an independent walk of the file rather than
 * against the value the publisher happened to store.  P-12: a replay may begin
 * at any span boundary, one begun at the complete point finds nothing, and a
 * bogus offset falls back to the header rather than being trusted -- the
 * properties that make a table's resume offset safe to hand to the replay. */
static void test_idxcache_valid_upto_is_span_boundary(void)
{
    char cachedir[PATH_MAX], tabpath[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *f;
    struct idxcache_count c;
    char *tab;
    size_t tablen;
    uint64_t vu, to;

    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 10; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%02d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "v", 1, 0));
    }

    ASSERT(idxcache_table_path(db, cachedir, tabpath, sizeof(tabpath)) != 0);
    tab = idxcache_slurp(tabpath, &tablen);
    ASSERT_NOT_NULL(tab);
    vu = zsi_get64(tab + ZSI_IDX_OFF_VALID_UPTO);
    to = zsi_get64(tab + ZSI_IDX_OFF_TERM_OFF);
    free(tab);

    f = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(f);

    /* An independent walk from the top agrees on where the last span ends. */
    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, ZSI_HEADER_LEN,
                                   idxcache_count_cb, &c));
    ASSERT_EQU(c.n, 10u);
    ASSERT_EQU(vu, (unsigned long long)f->complete);

    /* A walk starting there finds nothing further, which is what "boundary"
     * means, and leaves the complete point where it was. */
    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, (size_t)vu,
                                   idxcache_count_cb, &c));
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, vu);

    /* And term_off names a terminator that ends exactly at valid_upto. */
    {
        struct zsi_term term;
        const char *tb = zsi_file_at(f, (size_t)to, 1);
        ASSERT_NOT_NULL(tb);
        ASSERT_OK(zsi_term_decode(tb, f->size - (size_t)to, &term));
        ASSERT_EQU(to + term.len, vu);
    }

    /* A BOGUS resume offset -- and a table is exactly a file offering one --
     * must fall back to the header rather than being trusted: past the end,
     * below the header, and mid-span (a non-boundary, which F-24 treats as
     * content that fails to validate and stops the scan cold). */
    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, f->size + 8,
                                   idxcache_count_cb, &c));
    ASSERT_EQU(c.n, 10u);                      /* fell back to the header */

    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, 3, idxcache_count_cb, &c));
    ASSERT_EQU(c.n, 10u);                      /* fell back to the header */

    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, (size_t)vu - 4,
                                   idxcache_count_cb, &c));
    ASSERT_EQU(c.n, 0u);                       /* not a boundary: no records */

    ASSERT_OK(zs_db_close(&db));
}

/*
 * ============================================================
 * Seal and compact (D-25 .. D-29)
 * ============================================================
 */

/* D-25, D-25a: the active generation becomes an in-order file over the same
 * range, and no new generation appears. */
static void test_seal_converts_the_active_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    uint32_t gen;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "value", 5, 0));
    }

    gen = zsi_snapshot_active(db->snap)->hdr.start;
    ASSERT_OK(zs_db_seal(db));

    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, gen);
    ASSERT_EQU(db->snap->files[0]->hdr.end, gen);

    for (int i = 0; i < 20; i++) {
        char k[32];
        const char *v = NULL;
        size_t vl = 0;
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 5u);
    }
    ASSERT_OK(zs_db_check_consistency(db));

    /* The next write starts a fresh generation rather than reusing one. */
    ASSERT_OK(zs_db_store(db, "after", 5, "x", 1, 0));
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(zsi_snapshot_active(db->snap)->hdr.start, gen + 1);

    ASSERT_OK(zs_db_close(&db));
}

/* D-25a: repeated sealing must not consume generations.  This is the assertion
 * that separates converting in place from rolling over and then converting --
 * both reach one in-order file, only one of them burns a generation each time,
 * and generations are finite (D-9c). */
static void test_seal_creates_no_new_generation(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    uint32_t gen;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    gen = zsi_snapshot_active(db->snap)->hdr.start;

    for (int i = 0; i < 5; i++) ASSERT_OK(zs_db_seal(db));

    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_EQU(db->snap->files[0]->hdr.start, gen);
    ASSERT_EQU(db->snap->files[0]->hdr.end, gen);
    ASSERT_OK(zs_db_close(&db));
}

/* D-25d: a commit that grows the active file past rollover_size seals it in
 * the same commit, so a one-transaction bulk load -- cvt_cyrusdb's shape, a
 * single span the rollover check cannot split -- ends with an in-order file
 * rather than an oversized unordered one whose conversion the next writer
 * would have to pay for. */
static void test_commit_seals_oversized_active(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    char pad[512];

    memset(pad, 'x', sizeof(pad));
    setup.flags = ZS_CREATE;
    setup.rollover_size = 4096;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int i = 0; i < 32; i++) {
        char k[16];
        snprintf(k, sizeof(k), "key%04d", i);
        ASSERT_OK(zs_txn_store(txn, k, strlen(k), pad, sizeof(pad), 0));
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* Sealed in place: the only file is in-order 1-1, there is no active
     * file, and no generation was consumed (D-25a via D-25d). */
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, 1u);
    ASSERT_EQU(db->snap->files[0]->hdr.end, 1u);

    const char *v = NULL;
    size_t vl = 0;
    ASSERT_OK(zs_db_fetch(db, "key0007", 7, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(pad));
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-25d's gate: a commit below rollover_size must NOT seal, or every commit
 * would pay a conversion and the active file could never grow. */
static void test_commit_below_rollover_stays_unordered(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 65536;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));

    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_OK(zs_db_close(&db));
}

/* D-25e: the sealing commit publishes no pointer table for the file it
 * seals.  A table covers only unordered files (P-1), so it would be born
 * stale and merely wait for a sweep. */
static void test_seal_at_commit_skips_table_publish(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    char cachedir[PATH_MAX];
    char pad[512];

    memset(pad, 'x', sizeof(pad));
    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE;
    setup.rollover_size = 4096;
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    for (int i = 0; i < 32; i++) {
        char k[16];
        snprintf(k, sizeof(k), "key%04d", i);
        ASSERT_OK(zs_txn_store(txn, k, strlen(k), pad, sizeof(pad), 0));
    }
    ASSERT_OK(zs_txn_commit(&txn));

    /* The commit sealed, and left no table behind. */
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    {
        DIR *d = opendir(cachedir);
        struct dirent *de;
        int ntables = 0;

        ASSERT_NOT_NULL(d);
        while ((de = readdir(d)) != NULL)
            if (!strncmp(de->d_name, ZSI_IDX_NAME_PREFIX,
                         strlen(ZSI_IDX_NAME_PREFIX))) ntables++;
        closedir(d);
        ASSERT_EQ(ntables, 0);
    }

    ASSERT_OK(zs_db_close(&db));
}

/* D-25b, A-10: the no-op cases, each ZS_OK and each leaving the set alone. */
static void test_seal_noop_cases(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* A brand-new database: an active file with a header and no spans.  Sealing
     * it would write an empty in-order file and consume a generation. */
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_seal(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));

    /* Already sealed: no active file at all. */
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_EQU(db->snap->nfiles, 1u);

    /* And sealing inside an open write transaction is a usage error, not a
     * conversion of the file that transaction is about to append to. */
    {
        struct zs_txn *txn = NULL;
        ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
        ASSERT_EQ(zs_db_seal(db), ZS_BADUSAGE);
        ASSERT_OK(zs_txn_abort(&txn));
    }

    ASSERT_OK(zs_db_close(&db));
}

/* A-10, R-3: a read-only handle must not seal. */
static void test_seal_readonly(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(zs_db_seal(db), ZS_READONLY);
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));   /* untouched */
    ASSERT_OK(zs_db_close(&db));
}

/* D-25c: an unclean active file seals to its complete point, and the garbage
 * does not survive into the output. */
static void test_seal_unclean_active_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX];
    size_t clean_size;
    int fd;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    clean_size = zsi_snapshot_active(db->snap)->size;
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    fd = open(dbpath(name), O_WRONLY | O_APPEND);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8), 8);
    close(fd);

    setup.flags = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(zsi_snapshot_active(db->snap)->complete, clean_size);
    ASSERT(zsi_snapshot_active(db->snap)->size > clean_size);

    ASSERT_OK(zs_db_seal(db));

    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    {
        const char *v = NULL;
        size_t vl = 0;
        ASSERT_OK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 1u);
        ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 1u);
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-26, A-11: everything becomes one in-order file spanning the whole range. */
static void test_compact_to_one_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;          /* force several generations */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT(db->snap->nfiles > 1);

    ASSERT_OK(zs_db_compact(db));

    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT_EQU(db->snap->files[0]->hdr.start, 1u);
    ASSERT(!zs_db_should_repack(db));

    for (int i = 0; i < 80; i++) {
        char k[32];
        const char *v = NULL;
        size_t vl = 0;
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 10u);
    }
    ASSERT_OK(zs_db_check_consistency(db));

    /* Idempotent: compacting a single file must not rewrite it. */
    {
        uint32_t end = db->snap->files[0]->hdr.end;
        ASSERT_OK(zs_db_compact(db));
        ASSERT_EQU(db->snap->nfiles, 1u);
        ASSERT_EQU(db->snap->files[0]->hdr.end, end);
    }

    ASSERT_OK(zs_db_close(&db));
}

/* D-26a: compaction merges files a repack deliberately leaves alone.  Built so
 * zs_db_should_repack is FALSE first -- otherwise a repack would have done the
 * same thing and the test would prove nothing about the selection. */
static void test_compact_ignores_geometric_selection(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT_OK(zs_db_seal(db));
    while (zs_db_should_repack(db)) ASSERT_OK(zs_db_repack(db));

    /* D-16 says there is nothing left to do, and more than one file remains. */
    ASSERT(!zs_db_should_repack(db));
    ASSERT(db->snap->nfiles > 1);

    ASSERT_OK(zs_db_compact(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-27: a compaction spanning 1..N drops tombstones, which a repack cannot.
 *
 * Asserted by SIZE as well as by behaviour: "the key is absent" holds either
 * way, so only the file getting smaller shows the tombstone actually went.  Two
 * thirds of the keys are deleted so the difference cannot be noise. */
static void test_compact_drops_tombstones(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    size_t before = 0, after;
    const char *v = NULL;
    size_t vl = 0;

    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 60; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    for (int i = 0; i < 40; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_delete(db, k, strlen(k), 0));
    }
    ASSERT_OK(zs_db_seal(db));
    for (size_t i = 0; i < db->snap->nfiles; i++)
        before += db->snap->files[i]->size;

    ASSERT_OK(zs_db_compact(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    after = db->snap->files[0]->size;
    ASSERT(after < before);

    for (int i = 0; i < 40; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_EQ(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0),
                  ZS_NOTFOUND);
    }
    for (int i = 40; i < 60; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 10u);
    }

    /* A key deleted and then rewritten survives with its NEW value -- the case
     * dropping a tombstone carelessly would break. */
    ASSERT_OK(zs_db_store(db, "key000", 6, "again", 5, 0));
    ASSERT_OK(zs_db_compact(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_OK(zs_db_fetch(db, "key000", 6, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 5u);
    ASSERT_MEM_EQ(v, "again", 5);

    /* And the other deleted keys are still gone after the second compaction. */
    ASSERT_EQ(zs_db_fetch(db, "key001", 6, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);

    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-28, A-11: best effort in action, strict in reporting. */
static void test_compact_reports_and_fails_on_bad_file(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX];
    size_t files_before;
    int fd;

    /* The subject is a LAYOUT -- files on both sides of a damaged one -- so the
     * cascade must not merge it away, and the count must not depend on a
     * selection policy that is deliberately not normative (D-16). */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 512;
    setup.error = counting_error;

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 80; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789", 10, 0));
    }
    ASSERT_OK(zs_db_seal(db));
    ASSERT(db->snap->nfiles > 2);

    /* Pick a middle file, so the in-order prefix stops at it and files remain on
     * both sides -- the arrangement where "merge what you can" is meaningful. */
    zsi_name_format(name, db->uuid, db->snap->files[1]->hdr.start,
                    db->snap->files[1]->hdr.end);
    files_before = db->snap->nfiles;
    ASSERT_OK(zs_db_close(&db));

    /* Destroy its header, which D-10a tolerates and nothing can merge. */
    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(write(fd, "not a zeroskip header at all", 28), 28);
    close(fd);

    setup.flags = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    report_count = 0;

    ASSERT_EQ(zs_db_compact(db), ZS_BADFORMAT);

    /* It reported, and it still merged what it could. */
    ASSERT(report_count > 0);
    ASSERT(db->snap->nfiles > 1);
    ASSERT(db->snap->nfiles < files_before);

    ASSERT_OK(zs_db_close(&db));
}

/* A-11, R-3: a read-only handle must not compact. */
static void test_compact_readonly(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    setup.flags = ZS_SHARED;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(zs_db_compact(db), ZS_READONLY);
    ASSERT_OK(zs_db_close(&db));
}

/* D-25: sealing takes the write lock, and that lock is the ONLY thing making it
 * safe to convert the active file -- without it another writer may be appending
 * to the file being converted.
 *
 * Unobservable in one process: nothing else holds the lock, so removing it
 * changes nothing a single-threaded suite can see.  A peer plus ZS_NONBLOCKING
 * makes it deterministic rather than a timing measurement: while the child holds
 * the write lock, a nonblocking seal MUST report ZS_LOCKED, and a seal that
 * never takes the lock reports ZS_OK instead. */
static void test_seal_waits_for_the_write_lock(void)
{
    SKIP_IF_NO_FORK();

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    pid_t holder;

    clear_db();
    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    zs_db_close(&db);

    holder = fork();
    ASSERT(holder >= 0);
    if (holder == 0) {
        struct zs_open_data s2 = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *hdb = NULL;
        struct zs_txn *txn = NULL;
        s2.error = counting_error;
        if (zs_db_open(dbdir, &s2, &hdb) != ZS_OK) _exit(1);
        if (zs_db_begin_txn(hdb, 0, &txn) != ZS_OK) _exit(2);
        usleep(300000);
        zs_txn_abort(&txn);
        zs_db_close(&hdb);
        _exit(0);
    }

    usleep(30000);              /* let the child take the lock first */

    alarm(60);
    setup.flags = ZS_NONBLOCKING;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQ(zs_db_seal(db), ZS_LOCKED);
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));     /* untouched */
    zs_db_close(&db);
    alarm(0);

    ASSERT_EQ(reap(holder), 0);

    /* And once the peer is gone it succeeds, so the ZS_LOCKED above was the
     * lock and not some unrelated refusal. */
    setup.flags = 0;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    zs_db_close(&db);
}

/* C-1d: compaction takes REPACK then WRITE, and never the other way round.
 *
 * A wrong order does not fail locally -- it deadlocks against a PEER holding the
 * other lock, so it needs a second process to show at all.  A child holds the
 * write lock briefly while the parent compacts: the parent must wait for it,
 * then complete.  alarm() bounds the wait, because a wrong order would otherwise
 * hang the suite rather than fail it, and a hang is much worse evidence.
 *
 * The in-process assertion in zsi_lock_take catches the same mistake from the
 * other side, and did: it fired the first time compaction ran, because C-1d had
 * been amended in the spec without the assertion following. */
static void test_compact_lock_order(void)
{
    SKIP_IF_NO_FORK();

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char pad[150];
    pid_t holder;

    clear_db();
    memset(pad, 'p', sizeof(pad));
    setup.flags = ZS_CREATE;
    setup.rollover_size = 400;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 12; i++) {
        char k[16];
        snprintf(k, sizeof(k), "seed%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), pad, sizeof(pad), 0));
    }
    ASSERT(db->snap->nfiles > 1);
    zs_db_close(&db);

    /* A peer holding the write lock for ~200ms. */
    holder = fork();
    ASSERT(holder >= 0);
    if (holder == 0) {
        struct zs_open_data s2 = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *hdb = NULL;
        struct zs_txn *txn = NULL;
        s2.error = counting_error;
        if (zs_db_open(dbdir, &s2, &hdb) != ZS_OK) _exit(1);
        if (zs_db_begin_txn(hdb, 0, &txn) != ZS_OK) _exit(2);
        usleep(200000);
        zs_txn_abort(&txn);
        zs_db_close(&hdb);
        _exit(0);
    }

    usleep(20000);              /* let the child take the lock first */

    alarm(60);
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_compact(db));
    alarm(0);

    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT_EQ(reap(holder), 0);

    for (int i = 0; i < 12; i++) {
        char k[16];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "seed%02d", i);
        ASSERT_OK(zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0));
    }
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}


/*
 * ============================================================
 * Salvage (S-1 .. S-12)
 * ============================================================
 */

/* Collects every event, so a test can assert on kinds and counts. */
struct salv {
    int   kind_count[16];
    int   total;
    char  keys[64][64];
    int   keylen[64];
    int   nkeys;
    int   stale_keys;
};

static int salv_cb(void *rock, const struct zs_salvage_event *ev)
{
    struct salv *s = rock;

    if (ev->kind >= 0 && ev->kind < 16) s->kind_count[ev->kind]++;
    s->total++;

    if (ev->kind == ZS_SALVAGE_KEY_MAYBE_STALE) {
        s->stale_keys++;
        if (s->nkeys < 64 && ev->keylen < 64) {
            memcpy(s->keys[s->nkeys], ev->key, ev->keylen);
            s->keylen[s->nkeys] = (int)ev->keylen;
            s->nkeys++;
        }
    }
    return 0;
}

/* A scratch destination path under basedir. */
static const char *salv_out(void)
{
    static char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/salvaged", basedir);
    return path;
}

static void salv_reset_out(void)
{
    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", salv_out());
    if (system(cmd)) {}
}

/* Read one key out of the salvaged database. */
static int salv_fetch(const char *key, char *out, size_t outlen)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    const char *v;
    size_t vl;
    int r;

    if (zs_db_open(salv_out(), &setup, &db) != ZS_OK) return ZS_IOERROR;
    r = zs_db_fetch(db, key, strlen(key), NULL, NULL, &v, &vl, 0);
    if (r == ZS_OK) {
        if (vl >= outlen) vl = outlen - 1;
        memcpy(out, v, vl);
        out[vl] = '\0';
    }
    zs_db_close(&db);
    return r;
}

/* S-7: one bad span must not cost the spans after it.
 *
 * The stored spanlen is what makes recovery sound rather than a guess: a
 * candidate terminator names where its span began, so the span can be
 * CHECKSUMMED before it is believed.  Asserted three ways -- the later spans
 * come back, the damaged one does not, and a candidate whose data was also
 * corrupted is REJECTED rather than accepted. */
static void test_salvage_resyncs_after_a_bad_span(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX], val[64];
    size_t second_term;
    int fd;

    /* Four spans, one commit each. */
    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_store(db, "k2", 2, "v2", 2, 0));
    {
        struct zsi_file *f = zsi_snapshot_active(db->snap);
        second_term = f->last_term_off;     /* terminator of span 2 */
    }
    ASSERT_OK(zs_db_store(db, "k3", 2, "v3", 2, 0));
    ASSERT_OK(zs_db_store(db, "k4", 2, "v4", 2, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    /* Corrupt span 2's terminator checksum.  A reader stops there (F-24) and
     * loses k2, k3 and k4; salvage must lose only k2. */
    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)(second_term + 4), SEEK_SET),
              (off_t)(second_term + 4));
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff", 4), 4);
    close(fd);

    /* Confirm the premise: an ordinary open really does lose k3 and k4. */
    {
        const char *v;
        size_t vl;
        setup.flags = 0;
        ASSERT_OK(zs_db_open(dbdir, &setup, &db));
        ASSERT_OK(zs_db_fetch(db, "k1", 2, NULL, NULL, &v, &vl, 0));
        ASSERT_EQ(zs_db_fetch(db, "k3", 2, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
        ASSERT_EQ(zs_db_fetch(db, "k4", 2, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
        ASSERT_OK(zs_db_close(&db));
    }

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    /* The spans after the damage are back, and verified. */
    ASSERT_OK(salv_fetch("k1", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v1");
    ASSERT_OK(salv_fetch("k3", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v3");
    ASSERT_OK(salv_fetch("k4", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v4");

    /* The damaged span's own record is NOT recovered: its terminator is what
     * would have proved it (S-8). */
    ASSERT_EQ(salv_fetch("k2", val, sizeof(val)), ZS_NOTFOUND);

    ASSERT(s.kind_count[ZS_SALVAGE_RESYNC] >= 1);
    ASSERT(s.kind_count[ZS_SALVAGE_SPAN_LOST] >= 1);
}

/* S-8: the damaged span's records come back only when asked for, and are
 * reported as unverified when they do. */
static void test_salvage_unverified_needs_the_flag(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX], val[64];
    size_t second_term;
    int fd;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_store(db, "k2", 2, "v2", 2, 0));
    second_term = zsi_snapshot_active(db->snap)->last_term_off;
    ASSERT_OK(zs_db_store(db, "k3", 2, "v3", 2, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)(second_term + 4), SEEK_SET),
              (off_t)(second_term + 4));
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff", 4), 4);
    close(fd);

    /* Default: absent. */
    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));
    ASSERT_EQ(salv_fetch("k2", val, sizeof(val)), ZS_NOTFOUND);
    ASSERT_EQ(s.kind_count[ZS_SALVAGE_KEY_UNVERIFIED], 0);

    /* Asked for: present, and reported. */
    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.flags = ZS_SALVAGE_UNVERIFIED;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));
    ASSERT_OK(salv_fetch("k2", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v2");
    ASSERT(s.kind_count[ZS_SALVAGE_KEY_UNVERIFIED] >= 1);
}

static void test_convert_reencodes_engine_mismatch(void)
{
    /* F-32c: an engine-0 file sealed by an engine-1 handle.  The output
     * file's header says engine 1, so a verbatim record copy would carry a
     * ZERO checksum that fails under engine 1 for every record.  Conversion
     * must re-encode.  (The reverse mismatch would pass silently -- engine 0
     * verifies nothing -- which is why the test is this way round.) */
    struct zs_db *db;
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_open_data setup2 = ZS_OPEN_DATA_INITIALIZER;
    const char *v; size_t vl;

    clear_db();
    setup.flags = ZS_CREATE | ZS_CSUM_NONE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));
    zs_db_close(&db);

    setup2.flags = ZS_CSUM_XXHASH;
    ASSERT_OK(zs_db_open(dbdir, &setup2, &db));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_OK(zs_db_fetch(db, "k", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "v", 1);
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}

static void test_check_reports_record_csum(void)
{
    /* F-32a in check, both file kinds, both isolated so ONLY the per-record
     * checksum can fire: the in-order corruption lands before ib_finish so
     * the region checksum covers the corrupt bytes and validates; the
     * unordered corruption lands before sb_term so the span validates.  T-6
     * style -- reported, while the healthy key still reads. */
    struct zs_db *db;
    struct ib b;
    struct sb s;
    char name[ZSI_NAME_MAX];
    const char *v; size_t vl;

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    sb_init(&s, 2, ZSI_CSUM_XXHASH);
    sb_rec(&s, "c", 1, "third", 5);
    s.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'T';
    sb_term(&s, false);
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(sb_write(&s, name), 0);
    sb_free(&s);

    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_EQ(zs_db_check_consistency(db), ZS_BADCHECKSUM);
    ASSERT_OK(zs_db_fetch(db, "b", 1, NULL, NULL, &v, &vl, 0));
    zs_db_close(&db);

    /* F-5e: a NOCSUM handle's check skips the verification, like every other
     * read-side check it skips. */
    db = open_db(ZS_NOCSUM);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_check_consistency(db));
    zs_db_close(&db);
}

static void test_salvage_verifies_records_inorder(void)
{
    /* F-32 sharpens S-6's honesty: an in-order file's records were committed
     * by construction (published whole by rename, D-21), so byte-proof is
     * the only open question and the record checksum answers it per record.
     * The corrupt record is still recovered -- salvage never discards what
     * it can read -- but it alone is reported KEY_UNVERIFIED. */
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct salv s;
    struct ib b;
    char name[ZSI_NAME_MAX], val[64];

    clear_db();
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5);
    ib_rec(&b, "b", 1, "other", 5);
    b.buf[ZSI_HEADER_LEN + 4 + 1 + 1] = 'V';
    ib_finish(&b);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    /* Both keys recovered; only the corrupt one reported. */
    ASSERT_OK(salv_fetch("a", val, sizeof(val)));
    ASSERT_STR_EQ(val, "Value");
    ASSERT_OK(salv_fetch("b", val, sizeof(val)));
    ASSERT_STR_EQ(val, "other");
    ASSERT_EQ(s.kind_count[ZS_SALVAGE_KEY_UNVERIFIED], 1);
}

/* Run a shell command and collect its stdout, for comparing a directory before
 * and after. */
static int capture(const char *cmd, char *out, size_t outlen)
{
    FILE *fp = popen(cmd, "r");
    size_t n;

    out[0] = '\0';
    if (!fp) return -1;
    n = fread(out, 1, outlen - 1, fp);
    out[n] = '\0';
    pclose(fp);
    return 0;
}

/* S-9: a rolled-back span is deliberately aborted and is NEVER recovered, with
 * or without ZS_SALVAGE_UNVERIFIED.  Recovering it would resurrect a
 * transaction that did not happen, which no conforming reader has ever shown.
 *
 * Hand-built, as test_dump_shows_rollback is, because this writer buffers until
 * commit and so never produces one -- a streaming peer does. */
static void test_salvage_never_recovers_rollback(void)
{
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct salv s;
    struct sb b;
    char name[ZSI_NAME_MAX], val[64];

    clear_db();
    sb_init(&b, 1, ZSI_CSUM_XXHASH);
    sb_rec(&b, "live", 4, "1", 1);
    sb_term(&b, false);
    sb_rec(&b, "dead", 4, "2", 1);
    sb_term(&b, true);                          /* rolled back */
    zsi_name_current(name, test_uuid);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&b, name), 0);
    sb_free(&b);

    for (int pass = 0; pass < 2; pass++) {
        memset(&s, 0, sizeof(s));
        salv_reset_out();
        ss.report = salv_cb;
        ss.rock = &s;
        ss.error = counting_error;
        ss.flags = pass ? ZS_SALVAGE_UNVERIFIED : 0;

        ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

        ASSERT_OK(salv_fetch("live", val, sizeof(val)));
        ASSERT_STR_EQ(val, "1");
        ASSERT_EQ(salv_fetch("dead", val, sizeof(val)), ZS_NOTFOUND);
        ASSERT(s.kind_count[ZS_SALVAGE_SPAN_ROLLBACK] >= 1);
    }
}

/* S-2: one missing generation makes the database unopenable while every
 * surviving file stays perfectly readable.  Recovering those is the point. */
static void test_salvage_across_a_missing_generation(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX], val[64];
    uint32_t victim;

    /* A layout test: several generations, one of which is removed below.  The
     * cascade would merge them away, and the file count must not depend on a
     * selection policy that is not normative (D-16). */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 256;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 40; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "value", 5, 0));
    }
    ASSERT_OK(zs_db_seal(db));
    ASSERT(db->snap->nfiles > 2);
    victim = db->snap->files[1]->hdr.start;
    zsi_name_format(name, db->uuid, db->snap->files[1]->hdr.start,
                    db->snap->files[1]->hdr.end);
    ASSERT_OK(zs_db_close(&db));

    /* Remove a middle generation outright. */
    ASSERT_EQ(unlink(dbpath(name)), 0);

    /* The premise: the database no longer opens at all. */
    setup.flags = 0;
    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_AGAIN);
    ASSERT_NULL(db);

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT_EQ(s.kind_count[ZS_SALVAGE_GAP], 1);

    /* The first generation's keys survive, which is what an unopenable database
     * was previously denying entirely. */
    ASSERT_OK(salv_fetch("key00", val, sizeof(val)));
    ASSERT_STR_EQ(val, "value");
    (void)victim;
}

/* S-3: the newest surviving version of a key wins, which falls out of applying
 * records oldest first rather than from a recency pass. */
static void test_salvage_newest_version_wins(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char val[64];

    /* ZS_NOAUTOREPACK: the point is SEVERAL generations each holding a later
     * version of the same key, and D-16e would merge them into one file --
     * leaving nothing for the oldest-first ordering to get right (A-14). */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 256;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 30; i++) {
        char v[32];
        snprintf(v, sizeof(v), "v%02d", i);
        ASSERT_OK(zs_db_store(db, "same", 4, v, strlen(v), 0));
        ASSERT_OK(zs_db_store(db, "filler", 6, "0123456789012345678901234567890",
                              31, 0));
    }
    ASSERT_OK(zs_db_store(db, "gone", 4, "x", 1, 0));
    ASSERT_OK(zs_db_delete(db, "gone", 4, 0));

    /* Seal, so the NEWEST version of "same" lives in an in-order file rather
     * than in the active one.  Without this the active file carries it, and
     * since a start-0 entry sorts last regardless (D-1b leaves its generation
     * unknown to salvage's raw scan), the ordering among the generation-named
     * files could be reversed without changing any answer -- the fixture would
     * assert S-3 while being unable to see it broken. */
    ASSERT_OK(zs_db_seal(db));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT(db->snap->nfiles > 1);
    ASSERT_OK(zs_db_close(&db));

    salv_reset_out();
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT_OK(salv_fetch("same", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v29");

    /* A key whose newest surviving version is a deletion ends ABSENT, which is
     * what the database said rather than a loss. */
    ASSERT_EQ(salv_fetch("gone", val, sizeof(val)), ZS_NOTFOUND);
}

/* S-5: an unreadable header does not make a file unreadable.  The generation
 * comes from the filename; only the engine is genuinely unknown. */
static void test_salvage_invalid_header(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX], val[64];
    int fd;

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_store(db, "k2", 2, "v2", 2, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    /* Destroy the header's checksum, leaving the records intact. */
    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)ZSI_HDR_OFF_CSUM, SEEK_SET),
              (off_t)ZSI_HDR_OFF_CSUM);
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff", 4), 4);
    close(fd);

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT(s.kind_count[ZS_SALVAGE_HEADER_INVALID] >= 1);
    ASSERT(s.kind_count[ZS_SALVAGE_ENGINE_GUESSED] >= 1);

    ASSERT_OK(salv_fetch("k1", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v1");
    ASSERT_OK(salv_fetch("k2", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v2");
}

/* S-6: a pointer section that will not load makes a file unreadable under
 * section 7 while its records may be perfect. */
/* S-3's other half, which the sealed fixture above structurally cannot see:
 * the newest version living in the ACTIVE file.  D-1b leaves its generation
 * out of the name, and salvage reads the directory raw (S-1), so it arrives
 * with start == 0 and has to be forced last -- a naive sort puts it first and
 * makes the newest file in the database the first one applied. */
static void test_salvage_active_file_is_newest(void)
{
    char newest[16];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char val[64];

    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 256;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 30; i++) {
        char v[32];
        snprintf(v, sizeof(v), "v%02d", i);
        ASSERT_OK(zs_db_store(db, "same", 4, v, strlen(v), 0));
        ASSERT_OK(zs_db_store(db, "filler", 6, "0123456789012345678901234567890",
                              31, 0));
    }

    /* Deliberately NOT sealed: the newest version must be in the ACTIVE file,
     * which is what this test is about -- salvage must scan it LAST (S-3), and
     * the only way to tell that from the opposite is for the active file to
     * hold a version no other file has.
     *
     * Reached by storing until an active file exists rather than by trusting
     * the loop above to end that way: a commit crossing rollover_size seals in
     * place (D-25d), so whether the last iteration leaves a clean active file
     * is a function of the exact encoded record size, and this test started
     * failing the moment records got 4 bytes shorter, for a reason that had
     * nothing to do with salvage.
     *
     * Each attempt writes a DISTINCT value, which is the part that matters.  An
     * earlier version of this retry re-stored "v29", so when an attempt sealed,
     * "v29" ended up in both the sealed file and the next active one -- and the
     * ordering it exists to pin stopped mattering.  It passed, and
     * "salvage: active file sorts first, not last" went from caught to NOT
     * CAUGHT without anything in the library changing. */
    for (int i = 0; i < 8; i++) {
        snprintf(newest, sizeof(newest), "final%d", i);
        ASSERT_OK(zs_db_store(db, "same", 4, newest, strlen(newest), 0));
        if (zsi_snapshot_active(db->snap)) break;
    }
    ASSERT_NOT_NULL(zsi_snapshot_active(db->snap));
    ASSERT(db->snap->nfiles > 1);
    ASSERT_OK(zs_db_close(&db));

    salv_reset_out();
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT_OK(salv_fetch("same", val, sizeof(val)));
    ASSERT_STR_EQ(val, newest);      /* the ACTIVE file's version, not an older */
}

static void test_salvage_ignores_pointer_section(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX], val[64];
    size_t fsize;
    int fd;

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_store(db, "k2", 2, "v2", 2, 0));
    ASSERT_OK(zs_db_seal(db));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    fsize = db->snap->files[0]->size;
    zsi_name_format(name, db->uuid, db->snap->files[0]->hdr.start,
                    db->snap->files[0]->hdr.end);
    ASSERT_OK(zs_db_close(&db));

    /* Wreck the 16-byte trailer, which is how the pointer section is found. */
    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)(fsize - 16), SEEK_SET), (off_t)(fsize - 16));
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff\xff\xff\xff\xff", 8), 8);
    close(fd);

    /* The premise: it no longer opens. */
    setup.flags = 0;
    ASSERT(zs_db_open(dbdir, &setup, &db) != ZS_OK);

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT(s.kind_count[ZS_SALVAGE_PTRS_IGNORED] >= 1);
    ASSERT_OK(salv_fetch("k1", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v1");
    ASSERT_OK(salv_fetch("k2", val, sizeof(val)));
    ASSERT_STR_EQ(val, "v2");
}

/* S-10: only keys that COULD have been superseded by lost bytes are reported.
 * Asserted both ways -- a key written after the damage must NOT appear, or the
 * report degrades to "everything might be stale", which is true and useless. */
static void test_salvage_reports_maybe_stale(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct salv s;
    char name[ZSI_NAME_MAX];
    size_t term_after_old;
    int fd;
    bool saw_old = false, saw_new = false;

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "old", 3, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "doomed", 6, "2", 1, 0));
    term_after_old = zsi_snapshot_active(db->snap)->last_term_off;
    ASSERT_OK(zs_db_store(db, "new", 3, "3", 1, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    /* Lose the "doomed" span, leaving "old" before it and "new" after. */
    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)(term_after_old + 4), SEEK_SET),
              (off_t)(term_after_old + 4));
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff", 4), 4);
    close(fd);

    memset(&s, 0, sizeof(s));
    salv_reset_out();
    ss.report = salv_cb;
    ss.rock = &s;
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    for (int i = 0; i < s.nkeys; i++) {
        if (s.keylen[i] == 3 && !memcmp(s.keys[i], "old", 3)) saw_old = true;
        if (s.keylen[i] == 3 && !memcmp(s.keys[i], "new", 3)) saw_new = true;
    }

    ASSERT(saw_old);        /* written before the loss: could be stale */
    ASSERT(!saw_new);       /* written after it: definitely current */
}

/* S-1, S-12: salvage never writes the source.  Asserted by CONTENT rather than
 * by intent, and then again by running against a read-only directory, which is
 * the strongest form of the same claim. */
static void test_salvage_never_writes_the_source(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char before[8192] = "", after[8192] = "";
    char cmd[PATH_MAX + 64];

    setup.flags = ZS_CREATE;
    setup.rollover_size = 256;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    for (int i = 0; i < 20; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%02d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "value", 5, 0));
    }
    ASSERT_OK(zs_db_close(&db));

    /* Remove the lock file first, so its ABSENCE is part of what is compared.
     * Without this the source already has one and a salvage that opened the
     * source as a writable database -- which creates it (D-3a) -- would change
     * nothing observable.  R-3 already forbids a reader creating it; salvage
     * must not either. */
    {
        char lockpath[PATH_MAX];
        snprintf(lockpath, sizeof(lockpath), "%s/zeroskip.lock", dbdir);
        ASSERT_EQ(unlink(lockpath), 0);
    }

    /* Every name in the source, with sizes. */
    snprintf(cmd, sizeof(cmd), "ls -1l '%s' | awk '{print $5, $9}' | sort",
             dbdir);
    ASSERT_EQ(capture(cmd, before, sizeof(before)), 0);

    salv_reset_out();
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT_EQ(capture(cmd, after, sizeof(after)), 0);
    ASSERT_STR_EQ(before, after);

    /* And again with the source read-only, which no amount of care in the code
     * can fake.  Skipped as root, where the mode is not enforced. */
    if (geteuid() != 0) {
        ASSERT_EQ(chmod(dbdir, 0500), 0);
        salv_reset_out();
        ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));
        ASSERT_EQ(chmod(dbdir, 0700), 0);

        {
            char val[64];
            ASSERT_OK(salv_fetch("key00", val, sizeof(val)));
            ASSERT_STR_EQ(val, "value");
        }
    }
}

/* S-11: the report is STRUCTURED -- a kind, a location, a key where one applies
 * -- rather than prose.  S-10's report is the mitigation for emitting a
 * possibly stale value, so it has to be machine-readable rather than something
 * an operator parses back out of a message. */
static int fields_cb(void *rock, const struct zs_salvage_event *ev)
{
    int *bad = rock;

    /* A file-scoped event names its file and generation; a key event carries a
     * key with a length.  Neither ever arrives as text to be parsed. */
    switch (ev->kind) {
    case ZS_SALVAGE_HEADER_INVALID:
    case ZS_SALVAGE_ENGINE_GUESSED:
    case ZS_SALVAGE_SPAN_LOST:
    case ZS_SALVAGE_SPAN_ROLLBACK:
    case ZS_SALVAGE_RESYNC:
    case ZS_SALVAGE_PTRS_IGNORED:
        if (!ev->file || ev->generation == 0) (*bad)++;
        break;
    case ZS_SALVAGE_KEY_MAYBE_STALE:
    case ZS_SALVAGE_KEY_UNVERIFIED:
        if (!ev->key || ev->keylen == 0) (*bad)++;
        break;
    default:
        break;
    }
    return 0;
}

static int stop_after_first_cb(void *rock, const struct zs_salvage_event *ev)
{
    (void)ev;
    int *events = rock;
    (*events)++;
    return 1;                                   /* stop after the first event */
}

static void test_salvage_event_fields(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char name[ZSI_NAME_MAX];
    size_t term;
    int bad = 0, fd;

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_store(db, "k2", 2, "v2", 2, 0));
    term = zsi_snapshot_active(db->snap)->last_term_off;
    ASSERT_OK(zs_db_store(db, "k3", 2, "v3", 2, 0));
    zsi_name_current(name, db->uuid);
    ASSERT_OK(zs_db_close(&db));

    fd = open(dbpath(name), O_WRONLY);
    ASSERT(fd >= 0);
    ASSERT_EQ(lseek(fd, (off_t)(term + 4), SEEK_SET), (off_t)(term + 4));
    ASSERT_EQ(write(fd, "\xff\xff\xff\xff", 4), 4);
    close(fd);

    salv_reset_out();
    ss.report = fields_cb;
    ss.rock = &bad;
    ss.error = counting_error;
    ss.flags = ZS_SALVAGE_UNVERIFIED;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));
    ASSERT_EQ(bad, 0);

    /* A callback returning non-zero stops the salvage, which is reported as
     * ZS_DONE: stopped on request, distinct from both success and failure.  No
     * further event is delivered after the one that stopped it. */
    {
        int events = 0;
        salv_reset_out();
        ss.report = stop_after_first_cb;
        ss.rock = &events;
        ASSERT_EQ(zs_db_salvage(dbdir, salv_out(), &ss), ZS_DONE);
        ASSERT_EQ(events, 1);
    }
}

/* S-4: the source's comparator does not affect the OUTPUT -- that is ordered by
 * the caller's, and recency comes from generation and offset.  A mismatch is
 * still worth saying, because the source was built under an order we are not
 * reproducing. */
static void test_salvage_comparator_mismatch_reported(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char val[64];

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "k1", 2, "v1", 2, 0));
    ASSERT_OK(zs_db_close(&db));

    /* Salvage under a DIFFERENT named comparator with the same ordering.  The
     * data must still come back -- the source's order never mattered -- and the
     * divergence must be reported. */
    report_count = 0;
    salv_reset_out();
    ss.compar = zsi_compar_default;
    ss.compar_name = "notmemcmp";
    ss.error = counting_error;
    ASSERT_OK(zs_db_salvage(dbdir, salv_out(), &ss));

    ASSERT(report_count > 0);

    {
        struct zs_open_data o = ZS_OPEN_DATA_INITIALIZER;
        struct zs_db *sdb = NULL;
        const char *v;
        size_t vl;
        o.compar = zsi_compar_default;
        o.compar_name = "notmemcmp";
        ASSERT_OK(zs_db_open(salv_out(), &o, &sdb));
        ASSERT_OK(zs_db_fetch(sdb, "k1", 2, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 2u);
        ASSERT_OK(zs_db_close(&sdb));
    }
    (void)val;
}

/*
 * ============================================================
 * Cursor liveness (D-14j)
 * ============================================================
 */

static struct zs_db *live_db;
static struct zs_txn *live_txn;
static char live_log[512];

static void live_note(const char *k, size_t kl)
{
    strncat(live_log, "|", sizeof(live_log) - strlen(live_log) - 1);
    strncat(live_log, k, kl < 32 ? kl : 32);
}

/* Writes "c" -- a key the traversal has NOT yet reached -- when it sees "b". */
static int live_db_cb(void *rock, const char *k, size_t kl,
                      const char *v, size_t vl)
{
    (void)rock; (void)v; (void)vl;
    live_note(k, kl);
    if (kl == 1 && k[0] == 'b')
        CB_ASSERT(zs_db_store(live_db, "c", 1, "3", 1, 0) == ZS_OK);
    return 0;
}

static int live_txn_cb(void *rock, const char *k, size_t kl,
                       const char *v, size_t vl)
{
    (void)rock; (void)v; (void)vl;
    live_note(k, kl);
    if (kl == 1 && k[0] == 'b')
        CB_ASSERT(zs_txn_store(live_txn, "c", 1, "3", 1, 0) == ZS_OK);
    return 0;
}

static void live_seed(struct zs_open_data *setup)
{
    setup->flags = ZS_CREATE;
    setup->error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, setup, &live_db));
    ASSERT_OK(zs_db_store(live_db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(live_db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(live_db, "d", 1, "4", 1, 0));
}

/* D-14j, D-14j-b: a non-transactional foreach observes writes committed through
 * its OWN handle while it runs.  This is the cyrusdb semantic -- a traversal
 * whose callback drops something in must see it -- and it costs nothing,
 * because the commit already replaced the handle's snapshot. */
static void test_cursor_sees_own_handle_writes(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;

    live_seed(&setup);

    live_log[0] = '\0';
    ASSERT_OK(zs_db_foreach(live_db, NULL, 0, NULL, live_db_cb, NULL, 0));

    /* c was written from inside the callback, at a key not yet reached, and is
     * visible to the rest of the traversal.  And nothing is yielded twice. */
    ASSERT_STR_EQ(live_log, "|a|b|c|d");

    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j: inside an EXPLICIT transaction the file set is fixed (G-4), so another
 * handle's committed write is not visible -- but the transaction's own pending
 * write is (A-1a), including one made during the traversal. */
static void test_txn_cursor_sees_own_writes(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;

    live_seed(&setup);

    ASSERT_OK(zs_db_begin_txn(live_db, 0, &live_txn));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, live_txn_cb, NULL, 0));
    ASSERT_STR_EQ(live_log, "|a|b|c|d");
    ASSERT_OK(zs_txn_commit(&live_txn));

    /* And it really did commit. */
    {
        const char *v;
        size_t vl;
        ASSERT_OK(zs_db_fetch(live_db, "c", 1, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 1u);
    }
    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j-a: a write during a traversal MUST NOT cause a key to be yielded twice.
 *
 * The failure it guards against is silent -- the traversal simply processes a
 * record twice -- and it is what an INDEX into an ordered pending set would
 * produce: inserting ahead of the position shifts the element under it.
 *
 * "z" is pending before the walk starts, so the arm is live rather than
 * exhausted, which is the arrangement that exposed it. */
static void test_txn_cursor_no_duplicate_on_write(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;

    live_seed(&setup);

    ASSERT_OK(zs_db_begin_txn(live_db, 0, &live_txn));
    ASSERT_OK(zs_txn_store(live_txn, "z", 1, "9", 1, 0));

    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, live_txn_cb, NULL, 0));

    /* Every key exactly once, in order, including the one written mid-walk. */
    ASSERT_STR_EQ(live_log, "|a|b|c|d|z");

    ASSERT_OK(zs_txn_abort(&live_txn));
    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j: an explicit READ transaction keeps its fixed view (G-4), even while the
 * same handle commits underneath it.  This is the case cyrusdb calls a
 * transactional read, and it must NOT become live. */
/* G-4 for a read transaction with NO cursor, which is the configuration that
 * makes its file references load-bearing rather than redundant.
 *
 * A read transaction keeps its snapshot alive by referencing the SNAPSHOT, so
 * the bytes survive without any per-file hold.  What the hold does is raise the
 * active FILE's refcount, and that is what tells the commit-site fold somebody
 * is reading it -- so the fold rebuilds instead of extending the index and the
 * complete point in place (D-13b, G-6).  Mutated in place, this transaction's
 * fixed view would grow records committed after it began.
 *
 * test_txn_cursor_view_is_fixed cannot see that: it holds a cursor as well, and
 * either hold is enough to keep the refcount above one, so they mask each other
 * completely.  This is the configuration where the transaction's hold stands
 * alone. */
/* Open descriptors, for the leak test below.  /dev/fd is the process's own
 * descriptor table on macOS and a symlink to /proc/self/fd on Linux, so this
 * reads the same thing on both.  Returns -1 where neither exists, which the
 * caller treats as "cannot check" rather than as a failure. */
static int count_open_fds(void)
{
    DIR *d = opendir("/dev/fd");
    struct dirent *de;
    int n = 0;

    if (!d) return -1;
    while ((de = readdir(d))) if (de->d_name[0] != '.') n++;
    closedir(d);
    return n;                       /* includes the one opendir just took */
}

/* A-4a's other half: the references a hold takes must come BACK.
 *
 * zsi_hold_fini releasing them is what lets a retired file's descriptor and
 * mapping go at all -- a snapshot rebuild retires the old files, and they are
 * freed only when the last reference drops.  Leak the hold's references and
 * nothing is ever freed: descriptors accumulate for the life of the handle,
 * and a long-lived writer runs out of them.
 *
 * A leak is not a test failure by itself: it needs `make leaks` to show, and
 * mutation testing does not run that.  Counting descriptors turns it into
 * one.
 *
 * The loop commits between transactions on purpose: a commit replaces the
 * handle's snapshot, which is the only thing that retires a file and so the
 * only way a missing release becomes observable. */
static void test_hold_releases_its_references(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db;
    int before, after;

    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_size = 4096;         /* small, so generations turn over */
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* Warm up: reach a steady state before counting, so the baseline is not
     * measuring first-use allocation. */
    for (int i = 0; i < 10; i++) {
        char k[16];
        snprintf(k, sizeof(k), "w%d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "v", 1, 0));
    }

    before = count_open_fds();
    if (before < 0) SKIP("no /dev/fd on this platform");

    for (int i = 0; i < 60; i++) {
        struct zs_txn *rd = NULL;
        const char *v;
        size_t vl;
        char k[16];

        /* A SHARED transaction takes a hold per file... */
        ASSERT_OK(zs_db_begin_txn(db, ZS_SHARED, &rd));
        zs_txn_fetch(rd, "w0", 2, NULL, NULL, &v, &vl, 0);
        ASSERT_OK(zs_txn_abort(&rd));

        /* ...and a commit retires what it was holding. */
        snprintf(k, sizeof(k), "k%d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), "0123456789012345", 16, 0));
    }

    after = count_open_fds();
    ASSERT(after >= 0);

    /* The file count is bounded by the layout, not by the number of
     * transactions -- so a per-transaction leak shows up here as growth.  A few
     * descriptors of slack, since the set legitimately gains files as
     * generations turn over. */
    if (after > before + 8) {
        fprintf(stderr, "\n    FAIL open fds went %d -> %d over 60 "
                        "transactions: references are not being released\n",
                before, after);
        current_test_failed = 1;
    }

    zs_db_close(&db);
}

/* A-4 is per PRODUCER: a fetch borrow survives the end of an unrelated CURSOR on
 * the same transaction.
 *
 * "The lifetime of the transaction or cursor that produced them" says this, but
 * only if you read the two halves as independent -- and the mechanism does not
 * make it obvious, because a cursor's end IS the first moment a mapping it
 * yielded from may go (A-4a), and `zsi_cursor_free` really does release per-file
 * references there.  What keeps the fetch's bytes alive is that the transaction
 * has its own claim on the same files: a read transaction holds one per file
 * directly, and a write transaction pins the snapshot that does.
 *
 * The values come from a FILE rather than the pending set, so an unmap is a real
 * possibility rather than a formality, and both transaction kinds are covered
 * because only one of them takes A-4a's per-file references (a write transaction
 * deliberately does not).  Worth pinning because the downstream SQLite engine
 * depends on it for its savepoint undo log: before-images fetched inside a
 * transaction outlive the cursors that statement opens and closes.
 *
 * It is NOT the unique catcher for any mutation available, and that is recorded
 * rather than glossed: both mutants aimed at this property -- a cursor's end
 * releasing the transaction's claim, and a borrower's references released twice --
 * die in the A-4a swap tests a few lines above, which reach the same
 * reference-counting from a different direction.  What this adds is localisation.
 * A regression here reports as "a fetch borrow did not survive a cursor" against
 * the requirement a consumer reads, instead of as a segfault in a snapshot-swap
 * test whose subject is something else. */
static void test_fetch_borrow_survives_cursor_free(void)
{
    struct zs_db *db;
    char key[16];

    db = fresh_db_noautorepack();
    ASSERT_NOT_NULL(db);
    for (int i = 0; i < 50; i++) {
        snprintf(key, sizeof(key), "k%03d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), "value-here", 10, 0));
    }
    /* Reopened, so every record is in a file and every borrow is into a mapping
     * rather than into a transaction's own chunk buffer. */
    ASSERT_OK(zs_db_close(&db));

    for (int shared = 0; shared <= 1; shared++) {
        struct zs_txn *txn = NULL;
        struct zs_cursor *c = NULL;
        const char *v = NULL, *ck = NULL, *cv = NULL;
        size_t vl = 0, ckl = 0, cvl = 0;

        db = open_db(ZS_NOAUTOREPACK);
        ASSERT_NOT_NULL(db);
        ASSERT_OK(zs_db_begin_txn(db, shared, &txn));

        /* The borrow whose lifetime is the TRANSACTION's. */
        ASSERT_OK(zs_txn_fetch(txn, "k007", 4, NULL, NULL, &v, &vl, 0));
        ASSERT_EQU(vl, 10u);

        /* An unrelated cursor, opened, advanced far enough to have mapped and
         * held files of its own, and then ended. */
        ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, 0));
        for (int i = 0; i < 20; i++)
            ASSERT_OK(zs_cursor_next(c, &ck, &ckl, &cv, &cvl));
        zs_cursor_fini(&c);
        ASSERT_NULL(c);

        /* The fetch's bytes are still there.  Under ASan this is where a
         * use-after-unmap would be reported rather than silently comparing equal
         * against pages the kernel has not yet reclaimed. */
        ASSERT_MEM_EQ(v, "value-here", 10);

        ASSERT_OK(zs_txn_commit(&txn));
        ASSERT_OK(zs_db_close(&db));
    }
}

static void test_read_txn_view_is_fixed_without_a_cursor(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_txn *rd = NULL;
    const char *v;
    size_t vl;

    live_seed(&setup);                          /* a b d, committed */

    ASSERT_OK(zs_db_begin_txn(live_db, ZS_SHARED, &rd));

    /* Establish the view.  No cursor is opened, deliberately. */
    ASSERT_OK(zs_txn_fetch(rd, "a", 1, NULL, NULL, &v, &vl, 0));

    /* Commit through the handle behind its back, several times, so the active
     * file grows and the fold has something to fold. */
    for (int i = 0; i < 4; i++) {
        char k[8];
        snprintf(k, sizeof(k), "z%d", i);
        ASSERT_OK(zs_db_store(live_db, k, strlen(k), "new", 3, 0));
    }

    /* None of it is visible: the transaction's file set was fixed at begin. */
    for (int i = 0; i < 4; i++) {
        char k[8];
        snprintf(k, sizeof(k), "z%d", i);
        ASSERT_EQ(zs_txn_fetch(rd, k, strlen(k), NULL, NULL, &v, &vl, 0),
                  ZS_NOTFOUND);
    }
    /* ...and what was there still is. */
    ASSERT_OK(zs_txn_fetch(rd, "d", 1, NULL, NULL, &v, &vl, 0));

    ASSERT_OK(zs_txn_abort(&rd));

    /* The writes really did commit -- otherwise the assertions above would
     * hold for the wrong reason. */
    ASSERT_OK(zs_db_fetch(live_db, "z3", 2, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "new", 3);
    ASSERT_OK(zs_db_close(&live_db));
}

static void test_txn_cursor_view_is_fixed(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_txn *rd = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    char seen[128] = "";

    live_seed(&setup);

    ASSERT_OK(zs_db_begin_txn(live_db, 1, &rd));
    ASSERT_OK(zs_txn_begin_cursor(rd, NULL, 0, &c, 0));

    /* One step, then commit "c" through the handle behind its back. */
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    strncat(seen, k, kl);
    ASSERT_OK(zs_db_store(live_db, "c", 1, "3", 1, 0));

    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        strncat(seen, "|", sizeof(seen) - strlen(seen) - 1);
        strncat(seen, k, kl);
    }

    ASSERT_STR_EQ(seen, "a|b|d");        /* c is NOT visible: fixed view */

    zs_cursor_fini(&c);
    ASSERT_OK(zs_txn_abort(&rd));
    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j: a refresh must not lose the cursor's START position.
 *
 * Reported downstream as a bug involving deletions mixed into a traversal, and
 * the deletion is incidental: ANY pending write in the transaction is enough.
 * Two faults compounded -- the cursor did not record the transaction's change
 * counter at open, so its very first step saw a false change; and a refresh
 * before anything had been emitted re-seeked to the FIRST key, because there was
 * no last-yielded key to resume from.
 *
 * The prefix case is the dangerous one: restarting at the first key lands
 * outside the prefix, so the scan ends immediately and returns NOTHING.  A
 * silently empty result, from a scan that should have matched. */
static int live_note_cb(void *rock, const char *k, size_t kl,
                        const char *v, size_t vl)
{
    (void)rock; (void)v; (void)vl;
    live_note(k, kl);
    return 0;
}

static void test_cursor_start_key_survives_refresh(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_txn *txn = NULL;

    live_seed(&setup);
    ASSERT_OK(zs_db_store(live_db, "c", 1, "3", 1, 0));
    ASSERT_OK(zs_db_store(live_db, "e", 1, "5", 1, 0));
    /* a b c d e */

    /* Baseline: a start-key scan in a clean transaction. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &txn));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(txn, "d", 1, NULL, live_note_cb, NULL, 0));
    ASSERT_STR_EQ(live_log, "|d|e");
    ASSERT_OK(zs_txn_abort(&txn));

    /* The same scan with a write already pending on the transaction. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &txn));
    ASSERT_OK(zs_txn_delete(txn, "a", 1, 0));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(txn, "d", 1, NULL, live_note_cb, NULL, 0));
    ASSERT_STR_EQ(live_log, "|d|e");        /* NOT |b|c|d|e */
    ASSERT_OK(zs_txn_abort(&txn));

    /* And a prefix scan, where losing the start key empties the result. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &txn));
    ASSERT_OK(zs_txn_delete(txn, "a", 1, 0));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(txn, "e", 1, NULL, live_note_cb, NULL,
                             ZS_CURSOR_PREFIX));
    ASSERT_STR_EQ(live_log, "|e");          /* NOT empty */
    ASSERT_OK(zs_txn_abort(&txn));

    /* And the cases that isolate the fallback rather than the counter: a
     * GENUINE change before the first record has been emitted.  There is no
     * last-yielded key to resume from, so the re-seek must fall back to the
     * key the cursor was opened at -- not to the first key in the database.
     *
     * Both kinds of change, because they re-seek different arms: a pending
     * write repositions only the transaction arm, and a snapshot change
     * repositions every arm -- so only the second exercises the fallback for
     * the FILE arms. */
    {
        struct zs_cursor *c = NULL;
        const char *k, *v;
        size_t kl, vl;

        ASSERT_OK(zs_db_begin_cursor(live_db, "d", 1, &c, 0));
        ASSERT_OK(zs_txn_store(c->txn, "z", 1, "9", 1, 0));   /* pending write */
        ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
        ASSERT_EQU(kl, 1u);
        ASSERT_MEM_EQ(k, "d", 1);                             /* NOT "a" */
        ASSERT_OK(zs_cursor_abort(&c));
    }
    {
        struct zs_cursor *c = NULL;
        const char *k, *v;
        size_t kl, vl;

        /* ZS_SHARED, so the cursor holds no write lock and the same handle can
         * commit underneath it -- the handle-live snapshot swap (D-14j). */
        ASSERT_OK(zs_db_begin_cursor(live_db, "d", 1, &c, ZS_SHARED));
        ASSERT_OK(zs_db_store(live_db, "z", 1, "9", 1, 0));   /* snapshot change */
        ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
        ASSERT_EQU(kl, 1u);
        ASSERT_MEM_EQ(k, "d", 1);                             /* NOT "a" */
        ASSERT_OK(zs_cursor_abort(&c));
    }

    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j with deletions, which is how this was reported.  Deleting the key the
 * cursor is ON, and the key it is about to reach, must both behave: the current
 * one has already been yielded, and the next one must not be. */
static void test_cursor_delete_during_traversal(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    char seen[128] = "";

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &live_db));
    for (const char *p = "abcdef"; *p; p++)
        ASSERT_OK(zs_db_store(live_db, p, 1, "v", 1, 0));

    ASSERT_OK(zs_db_begin_cursor(live_db, NULL, 0, &c, 0));
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        strncat(seen, "|", sizeof(seen) - strlen(seen) - 1);
        strncat(seen, k, kl);

        /* Delete the key we are on... */
        if (kl == 1 && k[0] == 'b') ASSERT_OK(zs_cursor_delete(c, 0));
        /* ...and, elsewhere, the key we are about to reach. */
        if (kl == 1 && k[0] == 'd') ASSERT_OK(zs_txn_delete(c->txn, "e", 1, 0));
    }
    ASSERT_OK(zs_cursor_commit(&c));

    /* "e" was deleted before the walk reached it, so it is not yielded; "b" was
     * deleted after being yielded, so it is. */
    ASSERT_STR_EQ(seen, "|a|b|c|d|f");

    /* And both deletions actually took effect. */
    {
        const char *val;
        size_t vlen;
        ASSERT_EQ(zs_db_fetch(live_db, "b", 1, NULL, NULL, &val, &vlen, 0),
                  ZS_NOTFOUND);
        ASSERT_EQ(zs_db_fetch(live_db, "e", 1, NULL, NULL, &val, &vlen, 0),
                  ZS_NOTFOUND);
        ASSERT_OK(zs_db_fetch(live_db, "f", 1, NULL, NULL, &val, &vlen, 0));
    }
    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j-b: a key stored BEHIND the cursor during a traversal must not be
 * yielded.  It is before the resume point, so yielding it hands the caller a
 * key out of order and shifts everything after it by one.
 *
 * The trap is resolving the transaction arm from ITS OWN position after a
 * pending write: the arm's position is the last key consumed FROM THAT ARM,
 * which lags the merge's progress -- for an arm that was exhausted at open it
 * is "the beginning".  A reload from there resurfaces every pending key behind
 * the merge.  The resume point is the CURSOR's last yielded key, and only a
 * re-seek from it is correct.
 *
 * Two arrangements: the arm exhausted at open (empty pending), and the arm
 * having already yielded a key of its own, which lags the merge by less but
 * still lags. */
static int live_store_behind_cb(void *rock, const char *k, size_t kl,
                                const char *v, size_t vl)
{
    (void)rock; (void)v; (void)vl;
    live_note(k, kl);
    if (kl == 1 && k[0] == 'b')
        CB_ASSERT(zs_txn_store(live_txn, "aa", 2, "!", 1, 0) == ZS_OK);
    return 0;
}

static void test_txn_cursor_store_behind_not_yielded(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    const char *v;
    size_t vl;

    live_seed(&setup);         /* a b d, committed */

    /* Arm exhausted at open: nothing pending when the walk starts. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &live_txn));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, live_store_behind_cb,
                             NULL, 0));
    ASSERT_STR_EQ(live_log, "|a|b|d");          /* NOT |a|b|aa|d */

    /* The write itself took: it is in the transaction's view... */
    ASSERT_OK(zs_txn_fetch(live_txn, "aa", 2, NULL, NULL, &v, &vl, 0));
    /* ...and a FRESH traversal of the same transaction yields it in place. */
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, live_note_cb, NULL, 0));
    ASSERT_STR_EQ(live_log, "|a|aa|b|d");
    ASSERT_OK(zs_txn_abort(&live_txn));

    /* Arm already mid-array: "0" is pending before the walk starts and is
     * yielded first, so the arm's own position is "0" when the write lands --
     * behind the merge's "b", and "aa" sits in the gap between them. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &live_txn));
    ASSERT_OK(zs_txn_store(live_txn, "0", 1, "0", 1, 0));
    live_log[0] = '\0';
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, live_store_behind_cb,
                             NULL, 0));
    ASSERT_STR_EQ(live_log, "|0|a|b|d");        /* NOT |0|a|b|aa|d */
    ASSERT_OK(zs_txn_abort(&live_txn));

    ASSERT_OK(zs_db_close(&live_db));
}

/* D-14j-a at the size where the pending array actually MOVES.
 *
 * The transaction arm's position is a key rather than an index because a write
 * mid-traversal inserts into the sorted pending array and shifts everything
 * from the insertion point on.  That key is now BORROWED from the array's own
 * key block instead of copied -- three independent things make it sound, and
 * the weakest of them is the one worth pinning: zsi_pend_set allocates each key
 * once and neither the memmove of the ENTRIES, nor a realloc of the array, nor
 * an overwrite repointing off/len, moves a key block.
 *
 * The other D-14j-a tests cannot exercise any of that.  They store a handful of
 * one-byte keys, so the array never outgrows its initial 16 slots and never
 * reallocs, and they only ever insert keys that are absent -- so the overwrite
 * path never runs at all.  This one inserts 200 keys BELOW the cursor in a
 * single callback, which memmoves the whole array 200 times and reallocs it
 * through 64 -> 128 -> 256, and overwrites both the key just yielded and the
 * one the arm has already advanced onto.
 *
 * Everything below the resume point is invisible to the rest of the walk by
 * D-14j-b, so the expected output is just the "k" keys, once each, in order. */
struct pend_move_rock {
    size_t n;
    char   last[8];
    bool   out_of_order;
    bool   unexpected;
    bool   mutate;          /* do the mid-walk writes on the first yield */
};

static int pend_move_cb(void *rock, const char *k, size_t kl,
                        const char *v, size_t vl)
{
    struct pend_move_rock *r = rock;
    (void)v; (void)vl;

    if (kl != 4 || k[0] != 'k') { r->unexpected = true; return 0; }
    if (r->n && memcmp(k, r->last, 4) <= 0) r->out_of_order = true;
    memcpy(r->last, k, 4);
    r->last[4] = '\0';
    r->n++;

    if (r->mutate && r->n == 1) {
        char key[8];
        int i;

        /* 200 inserts, every one of them below the cursor's position, so the
         * array is rebuilt under the arm again and again. */
        for (i = 0; i < 200; i++) {
            snprintf(key, sizeof(key), "a%03d", i);
            CB_ASSERT(zs_txn_store(live_txn, key, 4, "v", 1, 0) == ZS_OK);
        }

        /* And OVERWRITES, the one zsi_pend_set path that could free a key block
         * the arm has borrowed.  Both the key just yielded and the NEXT one:
         * D-14e step 5 advances element 0 before the record is handed over, so
         * by the time this callback runs the arm is already loaded on "k001"
         * and it is "k001"'s block, not "k000"'s, that it holds a pointer to. */
        CB_ASSERT(zs_txn_store(live_txn, "k000", 4, "V", 1, 0) == ZS_OK);
        CB_ASSERT(zs_txn_store(live_txn, "k001", 4, "V", 1, 0) == ZS_OK);
    }
    return 0;
}

static void test_txn_cursor_survives_pending_array_growth(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct pend_move_rock rock;
    const char *v;
    size_t vl;
    char key[8];
    int i;

    setup.flags = ZS_CREATE;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &live_db));

    /* Everything pending and nothing committed, so the traversal is the
     * transaction arm alone and the mechanism is not masked by a file. */
    ASSERT_OK(zs_db_begin_txn(live_db, 0, &live_txn));
    for (i = 0; i < 40; i++) {
        snprintf(key, sizeof(key), "k%03d", i);
        ASSERT_OK(zs_txn_store(live_txn, key, 4, "v", 1, 0));
    }

    memset(&rock, 0, sizeof(rock));
    rock.mutate = true;
    ASSERT_OK(zs_txn_foreach(live_txn, NULL, 0, NULL, pend_move_cb, &rock, 0));

    ASSERT(!rock.unexpected);           /* no "a" key yielded: all behind */
    ASSERT(!rock.out_of_order);         /* and none yielded twice */
    ASSERT_EQU(rock.n, 40u);
    ASSERT_STR_EQ(rock.last, "k039");

    /* The writes really landed, including the overwrite. */
    ASSERT_OK(zs_txn_fetch(live_txn, "a199", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 1u);
    ASSERT_OK(zs_txn_fetch(live_txn, "k000", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "V", 1);

    /* And a fresh traversal, with the array at its grown size and nothing
     * writing underneath it, still walks the "k" range exactly once. */
    memset(&rock, 0, sizeof(rock));
    ASSERT_OK(zs_txn_foreach(live_txn, "k", 1, NULL, pend_move_cb, &rock,
                             ZS_CURSOR_PREFIX));
    ASSERT(!rock.unexpected);
    ASSERT(!rock.out_of_order);
    ASSERT_EQU(rock.n, 40u);

    ASSERT_OK(zs_txn_abort(&live_txn));
    ASSERT_OK(zs_db_close(&live_db));
}

/* A-4a with D-14j-b: the resume point is BORROWED from the record the cursor
 * yielded, not copied, so the bytes it points into must outlive the refresh
 * that reads them.  A-4a is exactly that promise -- the cursor references every
 * file in its snapshot, and a snapshot swap RETIRES the outgoing one into the
 * cursor's hold rather than releasing it -- but the other D-14j tests cannot
 * tell a sound borrow from a lucky one, because their databases are small
 * enough that the swap never retires anything: the same file object carries
 * over and nothing is ever unmapped.
 *
 * The lever is C-4c: an immutable file is SHARED across snapshots, so a swap
 * hands the same object back and nothing is retired -- but the ACTIVE file is
 * deliberately excluded from that cache, because its index and complete
 * boundary belong to the snapshot that built them.  So every handle-live swap
 * over a live active file retires that object, and the cursor's resume key
 * points straight into it.  Drop the hold and the re-seek below reads unmapped
 * memory. */
static void test_cursor_resume_key_survives_retirement(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_cursor *c = NULL;
    struct zsi_file *old_act;
    const char *k, *v;
    size_t kl, vl;
    char seen[256] = "";

    /* A layout test, so the cascade must not merge the layout away. */
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.error = counting_error;
    ASSERT_OK(zs_db_open(dbdir, &setup, &live_db));

    /* One live unordered file holding everything when the cursor opens. */
    for (const char *p = "abcd"; *p; p++)
        ASSERT_OK(zs_db_store(live_db, p, 1, "v", 1, 0));

    /* ZS_SHARED: no write lock, so this handle can commit underneath its own
     * cursor, which is the handle-live swap (D-14j). */
    ASSERT_OK(zs_db_begin_cursor(live_db, NULL, 0, &c, ZS_SHARED));

    /* One yield, so last_key now points into the active file's mapping. */
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_EQU(kl, 1u);
    ASSERT_MEM_EQ(k, "a", 1);

    ASSERT_EQU(live_db->snap->nfiles, 1u);
    old_act = live_db->snap->files[0];
    ASSERT(zsi_file_is_unordered(old_act));
    ASSERT(old_act->base != NULL);

    /* A commit through the same handle, so the cursor's next step swaps. */
    ASSERT_OK(zs_db_store(live_db, "z", 1, "9", 1, 0));

    /* The retirement really happened: the rebuilt set holds a DIFFERENT object
     * for the active file, because C-4c carries only immutable files across.
     * (Same file on disk, so identity here is the object, not the inode or the
     * name -- D-1b keeps the generation in the header, so the name is `.current`
     * either way.)
     *
     * This is also where the bug lands, which is why it is an equality and not
     * a comment: without the hold the old object is FREED during the store
     * above, and its address is then recycled for the replacement -- so the two
     * pointers come back equal. */
    ASSERT_EQU(live_db->snap->nfiles, 1u);
    ASSERT(live_db->snap->files[0] != old_act);

    /* So the cursor's own A-4a hold is now the only thing keeping it mapped. */
    ASSERT(old_act->refcount > 0);
    ASSERT(old_act->base != NULL);

    /* This step refreshes, and re-seeks every rebuilt arm from last_key, which
     * points into old_act.  Without the hold that is a use-after-unmap; with a
     * stale or lost resume point the order below breaks instead. */
    strncat(seen, "|", sizeof(seen) - strlen(seen) - 1);
    strncat(seen, k, kl);
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        strncat(seen, "|", sizeof(seen) - strlen(seen) - 1);
        strncat(seen, k, kl);
    }
    ASSERT_STR_EQ(seen, "|a|b|c|d|z");
    ASSERT_OK(zs_cursor_abort(&c));

    ASSERT_OK(zs_db_close(&live_db));
}

/*
 * ============================================================
 * Reverse iteration (D-14k, D-14l, A-12, A-13, A-1c)
 * ============================================================
 */

/* Walk a cursor to the end, joining yielded keys with '|'. */
static void collect_cursor_keys(struct zs_cursor *c, char *out, size_t outlen)
{
    const char *k, *v;
    size_t kl, vl;
    size_t used = 0;

    out[0] = '\0';
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        if (used + kl + 2 >= outlen) break;
        if (used) out[used++] = '|';
        memcpy(out + used, k, kl);
        used += kl;
        out[used] = '\0';
    }
}

/* D-14k: a reverse walk over a database spread across every source kind --
 * in-order files, the unordered active file, and the transaction's pending
 * array -- yields exactly the forward walk, reversed, newest version of each
 * key exactly once. */
static void test_cursor_reverse_walks_everything(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    char got[4096], want[4096];
    char val[120];

    memset(val, 'v', sizeof(val));
    setup.flags = ZS_CREATE;
    setup.rollover_size = 512;          /* several generations */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 30; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, strlen(k), val, sizeof(val), 0));
        /* One key rewritten in every generation, so reverse duplicate
         * suppression has stale versions to suppress in older files. */
        ASSERT_OK(zs_db_store(db, "dup", 3, k, strlen(k), 0));
    }

    /* Pending writes: below, between, above -- and an overwrite. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "aa", 2, "p", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "k015", 4, "NEW", 3, 0));
    ASSERT_OK(zs_txn_store(txn, "zz", 2, "p", 1, 0));

    /* Expected: aa, dup, k000..k029, zz -- reversed. */
    {
        size_t used = 0;
        used += (size_t)snprintf(want + used, sizeof(want) - used, "zz");
        for (int i = 29; i >= 0; i--)
            used += (size_t)snprintf(want + used, sizeof(want) - used,
                                     "|k%03d", i);
        used += (size_t)snprintf(want + used, sizeof(want) - used, "|dup|aa");
    }

    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, ZS_REVERSE));
    /* The overwritten key yields the PENDING value: newest source wins in
     * reverse exactly as forward (D-14, G-7). */
    {
        const char *k, *v;
        size_t kl, vl;
        char sofar[4096];
        size_t used = 0;
        sofar[0] = '\0';
        while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
            if (kl == 4 && memcmp(k, "k015", 4) == 0) {
                ASSERT_EQU(vl, 3u);
                ASSERT_MEM_EQ(v, "NEW", 3);
            }
            if (used) sofar[used++] = '|';
            memcpy(sofar + used, k, kl);
            used += kl;
            sofar[used] = '\0';
        }
        snprintf(got, sizeof(got), "%s", sofar);
    }
    zs_cursor_fini(&c);

    ASSERT_STR_EQ(got, want);

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* D-14k/A-13: seek semantics -- largest <= start, SKIPROOT strictly <. */
static void test_cursor_reverse_seek_and_skiproot(void)
{
    struct zs_db *db = NULL;
    struct zs_cursor *c = NULL;
    char got[256];

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "d", 1, "4", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    /* Present key: lands on it. */
    ASSERT_OK(zs_db_begin_cursor(db, "d", 1, &c, ZS_REVERSE));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "d|b");

    /* Absent key: largest below it. */
    ASSERT_OK(zs_db_begin_cursor(db, "e", 1, &c, ZS_REVERSE));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "d|b");

    /* SKIPROOT: strictly below an exact match. */
    ASSERT_OK(zs_db_begin_cursor(db, "d", 1, &c, ZS_REVERSE | ZS_SKIPROOT));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "b");

    /* Below everything: exhausted at once. */
    ASSERT_OK(zs_db_begin_cursor(db, "a", 1, &c, ZS_REVERSE));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "");

    /* Empty start: from the last key. */
    ASSERT_OK(zs_db_begin_cursor(db, NULL, 0, &c, ZS_REVERSE));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "f|d|b");

    ASSERT_OK(zs_db_close(&db));
}

/* D-14l/G-7: a tombstone at the largest candidate consumes the key and the
 * walk moves to the next smaller live one. */
static void test_cursor_reverse_tombstones(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    char got[256];

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_db_store(db, "c", 1, "3", 1, 0));
    ASSERT_OK(zs_db_store(db, "e", 1, "5", 1, 0));

    /* The newest version of "e" is a pending deletion. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_delete(txn, "e", 1, 0));

    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, ZS_REVERSE));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "c|a");

    /* And the fetch form agrees, because it IS this walk (D-14l). */
    {
        const char *k = NULL, *v = NULL;
        size_t kl = 0, vl = 0;
        ASSERT_OK(zs_txn_fetch(txn, "e", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
        ASSERT_EQU(kl, 1u);
        ASSERT_MEM_EQ(k, "c", 1);
    }

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* D-14k: reverse prefix scans.  The byte-successor bound must be exclusive
 * and exact -- a real key equal to the successor must not surface, trailing
 * 0xFF bytes must truncate, and an all-0xFF prefix means "from the end". */
static void test_cursor_reverse_prefix(void)
{
    struct zs_db *db = NULL;
    struct zs_cursor *c = NULL;
    char got[256];

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a1", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "b1", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "b2", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "b3", 2, "x", 1, 0));
    /* "c" is exactly the byte-successor of prefix "b": the seek must be
     * exclusive of it, or the scan starts on it, fails the prefix test, and
     * reports a populated range empty. */
    ASSERT_OK(zs_db_store(db, "c", 1, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "c1", 2, "x", 1, 0));

    ASSERT_OK(zs_db_begin_cursor(db, "b", 1, &c,
                                 ZS_REVERSE | ZS_CURSOR_PREFIX));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "b3|b2|b1");

    /* An empty range is DONE at once, not an error. */
    ASSERT_OK(zs_db_begin_cursor(db, "bb", 2, &c,
                                 ZS_REVERSE | ZS_CURSOR_PREFIX));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "");

    /* A prefix ending 0xFF: the successor truncates ("a\xFF" -> "b"). */
    ASSERT_OK(zs_db_store(db, "a\xFF", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "a\xFF\x01", 3, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "a\xFFz", 3, "x", 1, 0));
    ASSERT_OK(zs_db_begin_cursor(db, "a\xFF", 2, &c,
                                 ZS_REVERSE | ZS_CURSOR_PREFIX));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "a\xFFz|a\xFF\x01|a\xFF");

    /* An all-0xFF prefix has no successor: from the end, and correct, since
     * every key above it carries it. */
    ASSERT_OK(zs_db_store(db, "\xFF\xFF", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "\xFF\xFF\x41", 3, "x", 1, 0));
    ASSERT_OK(zs_db_begin_cursor(db, "\xFF\xFF", 2, &c,
                                 ZS_REVERSE | ZS_CURSOR_PREFIX));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "\xFF\xFF\x41|\xFF\xFF");

    ASSERT_OK(zs_db_close(&db));
}

/* LONG keys that agree for their first 64 bytes.
 *
 * Every other test in the suite uses keys of a few bytes, so nothing exercised
 * the pending set with keys whose comparisons cannot be settled early -- and
 * Cyrus's mailboxes.db is full of exactly that shape ("Ndomain!user." repeated
 * over long runs), as is any index whose keys share a structured head.
 *
 * It exists because a bounded inline prefix was tried in the pending set and
 * measured against full inlining: the alternatives all turn on which
 * comparisons a stored prefix can decide, and this is the case that separates
 * them.  The design was not kept; the coverage gap it exposed was real either
 * way.  Covered: two long keys differing only past 64 bytes, a long key that is
 * a strict PREFIX of another (F-11a's length tie-break at length), and keys at
 * 64 and 65 bytes. */
static void test_txn_long_keys_sharing_a_head(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    char got[512] = "";
    char p[128], a[192], b[192], pre[192], at64[192], at65[192];

    memset(p, 'P', 80); p[80] = '\0';               /* past the 64-byte bound */
    snprintf(a,   sizeof(a),   "%sAAA", p);          /* differ at byte 80 */
    snprintf(b,   sizeof(b),   "%sBBB", p);
    snprintf(pre, sizeof(pre), "%s",    p);          /* a strict prefix of both */
    memset(at64, 'Q', 64); at64[64] = '\0';         /* exactly the bound */
    memset(at65, 'Q', 65); at65[65] = '\0';         /* one past it */

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    /* Stored out of order, so the ordering below is the structure's doing. */
    ASSERT_OK(zs_txn_store(txn, b,     strlen(b),     "b",  1, 0));
    ASSERT_OK(zs_txn_store(txn, at65,  strlen(at65),  "65", 2, 0));
    ASSERT_OK(zs_txn_store(txn, pre,   strlen(pre),   "p",  1, 0));
    ASSERT_OK(zs_txn_store(txn, at64,  strlen(at64),  "64", 2, 0));
    ASSERT_OK(zs_txn_store(txn, a,     strlen(a),     "a",  1, 0));

    /* Every one is found -- a lookup that mis-decided a tie would miss. */
    ASSERT_OK(zs_txn_fetch(txn, a,    strlen(a),    NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "a", 1);
    ASSERT_OK(zs_txn_fetch(txn, b,    strlen(b),    NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "b", 1);
    ASSERT_OK(zs_txn_fetch(txn, pre,  strlen(pre),  NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "p", 1);
    ASSERT_OK(zs_txn_fetch(txn, at64, strlen(at64), NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "64", 2);
    ASSERT_OK(zs_txn_fetch(txn, at65, strlen(at65), NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "65", 2);

    /* A key that shares the whole prefix but was never stored must MISS. */
    {
        char miss[64];
        snprintf(miss, sizeof(miss), "%sCCC", p);
        ASSERT_EQ(zs_txn_fetch(txn, miss, strlen(miss), NULL, NULL, &v, &vl, 0),
                  ZS_NOTFOUND);
    }

    /* F-11a order: P*40 < P*40+"AAA" < P*40+"BBB" < Q*32 < Q*33. */
    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, 0));
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        if (got[0]) strncat(got, "|", sizeof(got) - strlen(got) - 1);
        strncat(got, v, sizeof(got) - strlen(got) - 1);
    }
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "p|a|b|64|65");

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A CALLER's comparator, over keys past the pending set's inlined prefix.
 *
 * Every other custom-comparator test opens a database and checks the name
 * agreement; none of them STORES several long keys and walks them back, so
 * nothing checked that the pending set is built and searched in the caller's
 * order rather than in byte order.  alt_compar reverses the order outright, so
 * anything that quietly assumed F-11a here would come out backwards.
 *
 * The keys differ inside their first bytes deliberately: a specialisation of
 * the built-in order can only go wrong where it decides something, so keys that
 * agree for a long head would pass either way. */
static void test_txn_caller_comparator_long_keys(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    char got[256] = "";
    char a[128], b[128], d[128];

    /* Long keys that differ INSIDE the inlined prefix, which is the only place
     * the shortcut decides anything: keys agreeing past the bound fall through
     * to the full key and the caller's comparator either way, so a test built
     * from those would pass whether the specialisation was guarded or not. */
    memset(a, 'A', 80); a[80] = '\0';
    memset(b, 'B', 80); b[80] = '\0';
    memset(d, 'D', 80); d[80] = '\0';

    setup.flags = ZS_CREATE;
    setup.compar = alt_compar;
    setup.compar_name = "reverse";
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    ASSERT_OK(zs_txn_store(txn, b, strlen(b), "b", 1, 0));
    ASSERT_OK(zs_txn_store(txn, d, strlen(d), "d", 1, 0));
    ASSERT_OK(zs_txn_store(txn, a, strlen(a), "a", 1, 0));

    ASSERT_OK(zs_txn_fetch(txn, a, strlen(a), NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "a", 1);
    ASSERT_OK(zs_txn_fetch(txn, d, strlen(d), NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "d", 1);

    /* Reversed: D before B before A. */
    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, 0));
    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        if (got[0]) strncat(got, "|", sizeof(got) - strlen(got) - 1);
        strncat(got, v, sizeof(got) - strlen(got) - 1);
    }
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "d|b|a");

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* The same exclusive bound, on the TRANSACTION arm.
 *
 * test_cursor_reverse_prefix above stores everything first, so its keys are in
 * files and only the file arms' seek is exercised.  The transaction arm is a
 * separate seek with its own inclusivity, and resuming past a yielded key is
 * handled a level up in zsi_cursor_reseek_arm -- so the only thing that
 * exercises the bound itself is this: a reverse prefix scan whose PENDING
 * records include the prefix's byte-successor as a real key. */
static void test_txn_cursor_reverse_prefix_bound(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    char got[256];

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    ASSERT_OK(zs_txn_store(txn, "b1", 2, "x", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "b2", 2, "x", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "b3", 2, "x", 1, 0));
    /* "c" is the byte-successor of prefix "b".  An inclusive seek starts the
     * scan ON it, the prefix test then fails, and a populated range reports
     * empty. */
    ASSERT_OK(zs_txn_store(txn, "c", 1, "x", 1, 0));

    ASSERT_OK(zs_txn_begin_cursor(txn, "b", 1, &c,
                                  ZS_REVERSE | ZS_CURSOR_PREFIX));
    collect_cursor_keys(c, got, sizeof(got));
    zs_cursor_fini(&c);
    ASSERT_STR_EQ(got, "b3|b2|b1");

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* D-14j reversed: a store BELOW the position -- ahead, in the direction of
 * travel -- is yielded when reached; one above is already passed and is not.
 * And write-through at the current position works (A-13). */
static void test_cursor_reverse_own_writes(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "d", 1, "4", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, ZS_REVERSE));

    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "f", 1);

    /* Ahead (below): seen later.  Behind (above): never. */
    ASSERT_OK(zs_txn_store(txn, "c", 1, "3", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "z", 1, "26", 2, 0));

    /* Write-through at the current position. */
    ASSERT_OK(zs_cursor_replace(c, "SIX", 3, 0));

    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "d", 1);
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "c", 1);
    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "b", 1);
    ASSERT_EQ(zs_cursor_next(c, &k, &kl, &v, &vl), ZS_DONE);

    zs_cursor_fini(&c);

    /* The replace took. */
    ASSERT_OK(zs_txn_fetch(txn, "f", 1, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 3u);
    ASSERT_MEM_EQ(v, "SIX", 3);

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-13/A-12: the rejected compositions are usage errors, not half-support. */
static void test_reverse_rejected_compositions(void)
{
    struct zs_db *db = NULL;
    struct zs_cursor *c = NULL;
    const char *v;
    size_t vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "a", 1, "1", 1, 0));

    ASSERT_EQ(zs_db_begin_cursor(db, NULL, 0, &c,
                                 ZS_REVERSE | ZS_CURSOR_LIVE), ZS_BADUSAGE);
    ASSERT_NULL(c);

    ASSERT_EQ(zs_db_foreach(db, NULL, 0, NULL, api_keys_cb, NULL,
                            ZS_REVERSE), ZS_BADUSAGE);

    ASSERT_EQ(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl,
                          ZS_FETCHNEXT | ZS_FETCHPREV), ZS_BADUSAGE);

    ASSERT_OK(zs_db_close(&db));
}

/* A-12: predecessor fetch, both forms, all the bounds. */
static void test_fetchprev_basic(void)
{
    struct zs_db *db = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "d", 1, "4", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    /* Exact hit. */
    ASSERT_OK(zs_db_fetch(db, "d", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "d", 1);
    ASSERT_MEM_EQ(v, "4", 1);

    /* Exact hit on the newest store, which sits in the index's DELTA rather
     * than its base -- the two sides take separate inclusive branches. */
    ASSERT_OK(zs_db_fetch(db, "f", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "f", 1);

    /* Gap: the largest below. */
    ASSERT_OK(zs_db_fetch(db, "e", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "d", 1);

    /* Above everything. */
    ASSERT_OK(zs_db_fetch(db, "z", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "f", 1);

    /* Strictly less. */
    ASSERT_OK(zs_db_fetch(db, "d", 1, &k, &kl, &v, &vl,
                          ZS_FETCHPREV | ZS_SKIPROOT));
    ASSERT_MEM_EQ(k, "b", 1);

    /* Below everything. */
    ASSERT_EQ(zs_db_fetch(db, "a", 1, &k, &kl, &v, &vl, ZS_FETCHPREV),
              ZS_NOTFOUND);

    /* And FETCHNEXT/FETCHPREV are inverses across a gap. */
    ASSERT_OK(zs_db_fetch(db, "c", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "d", 1);
    ASSERT_OK(zs_db_fetch(db, "c", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "b", 1);

    ASSERT_OK(zs_db_close(&db));
}

/* A-12: the transactional form sees pending writes -- a pending store can BE
 * the answer, and a pending delete pushes the answer lower (A-1a, D-14l). */
static void test_fetchprev_sees_txn_writes(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "d", 1, "4", 1, 0));

    ASSERT_OK(zs_txn_fetch(txn, "e", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "d", 1);           /* the pending store IS the answer */

    ASSERT_OK(zs_txn_delete(txn, "f", 1, 0));
    ASSERT_OK(zs_txn_fetch(txn, "z", 1, &k, &kl, &v, &vl, ZS_FETCHPREV));
    ASSERT_MEM_EQ(k, "d", 1);           /* the tombstone pushed it lower */

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-12: bare ZS_FETCHNEXT is the inclusive-≥ point form -- the family's
 * fourth cell, spelled by moving the strictness to ZS_SKIPROOT exactly as
 * FETCHPREV and a cursor seek already do. */
static void test_fetchnext_inclusive(void)
{
    struct zs_db *db = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "d", 1, "4", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    /* Exact hit: inclusive means the key itself. */
    ASSERT_OK(zs_db_fetch(db, "d", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "d", 1);
    ASSERT_MEM_EQ(v, "4", 1);

    /* Exact hit on the newest store, which sits in the index's DELTA rather
     * than its base -- the two sides take separate inclusive branches. */
    ASSERT_OK(zs_db_fetch(db, "f", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "f", 1);

    /* Gap: the smallest above. */
    ASSERT_OK(zs_db_fetch(db, "c", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "d", 1);

    /* Below everything. */
    ASSERT_OK(zs_db_fetch(db, "a", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "b", 1);

    /* Above everything. */
    ASSERT_EQ(zs_db_fetch(db, "z", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT),
              ZS_NOTFOUND);

    /* SKIPROOT: strictly greater -- the old bare-FETCHNEXT answer. */
    ASSERT_OK(zs_db_fetch(db, "d", 1, &k, &kl, &v, &vl,
                          ZS_FETCHNEXT | ZS_SKIPROOT));
    ASSERT_MEM_EQ(k, "f", 1);

    ASSERT_OK(zs_db_close(&db));
}

/* A-12: the transactional form sees pending writes -- a pending store can BE
 * the inclusive answer (A-1a, G-7). */
static void test_fetchnext_inclusive_sees_txn_writes(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "b", 1, "2", 1, 0));
    ASSERT_OK(zs_db_store(db, "f", 1, "6", 1, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "d", 1, "4", 1, 0));

    ASSERT_OK(zs_txn_fetch(txn, "d", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "d", 1);           /* the pending store IS the answer */

    ASSERT_OK(zs_txn_delete(txn, "d", 1, 0));
    ASSERT_OK(zs_txn_fetch(txn, "c", 1, &k, &kl, &v, &vl, ZS_FETCHNEXT));
    ASSERT_MEM_EQ(k, "f", 1);           /* the tombstone pushed it higher */

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-1c: several cursors on one transaction at once, directions mixed,
 * interleaved -- the sqlite shape, one query over several trees. */
static void test_txn_many_cursors(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *cf = NULL, *cr = NULL, *cp = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    for (int t = 1; t <= 3; t++)
        for (int i = 1; i <= 3; i++) {
            char key[8];
            snprintf(key, sizeof(key), "t%d|%d", t, i);
            ASSERT_OK(zs_db_store(db, key, strlen(key), key, strlen(key), 0));
        }

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    /* A pending write in one tree, visible to that tree's cursors only. */
    ASSERT_OK(zs_txn_store(txn, "t2|4", 4, "t2|4", 4, 0));

    ASSERT_OK(zs_txn_begin_cursor(txn, "t1|", 3, &cf, ZS_CURSOR_PREFIX));
    ASSERT_OK(zs_txn_begin_cursor(txn, "t2|", 3, &cr,
                                  ZS_REVERSE | ZS_CURSOR_PREFIX));
    ASSERT_OK(zs_txn_begin_cursor(txn, "t3|", 3, &cp, ZS_CURSOR_PREFIX));

    /* Interleave: each cursor holds its own position. */
    ASSERT_OK(zs_cursor_next(cf, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t1|1", 4);
    ASSERT_OK(zs_cursor_next(cr, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t2|4", 4);        /* the pending store, first in reverse */
    ASSERT_OK(zs_cursor_next(cp, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t3|1", 4);
    ASSERT_OK(zs_cursor_next(cr, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t2|3", 4);
    ASSERT_OK(zs_cursor_next(cf, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t1|2", 4);
    ASSERT_OK(zs_cursor_next(cr, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t2|2", 4);
    ASSERT_OK(zs_cursor_next(cr, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t2|1", 4);
    ASSERT_EQ(zs_cursor_next(cr, &k, &kl, &v, &vl), ZS_DONE);
    ASSERT_OK(zs_cursor_next(cf, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "t1|3", 4);
    ASSERT_EQ(zs_cursor_next(cf, &k, &kl, &v, &vl), ZS_DONE);

    zs_cursor_fini(&cf);
    zs_cursor_fini(&cr);
    zs_cursor_fini(&cp);
    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-1c/A-4: writes through the transaction while cursors are open never
 * invalidate pointers already returned.  The INSERT INTO t SELECT FROM t
 * shape: copy each row of one tree into another, mid-walk, then check every
 * saved pointer still reads back. */
static void test_txn_insert_select_self(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;
    const char *saved_k[8], *saved_v[8];
    size_t saved_kl[8], saved_vl[8];
    char expect_k[8][16], expect_v[8][16];
    size_t n = 0;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    for (int i = 1; i <= 4; i++) {
        char key[16], val[16];
        snprintf(key, sizeof(key), "t|%d", i);
        snprintf(val, sizeof(val), "row%d", i);
        ASSERT_OK(zs_db_store(db, key, strlen(key), val, strlen(val), 0));
    }

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_begin_cursor(txn, "t|", 2, &c, ZS_CURSOR_PREFIX));

    while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) {
        char copy[16];
        ASSERT(n < 8);
        /* Save the returned pointers AND an owned copy of what they said. */
        saved_k[n] = k; saved_kl[n] = kl;
        saved_v[n] = v; saved_vl[n] = vl;
        memcpy(expect_k[n], k, kl); expect_k[n][kl] = '\0';
        memcpy(expect_v[n], v, vl); expect_v[n][vl] = '\0';
        n++;

        /* The copy lands in another tree, through the same transaction,
         * while this cursor (and its yielded pointers) are live. */
        snprintf(copy, sizeof(copy), "u|%.*s", (int)(kl - 2), k + 2);
        ASSERT_OK(zs_txn_store(txn, copy, strlen(copy), v, vl, 0));
    }
    ASSERT_EQU(n, 4u);

    /* Every pointer handed out along the way still reads back (A-4). */
    for (size_t i = 0; i < n; i++) {
        ASSERT_MEM_EQ(saved_k[i], expect_k[i], saved_kl[i]);
        ASSERT_MEM_EQ(saved_v[i], expect_v[i], saved_vl[i]);
    }

    /* And the copies exist. */
    ASSERT_OK(zs_txn_fetch(txn, "u|3", 3, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 4u);
    ASSERT_MEM_EQ(v, "row3", 4);

    zs_cursor_fini(&c);
    ASSERT_OK(zs_txn_commit(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* D-14k at the arm: the reverse index cursor over a REAL base+delta split.
 * A key committed in an earlier session lands in the base at open; a store
 * through the open handle folds into the delta (D-13b) -- so the same key
 * sits in both arrays with the delta newer, which is the shape the reverse
 * tie and inclusive-seek rules exist for.  Driven on the internals, because
 * the API-level tests' write-transaction begins rebuild the snapshot and
 * quietly empty the delta, which is how two mutants here escaped the suite. */
static void test_index_cur_reverse_base_delta(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zsi_file *act;
    struct zsi_index_cur ic;
    struct zsi_rec r;
    size_t off;

    setup.flags = ZS_CREATE;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "aa", 2, "x", 1, 0));
    ASSERT_OK(zs_db_store(db, "dup", 3, "1", 1, 0));
    ASSERT_OK(zs_db_close(&db));

    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_store(db, "dup", 3, "2", 1, 0));

    act = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(act);
    ASSERT_NOT_NULL(act->index);
    /* The tie is real: dup's old record in the base, its new one in the
     * delta.  If a snapshot rebuild ever collapses this, the test must say
     * so rather than pass on an empty delta. */
    ASSERT(act->index->nbase >= 2);
    ASSERT_EQU(act->index->ndelta, 1u);

    /* From the end: dup's NEWEST record first (delta wins the tie)... */
    zsi_index_cur_seek_last(act->index, &ic);
    ASSERT_OK(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off));
    ASSERT_MEM_EQ(r.key, "dup", 3);
    ASSERT_MEM_EQ(r.val, "2", 1);

    /* ...and stepping past it consumes BOTH sides, so the base's stale copy
     * never surfaces: the next record is aa, not dup again. */
    zsi_index_cur_prev(act->index, db->compar, &ic);
    ASSERT_OK(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off));
    ASSERT_MEM_EQ(r.key, "aa", 2);
    zsi_index_cur_prev(act->index, db->compar, &ic);
    ASSERT_EQ(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off),
              ZS_DONE);

    /* An inclusive reverse seek lands ON the key, and on the DELTA's record. */
    zsi_index_cur_seek_rev(act->index, db->compar, "dup", 3, true, &ic);
    ASSERT_OK(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off));
    ASSERT_MEM_EQ(r.key, "dup", 3);
    ASSERT_MEM_EQ(r.val, "2", 1);

    /* An exclusive one lands strictly below it. */
    zsi_index_cur_seek_rev(act->index, db->compar, "dup", 3, false, &ic);
    ASSERT_OK(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off));
    ASSERT_MEM_EQ(r.key, "aa", 2);

    /* And the same on a key that is in the BASE ONLY.
     *
     * Seeking on "dup" cannot test the base's half of the inclusive rule: the
     * key is in both arrays and the delta's record wins the tie either way, so
     * breaking the base branch changes nothing observable.  "aa" was committed
     * in the first session and never rewritten, so it exists only in the base
     * -- and there is nothing below it, so getting the inclusive rule wrong
     * there does not yield the wrong record, it yields NOTHING.  That is what
     * "index: reverse inclusive seek dropped (base)" does, and what went
     * uncaught until this. */
    zsi_index_cur_seek_rev(act->index, db->compar, "aa", 2, true, &ic);
    ASSERT_OK(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off));
    ASSERT_MEM_EQ(r.key, "aa", 2);
    ASSERT_MEM_EQ(r.val, "x", 1);

    /* Exclusive of the lowest key really is the end. */
    zsi_index_cur_seek_rev(act->index, db->compar, "aa", 2, false, &ic);
    ASSERT_EQ(zsi_index_cur_get_rev(act->index, db->compar, &ic, &r, &off),
              ZS_DONE);

    ASSERT_OK(zs_db_close(&db));
}

/* C-8/F-21, the writer side: records were STREAMED before the abort, so the
 * ROLLBACK terminator is the only thing keeping a later commit's span from
 * enclosing them.  Both ways of getting it wrong are fatal: no terminator
 * corrupts or resurrects, a COMMIT terminator resurrects outright. */
static void test_stream_abort_writes_rollback(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *v = NULL;
    size_t vl = 0;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "dead1", 5, "x", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "dead2", 5, "y", 1, 0));
    ASSERT_OK(zs_txn_abort(&txn));

    ASSERT_EQ(zs_db_fetch(db, "dead1", 5, NULL, NULL, &v, &vl, 0),
              ZS_NOTFOUND);

    /* A later commit into the same file: its span begins after the rolled-
     * back one and must not disturb or be disturbed by it. */
    ASSERT_OK(zs_db_store(db, "live", 4, "z", 1, 0));
    ASSERT_OK(zs_db_fetch(db, "live", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_EQ(zs_db_fetch(db, "dead2", 5, NULL, NULL, &v, &vl, 0),
              ZS_NOTFOUND);
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));

    /* And through a fresh replay (F-25 skipping the ROLLBACK span). */
    db = open_db(0);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_fetch(db, "live", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "z", 1);
    ASSERT_EQ(zs_db_fetch(db, "dead1", 5, NULL, NULL, &v, &vl, 0),
              ZS_NOTFOUND);
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* The point of streaming (C-8): transaction memory is O(keys), and the
 * mapping list that backs A-4 grows LOGARITHMICALLY -- so this asserts the
 * map count against the bound, exercises the chunk-flush boundary and the
 * bigger-than-the-chunk direct write, and holds a pointer from before
 * megabytes of later writes to prove growth never invalidates it. */
static void test_stream_bounded_memory(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *v1 = NULL, *v = NULL;
    size_t vl1 = 0, vl = 0;
    char big[8192];
    char *huge;

    memset(big, 'v', sizeof(big));
    huge = malloc(200 * 1024);
    ASSERT_NOT_NULL(huge);
    memset(huge, 'H', 200 * 1024);

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    ASSERT_OK(zs_txn_store(txn, "early", 5, "EARLY", 5, 0));
    ASSERT_OK(zs_txn_fetch(txn, "early", 5, NULL, NULL, &v1, &vl1, 0));
    ASSERT_EQU(vl1, 5u);

    for (int i = 0; i < 600; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%04d", i);
        ASSERT_OK(zs_txn_store(txn, k, strlen(k), big, sizeof(big), 0));
    }
    /* Bigger than the chunk buffer: the direct-write path. */
    ASSERT_OK(zs_txn_store(txn, "huge", 4, huge, 200 * 1024, 0));

    /* Read back from the middle, which maps the grown file. */
    ASSERT_OK(zs_txn_fetch(txn, "k0300", 5, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(big));

    /* ~5MB streamed: the mapping list is logarithmic, not linear. */
    ASSERT(txn->nmaps <= 16);

    /* And the pointer from before all of it still reads back (A-4): the
     * mapping it points into was never unmapped by the growth. */
    ASSERT_MEM_EQ(v1, "EARLY", 5);

    ASSERT_OK(zs_txn_commit(&txn));

    ASSERT_OK(zs_db_fetch(db, "k0599", 5, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, sizeof(big));
    ASSERT_OK(zs_db_fetch(db, "huge", 4, NULL, NULL, &v, &vl, 0));
    ASSERT_EQU(vl, 200u * 1024u);
    ASSERT_MEM_EQ(v, huge, 200 * 1024);

    free(huge);
    ASSERT_OK(zs_db_close(&db));
}

/* A-4: a pointer returned for the transaction's OWN pending record survives a
 * later store to the same key.  Freeing the old value buffer in place would
 * leave every earlier fetch of that key dangling, which a caller holding a
 * pointer across an overwrite -- an undo log, say -- would find the hard
 * way.
 *
 * The second store of a same-sized value is the clobber: the allocator hands
 * the just-freed chunk straight back, so under the bug the saved pointer
 * reads the NEW bytes -- deterministic enough for a plain build, and a hard
 * use-after-free under ASan either way. */
static void test_txn_fetch_survives_overwrite(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *v1 = NULL, *v2 = NULL, *v3 = NULL;
    size_t vl1 = 0, vl2 = 0, vl3 = 0;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    ASSERT_OK(zs_txn_store(txn, "k", 1, "first", 5, 0));
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v1, &vl1, 0));
    ASSERT_EQU(vl1, 5u);

    /* Overwrite, then feed the allocator a same-sized value that would land
     * in the freed chunk. */
    ASSERT_OK(zs_txn_store(txn, "k", 1, "SECOND", 6, 0));
    ASSERT_OK(zs_txn_store(txn, "x", 1, "CLOB!", 5, 0));

    ASSERT_MEM_EQ(v1, "first", 5);      /* A-4: still the value it returned */

    /* A deletion retires the buffer the same way. */
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v2, &vl2, 0));
    ASSERT_EQU(vl2, 6u);
    ASSERT_OK(zs_txn_delete(txn, "k", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "y", 1, "CLOB2!", 6, 0));
    ASSERT_MEM_EQ(v2, "SECOND", 6);

    /* And the transaction's CURRENT view moved on regardless. */
    ASSERT_EQ(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v3, &vl3, 0),
              ZS_NOTFOUND);

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-13: reverse inherits A-4 unchanged -- pointers a reverse cursor returned
 * survive later writes through the same transaction. */
static void test_reverse_a4_lifetime(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;
    const char *k, *v;
    size_t kl, vl;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "m", 1, "13", 2, 0));
    ASSERT_OK(zs_db_store(db, "n", 1, "14", 2, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_begin_cursor(txn, NULL, 0, &c, ZS_REVERSE));

    ASSERT_OK(zs_cursor_next(c, &k, &kl, &v, &vl));
    ASSERT_MEM_EQ(k, "n", 1);

    ASSERT_OK(zs_txn_store(txn, "a", 1, "1", 1, 0));
    ASSERT_OK(zs_txn_store(txn, "z", 1, "26", 2, 0));

    /* The yielded pointers still read back after the stores. */
    ASSERT_MEM_EQ(k, "n", 1);
    ASSERT_MEM_EQ(v, "14", 2);

    zs_cursor_fini(&c);
    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-4b: what ZS_EPHEMERAL is FOR.  A read of a record this transaction just
 * stored must otherwise see it in the file, which forces out the chunk the
 * writer was filling -- one write(2) per record instead of one per 64KB.  The
 * observable is txn->flushed: with the flag it stays where the stores left it,
 * because every read is answered out of the buffer.
 *
 * The durable half of the test is not decoration.  Without it a chunk-serving
 * branch that never fires still passes, since both runs would simply flush. */
static void test_ephemeral_avoids_the_flush(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    size_t durable_grew, ephemeral_grew, ephemeral_buffered;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);

    /* Measured from the STREAM base, not from zero: flushed starts at the
     * active file's existing size, which the first store fills in and each
     * run inherits from the one before. */

    /* Durable: each read-back drags the chunk into the file. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "key00000", 8, "value", 5, 0));
    size_t dbase = txn->flushed;
    for (int i = 1; i < 200; i++) {
        char k[16];
        const char *v = NULL;
        size_t kl = (size_t)snprintf(k, sizeof(k), "key%05d", i), vl = 0;
        ASSERT_OK(zs_txn_store(txn, k, kl, "value", 5, 0));
        ASSERT_OK(zs_txn_fetch(txn, k, kl, NULL, NULL, &v, &vl, 0));
        ASSERT_MEM_EQ(v, "value", 5);
    }
    durable_grew = txn->flushed - dbase;
    ASSERT_OK(zs_txn_abort(&txn));

    /* Ephemeral: identical loop, and nothing reaches the file at all.  200
     * records of this size are far short of the 64KB chunk, so a correct
     * implementation flushes not once. */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "key00000", 8, "value", 5, 0));
    size_t ebase = txn->flushed;
    for (int i = 1; i < 200; i++) {
        char k[16];
        const char *v = NULL;
        size_t kl = (size_t)snprintf(k, sizeof(k), "key%05d", i), vl = 0;
        ASSERT_OK(zs_txn_store(txn, k, kl, "value", 5, 0));
        ASSERT_OK(zs_txn_fetch(txn, k, kl, NULL, NULL, &v, &vl,
                               ZS_EPHEMERAL));
        ASSERT_MEM_EQ(v, "value", 5);
    }
    ephemeral_grew = txn->flushed - ebase;
    ephemeral_buffered = txn->wsize - txn->flushed;
    ASSERT_OK(zs_txn_abort(&txn));

    ASSERT_EQU(ephemeral_grew, 0u);        /* not one write(2) */
    ASSERT(durable_grew > 0);              /* and the durable half did flush */
    ASSERT(ephemeral_buffered > 0);        /* the records really were made */

    /* A CONDITIONAL store is read-after-write on every call, and its probe
     * wants the answer rather than the bytes -- so it takes A-4b internally
     * whether the caller asked or not (nothing escapes zs_txn_store). */
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_OK(zs_txn_store(txn, "c00000", 6, "v", 1, 0));
    size_t cbase = txn->flushed;
    for (int i = 1; i < 200; i++) {
        char k[16];
        size_t kl = (size_t)snprintf(k, sizeof(k), "c%05d", i);
        ASSERT_OK(zs_txn_store(txn, k, kl, "v", 1, ZS_IFNOTEXIST));
        ASSERT_EQ(zs_txn_store(txn, k, kl, "v", 1, ZS_IFNOTEXIST), ZS_EXISTS);
    }
    ASSERT_EQU(txn->flushed - cbase, 0u);
    ASSERT_OK(zs_txn_abort(&txn));

    ASSERT_OK(zs_db_close(&db));
}

/* A lookup that MISSES inside a write transaction must not flush the writer's
 * chunk.  The match is decided against the pending array's own key, so no
 * record is materialised unless it is going to be returned.
 *
 * This is the read-before-insert shape: a caller probing for a key it is about
 * to store misses every time.  Materialising the neighbouring record to compare
 * it would flush the chunk on every one of those misses, so a bulk load would
 * issue a write(2) per row -- which no store-only benchmark can see.
 *
 * Deliberately WITHOUT ZS_EPHEMERAL: the flag covers reads that hit, and this
 * half must hold for a caller that never asked for the weaker lifetime. */
static void test_txn_miss_does_not_flush(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *v = NULL;
    size_t vl = 0;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    ASSERT_OK(zs_txn_store(txn, "key00000", 8, "value", 5, 0));
    size_t base = txn->flushed;

    /* Probe-then-insert, exactly as a btree layer does. */
    for (int i = 1; i < 200; i++) {
        char k[16];
        size_t kl = (size_t)snprintf(k, sizeof(k), "key%05d", i);
        ASSERT_EQ(zs_txn_fetch(txn, k, kl, NULL, NULL, &v, &vl, 0),
                  ZS_NOTFOUND);
        ASSERT_OK(zs_txn_store(txn, k, kl, "value", 5, 0));
    }
    ASSERT_EQU(txn->flushed - base, 0u);

    /* A miss below every pending key, and one above them all: the seek lands
     * off each end of the array, which must also touch nothing. */
    ASSERT_EQ(zs_txn_fetch(txn, "aaa", 3, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_EQ(zs_txn_fetch(txn, "zzz", 3, NULL, NULL, &v, &vl, 0), ZS_NOTFOUND);
    ASSERT_EQU(txn->flushed - base, 0u);

    /* And a HIT still returns the right bytes -- the load has to happen when
     * the record is actually wanted.  Without ZS_EPHEMERAL that one does
     * flush, which is A-4's promise being kept, not a regression. */
    ASSERT_OK(zs_txn_fetch(txn, "key00007", 8, NULL, NULL, &v, &vl, 0));
    ASSERT_MEM_EQ(v, "value", 5);

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* A-4b: the weaker lifetime is the ONLY difference.  An ephemeral fetch has to
 * answer exactly what a durable one would, including for the cases where the
 * record is not sitting conveniently in the chunk: one already flushed out of
 * it, an overwrite, a tombstone, and a key that lives in a committed file
 * rather than in this transaction at all. */
static void test_ephemeral_matches_durable(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    const char *v = NULL;
    size_t vl = 0;
    char big[70000];

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "committed", 9, "old", 3, 0));

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));

    /* A committed record: no chunk involved, and the flag changes nothing. */
    ASSERT_OK(zs_txn_fetch(txn, "committed", 9, NULL, NULL, &v, &vl,
                           ZS_EPHEMERAL));
    ASSERT_MEM_EQ(v, "old", 3);

    /* In the chunk. */
    ASSERT_OK(zs_txn_store(txn, "k", 1, "first", 5, 0));
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v, &vl, ZS_EPHEMERAL));
    ASSERT_MEM_EQ(v, "first", 5);

    /* Overwritten in the chunk: the newest version wins, as ever (D-17b). */
    ASSERT_OK(zs_txn_store(txn, "k", 1, "SECOND", 6, 0));
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v, &vl, ZS_EPHEMERAL));
    ASSERT_MEM_EQ(v, "SECOND", 6);

    /* Pushed OUT of the chunk by a record too big for it: "k" is now in the
     * file, so the ephemeral read has to fall through to the mapping. */
    memset(big, 'B', sizeof(big));
    ASSERT_OK(zs_txn_store(txn, "big", 3, big, sizeof(big), 0));
    ASSERT_OK(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v, &vl, ZS_EPHEMERAL));
    ASSERT_MEM_EQ(v, "SECOND", 6);
    ASSERT_OK(zs_txn_fetch(txn, "big", 3, NULL, NULL, &v, &vl, ZS_EPHEMERAL));
    ASSERT_EQU(vl, sizeof(big));
    ASSERT_MEM_EQ(v, big, sizeof(big));

    /* A tombstone reads as absent either way. */
    ASSERT_OK(zs_txn_delete(txn, "k", 1, 0));
    ASSERT_EQ(zs_txn_fetch(txn, "k", 1, NULL, NULL, &v, &vl, ZS_EPHEMERAL),
              ZS_NOTFOUND);

    /* And the committed record is still reachable under the flag after all
     * that streaming. */
    ASSERT_OK(zs_txn_fetch(txn, "committed", 9, NULL, NULL, &v, &vl,
                           ZS_EPHEMERAL));
    ASSERT_MEM_EQ(v, "old", 3);

    /* A-12's forms take it too, through the throwaway cursor they open and
     * free inside the call (D-14l).  Their answers must not move either. */
    const char *k = NULL;
    size_t kl = 0;
    ASSERT_OK(zs_txn_store(txn, "m", 1, "mid", 3, 0));
    ASSERT_OK(zs_txn_fetch(txn, "l", 1, &k, &kl, &v, &vl,
                           ZS_FETCHNEXT | ZS_EPHEMERAL));
    ASSERT_MEM_EQ(k, "m", 1);
    ASSERT_MEM_EQ(v, "mid", 3);
    ASSERT_OK(zs_txn_fetch(txn, "n", 1, &k, &kl, &v, &vl,
                           ZS_FETCHPREV | ZS_EPHEMERAL));
    ASSERT_MEM_EQ(k, "m", 1);
    ASSERT_MEM_EQ(v, "mid", 3);

    ASSERT_OK(zs_txn_abort(&txn));
    ASSERT_OK(zs_db_close(&db));
}

/* D-9d / A-15: rollover_txns seals a file that has taken too many SPANS, not
 * enough bytes.  rollover_size alone cannot see this -- 8 tiny commits are
 * nowhere near 2MB -- and the cost being bounded is the replay a rebuild pays,
 * which is linear in spans.
 *
 * Also, and not separably, this pins the count surviving the D-13b fold: a sole
 * writer never replays its own active file, so if the commit-site fold did not
 * carry nspans forward the count would sit at zero forever and nothing here
 * would ever seal.  ZS_NOAUTOREPACK because the subject is a file layout. */
static void test_rollover_txns_seals_on_span_count(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    clear_db();
    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_txns = 8;
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    /* Seven spans: under the bound, so still one unordered file, and the count
     * is visibly tracking. */
    for (int i = 0; i < 7; i++) {
        char k[16];
        size_t kl = (size_t)snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, kl, "v", 1, 0));
    }
    {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        ASSERT_NOT_NULL(act);
        ASSERT(zsi_file_is_unordered(act));
        ASSERT_EQU(act->nspans, 7u);
        ASSERT(act->size < db->rollover_size);   /* nowhere near the bytes */
    }

    /* The eighth reaches the bound and the commit seals in place (D-25d). */
    ASSERT_OK(zs_db_store(db, "k007", 4, "v", 1, 0));
    ASSERT_NULL(zsi_snapshot_active(db->snap));
    ASSERT_EQU(db->snap->nfiles, 1u);
    ASSERT(!zsi_file_is_unordered(db->snap->files[0]));
    ASSERT(db->snap->files[0]->size < db->rollover_size);

    /* Every record survived the seal. */
    for (int i = 0; i < 8; i++) {
        char k[16];
        const char *v = NULL;
        size_t kl = (size_t)snprintf(k, sizeof(k), "k%03d", i), vl = 0;
        ASSERT_OK(zs_db_fetch(db, k, kl, NULL, NULL, &v, &vl, 0));
        ASSERT_MEM_EQ(v, "v", 1);
    }
    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* D-9d: the count is over the REPLAY WINDOW, not over the file.  A published
 * pointer table moves the window's base, so a reader seeded from it replays
 * only what came after -- and a writer counting the file instead would seal
 * files whose rebuild was already cheap.
 *
 * With a threshold small enough to publish constantly, the window keeps
 * resetting and the same 8-span bound is never reached, though the file
 * accumulates far more than 8 spans. */
static void test_rollover_txns_counts_the_replay_window(void)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char cachedir[PATH_MAX];

    clear_db();
    idxcache_mkdir(cachedir, sizeof(cachedir));

    setup.flags = ZS_CREATE | ZS_NOAUTOREPACK;
    setup.rollover_txns = 8;
    setup.index_dir = cachedir;
    setup.index_threshold = 64;      /* publish about every other commit */
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));

    for (int i = 0; i < 40; i++) {
        char k[16];
        size_t kl = (size_t)snprintf(k, sizeof(k), "k%03d", i);
        ASSERT_OK(zs_db_store(db, k, kl, "v", 1, 0));
    }

    /* Still unordered after 40 commits: the window never held 8 at once. */
    struct zsi_file *act = zsi_snapshot_active(db->snap);
    ASSERT_NOT_NULL(act);
    ASSERT(zsi_file_is_unordered(act));
    ASSERT(act->nspans < 8u);
    ASSERT(act->cached_upto > ZSI_HEADER_LEN);   /* a table really did publish */

    ASSERT_OK(zs_db_check_consistency(db));
    ASSERT_OK(zs_db_close(&db));
}

/* A-4b: rejected where results are held across steps.  A cursor yields the
 * previous record while the caller looks at the next one, so an ephemeral
 * pointer there would be a promise nothing keeps -- A-13's reasoning, that a
 * rejected flag is cheaper than an untested one. */
static void test_ephemeral_rejected_on_cursor(void)
{
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    struct zs_cursor *c = NULL;

    db = open_db(ZS_CREATE);
    ASSERT_NOT_NULL(db);
    ASSERT_OK(zs_db_store(db, "k", 1, "v", 1, 0));

    ASSERT_EQ(zs_db_begin_cursor(db, NULL, 0, &c, ZS_EPHEMERAL),
              ZS_BADUSAGE);
    ASSERT_NULL(c);
    ASSERT_EQ(zs_db_foreach(db, NULL, 0, NULL, api_collect_cb, NULL,
                            ZS_EPHEMERAL), ZS_BADUSAGE);

    ASSERT_OK(zs_db_begin_txn(db, 0, &txn));
    ASSERT_EQ(zs_txn_begin_cursor(txn, NULL, 0, &c, ZS_EPHEMERAL),
              ZS_BADUSAGE);
    ASSERT_NULL(c);
    ASSERT_EQ(zs_txn_foreach(txn, NULL, 0, NULL, api_collect_cb, NULL,
                             ZS_EPHEMERAL), ZS_BADUSAGE);
    ASSERT_OK(zs_txn_abort(&txn));

    ASSERT_OK(zs_db_close(&db));
}

/*
 * ============================================================
 * Test runner
 * ============================================================
 */

struct test_entry {
    const char *name;
    void (*func)(void);
};

static struct test_entry tests[] = {
    { "test_strerror",                  test_strerror },
    { "test_le_accessors",              test_le_accessors },
    { "test_overflow_guards",           test_overflow_guards },
    { "test_interop_constants_csum",    test_interop_constants_csum },
    { "test_interop_constants_compar",  test_interop_constants_compar },
    { "test_interop_constants_uuid",    test_interop_constants_uuid },

    { "test_filenames",                 test_filenames },
    { "test_filename_rejections",       test_filename_rejections },
    { "test_filename_sort_property",    test_filename_sort_property },
    { "test_staging_names",             test_staging_names },

    { "test_magic",                     test_magic },
    { "test_magic_designed_corruptions", test_magic_designed_corruptions },
    { "test_header_roundtrip",          test_header_roundtrip },
    { "test_header_byte_layout",        test_header_byte_layout },
    { "test_header_versions",           test_header_versions },
    { "test_header_checksum",           test_header_checksum },
    { "test_header_reserved",           test_header_reserved },
    { "test_header_bounds_and_ranges",  test_header_bounds_and_ranges },

    { "test_type_byte_validity",        test_type_byte_validity },
    { "test_record_byte_layout",        test_record_byte_layout },
    { "test_record_byte_layout_big",    test_record_byte_layout_big },
    { "test_record_byte_layout_v2",     test_record_byte_layout_v2 },
    { "test_record_roundtrip",          test_record_roundtrip },
    { "test_record_canonical",          test_record_canonical },
    { "test_record_embedded_nul",       test_record_embedded_nul },
    { "test_record_bounds",             test_record_bounds },
    { "test_terminator",                test_terminator },

    { "test_file_bounds",               test_file_bounds },
    { "test_file_zero_length",          test_file_zero_length },
    { "test_file_bad_header",           test_file_bad_header },
    { "test_file_engine_from_header",   test_file_engine_from_header },
    { "test_file_open_failures",        test_file_open_failures },

    { "test_span_basic",                test_span_basic },
    { "test_span_rollback",             test_span_rollback },
    { "test_span_empty_file",           test_span_empty_file },
    { "test_span_torn_tail",            test_span_torn_tail },
    { "test_span_terminator_without_data", test_span_terminator_without_data },
    { "test_nocsum_still_rejects_bad_span",
                                    test_nocsum_still_rejects_bad_span },
    { "test_span_progress",             test_span_progress },
    { "test_span_bad_header_and_kind",  test_span_bad_header_and_kind },
    { "test_span_pointers_rejected",    test_span_pointers_rejected },
    { "test_span_engine_zero",          test_span_engine_zero },
    { "test_span_long_terminator",      test_span_long_terminator },

    { "test_index_committed_only",      test_index_committed_only },
    { "test_index_ordered_traversal",   test_index_ordered_traversal },
    { "test_index_delta",               test_index_delta },
    { "test_index_delta_shadows_base",  test_index_delta_shadows_base },
    { "test_index_delta_merge_with_duplicates",
                                        test_index_delta_merge_with_duplicates },
    { "test_index_binary_keys",         test_index_binary_keys },

    { "test_inorder_empty",             test_inorder_empty },
    { "test_inorder_search",            test_inorder_search },
    { "test_inorder_trailer_negatives", test_inorder_trailer_negatives },
    { "test_inorder_records_checksum",  test_inorder_records_checksum },
    { "test_inorder_widths_and_padding", test_inorder_widths_and_padding },
    { "test_inorder_ptrs64",            test_inorder_ptrs64 },
    { "test_inorder_kind_rules",        test_inorder_kind_rules },
    { "test_inorder_probe_ends_agrees", test_inorder_probe_ends_agrees },

    { "test_fcur_uniform",              test_fcur_uniform },
    { "test_fcur_empty_sources",        test_fcur_empty_sources },
    { "test_fcur_no_duplicate_keys",    test_fcur_no_duplicate_keys },
    { "test_fcur_deletions_visible",    test_fcur_deletions_visible },

    { "test_fileset_overlap_table",     test_fileset_overlap_table },
    { "test_fileset_first_vs_last",     test_fileset_first_vs_last },
    { "test_fileset_gaps",              test_fileset_gaps },
    { "test_fileset_uuid_discovery",    test_fileset_uuid_discovery },
    { "test_fileset_ignores_foreign",   test_fileset_ignores_foreign },
    { "test_fileset_next_gen",          test_fileset_next_gen },
    { "test_fileset_mid_conversion_stable", test_fileset_mid_conversion_stable },

    { "test_snapshot_basic",            test_snapshot_basic },
    { "test_snapshot_resolves_overlap", test_snapshot_resolves_overlap },
    { "test_snapshot_retries_and_bounds", test_snapshot_retries_and_bounds },
    { "test_snapshot_boundary",         test_snapshot_boundary },
    { "test_snapshot_bad_nonactive",    test_snapshot_bad_nonactive },
    { "test_snapshot_refcount",         test_snapshot_refcount },

    { "test_lock_basic",                test_lock_basic },
    { "test_lock_byte_offsets",         test_lock_byte_offsets },
    { "test_lock_excludes_other_process", test_lock_excludes_other_process },
    { "test_lock_dies_with_process",    test_lock_dies_with_process },
    { "test_lock_two_handles_one_process", test_lock_two_handles_one_process },
    { "test_lock_registry_keys_on_inode", test_lock_registry_keys_on_inode },
    { "test_lock_registry_is_per_database", test_lock_registry_is_per_database },
    { "test_lock_no_thread_machinery",  test_lock_no_thread_machinery },
    { "test_lock_never_uses_flock",     test_lock_never_uses_flock },

    { "test_open_create",               test_open_create },
    { "test_open_with_uuid",            test_open_with_uuid },
    { "test_open_comparator_agreement", test_open_comparator_agreement },
    { "test_open_engine_selection",     test_open_engine_selection },
    { "test_open_readonly_no_side_effects", test_open_readonly_no_side_effects },
    { "test_open_bad_nonactive",        test_open_bad_nonactive },
    { "test_open_lock_file_recreated",  test_open_lock_file_recreated },
    { "test_open_uuid_mismatch",        test_open_uuid_mismatch },

    { "test_read_d14f_duplicate_across_three_files",
                                        test_read_d14f_duplicate_across_three_files },
    { "test_read_cursor_invariant",     test_read_cursor_invariant },
    { "test_read_arrangements",         test_read_arrangements },
    { "test_read_seek_and_flags",       test_read_seek_and_flags },
    { "test_read_prefix_across_files",  test_read_prefix_across_files },
    { "test_read_model",                test_read_model },

    { "test_write_basic",               test_write_basic },
    { "test_write_txn_isolation",       test_write_txn_isolation },
    { "test_mp_read_sees_other_process_commit",
                                    test_mp_read_sees_other_process_commit },
    { "test_read_freshens_after_rollover", test_read_freshens_after_rollover },
    { "test_freshen_notices_a_rollover_by_inode",
                                        test_freshen_notices_a_rollover_by_inode },
    { "test_freshen_notices_the_active_file_going_away",
                                        test_freshen_notices_the_active_file_going_away },
    { "test_probe_no_change_reuses_snapshot",
                                    test_probe_no_change_reuses_snapshot },
    { "test_failed_refresh_keeps_probe_stale",
                                    test_failed_refresh_keeps_probe_stale },
    { "test_write_begin_reuses_snapshot",
                                    test_write_begin_reuses_snapshot },
    { "test_cursor_live_sees_other_handle_commit",
                                    test_cursor_live_sees_other_handle_commit },
    { "test_write_abort",               test_write_abort },
    { "test_commit_folds_index_incrementally",
                                        test_commit_folds_index_incrementally },
    { "test_index_fold_run_matches_replay",
                                        test_index_fold_run_matches_replay },
    { "test_write_rollover",            test_write_rollover },
    { "test_write_unclean_rollover",    test_write_unclean_rollover },
    { "test_file_grows_under_an_oversized_map",
                                    test_file_grows_under_an_oversized_map },
    { "test_active_file_headroom_is_bounded_by_size",
                                    test_active_file_headroom_is_bounded_by_size },
    { "test_cursor_resort_no_move",      test_cursor_resort_no_move },
    { "test_txn_arm_step_hint",          test_txn_arm_step_hint },
    { "test_ifchanged_writes_nothing",   test_ifchanged_writes_nothing },
    { "test_write_record_is_self_contained",
                                    test_write_record_is_self_contained },
    { "test_write_encoding_boundaries", test_write_encoding_boundaries },
    { "test_api_three_forms",           test_api_three_forms },
    { "test_api_cursor_replace",        test_api_cursor_replace },
    { "test_api_readonly",              test_api_readonly },
    { "test_api_pointer_lifetime",      test_api_pointer_lifetime },
    { "test_a4_borrow_survives_new_generation",
                                        test_a4_borrow_survives_new_generation },
    { "test_a4_borrow_survives_cursor_swap",
                                        test_a4_borrow_survives_cursor_swap },
    { "test_a4_borrow_survives_shared_snapshot_swap",
                                        test_a4_borrow_survives_shared_snapshot_swap },
    { "test_empty_value_is_not_null_on_read",
                                        test_empty_value_is_not_null_on_read },
    { "test_snapshot_reuses_immutable_files",
                                        test_snapshot_reuses_immutable_files },
    { "test_fcache_sweeps_superseded_files",
                                        test_fcache_sweeps_superseded_files },
    { "test_autorepack_bounds_the_file_count",
                                        test_autorepack_bounds_the_file_count },
    { "test_autorepack_only_at_a_new_generation",
                                        test_autorepack_only_at_a_new_generation },
    { "test_noautorepack_leaves_the_files",
                                        test_noautorepack_leaves_the_files },

    { "test_convert_basic",             test_convert_basic },
    { "test_convert_steady_state",      test_convert_steady_state },
    { "test_convert_only_one_unordered_file",
                                        test_convert_only_one_unordered_file },
    { "test_convert_staging_exclusive", test_convert_staging_exclusive },
    { "test_convert_remove_refuses_when_needed",
                                        test_convert_remove_refuses_when_needed },
    { "test_convert_readonly_does_nothing", test_convert_readonly_does_nothing },

    { "test_repack_selection",          test_repack_selection },
    { "test_db_stats_separates_repack_from_conversion",
                                        test_db_stats_separates_repack_from_conversion },
    { "test_repack_max_size",           test_repack_max_size },
    { "test_repack_one_record_per_key", test_repack_one_record_per_key },
    { "test_repack_version_order",       test_repack_version_order },
    { "test_repack_d19_newer_file_recreates",
                                    test_repack_d19_newer_file_recreates },
    { "test_repack_d19a_shadowed",      test_repack_d19a_shadowed },
    { "test_repack_d18_table",          test_repack_d18_table },
    { "test_repack_d19a_resurrection",  test_repack_d19a_resurrection },
    { "test_repack_empty_output",       test_repack_empty_output },
    { "test_repack_verifies_inputs",    test_repack_verifies_inputs },
    { "test_repack_verifies_inputs_nocsum", test_repack_verifies_inputs_nocsum },
    { "test_seal_verifies_spans_nocsum", test_seal_verifies_spans_nocsum },
    { "test_read_verifies_record_csum",  test_read_verifies_record_csum },
    { "test_read_verifies_record_csum_unordered",
                                    test_read_verifies_record_csum_unordered },
    { "test_record_csum_replay_no_truncate",
                                    test_record_csum_replay_no_truncate },
    { "test_record_csum_engine0",       test_record_csum_engine0 },
    { "test_repack_cascade",            test_repack_cascade },
    { "test_repack_never_touches_unordered",
                                        test_repack_never_touches_unordered },

    { "test_check_clean_database",      test_check_clean_database },
    { "test_check_out_of_order_pointers", test_check_out_of_order_pointers },
    { "test_check_records_checksum",    test_check_records_checksum },
    { "test_check_noncanonical",        test_check_noncanonical },
    { "test_check_unclean_reported",    test_check_unclean_reported },
    { "test_dump_line_format",          test_dump_line_format },
    { "test_dump_shows_rollback",       test_dump_shows_rollback },

    { "test_corpus_decode",             test_corpus_decode },
    { "test_corpus_encode_byte_identical",
                                        test_corpus_encode_byte_identical },
    { "test_corpus_engine_from_file_not_config",
                                        test_corpus_engine_from_file_not_config },

    { "test_malformed_never_hangs",     test_malformed_never_hangs },
    { "test_malformed_truncation",      test_malformed_truncation },
    { "test_malformed_bitflips",        test_malformed_bitflips },

    { "test_mp_writer_and_readers",     test_mp_writer_and_readers },
    { "test_mp_two_writers",            test_mp_two_writers },
    { "test_mp_killed_writer",          test_mp_killed_writer },
    { "test_mp_reader_across_repack",   test_mp_reader_across_repack },
    { "test_mp_racing_removers",        test_mp_racing_removers },
    { "test_mp_removal_needs_the_lock", test_mp_removal_needs_the_lock },
    { "test_mp_repack_and_writer_concurrent",
                                        test_mp_repack_and_writer_concurrent },
    { "test_mp_reader_sees_torn_span",  test_mp_reader_sees_torn_span },

    { "test_never_unlinks_the_lock_file", test_never_unlinks_the_lock_file },
    { "test_one_lock_descriptor",       test_one_lock_descriptor },
    { "test_reads_never_consult_ancestors",
                                        test_reads_never_consult_ancestors },
    { "test_no_yield_and_no_mvcc",      test_no_yield_and_no_mvcc },
    { "test_conversion_avoids_the_repack_lock",
                                        test_conversion_avoids_the_repack_lock },
    { "test_open_is_o1_in_records",     test_open_is_o1_in_records },

    { "test_idxcache_rejects_db_dir",   test_idxcache_rejects_db_dir },
    { "test_idxcache_threshold_defaults",
                                        test_idxcache_threshold_defaults },
    { "test_idxcache_header_byte_layout",
                                        test_idxcache_header_byte_layout },
    { "test_idxcache_published_name",   test_idxcache_published_name },
    { "test_idxcache_matches_full_build",
                                        test_idxcache_matches_full_build },
    { "test_idxcache_rejection_rules",  test_idxcache_rejection_rules },
    { "test_idxcache_rejects_bad_term_binding",
                                        test_idxcache_rejects_bad_term_binding },
    { "test_idxcache_seeded_suffix_folds_in_order",
                                        test_idxcache_seeded_suffix_folds_in_order },
    { "test_idxcache_threshold",        test_idxcache_threshold },
    { "test_idxcache_threshold_scales_with_the_file",
                                        test_idxcache_threshold_scales_with_the_file },
    { "test_idxcache_open_agrees",      test_idxcache_open_agrees },
    { "test_idxcache_publishes_by_rename",
                                        test_idxcache_publishes_by_rename },
    { "test_idxcache_publish_failure_is_not_fatal",
                                        test_idxcache_publish_failure_is_not_fatal },
    { "test_idxcache_only_unordered_files",
                                        test_idxcache_only_unordered_files },
    { "test_idxcache_sweeps_dead_generations",
                                        test_idxcache_sweeps_dead_generations },
    { "test_corpus_index_table",        test_corpus_index_table },
    { "test_idxcache_uses_file_engine", test_idxcache_uses_file_engine },
    { "test_idxcache_valid_upto_is_span_boundary",
                                        test_idxcache_valid_upto_is_span_boundary },
    { "test_index_local_publishes",     test_index_local_publishes },
    { "test_index_local_readonly_creates_nothing",
                                        test_index_local_readonly_creates_nothing },
    { "test_index_local_and_dir_is_badusage",
                                        test_index_local_and_dir_is_badusage },
    { "test_index_local_sweeps_foreign_uuid",
                                        test_index_local_sweeps_foreign_uuid },

    { "test_seal_converts_the_active_file",
                                        test_seal_converts_the_active_file },
    { "test_seal_creates_no_new_generation",
                                        test_seal_creates_no_new_generation },
    { "test_commit_seals_oversized_active",
                                        test_commit_seals_oversized_active },
    { "test_commit_below_rollover_stays_unordered",
                                        test_commit_below_rollover_stays_unordered },
    { "test_seal_at_commit_skips_table_publish",
                                        test_seal_at_commit_skips_table_publish },
    { "test_seal_noop_cases",           test_seal_noop_cases },
    { "test_seal_readonly",             test_seal_readonly },
    { "test_seal_unclean_active_file",  test_seal_unclean_active_file },
    { "test_compact_to_one_file",       test_compact_to_one_file },
    { "test_compact_ignores_geometric_selection",
                                        test_compact_ignores_geometric_selection },
    { "test_compact_drops_tombstones",  test_compact_drops_tombstones },
    { "test_compact_reports_and_fails_on_bad_file",
                                        test_compact_reports_and_fails_on_bad_file },
    { "test_compact_readonly",          test_compact_readonly },
    { "test_seal_waits_for_the_write_lock",
                                        test_seal_waits_for_the_write_lock },
    { "test_compact_lock_order",        test_compact_lock_order },

    { "test_salvage_resyncs_after_a_bad_span",
                                        test_salvage_resyncs_after_a_bad_span },
    { "test_convert_reencodes_engine_mismatch",
                                    test_convert_reencodes_engine_mismatch },
    { "test_check_reports_record_csum", test_check_reports_record_csum },
    { "test_salvage_verifies_records_inorder",
                                    test_salvage_verifies_records_inorder },
    { "test_salvage_unverified_needs_the_flag",
                                        test_salvage_unverified_needs_the_flag },
    { "test_salvage_never_recovers_rollback",
                                        test_salvage_never_recovers_rollback },
    { "test_salvage_across_a_missing_generation",
                                        test_salvage_across_a_missing_generation },
    { "test_salvage_newest_version_wins",
                                        test_salvage_newest_version_wins },
    { "test_salvage_active_file_is_newest",
                                        test_salvage_active_file_is_newest },
    { "test_salvage_invalid_header",    test_salvage_invalid_header },
    { "test_salvage_ignores_pointer_section",
                                        test_salvage_ignores_pointer_section },
    { "test_salvage_reports_maybe_stale",
                                        test_salvage_reports_maybe_stale },
    { "test_salvage_never_writes_the_source",
                                        test_salvage_never_writes_the_source },
    { "test_salvage_event_fields",      test_salvage_event_fields },
    { "test_salvage_comparator_mismatch_reported",
                                        test_salvage_comparator_mismatch_reported },

    { "test_cursor_sees_own_handle_writes",
                                        test_cursor_sees_own_handle_writes },
    { "test_txn_cursor_sees_own_writes", test_txn_cursor_sees_own_writes },
    { "test_txn_cursor_no_duplicate_on_write",
                                        test_txn_cursor_no_duplicate_on_write },
    { "test_hold_releases_its_references",
                                    test_hold_releases_its_references },
    { "test_fetch_borrow_survives_cursor_free",
                                        test_fetch_borrow_survives_cursor_free },
    { "test_read_txn_view_is_fixed_without_a_cursor",
                                    test_read_txn_view_is_fixed_without_a_cursor },
    { "test_txn_cursor_view_is_fixed",  test_txn_cursor_view_is_fixed },
    { "test_cursor_start_key_survives_refresh",
                                        test_cursor_start_key_survives_refresh },
    { "test_cursor_delete_during_traversal",
                                        test_cursor_delete_during_traversal },
    { "test_cursor_reverse_walks_everything",
                                        test_cursor_reverse_walks_everything },
    { "test_cursor_reverse_seek_and_skiproot",
                                        test_cursor_reverse_seek_and_skiproot },
    { "test_cursor_reverse_tombstones", test_cursor_reverse_tombstones },
    { "test_cursor_reverse_prefix",     test_cursor_reverse_prefix },
    { "test_txn_long_keys_sharing_a_head",
                                        test_txn_long_keys_sharing_a_head },
    { "test_txn_caller_comparator_long_keys",
                                        test_txn_caller_comparator_long_keys },
    { "test_txn_cursor_reverse_prefix_bound",
                                        test_txn_cursor_reverse_prefix_bound },
    { "test_cursor_reverse_own_writes", test_cursor_reverse_own_writes },
    { "test_reverse_rejected_compositions",
                                        test_reverse_rejected_compositions },
    { "test_fetchprev_basic",           test_fetchprev_basic },
    { "test_fetchprev_sees_txn_writes", test_fetchprev_sees_txn_writes },
    { "test_fetchnext_inclusive",       test_fetchnext_inclusive },
    { "test_fetchnext_inclusive_sees_txn_writes",
                                test_fetchnext_inclusive_sees_txn_writes },
    { "test_txn_many_cursors",          test_txn_many_cursors },
    { "test_txn_insert_select_self",    test_txn_insert_select_self },
    { "test_index_cur_reverse_base_delta",
                                        test_index_cur_reverse_base_delta },
    { "test_stream_abort_writes_rollback",
                                        test_stream_abort_writes_rollback },
    { "test_stream_bounded_memory",     test_stream_bounded_memory },
    { "test_txn_fetch_survives_overwrite",
                                        test_txn_fetch_survives_overwrite },
    { "test_reverse_a4_lifetime",       test_reverse_a4_lifetime },
    { "test_rollover_txns_seals_on_span_count",
      test_rollover_txns_seals_on_span_count },
    { "test_rollover_txns_counts_the_replay_window",
      test_rollover_txns_counts_the_replay_window },
    { "test_txn_miss_does_not_flush",   test_txn_miss_does_not_flush },
    { "test_ephemeral_avoids_the_flush", test_ephemeral_avoids_the_flush },
    { "test_ephemeral_matches_durable", test_ephemeral_matches_durable },
    { "test_ephemeral_rejected_on_cursor", test_ephemeral_rejected_on_cursor },

    { "test_txn_cursor_store_behind_not_yielded",
                                        test_txn_cursor_store_behind_not_yielded },
    { "test_cursor_resume_key_survives_retirement",
                                        test_cursor_resume_key_survives_retirement },
    { "test_txn_cursor_survives_pending_array_growth",
                                        test_txn_cursor_survives_pending_array_growth },

    { NULL, NULL }
};

int main(int argc, char **argv)
{
    const char *filter = NULL;
    if (argc > 1) filter = argv[1];

    for (struct test_entry *t = tests; t->name; t++) {
        if (filter && !strstr(t->name, filter)) continue;

        total_tests++;
        current_test_failed = 0;
        cb_failures = 0;

        if (setup() != 0) {
            fprintf(stderr, "  FAIL: setup failed for %s\n", t->name);
            total_failed++;
            teardown();
            continue;
        }

        fprintf(stderr, "  %-40s ", t->name);
        t->func();

        if (current_test_failed || cb_failures) {
            fprintf(stderr, "FAIL\n");
            total_failed++;
        } else {
            fprintf(stderr, "ok\n");
            total_passed++;
        }

        teardown();
    }

    fprintf(stderr, "\n%d tests: %d passed, %d failed, %d skipped\n",
            total_tests, total_passed, total_failed, total_skipped);

    return total_failed ? 1 : 0;
}
