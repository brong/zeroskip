/* zstest.c - zeroskip test suite
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
#include <sys/stat.h>
#include <sys/types.h>
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

/* Build a valid header into buf, for a test to then damage. */
static void make_header(char *buf, uint32_t start, uint32_t end, unsigned engine)
{
    static const zsi_uuid_t u = {
        0x49, 0x41, 0xda, 0x54, 0x94, 0x06, 0x4f, 0xaa,
        0xa4, 0x57, 0xc4, 0xb6, 0x5b, 0xea, 0xe3, 0xeb
    };
    struct zsi_header h;
    memset(&h, 0, sizeof(h));
    h.version_read  = ZSI_VERSION_READ;
    h.version_write = ZSI_VERSION_WRITE;
    h.flags         = (uint16_t)engine;
    memcpy(h.uuid, u, 16);
    h.start = start;
    h.end   = end;
    memcpy(h.compar_name, "memcmp", 6);
    zsi_header_encode(buf, &h, zsi_csum_for_id(engine, NULL));
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

    { "test_magic",                     test_magic },
    { "test_magic_designed_corruptions", test_magic_designed_corruptions },
    { "test_header_roundtrip",          test_header_roundtrip },
    { "test_header_byte_layout",        test_header_byte_layout },
    { "test_header_versions",           test_header_versions },
    { "test_header_checksum",           test_header_checksum },
    { "test_header_reserved",           test_header_reserved },
    { "test_header_bounds_and_ranges",  test_header_bounds_and_ranges },

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
