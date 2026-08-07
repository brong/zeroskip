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

static void test_zmalloc(void)
{
    /* Zeroed, because several later structures rely on a fresh allocation
     * having NULL pointers and zero counts rather than setting each by hand. */
    for (size_t n = 1; n <= 128; n++) {
        unsigned char *p = zsi_zmalloc(n);
        ASSERT_NOT_NULL(p);
        for (size_t i = 0; i < n; i++) {
            if (p[i] != 0) {
                fprintf(stderr, "\n    FAIL byte %zu of %zu is 0x%02X\n",
                        i, n, p[i]);
                current_test_failed = 1;
                free(p);
                return;
            }
        }
        free(p);
    }
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
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
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
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-00000001");

    zsi_name_format(name, test_uuid, 1, 10);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-00000001-0000000A");

    zsi_name_format(name, test_uuid, 5, 5);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-00000005-00000005");

    /* The full 32-bit range has a name, which is what 8 digits buys (D-1). */
    zsi_name_format(name, test_uuid, 0xFFFFFFFFu, 0);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-FFFFFFFF");
    zsi_name_format(name, test_uuid, 0xABCDEF01u, 0xFEDCBA98u);
    ASSERT_STR_EQ(name, "zeroskip-" TEST_UUID_STR "-ABCDEF01-FEDCBA98");

    /* Round-trip, both kinds. */
    zsi_name_format(name, test_uuid, 7, 0);
    ASSERT_EQ(zsi_name_parse(name, u, &s, &e), ZSI_NAME_UNORDERED);
    ASSERT_MEM_EQ(u, test_uuid, 16);
    ASSERT_EQU(s, 7u);
    ASSERT_EQU(e, 0u);

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
        /* an extension: D-1a forbids one, and D-5's ordering depends on it */
        "zeroskip-" TEST_UUID_STR "-00000001.zs",
        "zeroskip-" TEST_UUID_STR "-00000001-00000004.zs",
        "zeroskip-" TEST_UUID_STR "-00000001.tmp",
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
                             "-00000001", u, &s, &e), ZSI_NAME_UNORDERED);
}

static void test_filename_prefix_property(void)
{
    /* D-1a, asserted directly on generated names.
     *
     * D-5 resolves an overlap by taking the LAST file whose start matches, which
     * is only correct because an unordered file's name sorts before the in-order
     * name for the same generation.  That holds because the unordered name is a
     * strict prefix of the in-order one -- which is true only while data files
     * carry no extension.
     *
     * T-9 requires this be a test so that adding an extension later breaks a test
     * rather than the database. */
    for (uint32_t g = 1; g <= 300; g++) {
        char un[ZSI_NAME_MAX], in[ZSI_NAME_MAX];
        zsi_name_format(un, test_uuid, g, 0);
        zsi_name_format(in, test_uuid, g, g);

        size_t ul = strlen(un);
        if (strncmp(un, in, ul) != 0) {
            fprintf(stderr, "\n    FAIL gen %u: '%s' is not a prefix of '%s'\n",
                    g, un, in);
            current_test_failed = 1;
            return;
        }
        if (strcmp(un, in) >= 0) {
            fprintf(stderr, "\n    FAIL gen %u: '%s' does not sort before '%s'\n",
                    g, un, in);
            current_test_failed = 1;
            return;
        }
    }

    /* And the three-way case D-5a's table ends with: unordered N, N-N, and a
     * wider N-M must sort in that order, so "last" is the widest. */
    char a[ZSI_NAME_MAX], b[ZSI_NAME_MAX], c[ZSI_NAME_MAX];
    zsi_name_format(a, test_uuid, 5, 0);
    zsi_name_format(b, test_uuid, 5, 5);
    zsi_name_format(c, test_uuid, 5, 9);
    ASSERT(strcmp(a, b) < 0);
    ASSERT(strcmp(b, c) < 0);
    ASSERT(strcmp(a, c) < 0);
}

static void test_filename_lexical_order(void)
{
    /* D-1: fixed-width hex keeps lexical and numeric order identical, which is
     * what lets D-5 sweep a sorted name list and get numeric ordering for free.
     * Checked across the boundaries where a variable-width encoding would break
     * it -- 9 to 10, 15 to 16, 255 to 256 -- and at the top of the range. */
    static const uint32_t gens[] = {
        1, 2, 9, 10, 15, 16, 17, 255, 256, 4095, 4096, 65535, 65536,
        0x00FFFFFF, 0x01000000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF
    };
    size_t n = sizeof(gens) / sizeof(gens[0]);

    for (size_t i = 0; i < n; i++) {
        for (size_t j = 0; j < n; j++) {
            char x[ZSI_NAME_MAX], y[ZSI_NAME_MAX];
            zsi_name_format(x, test_uuid, gens[i], 0);
            zsi_name_format(y, test_uuid, gens[j], 0);
            int lex = strcmp(x, y);
            int num = (gens[i] > gens[j]) - (gens[i] < gens[j]);
            if (((lex > 0) - (lex < 0)) != num) {
                fprintf(stderr, "\n    FAIL %08X vs %08X: lexical %d, numeric %d\n",
                        gens[i], gens[j], lex, num);
                current_test_failed = 1;
                return;
            }
        }
    }

    /* For a shared start, the wider end sorts last -- which is what makes D-5's
     * "take the last" pick a repack output over its own inputs (D-5a). */
    static const uint32_t ends[] = { 1, 2, 4, 9, 10, 16, 255, 0xFFFFFFFF };
    for (size_t i = 1; i < sizeof(ends) / sizeof(ends[0]); i++) {
        char x[ZSI_NAME_MAX], y[ZSI_NAME_MAX];
        zsi_name_format(x, test_uuid, 1, ends[i - 1]);
        zsi_name_format(y, test_uuid, 1, ends[i]);
        ASSERT(strcmp(x, y) < 0);
    }
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

/* The engine a test uses to stand in for a caller-supplied one (engine 2).
 * zsi_csum_none is a legitimate choice: engine 2 is whatever the caller supplies,
 * and the point of these tests is which function gets selected, not what it
 * computes. */
#define TEST_EXTERNAL_CSUM zsi_csum_none

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
        0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 24 uuid */
        0x49, 0x41, 0xDA, 0x54, 0x94, 0x06, 0x4F, 0xAA,
        0xA4, 0x57, 0xC4, 0xB6, 0x5B, 0xEA, 0xE3, 0xEB,
        /* 40 start = 0x01020304 LE, 44 end = 0x05060708 LE */
        0x04, 0x03, 0x02, 0x01, 0x08, 0x07, 0x06, 0x05,
        /* 48 comparator name, NUL-padded to 16 */
        0x6D, 0x65, 0x6D, 0x63, 0x6D, 0x70, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* 64 reserved, 68 checksum of [0, 68) */
        0x00, 0x00, 0x00, 0x00, 0xA7, 0xA7, 0xCF, 0x9D
    };

    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    struct zsi_header h;
    char buf[ZSI_HEADER_LEN];

    ASSERT_EQ(ZSI_HEADER_LEN, 72);
    ASSERT_EQ(ZSI_MAGIC_LEN, 16);

    memset(&h, 0, sizeof(h));
    h.version_read  = 1;
    h.version_write = 1;
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
    ASSERT_EQ((unsigned char)buf[16], 1);
    ASSERT_EQ((unsigned char)buf[17], 1);
    ASSERT_EQU(zsi_get16(buf + 18), 1u);
    ASSERT_EQU(zsi_get32(buf + 20), 0u);
    ASSERT_MEM_EQ(buf + 24, u, 16);
    ASSERT_EQU(zsi_get32(buf + 40), 0x01020304u);
    ASSERT_EQU(zsi_get32(buf + 44), 0x05060708u);
    ASSERT_MEM_EQ(buf + 48, "memcmp\0\0\0\0\0\0\0\0\0\0", 16);
    ASSERT_EQU(zsi_get32(buf + 64), 0u);
    ASSERT_EQU(zsi_get32(buf + 68), 0x9DCFA7A7u);

    /* And it decodes back to what it came from. */
    ASSERT_OK(zsi_header_decode((const char *)golden, ZSI_HEADER_LEN,
                                zsi_csum_xxhash, &h));
    ASSERT_EQU(h.start, 0x01020304u);
    ASSERT_EQU(h.end, 0x05060708u);

    /* A full 16-byte comparator name, which has no NUL padding at all: the copy
     * must be the field width, not the string length. */
    memset(&h, 0, sizeof(h));
    h.version_read = 1;
    h.version_write = 1;
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
     * fourteen in F-12's table are accepted and the other 242 rejected. */
    static const uint8_t legal[] = {
        0x01, 0x03, 0x05, 0x07, 0x09, 0x0B, 0x0D, 0x0F,
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
    ASSERT_EQ(naccepted, 14);

    /* A bitfield admits far more values than it defines, so the near-misses
     * matter most: each of these is a plausible single flipped bit away from a
     * valid type, and each must be rejected rather than half-interpreted. */

    /* two family bits set at once */
    ASSERT(!zsi_type_valid(ZSI_HASKEY | ZSI_SPANTERM));       /* 0x11 */
    ASSERT(!zsi_type_valid(ZSI_HASKEY | ZSI_POINTERS));       /* 0x21 */
    ASSERT(!zsi_type_valid(ZSI_SPANTERM | ZSI_POINTERS));     /* 0x30 */
    ASSERT(!zsi_type_valid(0x31));

    /* HasAncestor without HasKey */
    ASSERT(!zsi_type_valid(ZSI_HASANCESTOR));                 /* 0x08 */
    ASSERT(!zsi_type_valid(ZSI_HASANCESTOR | ZSI_SPANTERM));  /* 0x18 */
    ASSERT(!zsi_type_valid(ZSI_HASANCESTOR | ZSI_POINTERS));  /* 0x28 */

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
    ASSERT(ZSI_KEYVALUE_ANC == (ZSI_KEYVALUE | ZSI_HASANCESTOR));
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

    /* KEYVALUE (0x01): key "ab", value "xy", ancestor omitted.
     *   +0 type, +1 keylen, +2 vallen(LE16), +4 key NUL value NUL, pad to 8
     *   len = roundup8(4 + 2 + 1 + 2 + 1) = roundup8(10) = 16 */
    static const unsigned char kv[16] = {
        0x01, 0x02, 0x02, 0x00, 'a', 'b', 0x00, 'x',
        'y',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 2, false, false), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, "ab", 2, "xy", 2, false, 0);
    ASSERT_MEM_EQ(buf, kv, 16);

    /* KEYVALUE_ANC (0x09): the same with ancestor 5 stored.
     *   +4 ancestor, +8 key NUL value NUL
     *   len = roundup8(8 + 6) = 16 -- the ancestor is free here, because the
     *   padding absorbed it */
    static const unsigned char kva[16] = {
        0x09, 0x02, 0x02, 0x00, 0x05, 0x00, 0x00, 0x00,
        'a',  'b',  0x00, 'x',  'y',  0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 2, false, true), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, "ab", 2, "xy", 2, true, 5);
    ASSERT_MEM_EQ(buf, kva, 16);

    /* DELETION (0x03): key "ab", no value field at all.
     *   +0 type, +1 keylen, +2 pad(2), +4 key NUL, pad to 8
     *   len = roundup8(4 + 3) = 8 */
    static const unsigned char del[8] = {
        0x03, 0x02, 0x00, 0x00, 'a', 'b', 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 0, true, false), 8u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, "ab", 2, NULL, 0, false, 0);
    ASSERT_MEM_EQ(buf, del, 8);

    /* DELETION_ANC (0x0B): +4 ancestor, +8 key NUL
     *   len = roundup8(8 + 3) = 16 */
    static const unsigned char dela[16] = {
        0x0B, 0x02, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
        'a',  'b',  0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 0, true, true), 16u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, "ab", 2, NULL, 0, true, 5);
    ASSERT_MEM_EQ(buf, dela, 16);

    /* An empty value is legal and distinct from an absent key (F-14, A-1).
     *   len = roundup8(4 + 2 + 1 + 0 + 1) = 8 */
    static const unsigned char kv_empty[8] = {
        0x01, 0x02, 0x00, 0x00, 'a', 'b', 0x00, 0x00
    };
    ASSERT_EQU(zsi_rec_encoded_len(2, 0, false, false), 8u);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, "ab", 2, "", 0, false, 0);
    ASSERT_MEM_EQ(buf, kv_empty, 8);

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
     * the body placement is checked by decoding. */
    size_t keylen = 256;                 /* one past the short form's limit */
    char *key = malloc(keylen);
    ASSERT_NOT_NULL(key);
    for (size_t i = 0; i < keylen; i++) key[i] = (char)('A' + (i % 26));

    /* BIGKEYVALUE (0x05): +0 type, +1 pad(7), +8 keylen(LE64), +16 vallen(LE64),
     *                     +24 key NUL value NUL
     *   len = roundup8(24 + 256 + 1 + 2 + 1) = roundup8(284) = 288 */
    size_t want = 288;
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 2, false, false), want);
    char *buf = malloc(want + 16);
    ASSERT_NOT_NULL(buf);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, key, keylen, "xy", 2, false, 0);

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

    /* BIGKEYVALUE_ANC (0x0D): the ancestor lands in padding the shape already
     * carries, so the header stays 24 bytes and the total is unchanged (F-12c). */
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 2, false, true), want);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, key, keylen, "xy", 2, true, 5);
    static const unsigned char bkva_hdr[24] = {
        0x0D, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,   /* +4 ancestor = 5 */
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_MEM_EQ(buf, bkva_hdr, 24);

    /* BIGDELETION (0x07): +0 type, +1 pad(7), +8 keylen, +16 key NUL
     *   len = roundup8(16 + 256 + 1) = roundup8(273) = 280 */
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 0, true, false), 280u);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, key, keylen, NULL, 0, false, 0);
    static const unsigned char bdel_hdr[16] = {
        0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_MEM_EQ(buf, bdel_hdr, 16);
    ASSERT_MEM_EQ(buf + 16, key, keylen);

    /* BIGDELETION_ANC (0x0F): +1 pad(3), +4 ancestor, +8 keylen, +16 key NUL */
    ASSERT_EQU(zsi_rec_encoded_len(keylen, 0, true, true), 280u);
    memset(buf, 0xAA, want + 16);
    zsi_rec_encode(buf, key, keylen, NULL, 0, true, 5);
    static const unsigned char bdela_hdr[16] = {
        0x0F, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    ASSERT_MEM_EQ(buf, bdela_hdr, 16);

    free(buf);
    free(key);
}

static void test_record_roundtrip(void)
{
    /* Every one of the eight data shapes encodes and decodes back unchanged, with
     * a total length that is a multiple of 8 and all padding zero (F-2). */
    struct {
        const char *what;
        size_t keylen, vallen;
        bool isdelete, anc;
        uint8_t type;
    } shapes[] = {
        { "KEYVALUE",        2,   2, false, false, ZSI_KEYVALUE        },
        { "KEYVALUE_ANC",    2,   2, false, true,  ZSI_KEYVALUE_ANC    },
        { "DELETION",        2,   0, true,  false, ZSI_DELETION        },
        { "DELETION_ANC",    2,   0, true,  true,  ZSI_DELETION_ANC    },
        { "BIGKEYVALUE",   300,   2, false, false, ZSI_BIGKEYVALUE     },
        { "BIGKEYVALUE_ANC", 300, 2, false, true,  ZSI_BIGKEYVALUE_ANC },
        { "BIGDELETION",   300,   0, true,  false, ZSI_BIGDELETION     },
        { "BIGDELETION_ANC", 300, 0, true,  true,  ZSI_BIGDELETION_ANC }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t kl = shapes[i].keylen, vl = shapes[i].vallen;
        char *key = malloc(kl), *val = malloc(vl ? vl : 1);
        ASSERT_NOT_NULL(key);
        ASSERT_NOT_NULL(val);
        for (size_t j = 0; j < kl; j++) key[j] = (char)(1 + (j % 255));
        for (size_t j = 0; j < vl; j++) val[j] = (char)(255 - (j % 255));

        size_t len = zsi_rec_encoded_len(kl, vl, shapes[i].isdelete,
                                         shapes[i].anc);
        ASSERT(len > 0);
        ASSERT_EQU(len % 8, 0u);

        char *buf = malloc(len);
        ASSERT_NOT_NULL(buf);
        memset(buf, 0xAA, len);
        zsi_rec_encode(buf, key, kl, shapes[i].isdelete ? NULL : val, vl,
                       shapes[i].anc, 7);

        ASSERT_EQ((unsigned char)buf[0], shapes[i].type);

        struct zsi_rec r;
        int rc = zsi_rec_decode(buf, len, 99, &r);
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

        /* F-17: a stored ancestor is read back; an omitted one resolves to the
         * containing file's start generation, which the caller supplies. */
        ASSERT_EQU(r.ancestor, shapes[i].anc ? 7u : 99u);

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
    ASSERT_EQ(zsi_rec_type_for(255, 0, false, false), ZSI_KEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(256, 0, false, false), ZSI_BIGKEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(1, 65535, false, false), ZSI_KEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(1, 65536, false, false), ZSI_BIGKEYVALUE);
    ASSERT_EQ(zsi_rec_type_for(255, 0, true, false), ZSI_DELETION);
    ASSERT_EQ(zsi_rec_type_for(256, 0, true, false), ZSI_BIGDELETION);

    /* A deletion has no value field, so a value length cannot promote it. */
    ASSERT_EQ(zsi_rec_type_for(1, 70000, true, false), ZSI_DELETION);

    /* The ancestor never promotes a record to the big form: it is 4 bytes
     * whenever present (F-15). */
    ASSERT_EQ(zsi_rec_type_for(255, 65535, false, true), ZSI_KEYVALUE_ANC);
    ASSERT_EQ(zsi_rec_type_for(255, 0, true, true), ZSI_DELETION_ANC);

    /* Encoding at each boundary produces the type the table says. */
    memset(buf, 0, sizeof(buf));
    zsi_rec_encode(buf, key, 255, "", 0, false, 0);
    ASSERT_EQ((unsigned char)buf[0], ZSI_KEYVALUE);

    char *big = malloc(zsi_rec_encoded_len(256, 0, false, false));
    ASSERT_NOT_NULL(big);
    zsi_rec_encode(big, key, 256, "", 0, false, 0);
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
    ASSERT_OK(zsi_rec_decode(buf, 64, 1, &r));
    ASSERT_EQU(r.keylen, 2u);
    ASSERT_MEM_EQ(r.key, "ab", 2);      /* the data survives, which is the point */
    ASSERT_EQU(r.vallen, 2u);
    ASSERT_MEM_EQ(r.val, "xy", 2);
    ASSERT(!zsi_rec_is_canonical(&r, 1));

    /* A big record that genuinely needs the big form decodes and is canonical. */
    size_t n = zsi_rec_encoded_len(300, 2, false, false);
    char *ok = malloc(n);
    ASSERT_NOT_NULL(ok);
    zsi_rec_encode(ok, key, 300, "xy", 2, false, 0);
    ASSERT_OK(zsi_rec_decode(ok, n, 1, &r));
    ASSERT_EQU(r.keylen, 300u);
    ASSERT(zsi_rec_is_canonical(&r, 1));
    free(ok);

    /* And the same for a big deletion, where only keylen can justify the form. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGDELETION;
    zsi_put64(buf + 8, 2);
    memcpy(buf + 16, "ab", 2);
    ASSERT_OK(zsi_rec_decode(buf, 64, 1, &r));
    ASSERT_MEM_EQ(r.key, "ab", 2);
    ASSERT(!zsi_rec_is_canonical(&r, 1));

    /* Everything this implementation writes is canonical, across all eight
     * shapes -- which is the writer-side half of F-15. */
    struct { size_t kl, vl; bool del, anc; } shapes[] = {
        { 2, 2, false, false }, { 2, 2, false, true },
        { 2, 0, true,  false }, { 2, 0, true,  true },
        { 300, 2, false, false }, { 300, 2, false, true },
        { 300, 0, true, false },  { 300, 0, true, true }
    };
    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t len = zsi_rec_encoded_len(shapes[i].kl, shapes[i].vl,
                                         shapes[i].del, shapes[i].anc);
        char *b = malloc(len);
        ASSERT_NOT_NULL(b);
        zsi_rec_encode(b, key, shapes[i].kl,
                       shapes[i].del ? NULL : val, shapes[i].vl,
                       shapes[i].anc, 7);
        ASSERT_OK(zsi_rec_decode(b, len, 1, &r));
        if (!zsi_rec_is_canonical(&r, 1)) {
            fprintf(stderr, "\n    FAIL shape %zu encoded non-canonically\n", i);
            current_test_failed = 1;
            free(b);
            return;
        }
        free(b);
    }

    /* F-17: storing an ancestor equal to the file's own start is non-canonical,
     * because the rule is to omit it exactly then.  This is the case T-6 names
     * explicitly, and it decodes to the same value either way -- which is why
     * only a canonicality check can see it. */
    size_t l2 = zsi_rec_encoded_len(2, 2, false, true);
    char *b2 = malloc(l2);
    ASSERT_NOT_NULL(b2);
    zsi_rec_encode(b2, "ab", 2, "xy", 2, true, 5);
    ASSERT_OK(zsi_rec_decode(b2, l2, 5, &r));
    ASSERT_EQU(r.ancestor, 5u);
    ASSERT(!zsi_rec_is_canonical(&r, 5));   /* stored, but equals file start */
    ASSERT_OK(zsi_rec_decode(b2, l2, 9, &r));
    ASSERT_EQU(r.ancestor, 5u);
    ASSERT(zsi_rec_is_canonical(&r, 9));    /* stored, and differs: correct */
    free(b2);
}

static void test_record_embedded_nul(void)
{
    /* F-13: lengths are authoritative; keys and values MAY contain NUL bytes,
     * and stored lengths MUST NOT include the terminators. */
    char buf[64];
    struct zsi_rec r;

    const char key[] = { 'a', '\0', 'b' };
    const char val[] = { '\0', 'x', '\0' };

    size_t len = zsi_rec_encoded_len(3, 3, false, false);
    ASSERT_EQU(len, 16u);               /* roundup8(4 + 3 + 1 + 3 + 1) = 16 */
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, key, 3, val, 3, false, 0);
    ASSERT_OK(zsi_rec_decode(buf, len, 1, &r));
    ASSERT_EQU(r.keylen, 3u);
    ASSERT_MEM_EQ(r.key, key, 3);
    ASSERT_EQU(r.vallen, 3u);
    ASSERT_MEM_EQ(r.val, val, 3);

    /* The stored length is 3, not the 1 that strlen would report. */
    ASSERT_EQ((unsigned char)buf[1], 3);
    ASSERT_EQU(zsi_get16(buf + 2), 3u);

    /* A key that is entirely NULs. */
    const char nuls[] = { '\0', '\0', '\0', '\0' };
    len = zsi_rec_encoded_len(4, 0, false, false);
    memset(buf, 0xAA, sizeof(buf));
    zsi_rec_encode(buf, nuls, 4, "", 0, false, 0);
    ASSERT_OK(zsi_rec_decode(buf, len, 1, &r));
    ASSERT_EQU(r.keylen, 4u);
    ASSERT_MEM_EQ(r.key, nuls, 4);
    ASSERT_EQU(r.vallen, 0u);
    ASSERT_NOT_NULL(r.val);             /* empty, not absent */
}

static void test_record_bounds(void)
{
    char buf[512];
    struct zsi_rec r;

    /* For each shape, decoding with len one byte short of the true length is
     * rejected rather than reading past the end. */
    struct { size_t kl, vl; bool del, anc; } shapes[] = {
        { 2, 2, false, false }, { 2, 2, false, true },
        { 2, 0, true,  false }, { 2, 0, true,  true },
        { 300, 2, false, false }, { 300, 0, true, false }
    };

    for (size_t i = 0; i < sizeof(shapes) / sizeof(shapes[0]); i++) {
        size_t len = zsi_rec_encoded_len(shapes[i].kl, shapes[i].vl,
                                         shapes[i].del, shapes[i].anc);
        char *b = malloc(len);
        ASSERT_NOT_NULL(b);
        char *k = malloc(shapes[i].kl);
        ASSERT_NOT_NULL(k);
        memset(k, 'k', shapes[i].kl);
        zsi_rec_encode(b, k, shapes[i].kl,
                       shapes[i].del ? NULL : "xy", shapes[i].vl,
                       shapes[i].anc, 3);

        ASSERT_OK(zsi_rec_decode(b, len, 1, &r));
        /* every length below the true one fails */
        for (size_t l = 0; l < len; l++) {
            if (zsi_rec_decode(b, l, 1, &r) == ZS_OK) {
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
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);

    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, (uint64_t)SIZE_MAX - 1);
    zsi_put64(buf + 16, 4);
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);

    /* F-14: a key must be at least 1 byte, in every shape. */
    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_KEYVALUE;
    buf[1] = 0;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);

    memset(buf, 0, sizeof(buf));
    buf[0] = (char)ZSI_BIGKEYVALUE;
    zsi_put64(buf + 8, 0);
    zsi_put64(buf + 16, 70000);
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);

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
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_COMMIT;           /* a terminator is not a data record */
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_ROLLBACK;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_COMMIT_LONG;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS32;           /* nor is a pointer section */
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = (char)ZSI_PTRS64;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);
    buf[0] = 0x00;
    ASSERT_EQ(zsi_rec_decode(buf, sizeof(buf), 1, &r), ZS_BADFORMAT);

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
    ASSERT_EQ(zsi_rec_decode(buf, 0, 1, &r), ZS_BADFORMAT);
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

static void test_terminator_detects_tear(void)
{
    /* F-22: because the checksum covers the span AND the terminator, a
     * terminator that reaches disk without its data fails validation and reads as
     * absent.  This is the property recovery (F-24) and lock-free concurrent
     * reading (C-4f) both rest on, so it gets its own test.
     *
     * Modelled here at the byte level: build a valid span-plus-terminator, then
     * damage each part in turn and confirm a reader recomputing the checksum
     * notices. */
    char span[64], buf[8];
    for (size_t i = 0; i < sizeof(span); i++) span[i] = (char)i;

    zsi_term_encode(buf, 64, false, span, zsi_csum_xxhash, ZSI_CSUM_XXHASH);
    struct zsi_term t;
    ASSERT_OK(zsi_term_decode(buf, 8, &t));

    uint32_t good = zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH, span, 64, buf, 4);
    ASSERT_EQU(t.csum, good);

    /* Damage any byte of the span: caught. */
    for (size_t i = 0; i < sizeof(span); i++) {
        span[i] ^= 0x01;
        uint32_t now = zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH,
                                 span, 64, buf, 4);
        if (now == t.csum) {
            fprintf(stderr, "\n    FAIL span byte %zu flip not detected\n", i);
            current_test_failed = 1;
            return;
        }
        span[i] ^= 0x01;
    }

    /* Damage any covered byte of the terminator: caught. */
    for (size_t i = 0; i < 4; i++) {
        char tmp[8];
        memcpy(tmp, buf, 8);
        tmp[i] ^= 0x01;
        uint32_t now = zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH,
                                 span, 64, tmp, 4);
        if (now == t.csum) {
            fprintf(stderr, "\n    FAIL term byte %zu flip not detected\n", i);
            current_test_failed = 1;
            return;
        }
    }

    /* The span arriving short -- the actual torn-tail shape, where the
     * terminator landed but some of its data did not. */
    for (size_t shortby = 1; shortby <= 8; shortby++) {
        uint32_t now = zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH,
                                 span, 64 - shortby, buf, 4);
        ASSERT(now != t.csum);
    }

    /* Under engine 0 none of this holds: a torn tail is undetectable, which is
     * the documented cost (F-5c). */
    zsi_term_encode(buf, 64, false, span, zsi_csum_none, ZSI_CSUM_NONE);
    ASSERT_OK(zsi_term_decode(buf, 8, &t));
    ASSERT_EQU(t.csum, 0u);
    span[0] ^= 0x01;
    ASSERT_EQU(zsi_csum2(zsi_csum_none, ZSI_CSUM_NONE, span, 64, buf, 4), 0u);
    span[0] ^= 0x01;
}

static void test_terminator_long_span(void)
{
    /* T-4: a 16MB span forcing a long terminator, with real data behind it so the
     * checksum path is exercised at that size rather than only the length field. */
    size_t n = 16u * 1024 * 1024;
    char *span = malloc(n);
    if (!span) SKIP("cannot allocate 16MB");
    for (size_t i = 0; i < n; i++) span[i] = (char)(i * 31);

    char buf[24];
    struct zsi_term t;

    ASSERT_EQU(zsi_term_encoded_len(n), 24u);
    zsi_term_encode(buf, n, false, span, zsi_csum_xxhash, ZSI_CSUM_XXHASH);
    ASSERT_EQ((unsigned char)buf[0], ZSI_COMMIT_LONG);
    ASSERT_OK(zsi_term_decode(buf, 24, &t));
    ASSERT_EQU(t.spanlen, n);
    ASSERT_EQU(t.csum, zsi_csum2(zsi_csum_xxhash, ZSI_CSUM_XXHASH,
                                 span, n, buf, 20));

    /* Exactly at the boundary, the short form is still used. */
    ASSERT_EQU(zsi_term_encoded_len(ZSI_SHORT_SPANLEN_MAX), 8u);
    zsi_term_encode(buf, ZSI_SHORT_SPANLEN_MAX, false, span,
                    zsi_csum_xxhash, ZSI_CSUM_XXHASH);
    ASSERT_EQ((unsigned char)buf[0], ZSI_COMMIT);
    ASSERT_OK(zsi_term_decode(buf, 8, &t));
    ASSERT_EQU(t.spanlen, (uint64_t)ZSI_SHORT_SPANLEN_MAX);

    free(span);
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
                   const char *val, size_t vallen,
                   bool store_anc, uint32_t anc)
{
    size_t n = zsi_rec_encoded_len(keylen, vallen, val == NULL, store_anc);
    assert(n > 0);
    sb_reserve(s, n);
    zsi_rec_encode(s->buf + s->len, key, keylen, val, vallen, store_anc, anc);
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
                   const char *val, size_t vallen, bool anc, uint32_t ancgen)
{
    size_t n = zsi_rec_encoded_len(keylen, vallen, val == NULL, anc);
    assert(n > 0);
    while (b->len + n > b->alloc) {
        b->alloc *= 2;
        b->buf = realloc(b->buf, b->alloc);
        assert(b->buf);
    }
    assert(b->n < 256);
    b->offs[b->n++] = b->len;
    zsi_rec_encode(b->buf + b->len, key, keylen, val, vallen, anc, ancgen);
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
    zsi_name_format(name, test_uuid, gen, 0);
    if (mkdbdir() != 0) return -1;
    if (sb_write(s, name) != 0) return -1;
    memset(c, 0, sizeof(*c));
    if (zsi_file_open(dbdir, name, gen, TEST_EXTERNAL_CSUM, fp) != ZS_OK)
        return -1;
    return zsi_unordered_replay(*fp, false, collect_cb, c);
}

/*
 * ============================================================
 * File object (part of T-6)
 * ============================================================
 */

static void test_file_open_unordered(void)
{
    char hdr[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);

    /* A 72-byte header and no spans: a legal empty unordered file, which is
     * exactly what creating a database produces (D-8a, F-26h). */
    make_header(hdr, 1, 0, ZSI_CSUM_XXHASH);
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_EQ(filesize(name), 72);

    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT_NOT_NULL(f);
    ASSERT(f->hdr_valid);
    ASSERT_EQU(f->size, 72u);
    ASSERT_EQU(f->hdr.start, 1u);
    ASSERT_EQU(f->hdr.end, 0u);
    ASSERT(zsi_file_is_unordered(f));
    ASSERT(f->csum == zsi_csum_xxhash);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_XXHASH);
    zsi_file_close(&f);
    ASSERT_NULL(f);
}

static void test_file_open_inorder(void)
{
    /* The smallest valid in-order file is 96 bytes: a 72-byte header, an 8-byte
     * PTRS32 section with count == 0, and the 16-byte trailer (F-26g).  Built by
     * hand here -- the pointer section proper arrives in a later section, but the
     * kind must already be recognised from the header alone. */
    char buf[96];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);

    memset(buf, 0, sizeof(buf));
    make_header(buf, 5, 5, ZSI_CSUM_XXHASH);
    buf[72] = (char)ZSI_PTRS32;             /* count stays 0 */
    zsi_put64(buf + 80, 72);                /* trailer: back pointer */
    zsi_put32(buf + 88, zsi_csum_xxhash(buf + 72, 0));   /* records region: empty */
    zsi_put32(buf + 92, zsi_csum_xxhash(buf + 72, 20));  /* section through +92 */

    zsi_name_format(name, test_uuid, 5, 5);
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_EQ(filesize(name), 96);

    ASSERT_OK(zsi_file_open(dbdir, name, 5, NULL, &f));
    ASSERT(f->hdr_valid);
    ASSERT_EQU(f->hdr.start, 5u);
    ASSERT_EQU(f->hdr.end, 5u);

    /* The kind comes from the header alone, before anything else is read. */
    ASSERT(!zsi_file_is_unordered(f));
    zsi_file_close(&f);
}

static void test_file_bounds(void)
{
    char hdr[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    make_header(hdr, 1, 0, ZSI_CSUM_XXHASH);
    zsi_name_format(name, test_uuid, 1, 0);
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

    zsi_file_close(&f);
}

static void test_file_zero_length(void)
{
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_format(name, test_uuid, 3, 0);
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
    zsi_file_close(&f);
}

static void test_file_bad_header(void)
{
    char buf[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_format(name, test_uuid, 4, 0);

    /* Garbage where a header should be (D-10).  Opens, reports the header as
     * invalid, and takes its generation from the name. */
    memset(buf, 0xFF, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    ASSERT_EQU(f->hdr.start, 4u);
    ASSERT_EQU(f->hdr.end, 0u);
    ASSERT(zsi_file_is_unordered(f));
    zsi_file_close(&f);

    /* All zeroes: no magic, and byte 0 of 0x00 is also an invalid type byte. */
    memset(buf, 0, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_close(&f);

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
        zsi_file_close(&f);
    }

    /* A header whose checksum does not match: unverifiable, so invalid. */
    make_header(buf, 4, 0, ZSI_CSUM_XXHASH);
    buf[ZSI_HDR_OFF_UUID] ^= 0xFF;
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 4, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_close(&f);
}

static void test_file_engine_from_header(void)
{
    char hdr[ZSI_HEADER_LEN];
    char name[ZSI_NAME_MAX];
    struct zsi_file *f = NULL;

    ASSERT_EQ(mkdbdir(), 0);
    zsi_name_format(name, test_uuid, 1, 0);

    /* F-5a: a file's engine comes from its OWN header, so files written under
     * different engines coexist and a reader's configuration never overrides
     * what a file records. */
    make_header(hdr, 1, 0, ZSI_CSUM_NONE);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(f->hdr_valid);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_NONE);
    ASSERT(f->csum == zsi_csum_none);
    zsi_file_close(&f);

    make_header(hdr, 1, 0, ZSI_CSUM_XXHASH);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT_EQ(f->csum_id, ZSI_CSUM_XXHASH);
    ASSERT(f->csum == zsi_csum_xxhash);
    zsi_file_close(&f);

    /* Engine 2 with the caller's function supplied: readable, and the engine is
     * reported as external rather than as whatever function happened to match. */
    make_header(hdr, 1, 0, ZSI_CSUM_EXTERNAL);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT(f->hdr_valid);
    ASSERT_EQ(f->csum_id, ZSI_CSUM_EXTERNAL);
    ASSERT(f->csum == TEST_EXTERNAL_CSUM);
    zsi_file_close(&f);

    /* The same file with NO function supplied: unverifiable, so the header is not
     * accepted.  A-6 makes this an error at the database level; here it comes back
     * as an invalid header for the caller to judge, because a tool must still be
     * able to inspect the file. */
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_close(&f);

    /* An unknown engine id, likewise. */
    make_header(hdr, 1, 0, 3);
    ASSERT_EQ(writefile(name, hdr, sizeof(hdr)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, NULL, &f));
    ASSERT(!f->hdr_valid);
    zsi_file_close(&f);
}

static void test_file_open_failures(void)
{
    struct zsi_file *f = NULL;
    char name[ZSI_NAME_MAX];

    ASSERT_EQ(mkdbdir(), 0);

    /* A missing file is ZS_NOTFOUND specifically, not a generic I/O error: the
     * snapshot protocol distinguishes them, restarting its scan on ENOENT because
     * a file may legitimately be unlinked mid-scan (C-4 step 3). */
    zsi_name_format(name, test_uuid, 99, 0);
    ASSERT_EQ(zsi_file_open(dbdir, name, 99, NULL, &f), ZS_NOTFOUND);
    ASSERT_NULL(f);

    /* A directory where a data file should be is malformed, not missing. */
    char sub[PATH_MAX];
    snprintf(sub, sizeof(sub), "%s/notafile", dbdir);
    ASSERT_EQ(mkdir(sub, 0700), 0);
    ASSERT_EQ(zsi_file_open(dbdir, "notafile", 1, NULL, &f), ZS_BADFORMAT);
    ASSERT_NULL(f);

    /* Closing a NULL handle is a no-op, so cleanup paths need no guard. */
    zsi_file_close(&f);
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
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_rec(&s, "c", 1, "3", 1, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_STR_EQ(c.key[1], "b");
    ASSERT_STR_EQ(c.key[2], "c");
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    zsi_file_close(&f);
    sb_free(&s);

    /* Several spans, all committed, replay in order across span boundaries. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_rec(&s, "c", 1, "3", 1, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_STR_EQ(c.key[2], "c");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* An empty span -- a terminator with no records -- is legal (F-23 says zero
     * or more), and contributes nothing. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_term(&s, false);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* Deletions are presented as records with a NULL value (A-1); the replay does
     * not filter them, because resolving a deletion into "absent" is the read
     * path's job, and an index must know the tombstone exists. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_rec(&s, "a", 1, NULL, 0, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 2u);
    ASSERT(!c.isdel[0]);
    ASSERT(c.isdel[1]);
    zsi_file_close(&f);
    sb_free(&s);
}

static void test_span_rollback(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* A rolled-back span replays nothing (F-21, F-25). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    /* ...but the file is still clean: a rollback is a valid span terminator, so
     * the writer may keep appending to it (F-26h). */
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    zsi_file_close(&f);
    sb_free(&s);

    /* F-25 directly: visibility is per span, NOT a watermark.  A rolled-back span
     * sits between two live ones, and both live spans must survive it -- a
     * high-water-mark implementation would lose the third span or keep the
     * second. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "first", 5, "1", 1, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "aborted", 7, "x", 1, false, 0);
    sb_term(&s, true);
    sb_rec(&s, "third", 5, "3", 1, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 2u);
    ASSERT_STR_EQ(c.key[0], "first");
    ASSERT_STR_EQ(c.key[1], "third");
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* Every span rolled back: clean, zero records, not an error (F-26h). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, true);
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT(zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* Interleaved, several of each, to catch an implementation that skips one
     * rollback correctly but loses its place afterwards. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    for (int i = 0; i < 6; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%d", i);
        sb_rec(&s, k, strlen(k), "v", 1, false, 0);
        sb_term(&s, (i % 2) == 1);       /* odd spans rolled back */
    }
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 3u);
    ASSERT_STR_EQ(c.key[0], "k0");
    ASSERT_STR_EQ(c.key[1], "k2");
    ASSERT_STR_EQ(c.key[2], "k4");
    zsi_file_close(&f);
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
    zsi_file_close(&f);
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
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    good_end = s.len;
    sb_rec(&s, "b", 1, "2", 1, false, 0);       /* no terminator */
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* The same file with the terminator present but a data byte flipped.  F-22:
     * because the checksum covers the span AND the terminator, the outcome is
     * identical -- the span reads as absent. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    good_end = s.len;
    size_t victim = s.len;
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_term(&s, false);
    s.buf[victim + 5] ^= 0x01;                  /* damage the span's data */
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* Trailing garbage after a valid span. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    good_end = s.len;
    sb_raw(&s, "\xde\xad\xbe\xef\xde\xad\xbe\xef", 8);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);
    sb_free(&s);

    /* Truncated at every byte offset past the first valid span.  Whatever the
     * truncation point, the answer is the committed prefix and never a crash. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    good_end = s.len;
    sb_rec(&s, "bb", 2, "22", 2, false, 0);
    sb_term(&s, false);
    size_t full = s.len;

    for (size_t cut = good_end; cut < full; cut++) {
        char name[ZSI_NAME_MAX];
        zsi_name_format(name, test_uuid, 1, 0);
        ASSERT_EQ(mkdbdir(), 0);
        ASSERT_EQ(writefile(name, s.buf, cut), 0);
        memset(&c, 0, sizeof(c));
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        ASSERT_OK(zsi_unordered_replay(f, false, collect_cb, &c));
        if (c.n != 1 || f->complete != good_end) {
            fprintf(stderr, "\n    FAIL cut at %zu: n=%zu complete=%zu (want 1, %zu)\n",
                    cut, c.n, f->complete, good_end);
            current_test_failed = 1;
            zsi_file_close(&f);
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

        zsi_file_close(&f);
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
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    size_t good_end = s.len;

    size_t data_at = s.len;
    sb_rec(&s, "ghost", 5, "value", 5, false, 0);
    size_t data_len = s.len - data_at;
    sb_term(&s, false);
    memset(s.buf + data_at, 0, data_len);       /* data never landed */

    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_STR_EQ(c.key[0], "a");
    ASSERT_EQU(f->complete, good_end);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);

    /* And with checksums not verified, the same file yields the ghost record --
     * which is exactly the guarantee ZS_NOCSUM gives up (F-5e).  Asserted so the
     * cost is visible rather than merely stated in a comment. */
    {
        char name[ZSI_NAME_MAX];
        zsi_name_format(name, test_uuid, 1, 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        memset(&c, 0, sizeof(c));
        ASSERT_OK(zsi_unordered_replay(f, true, collect_cb, &c));
        /* the zeroed region does not decode as a record, so the span is rejected
         * on structure rather than on checksum -- the point is that the OUTCOME
         * differs from the verified case only when the damage is undetectable
         * structurally, which the next case constructs */
        zsi_file_close(&f);
    }
    sb_free(&s);

    /* A structurally valid span whose contents were altered: only the checksum
     * distinguishes it.  Verified, it is rejected; unverified, the altered data is
     * returned as though committed. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    size_t at = s.len;
    sb_rec(&s, "key", 3, "aaa", 3, false, 0);
    sb_term(&s, false);
    s.buf[at + 4 + 3 + 1] = 'b';                /* first value byte: aaa -> baa */

    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);                        /* checksum catches it */
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    zsi_file_close(&f);

    {
        char name[ZSI_NAME_MAX];
        zsi_name_format(name, test_uuid, 1, 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        memset(&c, 0, sizeof(c));
        ASSERT_OK(zsi_unordered_replay(f, true, collect_cb, &c));
        ASSERT_EQU(c.n, 1u);                    /* undetected without the csum */
        ASSERT_STR_EQ(c.key[0], "key");
        zsi_file_close(&f);
    }
    sb_free(&s);
}

static void test_span_bad_length(void)
{
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    /* A terminator whose span length disagrees with the bytes actually present.
     * F-23 requires they match, and this is checked independently of the checksum
     * so that a length-only corruption is caught by the rule that names it. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);
    size_t good_end = s.len;
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_term_badlen(&s, 999);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);
    ASSERT_EQU(f->complete, good_end);
    zsi_file_close(&f);
    sb_free(&s);

    /* Claiming zero when records are present. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term_badlen(&s, 0);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, (size_t)ZSI_HEADER_LEN);
    zsi_file_close(&f);
    sb_free(&s);
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
    zsi_file_close(&f);
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
    zsi_file_close(&f);
    sb_free(&s);

    /* A file of zero bytes throughout: 0x00 is an invalid type byte (F-12), which
     * is what a sparse hole reads as. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    memset(junk, 0x00, sizeof(junk));
    sb_raw(&s, junk, sizeof(junk));
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    zsi_file_close(&f);
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
    zsi_name_format(name, test_uuid, 2, 0);

    /* D-10: an invalid header means zero spans, complete at 0, and NOT clean --
     * so a writer moves on rather than appending past a boundary that failed to
     * validate (R-4). */
    memset(buf, 0xFF, sizeof(buf));
    ASSERT_EQ(writefile(name, buf, sizeof(buf)), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, NULL, &f));
    memset(&c, 0, sizeof(c));
    ASSERT_OK(zsi_unordered_replay(f, false, collect_cb, &c));
    ASSERT_EQU(c.n, 0u);
    ASSERT_EQU(f->complete, 0u);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);

    /* A zero-length file, likewise: complete == size == 0, and yet NOT clean,
     * because D-9 requires a valid header.  This is the case where a
     * complete == size test alone would wrongly report clean and let a writer
     * append to a file with no header. */
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 2, NULL, &f));
    ASSERT_OK(zsi_unordered_replay(f, false, collect_cb, &c));
    ASSERT_EQU(f->complete, 0u);
    ASSERT_EQU(f->size, 0u);
    ASSERT(!zsi_unordered_is_clean(f));
    zsi_file_close(&f);

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
    ASSERT_EQ(zsi_unordered_replay(f, false, collect_cb, &c), ZS_BADUSAGE);
    zsi_file_close(&f);
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
        sb_rec(&s, "a", 1, "1", 1, false, 0);
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
        zsi_file_close(&f);
        sb_free(&s);
    }
}

static void test_span_engine_zero(void)
{
    /* Engine 0 writes zeros and never verifies (F-5c), so a torn tail is
     * undetectable.  The span-length check still applies, which is why a
     * length-only corruption is caught even here -- but altered data is not. */
    struct sb s;
    struct collected c;
    struct zsi_file *f = NULL;

    sb_init(&s, 1, ZSI_CSUM_NONE);
    size_t at = s.len;
    sb_rec(&s, "key", 3, "aaa", 3, false, 0);
    sb_term(&s, false);
    s.buf[at + 4 + 3 + 1] = 'b';
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 1u);                        /* undetected: engine 0 */
    ASSERT_EQ(f->csum_id, ZSI_CSUM_NONE);
    zsi_file_close(&f);
    sb_free(&s);

    /* But a length disagreement is structural and still caught. */
    sb_init(&s, 1, ZSI_CSUM_NONE);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term_badlen(&s, 999);
    ASSERT_EQ(replay_file(&s, 1, &c, &f), ZS_OK);
    ASSERT_EQU(c.n, 0u);
    zsi_file_close(&f);
    sb_free(&s);
}

static void test_span_long_terminator(void)
{
    /* A span over 0xFFFFFF bytes forces a long terminator (F-15), and the walk
     * must handle the 24-byte form -- including that its checksum lives at +20,
     * not +4. */
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
        sb_rec(&s, k, strlen(k), val, sizeof(val), false, 0);
    }
    ASSERT(s.len - s.span_start > ZSI_SHORT_SPANLEN_MAX);
    size_t nrecs = n;
    sb_term(&s, false);

    /* The terminator really is the long form. */
    ASSERT_EQ((unsigned char)s.buf[s.span_start - ZSI_TERMLEN_LONG],
              ZSI_COMMIT_LONG);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));

    /* A counting callback, since the span holds far more than the 64-entry
     * collector holds -- and counting is the assertion that matters: every record
     * of a large span must be presented, not just the ones before some limit. */
    size_t count = 0;
    ASSERT_OK(zsi_unordered_replay(f, false, count_cb, &count));
    ASSERT_EQU(count, nrecs);
    ASSERT(zsi_unordered_is_clean(f));
    ASSERT_EQU(f->complete, s.len);
    ASSERT(nrecs > 2000);       /* enough records that the span really is large */

    (void)c;
    zsi_file_close(&f);
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
    zsi_name_format(name, test_uuid, gen, 0);
    if (mkdbdir() != 0) return -1;
    if (sb_write(s, name) != 0) return -1;
    if (zsi_file_open(dbdir, name, gen, TEST_EXTERNAL_CSUM, fp) != ZS_OK)
        return -1;
    return zsi_index_build(*fp, zsi_compar_default, false);
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
    sb_rec(&s, "live", 4, "1", 1, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "aborted", 7, "x", 1, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "live", 4, &off));
    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "aborted", 7, &off),
              ZS_NOTFOUND);

    char keys[256];
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "live");
    zsi_file_close(&f);
    sb_free(&s);

    /* A key committed, then rewritten in a rolled-back span: the committed
     * version survives and the aborted rewrite is invisible.  An implementation
     * that walked every record would return the aborted value. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "good", 4, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "k", 1, "bad", 3, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    struct zsi_rec r;
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_EQU(r.vallen, 4u);
    ASSERT_MEM_EQ(r.val, "good", 4);
    zsi_file_close(&f);
    sb_free(&s);

    /* A key deleted in a rolled-back span stays present. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "v", 1, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "k", 1, NULL, 0, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_NOT_NULL(r.val);
    zsi_file_close(&f);
    sb_free(&s);
}

static void test_index_newest_per_key(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;
    struct zsi_rec r;
    char keys[256];

    /* A key written three times at increasing offsets yields exactly one entry:
     * the one at the highest offset (D-14, D-14h). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "v1", 2, false, 0);
    sb_rec(&s, "k", 1, "v2", 2, false, 0);
    sb_rec(&s, "k", 1, "v3", 2, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "k");           /* once, not three times */

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_MEM_EQ(r.val, "v3", 2);
    zsi_file_close(&f);
    sb_free(&s);

    /* Across spans, too, and with a deletion last: the tombstone is the newest
     * version and the index must expose it rather than the value before it.
     * Resolving a deletion into "absent" is the read path's job (D-14); an index
     * that dropped tombstones would make a deleted key reappear from an older
     * file. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "k", 1, "v1", 2, false, 0);
    sb_term(&s, false);
    sb_rec(&s, "k", 1, NULL, 0, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_NULL(r.val);                 /* the tombstone, not "v1" */
    zsi_file_close(&f);
    sb_free(&s);

    /* Interleaved keys, each rewritten: every key appears once, with its newest
     * value, and the ordering is by key rather than by offset. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "c", 1, "c1", 2, false, 0);
    sb_rec(&s, "a", 1, "a1", 2, false, 0);
    sb_rec(&s, "b", 1, "b1", 2, false, 0);
    sb_rec(&s, "a", 1, "a2", 2, false, 0);
    sb_rec(&s, "c", 1, "c2", 2, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c");

    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "a", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_MEM_EQ(r.val, "a2", 2);
    ASSERT_OK(zsi_index_find(f->index, zsi_compar_default, "c", 1, &off));
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
    ASSERT_MEM_EQ(r.val, "c2", 2);
    zsi_file_close(&f);
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
        sb_rec(&s, ins[i], strlen(ins[i]), "v", 1, false, 0);
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
    zsi_file_close(&f);
    sb_free(&s);

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "abc", 3, "v", 1, false, 0);
    sb_rec(&s, "ab", 2, "v", 1, false, 0);
    sb_rec(&s, "a", 1, "v", 1, false, 0);
    sb_rec(&s, "b", 1, "v", 1, false, 0);
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|ab|abc|b");
    zsi_file_close(&f);
    sb_free(&s);
}

static void test_index_empty(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;
    struct zsi_index_cur c;
    struct zsi_rec r;
    char keys[64];

    /* An index over a file with no committed records: lookup returns ZS_NOTFOUND
     * and the cursor is immediately exhausted.  D-14b requires this be an ordinary
     * case rather than a special one, because a binary search over a zero-length
     * array and an index for an empty file are both routine (F-26h). */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    ASSERT_NOT_NULL(f->index);
    ASSERT_EQU(f->index->nbase, 0u);

    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "k", 1, &off),
              ZS_NOTFOUND);
    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "", 0, &off),
              ZS_NOTFOUND);

    zsi_index_cur_seek_first(&c);
    ASSERT_EQ(zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL),
              ZS_DONE);
    /* Advancing an exhausted cursor is a no-op rather than an overrun. */
    zsi_index_cur_next(f->index, zsi_compar_default, &c);
    ASSERT_EQ(zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL),
              ZS_DONE);

    zsi_index_cur_seek(f->index, zsi_compar_default, "anything", 8, &c);
    ASSERT_EQ(zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL),
              ZS_DONE);

    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "");
    zsi_file_close(&f);
    sb_free(&s);

    /* Same for a file whose every span was rolled back. */
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, true);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);
    ASSERT_EQU(f->index->nbase, 0u);
    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "a", 1, &off),
              ZS_NOTFOUND);
    zsi_file_close(&f);
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
    sb_rec(&s, "base0", 5, "b", 1, false, 0);
    sb_rec(&s, "base1", 5, "b", 1, false, 0);
    sb_term(&s, false);

    /* Record the offsets of everything appended after the base span. */
    size_t offs[ZSI_DELTA_MAX + 40];
    size_t n = 0;
    for (n = 0; n < ZSI_DELTA_MAX + 32; n++) {
        char k[32];
        snprintf(k, sizeof(k), "d%06zu", n);
        offs[n] = s.len;
        sb_rec(&s, k, strlen(k), "v", 1, false, 0);
    }
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));

    /* Build the index over the FIRST span only, by replaying a truncated view.
     * Simpler: build over everything, then reset to just the base entries and
     * re-insert.  Instead of contriving that, build normally and then assert the
     * insert path against a fresh index containing only the base span's keys. */
    ASSERT_OK(zsi_index_build(f, zsi_compar_default, false));
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
        ASSERT_OK(zsi_index_insert(f->index, zsi_compar_default, offs[i]));
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
                zsi_file_close(&f);
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

    zsi_file_close(&f);
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
    sb_rec(&s, "a", 1, "old", 3, false, 0);
    sb_rec(&s, "b", 1, "bee", 3, false, 0);
    sb_rec(&s, "c", 1, "see", 3, false, 0);
    sb_term(&s, false);
    size_t newer_a = s.len;
    sb_rec(&s, "a", 1, "new", 3, false, 0);
    size_t newer_c = s.len;
    sb_rec(&s, "c", 1, "cee", 3, false, 0);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default, false));

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
    ix->base[1] = ZSI_HEADER_LEN + zsi_rec_encoded_len(1, 3, false, false);
    ix->base[2] = ix->base[1] + zsi_rec_encoded_len(1, 3, false, false);
    ix->nbase = 3;
    ix->ndelta = 0;

    index_keys(f, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b|c");

    ASSERT_OK(zsi_index_insert(ix, zsi_compar_default, newer_a));
    ASSERT_OK(zsi_index_insert(ix, zsi_compar_default, newer_c));
    ASSERT_EQU(ix->ndelta, 2u);

    /* Lookup prefers the delta. */
    ASSERT_OK(zsi_index_find(ix, zsi_compar_default, "a", 1, &off));
    ASSERT_EQU(off, newer_a);
    ASSERT_OK(zsi_rec_decode(zsi_file_at(f, off, 1), f->size - off,
                             f->hdr.start, &r));
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
    ASSERT_OK(zsi_index_insert(ix, zsi_compar_default, newer_a));
    ASSERT_EQU(ix->ndelta, 2u);

    zsi_file_close(&f);
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
        sb_rec(&s, k, strlen(k), "old", 3, false, 0);
    }
    sb_term(&s, false);

    /* Span two: the same keys with value "new". */
    for (size_t i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof(k), "k%06zu", i);
        new_off[i] = s.len;
        sb_rec(&s, k, strlen(k), "new", 3, false, 0);
    }
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default, false));

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
        ASSERT_OK(zsi_index_insert(ix, zsi_compar_default, new_off[i]));
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
    zsi_file_close(&f);
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
    sb_rec(&s, k2, 3, "v2", 2, false, 0);
    sb_rec(&s, k1, 3, "v1", 2, false, 0);
    sb_rec(&s, k4, 2, "v4", 2, false, 0);
    sb_rec(&s, k3, 1, "v3", 2, false, 0);
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
    zsi_file_close(&f);
    sb_free(&s);
}

static void test_index_many(void)
{
    struct sb s;
    struct zsi_file *f = NULL;
    size_t off;

    /* Enough records that the sort matters, with keys generated in an order that
     * is neither ascending nor descending. */
    enum { N = 3000 };
    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    for (size_t i = 0; i < N; i++) {
        char k[32];
        /* a multiplicative shuffle: hits every value once, in scrambled order */
        snprintf(k, sizeof(k), "key%08zu", (i * 2654435761u) % N);
        sb_rec(&s, k, strlen(k), "v", 1, false, 0);
    }
    sb_term(&s, false);
    ASSERT_EQ(index_file(&s, 1, &f), ZS_OK);

    /* Every generated key is present, and the count matches the distinct keys. */
    ASSERT(f->index->nbase > 0);
    ASSERT(f->index->nbase <= N);

    for (size_t i = 0; i < N; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%08zu", (i * 2654435761u) % N);
        if (zsi_index_find(f->index, zsi_compar_default, k, strlen(k), &off)
            != ZS_OK) {
            fprintf(stderr, "\n    FAIL missing %s\n", k);
            current_test_failed = 1;
            zsi_file_close(&f);
            sb_free(&s);
            return;
        }
    }

    /* Traversal is sorted and yields each key once. */
    struct zsi_index_cur c;
    struct zsi_rec r;
    zsi_index_cur_seek_first(&c);
    size_t seen = 0;
    char prev[32];
    size_t prevlen = 0;
    while (zsi_index_cur_get(f->index, zsi_compar_default, &c, &r, NULL) == ZS_OK) {
        if (prevlen &&
            zsi_compar_default(prev, prevlen, r.key, r.keylen) >= 0) {
            fprintf(stderr, "\n    FAIL out of order at %zu\n", seen);
            current_test_failed = 1;
            zsi_file_close(&f);
            sb_free(&s);
            return;
        }
        memcpy(prev, r.key, r.keylen);
        prevlen = r.keylen;
        seen++;
        zsi_index_cur_next(f->index, zsi_compar_default, &c);
    }
    ASSERT_EQU(seen, f->index->nbase);

    /* An absent key is absent. */
    ASSERT_EQ(zsi_index_find(f->index, zsi_compar_default, "nope", 4, &off),
              ZS_NOTFOUND);

    zsi_file_close(&f);
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
    zsi_file_close(&f);

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
    zsi_file_close(&f);
    ib_free(&b0);

    ib_free(&b);
}

static void test_inorder_roundtrip(void)
{
    struct ib b;
    struct zsi_file *f = NULL;
    struct zsi_rec r;

    /* Records in key order, one per key, as a repack emits (D-17). */
    ib_init(&b, 1, 4, ZSI_CSUM_XXHASH);
    ib_rec(&b, "alpha", 5, "1", 1, false, 0);
    ib_rec(&b, "beta", 4, "2", 2, false, 0);
    ib_rec(&b, "gamma", 5, NULL, 0, true, 2);      /* a retained tombstone */
    ib_rec(&b, "delta", 5, "444", 3, false, 0);    /* deliberately out of order */
    ib_finish(&b);

    ASSERT_EQ(ib_load(&b, 1, 4, &f), ZS_OK);
    ASSERT_EQU(f->nptrs, 4u);
    ASSERT(!f->ptr_wide);
    ASSERT_OK(zsi_ptrs_verify_records(f));

    /* Every pointer resolves to the record it was built from. */
    ASSERT_OK(zsi_ptrs_rec(f, 0, &r));
    ASSERT_MEM_EQ(r.key, "alpha", 5);
    ASSERT_OK(zsi_ptrs_rec(f, 2, &r));
    ASSERT_MEM_EQ(r.key, "gamma", 5);
    ASSERT_NULL(r.val);                            /* the tombstone */
    ASSERT_EQU(r.ancestor, 2u);                    /* stored, and below start */
    ASSERT_OK(zsi_ptrs_rec(f, 3, &r));
    ASSERT_MEM_EQ(r.key, "delta", 5);

    /* A record with no stored ancestor decodes to the file's start (F-17). */
    ASSERT_OK(zsi_ptrs_rec(f, 0, &r));
    ASSERT_EQU(r.ancestor, 1u);

    zsi_file_close(&f);
    ib_free(&b);
}

static void test_inorder_search(void)
{
    struct ib b;
    struct zsi_file *f = NULL;
    uint64_t idx;
    bool exact;

    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    static const char *keys[] = { "b", "d", "f", "h", "j" };
    for (size_t i = 0; i < 5; i++)
        ib_rec(&b, keys[i], 1, "v", 1, false, 0);
    ib_finish(&b);
    ASSERT_EQ(ib_load(&b, 1, 1, &f), ZS_OK);

    /* Exact hits at every position, including both ends. */
    for (size_t i = 0; i < 5; i++) {
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, keys[i], 1,
                                  &idx, &exact));
        ASSERT(exact);
        ASSERT_EQU(idx, i);
    }

    /* Misses land on the first key greater than the target. */
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "a", 1, &idx, &exact));
    ASSERT(!exact); ASSERT_EQU(idx, 0u);
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "c", 1, &idx, &exact));
    ASSERT(!exact); ASSERT_EQU(idx, 1u);
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "i", 1, &idx, &exact));
    ASSERT(!exact); ASSERT_EQU(idx, 4u);
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "z", 1, &idx, &exact));
    ASSERT(!exact); ASSERT_EQU(idx, 5u);            /* past every key */
    ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "", 0, &idx, &exact));
    ASSERT(!exact); ASSERT_EQU(idx, 0u);
    zsi_file_close(&f);
    ib_free(&b);

    /* One record, and two records: the sizes where an off-by-one in the probe or
     * the bisection shows up. */
    for (size_t n = 1; n <= 2; n++) {
        ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
        for (size_t i = 0; i < n; i++)
            ib_rec(&b, keys[i * 2], 1, "v", 1, false, 0);
        ib_finish(&b);
        ASSERT_EQ(ib_load(&b, 1, 1, &f), ZS_OK);
        ASSERT_EQU(f->nptrs, n);

        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "b", 1, &idx, &exact));
        ASSERT(exact); ASSERT_EQU(idx, 0u);
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "a", 1, &idx, &exact));
        ASSERT(!exact); ASSERT_EQU(idx, 0u);
        ASSERT_OK(zsi_ptrs_search(f, zsi_compar_default, "z", 1, &idx, &exact));
        ASSERT(!exact); ASSERT_EQU(idx, n);
        zsi_file_close(&f);
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
    ib_rec(&b, "a", 1, "1", 1, false, 0);
    ib_rec(&b, "b", 1, "2", 1, false, 0);
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
            zsi_file_close(&f);
            goto done;
        }
        zsi_file_close(&f);
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
    zsi_file_close(&f);

    /* A file shorter than header plus trailer, at every length. */
    for (size_t len = 0; len < ZSI_HEADER_LEN + ZSI_TRAILER_LEN; len += 8) {
        ASSERT_EQ(writefile(name, orig, len), 0);
        ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
        ASSERT(zsi_ptrs_load(f) != ZS_OK);
        zsi_file_close(&f);
    }

    /* A corrupted pad byte inside the section is caught by the section checksum
     * (F-26d says the checksum covers the padding). */
    memcpy(b.buf, orig, full);
    ASSERT(full - ZSI_TRAILER_LEN > ptr_off + 8);
    b.buf[full - ZSI_TRAILER_LEN - 1] ^= 0x01;
    ASSERT_EQ(writefile(name, b.buf, full), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADCHECKSUM);
    zsi_file_close(&f);

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
    zsi_file_close(&f);

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
    zsi_file_close(&f);

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
    zsi_file_close(&f);

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
    ib_rec(&b, "a", 1, "value", 5, false, 0);
    ib_rec(&b, "b", 1, "other", 5, false, 0);
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
    zsi_file_close(&f);

    /* Undamaged, the same check passes. */
    ib_free(&b);
    ib_init(&b, 1, 1, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "value", 5, false, 0);
    ib_rec(&b, "b", 1, "other", 5, false, 0);
    ib_finish(&b);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_ptrs_load(f));
    ASSERT_OK(zsi_ptrs_verify_records(f));
    zsi_file_close(&f);
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
            ib_rec(&b, k, strlen(k), "v", 1, false, 0);
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
        zsi_file_close(&f);
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
    size_t reclen = zsi_rec_encoded_len(1, 1, false, false);
    size_t records_end = ZSI_HEADER_LEN + 2 * reclen;
    size_t seclen = 16 + 2 * 8;
    size_t total = records_end + seclen + ZSI_TRAILER_LEN;

    char *buf = calloc(1, total);
    ASSERT_NOT_NULL(buf);
    make_header(buf, 1, 1, ZSI_CSUM_XXHASH);
    zsi_rec_encode(buf + ZSI_HEADER_LEN, "a", 1, "1", 1, false, 0);
    zsi_rec_encode(buf + ZSI_HEADER_LEN + reclen, "b", 1, "2", 1, false, 0);

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

    zsi_file_close(&f);
    free(buf);
}

static void test_inorder_kind_rules(void)
{
    /* Loading a pointer section from an UNORDERED file is a usage error: it has
     * none, and the kind is knowable from the header alone (section 2). */
    struct sb s;
    struct zsi_file *f = NULL;

    sb_init(&s, 1, ZSI_CSUM_XXHASH);
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT(zsi_file_is_unordered(f));
    ASSERT_EQ(zsi_ptrs_load(f), ZS_BADUSAGE);
    zsi_file_close(&f);
    sb_free(&s);

    /* And a pointers block is present exactly when end != 0 (T-6): an in-order
     * file loads one, an unordered file has none. */
    struct ib b;
    ib_init(&b, 3, 3, ZSI_CSUM_XXHASH);
    ib_rec(&b, "a", 1, "1", 1, false, 0);
    ib_finish(&b);
    ASSERT_EQ(ib_load(&b, 3, 3, &f), ZS_OK);
    ASSERT(!zsi_file_is_unordered(f));
    ASSERT_EQU(f->nptrs, 1u);
    zsi_file_close(&f);
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
        ib_rec(&b, keys[i], 1, "v", 1, false, 0);
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
            zsi_file_close(&f);
            ib_free(&b);
            return;
        }
    }

    zsi_file_close(&f);
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
        sb_rec(s, keys[i], strlen(keys[i]), "v", 1, false, 0);
    sb_term(s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(sb_write(s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, uf));
    ASSERT_OK(zsi_index_build(*uf, zsi_compar_default, false));

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
        ib_rec(b, sorted[i], strlen(sorted[i]), "v", 1, false, 0);
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
    zsi_file_close(&uf);
    zsi_file_close(&inf);
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
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &uf));
    ASSERT_OK(zsi_index_build(uf, zsi_compar_default, false));

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

    zsi_file_close(&uf);
    zsi_file_close(&inf);
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
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_rec(&s, "k", 1, "v1", 2, false, 0);
    sb_rec(&s, "k", 1, "v2", 2, false, 0);
    sb_rec(&s, "z", 1, "9", 1, false, 0);
    sb_rec(&s, "k", 1, "v3", 2, false, 0);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default, false));

    zsi_fcur_init_file(&fc, f, zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|k|z");            /* k once, not three times */

    /* And it is the newest version. */
    ASSERT_OK(zsi_fcur_seek(&fc, "k", 1));
    ASSERT(!fc.exhausted);
    ASSERT_MEM_EQ(fc.cur.val, "v3", 2);

    zsi_file_close(&f);
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
    sb_rec(&s, "a", 1, "1", 1, false, 0);
    sb_rec(&s, "b", 1, "2", 1, false, 0);
    sb_rec(&s, "b", 1, NULL, 0, false, 0);
    sb_term(&s, false);

    char name[ZSI_NAME_MAX];
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(sb_write(&s, name), 0);
    ASSERT_OK(zsi_file_open(dbdir, name, 1, TEST_EXTERNAL_CSUM, &f));
    ASSERT_OK(zsi_index_build(f, zsi_compar_default, false));

    zsi_fcur_init_file(&fc, f, zsi_compar_default);
    fcur_keys_from(&fc, NULL, 0, keys, sizeof(keys));
    ASSERT_STR_EQ(keys, "a|b");

    ASSERT_OK(zsi_fcur_seek(&fc, "b", 1));
    ASSERT(!fc.exhausted);
    ASSERT_NULL(fc.cur.val);                 /* the tombstone reaches the merge */

    struct zsi_rec r;
    ASSERT_OK(zsi_fcur_find(&fc, "b", 1, &r));
    ASSERT_NULL(r.val);

    zsi_file_close(&f);
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

/* Format a data-file name for the test UUID into a static rotating buffer. */
static const char *dn(uint32_t start, uint32_t end)
{
    static char bufs[8][ZSI_NAME_MAX];
    static int which = 0;
    char *b = bufs[which = (which + 1) % 8];
    zsi_name_format(b, test_uuid, start, end);
    return b;
}

static void test_fileset_derives_from_names(void)
{
    struct zsi_fileset fs;
    char got[128];

    const char *names[] = { dn(1, 4), dn(5, 5), dn(6, 0), NULL };
    seed_names(names);

    ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
    ASSERT(fs.have_uuid);
    ASSERT_MEM_EQ(fs.uuid, test_uuid, 16);
    ASSERT_EQU(fs.nall, 3u);

    ASSERT_OK(zsi_fileset_resolve(&fs));
    resolved_gens(&fs, got, sizeof(got));
    ASSERT_STR_EQ(got, "1-4|5-5|6");

    uint32_t next;
    ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
    ASSERT_EQU(next, 7u);
    zsi_fileset_fini(&fs);
}

static void test_fileset_overlap_table(void)
{
    /* Each row of D-5a's table.  An overlap is resolved, not rejected: an output
     * is renamed into place before its inputs are removed, so a scan legitimately
     * sees both. */
    struct zsi_fileset fs;
    char got[128];

    /* A repack output [1-4] present with its inputs [1-1]..[4-4].  The widest
     * wins, because fixed-width hex makes lexical order numeric. */
    {
        const char *names[] = { dn(1,1), dn(2,2), dn(3,3), dn(4,4), dn(1,4),
                                dn(5,0), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5");
        zsi_fileset_fini(&fs);
    }

    /* The same with some inputs already unlinked: the set still tiles. */
    {
        const char *names[] = { dn(2,2), dn(1,4), dn(5,0), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5");
        zsi_fileset_fini(&fs);
    }

    /* A conversion output present with its input: the IN-ORDER file wins, because
     * the unordered name is a strict prefix and so sorts first (D-1a). */
    {
        const char *names[] = { dn(5,0), dn(5,5), dn(1,4), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        resolved_gens(&fs, got, sizeof(got));
        ASSERT_STR_EQ(got, "1-4|5-5");
        zsi_fileset_fini(&fs);
    }

    /* All three at once: unordered N, N-N, and a wider N-M.  The widest wins. */
    {
        const char *names[] = { dn(1,4), dn(5,0), dn(5,5), dn(5,9), NULL };
        seed_names(names);
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
    seed_names(names);
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
        seed_names(names);
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
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_resolve(&fs));
        ASSERT_EQU(fs.nresolved, 2u);
        zsi_fileset_fini(&fs);
    }

    /* A gap immediately after the first file. */
    {
        const char *names[] = { dn(1,2), dn(4,0), NULL };
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_resolve(&fs), ZS_AGAIN);
        zsi_fileset_fini(&fs);
    }

    /* D-5c: a PARTIAL overlap -- ranges that intersect where neither contains the
     * other -- cannot arise from any legal sequence and is corruption, reported
     * rather than resolved. */
    {
        const char *names[] = { dn(1,5), dn(3,8), NULL };
        seed_names(names);
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
    char othername[ZSI_NAME_MAX];
    zsi_name_format(othername, other, 1, 0);

    const char *names[] = { dn(1, 0), othername, NULL };
    seed_names(names);
    ASSERT_EQ(zsi_fileset_scan(dbdir, NULL, &fs), ZS_BADFORMAT);

    /* Two of one and one of the other: still an error, not a vote. */
    const char *names2[] = { dn(1, 0), dn(2, 0), othername, NULL };
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
    seed_names(names);

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
        seed_names(names);
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
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_OK(zsi_fileset_next_gen(&fs, &next));
        ASSERT_EQU(next, 6u);
        zsi_fileset_fini(&fs);
    }

    /* And after files have been removed: the highest present still decides. */
    {
        const char *names[] = { dn(1,4), dn(5,5), NULL };
        seed_names(names);
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
        seed_names(names);
        ASSERT_OK(zsi_fileset_scan(dbdir, NULL, &fs));
        ASSERT_EQ(zsi_fileset_next_gen(&fs, &next), ZS_FULL);
        zsi_fileset_fini(&fs);
    }

    /* One below the ceiling still allocates. */
    {
        const char *names[] = { dn(0xFFFFFFFEu, 0), NULL };
        seed_names(names);
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
    seed_names(names);

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
        sb_rec(&s, keys[i], strlen(keys[i]), "v", 1, false, 0);
    sb_term(&s, false);
    zsi_name_format(name, test_uuid, gen, 0);
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
        ib_rec(&b, sorted[i], strlen(sorted[i]), "v", 1, false, 0);
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
                                TEST_EXTERNAL_CSUM, false, &s));
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
                                TEST_EXTERNAL_CSUM, false, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT_NULL(zsi_snapshot_active(s));
    zsi_snapshot_release(&s);

    /* An empty directory snapshots to zero files rather than failing: that is the
     * state D-8a turns into a new database. */
    clear_db();
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &s));
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
                                TEST_EXTERNAL_CSUM, false, &s));
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
                                TEST_EXTERNAL_CSUM, false, &s));
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
                                TEST_EXTERNAL_CSUM, false, &s), ZS_AGAIN);
    ASSERT_NULL(s);

    /* A partial overlap is corruption rather than a stale scan, so it is
     * reported immediately rather than retried to exhaustion. */
    clear_db();
    put_inorder(1, 5, k);
    put_inorder(3, 8, k);
    ASSERT_EQ(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &s), ZS_BADFORMAT);

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
    sb_rec(&s, "visible", 7, "1", 1, false, 0);
    sb_term(&s, false);
    size_t boundary = s.len;
    sb_rec(&s, "invisible", 9, "2", 1, false, 0);   /* no terminator */
    zsi_name_format(name, test_uuid, 1, 0);
    ASSERT_EQ(sb_write(&s, name), 0);

    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &snap));
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

    /* Corrupt the ACTIVE (highest, unordered) file: tolerated. */
    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_format(name, test_uuid, 2, 0);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT(!s->files[1]->hdr_valid);
    ASSERT_EQU(s->files[1]->complete, 0u);
    ASSERT(!zsi_unordered_is_clean(s->files[1]));
    zsi_snapshot_release(&s);

    /* Corrupt a NON-ACTIVE file: an error. */
    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_format(name, test_uuid, 1, 1);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);
    ASSERT_EQ(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &s), ZS_BADFORMAT);
    ASSERT_NULL(s);

    /* A zero-length active file, likewise tolerated (D-10). */
    clear_db();
    put_inorder(1, 1, k);
    zsi_name_format(name, test_uuid, 2, 0);
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_OK(zsi_snapshot_take(dbdir, &test_uuid, zsi_compar_default,
                                TEST_EXTERNAL_CSUM, false, &s));
    ASSERT_EQU(s->nfiles, 2u);
    ASSERT(!s->files[1]->hdr_valid);
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
                                TEST_EXTERNAL_CSUM, false, &s));
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
static void test_lock_two_handles_one_process(void)
{
    struct zsi_locks a, b;

    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_OK(zsi_lock_open(&a, dbdir));
    ASSERT_OK(zsi_lock_open(&b, dbdir));

    ASSERT_OK(zsi_lock_take(&a, ZSI_LOCK_WRITE, 0));

    /* Not excluded -- by design, and stated in G-5 and C-1f.  If this ever starts
     * returning ZS_LOCKED, either F_OFD_SETLK was adopted (C-1i permits it, and it
     * WOULD exclude these) or a mutex crept back in.  The first is an improvement
     * worth documenting; the second is the bug this test guards. */
    ASSERT_OK(zsi_lock_take(&b, ZSI_LOCK_WRITE, ZS_NONBLOCKING));

    zsi_lock_close(&a);
    zsi_lock_close(&b);
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
    zsi_name_format(name, db->uuid, 1, 0);
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
    zsi_name_format(name, test_uuid, 1, 0);
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
    zsi_name_format(name, test_uuid, 2, 0);
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
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
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

    ASSERT_EQ(zs_db_open(dbdir, &setup, &db), ZS_BADFORMAT);
    ASSERT_NULL(db);

    /* The same corruption in the ACTIVE file opens fine (D-10, G-3): any state a
     * crash can produce must open. */
    clear_db();
    put_inorder(1, 1, k);
    put_unordered(2, k);
    zsi_name_format(name, test_uuid, 2, 0);
    ASSERT_EQ(writefile(name, junk, sizeof(junk)), 0);
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_EQU(db->snap->nfiles, 2u);
    ASSERT(!db->snap->files[1]->hdr_valid);
    ASSERT_OK(zs_db_close(&db));

    /* And a zero-length active file. */
    zsi_name_format(name, test_uuid, 2, 0);
    ASSERT_EQ(writefile(name, "", 0), 0);
    ASSERT_OK(zs_db_open(dbdir, &setup, &db));
    ASSERT_OK(zs_db_close(&db));
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
    zsi_name_format(othername, other, 2, 0);
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
    int rc = zsi_lookup(db, db->snap, NULL, key, keylen, &r);

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
               kvs[i].v, kvs[i].v ? strlen(kvs[i].v) : 0, false, 0);
    sb_term(&s, false);
    zsi_name_format(name, test_uuid, gen, 0);
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
               sorted[i].v, sorted[i].v ? strlen(sorted[i].v) : 0, false, 0);
    ib_finish(&b);
    zsi_name_format(name, test_uuid, start, end);
    ASSERT_EQ(mkdbdir(), 0);
    ASSERT_EQ(writefile(name, b.buf, b.len), 0);
    ib_free(&b);
}

static void test_read_newest_wins(void)
{
    /* D-14: across sources, the first record found walking newest to oldest
     * wins.  Three files each holding a version of "k", and the newest must
     * decide -- for the point lookup AND for the scan, which is G-7. */
    struct zs_db *db;
    char got[256];

    clear_db();
    static const struct kv g1[] = { {"k","one"}, {"a","A"}, {NULL,NULL} };
    static const struct kv g2[] = { {"k","two"}, {NULL,NULL} };
    static const struct kv g3[] = { {"k","three"}, {"z","Z"}, {NULL,NULL} };
    put_inorder_kv(1, 1, g1);
    put_inorder_kv(2, 2, g2);
    put_unordered_kv(3, g3);

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "three");

    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=A|k=three|z=Z");

    zs_db_close(&db);
}

static void test_read_deletion_hides_older(void)
{
    /* A deletion in a newer file hides the key entirely, even though older files
     * still hold values (D-14).  The key is consumed either way (D-14e step 4),
     * which is what stops the older value surfacing. */
    struct zs_db *db;
    char got[256];

    clear_db();
    static const struct kv g1[] = { {"a","A"}, {"k","value"}, {"z","Z"},
                                    {NULL,NULL} };
    static const struct kv g2[] = { {"k",NULL}, {NULL,NULL} };
    put_inorder_kv(1, 1, g1);
    put_unordered_kv(2, g2);

    db = open_db(0);
    ASSERT_NOT_NULL(db);

    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "-");

    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=A|z=Z");          /* k absent, neighbours intact */

    zs_db_close(&db);

    /* And a value written AFTER a deletion brings it back. */
    static const struct kv g3[] = { {"k","again"}, {NULL,NULL} };
    put_unordered_kv(3, g3);
    db = open_db(0);
    db_get(db, "k", 1, got, sizeof(got));
    ASSERT_STR_EQ(got, "again");
    db_scan(db, 0, NULL, 0, got, sizeof(got));
    ASSERT_STR_EQ(got, "a=A|k=again|z=Z");
    zs_db_close(&db);
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

    /* 2. several unordered files */
    clear_db();
    { static const struct kv p[] = {{"a","A"},{"b","B"},{NULL,NULL}};
      put_unordered_kv(1, p); }
    { static const struct kv p[] = {{"c","C"},{NULL,NULL}};
      put_unordered_kv(2, p); }
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

        /* Alternate the file kind, so the merge sees both primitives. */
        if (gen % 2 == 0) {
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
 * Test runner
 * ============================================================
 */

struct test_entry {
    const char *name;
    void (*func)(void);
};

static struct test_entry tests[] = {
    { "test_strerror",                  test_strerror },
    { "test_zmalloc",                   test_zmalloc },
    { "test_le_accessors",              test_le_accessors },
    { "test_overflow_guards",           test_overflow_guards },
    { "test_interop_constants_csum",    test_interop_constants_csum },
    { "test_interop_constants_compar",  test_interop_constants_compar },
    { "test_interop_constants_uuid",    test_interop_constants_uuid },

    { "test_filenames",                 test_filenames },
    { "test_filename_rejections",       test_filename_rejections },
    { "test_filename_prefix_property",  test_filename_prefix_property },
    { "test_filename_lexical_order",    test_filename_lexical_order },
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
    { "test_record_roundtrip",          test_record_roundtrip },
    { "test_record_canonical",          test_record_canonical },
    { "test_record_embedded_nul",       test_record_embedded_nul },
    { "test_record_bounds",             test_record_bounds },
    { "test_terminator",                test_terminator },
    { "test_terminator_detects_tear",   test_terminator_detects_tear },
    { "test_terminator_long_span",      test_terminator_long_span },

    { "test_file_open_unordered",       test_file_open_unordered },
    { "test_file_open_inorder",         test_file_open_inorder },
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
    { "test_span_bad_length",           test_span_bad_length },
    { "test_span_progress",             test_span_progress },
    { "test_span_bad_header_and_kind",  test_span_bad_header_and_kind },
    { "test_span_pointers_rejected",    test_span_pointers_rejected },
    { "test_span_engine_zero",          test_span_engine_zero },
    { "test_span_long_terminator",      test_span_long_terminator },

    { "test_index_committed_only",      test_index_committed_only },
    { "test_index_newest_per_key",      test_index_newest_per_key },
    { "test_index_ordered_traversal",   test_index_ordered_traversal },
    { "test_index_empty",               test_index_empty },
    { "test_index_delta",               test_index_delta },
    { "test_index_delta_shadows_base",  test_index_delta_shadows_base },
    { "test_index_delta_merge_with_duplicates",
                                        test_index_delta_merge_with_duplicates },
    { "test_index_binary_keys",         test_index_binary_keys },
    { "test_index_many",                test_index_many },

    { "test_inorder_empty",             test_inorder_empty },
    { "test_inorder_roundtrip",         test_inorder_roundtrip },
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

    { "test_fileset_derives_from_names", test_fileset_derives_from_names },
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

    { "test_read_newest_wins",          test_read_newest_wins },
    { "test_read_deletion_hides_older", test_read_deletion_hides_older },
    { "test_read_d14f_duplicate_across_three_files",
                                        test_read_d14f_duplicate_across_three_files },
    { "test_read_cursor_invariant",     test_read_cursor_invariant },
    { "test_read_arrangements",         test_read_arrangements },
    { "test_read_seek_and_flags",       test_read_seek_and_flags },
    { "test_read_prefix_across_files",  test_read_prefix_across_files },
    { "test_read_model",                test_read_model },

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
