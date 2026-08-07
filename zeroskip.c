/* zeroskip.c - append-only ordered key-value store
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * A directory of immutable and append-only files, with lock-free readers and a
 * single writer.  The design rests on one invariant, without exception:
 * nothing is ever written except by appending to a file or by creating a new
 * file.  No file is ever modified in place or truncated, and there is no
 * mutable object of any kind - no manifest, no shared cache.
 *
 * The format and protocol are specified in
 * doc/specification.md.  Requirement labels in
 * the comments below (F-n format, D-n database, C-n concurrency, R-n recovery,
 * A-n API, G-n guarantee) cite that document, and doc/conformance.md maps each
 * to the test that enforces it.
 *
 * Sections appear in dependency order: each may only call downwards into those
 * above it.
 */

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zeroskip.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

/********** TUNING *************/

/* A writer moves to a new file when the active file exceeds this (D-9a).
 * It bounds two other things as a side effect: how much a snapshot must replay
 * to build a private index (D-13d), and how much work one conversion does
 * (D-12d). */
#define ZSI_DEFAULT_ROLLOVER (2 * 1024 * 1024)

/********** LIBRARY SUPPORT *************/

static void *zsi_zmalloc(size_t bytes)
{
    void *res = malloc(bytes);
    if (!res) return NULL;
    memset(res, 0, bytes);
    return res;
}

/* Fill a buffer with random bytes from /dev/urandom (present on Linux, macOS
 * and every BSD).  Returns false if we couldn't read the whole buffer, in
 * which case the caller needs its own fallback. */
static bool zsi_random_bytes(void *buf, size_t len)
{
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = read(fd, (char *)buf + off, len - off);
        if (n <= 0) break;
        off += (size_t)n;
    }
    close(fd);
    return off == len;
}

/* A mix of weak entropy sources, for when /dev/urandom isn't available.  Only
 * ever used to avoid failing outright when generating a database UUID. */
static uint64_t zsi_weak_entropy(void)
{
    uint64_t a = (uint64_t)time(NULL);
    uint64_t b = (uint64_t)getpid();
    uint64_t c = (uint64_t)(uintptr_t)&a;
    return a ^ (b << 32) ^ c;
}

typedef unsigned char zsi_uuid_t[16];
#define ZSI_UUID_STR_LEN 37     /* 36 characters plus NUL */

/* Fill a 16-byte buffer with a random version-4 UUID.  Falls back to weak
 * entropy if /dev/urandom is somehow unavailable, so we always produce a
 * distinct identifier rather than failing.  The value is arbitrary and opaque;
 * only its 16-byte encoding (F-11) and its textual form (D-0) are fixed. */
static void zsi_uuid_generate(zsi_uuid_t uuid)
{
    if (!zsi_random_bytes(uuid, sizeof(zsi_uuid_t))) {
        uint64_t a = zsi_weak_entropy();
        uint64_t b = zsi_weak_entropy() * 0x2545F4914F6CDD1DULL;
        memcpy((char *)uuid + 0, &a, 8);
        memcpy((char *)uuid + 8, &b, 8);
    }
    uuid[6] = (unsigned char)((uuid[6] & 0x0f) | 0x40);  /* version 4 */
    uuid[8] = (unsigned char)((uuid[8] & 0x3f) | 0x80);  /* RFC 4122 variant */
}

/* Format as the canonical lowercase 8-4-4-4-12 string (D-0), matching
 * libuuid's uuid_unparse.  out must hold ZSI_UUID_STR_LEN bytes.
 *
 * Lowercase here, against the uppercase generations of D-1.  The contrast is
 * deliberate: it makes a malformed filename obvious on sight. */
static void zsi_uuid_unparse(const zsi_uuid_t uuid, char *out)
{
    snprintf(out, ZSI_UUID_STR_LEN,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6],
             uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
             uuid[14], uuid[15]);
}

static int zsi_hexval(unsigned char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;                          /* deliberately not 'A'-'F': see below */
}

/* Parse exactly the 36-character lowercase hyphenated form and nothing else:
 * no uppercase, no braces, no urn: prefix, no missing or extra hyphens.
 *
 * D-0 pins the spelling, and a database's file set is "the names matching
 * zeroskip-<uuid>-*" (D-4).  A lenient parser would let two implementations
 * disagree about which files belong to a database, which is a corruption bug
 * wearing a compatibility costume.  Returns 0 on success.
 *
 * in need not be NUL-terminated at 36; the caller may pass a pointer into a
 * longer filename, and we read exactly 36 bytes. */
static int zsi_uuid_parse(const char *in, zsi_uuid_t out)
{
    static const int hyphens[4] = { 8, 13, 18, 23 };
    size_t i, o = 0, h = 0;

    for (i = 0; i < 36; i++) {
        if (h < 4 && (int)i == hyphens[h]) {
            if (in[i] != '-') return -1;
            h++;
            continue;
        }
        int hi = zsi_hexval((unsigned char)in[i]);
        int lo = zsi_hexval((unsigned char)in[i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[o++] = (unsigned char)((hi << 4) | lo);
        i++;                            /* consumed a pair */
    }

    return (o == 16 && h == 4) ? 0 : -1;
}

/* Little-endian accessors (F-1, G-0a).
 *
 * Assembled from individual bytes through an unsigned char *, which is always
 * permitted regardless of alignment or host byte order.  Nothing here casts the
 * mapped buffer to a wider integer type, so these are correct on a big-endian
 * host and on a target that faults on unaligned loads, and they compile down to
 * a single load on the platforms where that is legal. */
static uint16_t zsi_get16(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    return (uint16_t)((uint16_t)u[0] | ((uint16_t)u[1] << 8));
}

static uint32_t zsi_get24(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    return (uint32_t)u[0] | ((uint32_t)u[1] << 8) | ((uint32_t)u[2] << 16);
}

static uint32_t zsi_get32(const char *p)
{
    const unsigned char *u = (const unsigned char *)p;
    return (uint32_t)u[0] | ((uint32_t)u[1] << 8)
         | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
}

static uint64_t zsi_get64(const char *p)
{
    return (uint64_t)zsi_get32(p) | ((uint64_t)zsi_get32(p + 4) << 32);
}

static void zsi_put16(char *p, uint16_t v)
{
    unsigned char *u = (unsigned char *)p;
    u[0] = (unsigned char)(v & 0xFF);
    u[1] = (unsigned char)((v >> 8) & 0xFF);
}

static void zsi_put24(char *p, uint32_t v)
{
    unsigned char *u = (unsigned char *)p;
    u[0] = (unsigned char)(v & 0xFF);
    u[1] = (unsigned char)((v >> 8) & 0xFF);
    u[2] = (unsigned char)((v >> 16) & 0xFF);
}

static void zsi_put32(char *p, uint32_t v)
{
    unsigned char *u = (unsigned char *)p;
    u[0] = (unsigned char)(v & 0xFF);
    u[1] = (unsigned char)((v >> 8) & 0xFF);
    u[2] = (unsigned char)((v >> 16) & 0xFF);
    u[3] = (unsigned char)((v >> 24) & 0xFF);
}

static void zsi_put64(char *p, uint64_t v)
{
    zsi_put32(p, (uint32_t)(v & 0xFFFFFFFFu));
    zsi_put32(p + 4, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* Overflow-checked arithmetic (G-0b).
 *
 * Every length, count and offset read from a file is attacker- and
 * corruption-controlled.  "keylen + vallen + 2" and "offset + record_length"
 * are exactly the expressions that turn a bounds check into a bounds-check
 * bypass when they wrap, so they go through these rather than being written
 * inline.  Each returns false on overflow and leaves *out untouched. */
static bool zsi_add_sz(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b) return false;
    *out = a + b;
    return true;
}

static bool zsi_add3_sz(size_t a, size_t b, size_t c, size_t *out)
{
    size_t t;
    if (!zsi_add_sz(a, b, &t)) return false;
    return zsi_add_sz(t, c, out);
}

/* Round up to a multiple of 8 (F-2), returning 0 when the result would not fit.
 *
 * Callers treat 0 as "not a valid length", which matters because roundup8 is
 * applied to lengths straight out of a file, where a value near SIZE_MAX is one
 * corruption away.
 *
 * The guard is documentation rather than arithmetic: unsigned overflow is
 * well-defined in C, and for n >= SIZE_MAX-6 the wrapping expression already
 * yields 0, so removing the guard changes nothing observable.  It stays because
 * the contract should be visible at the top of the function rather than deduced
 * from modular arithmetic, and because it keeps holding if the rounding width or
 * the parameter type ever changes. */
static size_t zsi_roundup8(size_t n)
{
    if (n > SIZE_MAX - 7) return 0;
    return (n + 7) & ~(size_t)7;
}

/********** COMPARATORS *************/

/* The default comparator (F-11a).
 *
 * Compare min(alen, blen) bytes as UNSIGNED octets; if they differ, that
 * decides.  If they are equal, the shorter key sorts first.
 *
 * Written out rather than delegating to the C library, per F-11a.  Being precise
 * about why, because the folklore version of this warning is wrong and leads
 * people to "fix" the wrong thing:
 *
 *   - memcmp is defined to compare as unsigned char, so using it for the common
 *     prefix would in fact be correct.  What it does not do is order keys of
 *     differing length -- that is half of this function's job -- and its return
 *     magnitude is unspecified, so only its sign may be used.  A comparator that
 *     is just `return memcmp(a, b, min(alen, blen));` is broken for a key and
 *     its own prefix, which is the case T-2c leads with.
 *   - the platform hazard is comparing plain `char` directly, whose signedness
 *     varies: that orders keys above 0x7F differently on ARM than on x86 and
 *     produces pointer sections the other platform cannot read.  Hence the
 *     explicit unsigned char pointers below.
 *
 * The signedness failure is silent and survives every test that uses ASCII keys,
 * which is why T-2c's table includes 0x7F against 0x80.  A memcmp-plus-length
 * variant is not detectably different from this loop, so no test enforces the
 * loop itself -- only the ordering it produces. */
static int zsi_compar_default(const char *a, size_t alen,
                              const char *b, size_t blen)
{
    const unsigned char *ua = (const unsigned char *)a;
    const unsigned char *ub = (const unsigned char *)b;
    size_t n = alen < blen ? alen : blen;

    for (size_t i = 0; i < n; i++) {
        if (ua[i] != ub[i]) return ua[i] < ub[i] ? -1 : 1;
    }

    if (alen == blen) return 0;
    return alen < blen ? -1 : 1;
}

/* A caller supplying its own comparator MUST supply a name; names are compared
 * byte for byte, and an empty name is invalid (F-11b). */
static bool zsi_compar_name_valid(const char *name)
{
    if (!name) return false;
    size_t n = strlen(name);
    return n >= 1 && n <= 16;
}

/********** CHECKSUMS *************/

/* Exactly three engines exist (F-5).  The id lives in the low 4 bits of each
 * file header's flags field, so every file is self-describing and files written
 * under different engines may coexist in one database (F-5a). */
#define ZSI_CSUM_NONE     0
#define ZSI_CSUM_XXHASH   1
#define ZSI_CSUM_EXTERNAL 2
#define ZSI_CSUM_MASK     0x000F

static uint32_t zsi_csum_none(const char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return 0;
}

/* Engine 1: XXH3_64bits with the default seed of 0, truncated to the low 32
 * bits, stored little-endian like every other integer (F-5b).  Both the seed
 * and which half is kept are pinned, because otherwise two implementations
 * produce different bytes from the same input.
 *
 * Note there is NO "if (!len) return 0;" short-circuit here, unlike twom's
 * equivalent.  F-26g requires the engine's value for empty input -- an in-order
 * file with zero records checksums an empty records region and must store
 * 0x38D394C2, not zero.  Adding the short-circuit back would make every empty
 * in-order file fail its own consistency check. */
static uint32_t zsi_csum_xxhash(const char *buf, size_t len)
{
    return (uint32_t)(XXH3_64bits(buf, len) & 0xFFFFFFFFu);
}

/* Resolve an engine id to its function.  Returns NULL for an unknown id, and
 * for engine 2 returns the caller-supplied function, which may itself be NULL
 * -- opening a database whose files use engine 2 without supplying one is an
 * error the caller reports, not something to paper over here (A-6).
 *
 * Takes the external function directly rather than a struct zs_db *, so this
 * section has no dependency on anything defined later in the file. */
static zs_csum *zsi_csum_for_id(unsigned id, zs_csum *external)
{
    switch (id) {
    case ZSI_CSUM_NONE:     return zsi_csum_none;
    case ZSI_CSUM_XXHASH:   return zsi_csum_xxhash;
    case ZSI_CSUM_EXTERNAL: return external;
    }

    return NULL;
}

/* Which engine to write into files this handle creates (A-6).  Never overrides
 * what an existing file records. */
static unsigned zsi_csum_id_for_flags(uint32_t flags)
{
    if (flags & ZS_CSUM_EXTERNAL) return ZSI_CSUM_EXTERNAL;
    if (flags & ZS_CSUM_NONE)     return ZSI_CSUM_NONE;
    if (flags & ZS_CSUM_XXHASH)   return ZSI_CSUM_XXHASH;

    return ZSI_CSUM_XXHASH;             /* the default (F-5) */
}

/* Checksum two regions as though concatenated.
 *
 * A span terminator's checksum covers the span's data bytes followed by the
 * terminator's own bytes up to the checksum field (F-19).  The span may be
 * large, so joining them into one buffer is not an option on the hot path.
 *
 * Engine 1 uses XXH3's streaming state, which is required to agree with the
 * one-shot form over the concatenation -- asserted in test_interop_constants
 * rather than assumed.  Engine 0 ignores its input entirely.  Engine 2 is
 * caller-supplied and outside the conformance corpus (F-5d), so it falls back
 * to a temporary join and accepts the allocation. */
static uint32_t zsi_csum2(zs_csum *csum, unsigned id,
                          const char *a, size_t alen,
                          const char *b, size_t blen)
{
    if (id == ZSI_CSUM_NONE) return 0;

    if (id == ZSI_CSUM_XXHASH) {
        XXH3_state_t st;
        XXH3_64bits_reset(&st);
        if (alen) XXH3_64bits_update(&st, a, alen);
        if (blen) XXH3_64bits_update(&st, b, blen);
        return (uint32_t)(XXH3_64bits_digest(&st) & 0xFFFFFFFFu);
    }

    /* engine 2 */
    size_t total;
    if (!zsi_add_sz(alen, blen, &total)) return 0;
    char *join = malloc(total ? total : 1);
    if (!join) return 0;
    if (alen) memcpy(join, a, alen);
    if (blen) memcpy(join + alen, b, blen);
    uint32_t r = csum(join, total);
    free(join);
    return r;
}

/********** FILENAMES *************/

/* The directory is the file set (section 5.2).  There is no manifest: filenames
 * carry each file's generation range, so one readdir yields the set and every
 * range without opening a single file.
 *
 *   zeroskip-<uuid>-<gen>            unordered file, one generation
 *   zeroskip-<uuid>-<start>-<end>    in-order file
 *   zeroskip.tmp.<pid>.<n>           staging for a repack or conversion output
 *   zeroskip.lock                    holds the fcntl locks
 *
 * zeroskip-* matches data files only and zeroskip.* matches metadata, so both
 * sets are prefix-globbable and staging names can never match the data-file
 * pattern (D-2).
 *
 * Two properties of this spelling are load-bearing for D-5's overlap resolution,
 * and both are asserted directly on generated names in test_filename_*:
 *
 *   - generations are UPPERCASE hex, zero-padded to exactly 8 digits (D-1).
 *     Fixed width keeps lexical and numeric order identical, and 8 digits is
 *     exactly the range of a 32-bit generation, so every representable
 *     generation has a name and the width never needs to change.
 *   - data files carry NO extension (D-1a).  An unordered file's name is
 *     therefore a strict *prefix* of the in-order name covering the same
 *     generation -- "...-00000005" against "...-00000005-00000005" -- and so
 *     sorts before it, which D-5's "take the last" rule requires.  Any suffix
 *     sorting after '-' would reverse that pair and silently break resolution.
 *
 * The UUID is lowercase (D-0) against those uppercase generations.  The contrast
 * is deliberate: it makes a malformed name obvious on sight. */

#define ZSI_NAME_PREFIX     "zeroskip-"
#define ZSI_NAME_PREFIX_LEN 9
#define ZSI_LOCK_NAME       "zeroskip.lock"
#define ZSI_STAGING_PREFIX  "zeroskip.tmp."

/* "zeroskip-" + 36 + "-" + 8 + "-" + 8 + NUL = 64.  Rounded up for headroom. */
#define ZSI_NAME_MAX 80

enum zsi_nametype {
    ZSI_NAME_OTHER = 0,      /* not a data file of ours -- ignore (D-4) */
    ZSI_NAME_UNORDERED,      /* zeroskip-<uuid>-<gen> */
    ZSI_NAME_INORDER         /* zeroskip-<uuid>-<start>-<end> */
};

static void zsi_name_format(char *out, const zsi_uuid_t uuid,
                            uint32_t start, uint32_t end)
{
    char ustr[ZSI_UUID_STR_LEN];
    zsi_uuid_unparse(uuid, ustr);

    if (end == 0)
        snprintf(out, ZSI_NAME_MAX, "%s%s-%08X", ZSI_NAME_PREFIX, ustr, start);
    else
        snprintf(out, ZSI_NAME_MAX, "%s%s-%08X-%08X",
                 ZSI_NAME_PREFIX, ustr, start, end);
}

/* Parse exactly 8 uppercase hex digits.  Returns the character count consumed,
 * or 0 on any deviation: lowercase, fewer or more digits, a 0x prefix, a sign. */
static size_t zsi_parse_gen8(const char *p, uint32_t *out)
{
    uint32_t v = 0;

    for (size_t i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)p[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return 0;                  /* lowercase deliberately excluded */
        v = (v << 4) | (uint32_t)d;
    }

    *out = v;
    return 8;
}

/* Classify a directory entry.  Fills uuid/start/end when it is a data file; for
 * an unordered file *end is 0, which F-9 makes unambiguous.
 *
 * Strict by design.  A lenient parser here would let two implementations
 * disagree about which files belong to a database, and D-4's "a file
 * participates if its name matches" makes that disagreement a correctness bug
 * rather than a cosmetic one. */
static enum zsi_nametype zsi_name_parse(const char *name, zsi_uuid_t uuid,
                                        uint32_t *start, uint32_t *end)
{
    /* Anything beginning "zeroskip." is metadata, not data (D-2): the lock file
     * and staging names live there and must never match. */
    if (strncmp(name, ZSI_NAME_PREFIX, ZSI_NAME_PREFIX_LEN) != 0)
        return ZSI_NAME_OTHER;

    const char *p = name + ZSI_NAME_PREFIX_LEN;

    /* 36 characters of lowercase hyphenated UUID, then a separator. */
    if (strlen(p) < 36 + 1) return ZSI_NAME_OTHER;
    if (zsi_uuid_parse(p, uuid) != 0) return ZSI_NAME_OTHER;
    p += 36;
    if (*p != '-') return ZSI_NAME_OTHER;
    p++;

    uint32_t s, e;
    if (zsi_parse_gen8(p, &s) != 8) return ZSI_NAME_OTHER;
    p += 8;

    /* F-9: generations start at 1, so 0 is never a legitimate start. */
    if (s == 0) return ZSI_NAME_OTHER;

    if (*p == '\0') {
        *start = s;
        *end = 0;
        return ZSI_NAME_UNORDERED;
    }

    if (*p != '-') return ZSI_NAME_OTHER;
    p++;

    if (zsi_parse_gen8(p, &e) != 8) return ZSI_NAME_OTHER;
    p += 8;

    /* No extension, and nothing trailing (D-1a). */
    if (*p != '\0') return ZSI_NAME_OTHER;

    /* end == 0 would make an in-order name indistinguishable from an unordered
     * one, and a range must not run backwards. */
    if (e == 0 || e < s) return ZSI_NAME_OTHER;

    *start = s;
    *end = e;
    return ZSI_NAME_INORDER;
}

/* A staging name.  The pid is for human legibility only -- it is NOT what makes
 * the name unique, because a pid is not unique on shared storage where two hosts
 * readily have the same one.  D-20a requires O_CREAT|O_EXCL and advancing <n>
 * until it succeeds; that is what actually guarantees exclusivity, and two
 * processes writing one staging file would otherwise produce an interleaved
 * output that then gets renamed into place as though complete. */
static void zsi_staging_name(char *out, unsigned n)
{
    snprintf(out, ZSI_NAME_MAX, "%s%d.%u", ZSI_STAGING_PREFIX, (int)getpid(), n);
}

/********** FILE HEADER *************/

/* Every file begins with the same 16 bytes (section 4.2):
 *
 *     89 7A 65 72 6F 73 6B 69 70 31 0D 0A 1A 0A 00 00
 *     \x89  z  e  r  o  s  k  i  p  1 \r \n ^Z \n \0 \0
 *
 * Each part earns its place, following the reasoning behind the PNG signature:
 *
 *   89        high bit set, so no text file can be mistaken for a database and a
 *             transfer that strips the eighth bit is detected.  Also makes the
 *             sequence invalid UTF-8 (F-6a): 0x89 is in the continuation-byte
 *             range 0x80-0xBF, and a continuation byte cannot begin a sequence.
 *             Anything validating the file as text fails at byte 0 rather than
 *             part way through, and anything that sanitises invalid UTF-8 by
 *             substitution replaces it with U+FFFD, destroying the magic
 *             detectably instead of silently corrupting the body.
 *   zeroskip  human-readable in a hex dump and to file(1)
 *   1         major format version in the magic, so an incompatible future
 *             format is distinguishable without parsing
 *   0D 0A     CR-LF trap: newline translation in either direction alters it
 *   1A        DOS end-of-file, so accidentally type-ing a file stops early
 *   0A        bare LF, catching the inverse newline translation
 *   00 00     NUL-terminates the printable part and pads to 16
 *
 * A reader MUST validate all 16 bytes, not a prefix (F-6). */
#define ZSI_MAGIC_LEN 16

static const unsigned char zsi_magic[ZSI_MAGIC_LEN] = {
    0x89, 0x7A, 0x65, 0x72, 0x6F, 0x73, 0x6B, 0x69,
    0x70, 0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00
};

/* Field offsets within the 72-byte header (section 4.3).  Spelled out rather
 * than derived by summing sizes, so a table in the spec maps to a table here. */
#define ZSI_HEADER_LEN         72
#define ZSI_HDR_OFF_MAGIC       0   /* 16 */
#define ZSI_HDR_OFF_VREAD      16   /*  1 */
#define ZSI_HDR_OFF_VWRITE     17   /*  1 */
#define ZSI_HDR_OFF_FLAGS      18   /*  2 */
#define ZSI_HDR_OFF_RESERVED1  20   /*  4 */
#define ZSI_HDR_OFF_UUID       24   /* 16 */
#define ZSI_HDR_OFF_START      40   /*  4 */
#define ZSI_HDR_OFF_END        44   /*  4 */
#define ZSI_HDR_OFF_COMPAR     48   /* 16 */
#define ZSI_HDR_OFF_RESERVED2  64   /*  4 */
#define ZSI_HDR_OFF_CSUM       68   /*  4, covers [0, 68) */

/* The lowest library version able to read, and to write, a file we produce. */
#define ZSI_VERSION_READ  1
#define ZSI_VERSION_WRITE 1

#define ZSI_COMPAR_NAME_LEN 16

struct zsi_header {
    uint8_t     version_read;
    uint8_t     version_write;
    uint16_t    flags;                            /* low 4 bits: csum engine */
    zsi_uuid_t  uuid;
    uint32_t    start;
    uint32_t    end;                              /* 0 == unordered (F-9) */
    char        compar_name[ZSI_COMPAR_NAME_LEN]; /* NUL-padded, not NUL-terminated */
};

/* end == 0 means unordered with no pointer section; end != 0 means in-order
 * with pointers.  The two kinds are exhaustive and distinguishable from the
 * header alone, so a reader always knows, before reading anything else, whether
 * a pointer section must be present.  Generations start at 1, so end == 0 is
 * never a legitimate generation (F-9). */
static bool zsi_header_is_unordered(const struct zsi_header *h)
{
    return h->end == 0;
}

static void zsi_header_encode(char *buf, const struct zsi_header *hdr,
                              zs_csum *csum)
{
    memset(buf, 0, ZSI_HEADER_LEN);

    memcpy(buf + ZSI_HDR_OFF_MAGIC, zsi_magic, ZSI_MAGIC_LEN);
    buf[ZSI_HDR_OFF_VREAD]  = (char)hdr->version_read;
    buf[ZSI_HDR_OFF_VWRITE] = (char)hdr->version_write;
    zsi_put16(buf + ZSI_HDR_OFF_FLAGS, hdr->flags);
    /* RESERVED1 and RESERVED2 stay zero: written as zero, ignored on read
     * (F-8).  The memset above is what writes them. */
    memcpy(buf + ZSI_HDR_OFF_UUID, hdr->uuid, 16);
    zsi_put32(buf + ZSI_HDR_OFF_START, hdr->start);
    zsi_put32(buf + ZSI_HDR_OFF_END, hdr->end);
    memcpy(buf + ZSI_HDR_OFF_COMPAR, hdr->compar_name, ZSI_COMPAR_NAME_LEN);

    /* F-4: the checksum is the last 4 bytes and covers everything before it.
     * No field-zeroing anywhere. */
    zsi_put32(buf + ZSI_HDR_OFF_CSUM, csum(buf, ZSI_HDR_OFF_CSUM));
}

/* Decode and validate.  Returns ZS_BADFORMAT if the buffer is too short, the
 * magic is wrong, the header checksum fails, or version_read exceeds ours.
 *
 * Two things this deliberately does NOT do:
 *
 *   - it does not reject a nonzero reserved field.  F-8 says write zero and
 *     ignore on read; the checksum already covers them, and rejecting would make
 *     a future extension unreadable by this version, which is exactly what the
 *     version fields exist to decide instead.
 *   - it does not enforce version_write.  That gate belongs to the writer, so the
 *     value is recorded and the caller decides (F-7).  This is what lets a file
 *     that is readable but not writable be opened read-only rather than refused.
 *
 * csum must be the engine named by the header's own flags.  Callers get it by
 * reading those flags as plain data first -- see zsi_header_engine_id below. */
static int zsi_header_decode(const char *buf, size_t len,
                             zs_csum *csum, struct zsi_header *out)
{
    if (len < ZSI_HEADER_LEN) return ZS_BADFORMAT;

    if (memcmp(buf + ZSI_HDR_OFF_MAGIC, zsi_magic, ZSI_MAGIC_LEN) != 0)
        return ZS_BADFORMAT;

    if (zsi_get32(buf + ZSI_HDR_OFF_CSUM) != csum(buf, ZSI_HDR_OFF_CSUM))
        return ZS_BADCHECKSUM;

    uint8_t vread = (uint8_t)buf[ZSI_HDR_OFF_VREAD];
    if (vread > ZSI_VERSION_READ) return ZS_BADFORMAT;

    out->version_read  = vread;
    out->version_write = (uint8_t)buf[ZSI_HDR_OFF_VWRITE];
    out->flags         = zsi_get16(buf + ZSI_HDR_OFF_FLAGS);
    memcpy(out->uuid, buf + ZSI_HDR_OFF_UUID, 16);
    out->start         = zsi_get32(buf + ZSI_HDR_OFF_START);
    out->end           = zsi_get32(buf + ZSI_HDR_OFF_END);
    memcpy(out->compar_name, buf + ZSI_HDR_OFF_COMPAR, ZSI_COMPAR_NAME_LEN);

    /* F-9: generations start at 1, so a start of 0 is never legitimate.  This is
     * checked here rather than left to the caller because every use of start --
     * resolving an omitted ancestor (F-17), ordering the file set (D-5) -- would
     * otherwise silently work with a nonsense value. */
    if (out->start == 0) return ZS_BADFORMAT;

    /* An in-order file covers start..end inclusive, so end < start is
     * incoherent.  end == 0 is the unordered marker and not a range. */
    if (out->end != 0 && out->end < out->start) return ZS_BADFORMAT;

    return ZS_OK;
}

/* The checksum engine id, read as plain data before any verification (F-5a).
 *
 * There is no bootstrapping problem only because this comes first: the checksum
 * cannot be verified until the engine is known, and the engine is recorded in
 * the very header the checksum protects.  The field is plain data, so reading it
 * unverified is safe -- a wrong value yields a failed checksum, not a wrong
 * interpretation.
 *
 * Requires len >= ZSI_HEADER_LEN; the caller checks that first. */
static unsigned zsi_header_engine_id(const char *buf)
{
    return (unsigned)(zsi_get16(buf + ZSI_HDR_OFF_FLAGS) & ZSI_CSUM_MASK);
}

/********** RECORDS *************/

/* The type byte is a bitfield of six independent properties (section 4.4).
 * Each bit is meaningful in isolation: IsBig selects the wide layout in all
 * three families, HasAncestor says whether the ancestor field is present, and
 * IsDelete means negation -- whether of a key or of a span.  A decoder reads a
 * record's shape from the bits (F-12a). */
#define ZSI_HASKEY      0x01    /* a data record: carries a key */
#define ZSI_ISDELETE    0x02    /* negation -- of a key, or of a span */
#define ZSI_ISBIG       0x04    /* wide length fields */
#define ZSI_HASANCESTOR 0x08    /* an ancestor generation is stored */
#define ZSI_SPANTERM    0x10    /* ends a span */
#define ZSI_POINTERS    0x20    /* begins a pointer section */

/* The fourteen legal type bytes (F-12), and no others.  Bits 0x40 and 0x80 are
 * reserved and always zero. */
#define ZSI_KEYVALUE         0x01   /* HasKey                               */
#define ZSI_DELETION         0x03   /* HasKey IsDelete                      */
#define ZSI_BIGKEYVALUE      0x05   /* HasKey IsBig                         */
#define ZSI_BIGDELETION      0x07   /* HasKey IsDelete IsBig                */
#define ZSI_KEYVALUE_ANC     0x09   /* HasKey HasAncestor                   */
#define ZSI_DELETION_ANC     0x0B   /* HasKey IsDelete HasAncestor          */
#define ZSI_BIGKEYVALUE_ANC  0x0D   /* HasKey IsBig HasAncestor             */
#define ZSI_BIGDELETION_ANC  0x0F   /* HasKey IsDelete IsBig HasAncestor    */
#define ZSI_COMMIT           0x10   /* SpanTerminator                       */
#define ZSI_ROLLBACK         0x12   /* SpanTerminator IsDelete              */
#define ZSI_COMMIT_LONG      0x14   /* SpanTerminator IsBig                 */
#define ZSI_ROLLBACK_LONG    0x16   /* SpanTerminator IsDelete IsBig        */
#define ZSI_PTRS32           0x20   /* Pointers                             */
#define ZSI_PTRS64           0x24   /* Pointers IsBig                       */

/* True for exactly the fourteen types above and nothing else, including 0x00.
 *
 * Written as an explicit switch rather than as a bit-property computation.  The
 * table in F-12 is normative, and a computed predicate would be a second
 * specification that can drift from it -- a bitfield admits far more values than
 * it defines, and the near-misses are what matter: two family bits set at once,
 * HasAncestor without HasKey, IsDelete with Pointers, either reserved bit set.
 * Each is a plausible result of a single flipped bit in a valid type, and each
 * must be rejected rather than half-interpreted (T-2b). */
static bool zsi_type_valid(uint8_t type)
{
    switch (type) {
    case ZSI_KEYVALUE:
    case ZSI_DELETION:
    case ZSI_BIGKEYVALUE:
    case ZSI_BIGDELETION:
    case ZSI_KEYVALUE_ANC:
    case ZSI_DELETION_ANC:
    case ZSI_BIGKEYVALUE_ANC:
    case ZSI_BIGDELETION_ANC:
    case ZSI_COMMIT:
    case ZSI_ROLLBACK:
    case ZSI_COMMIT_LONG:
    case ZSI_ROLLBACK_LONG:
    case ZSI_PTRS32:
    case ZSI_PTRS64:
        return true;
    }

    return false;
}

/* Encoding limits (F-15).  The short form is mandatory whenever the lengths fit,
 * so these are not tuning knobs -- they are part of the format. */
#define ZSI_SHORT_KEYLEN_MAX  255        /* one byte */
#define ZSI_SHORT_VALLEN_MAX  65535      /* two bytes */
#define ZSI_SHORT_SPANLEN_MAX 0xFFFFFF   /* three bytes */

/* Fixed header sizes per shape, from section 4.5's diagrams. */
#define ZSI_HDRLEN_KEYVALUE        4
#define ZSI_HDRLEN_KEYVALUE_ANC    8
#define ZSI_HDRLEN_DELETION        4
#define ZSI_HDRLEN_DELETION_ANC    8
#define ZSI_HDRLEN_BIGKEYVALUE    24
#define ZSI_HDRLEN_BIGDELETION    16
#define ZSI_TERMLEN_SHORT          8
#define ZSI_TERMLEN_LONG          24

struct zsi_rec {
    uint8_t     type;
    const char *key;    size_t keylen;
    const char *val;    size_t vallen;   /* val == NULL for a deletion */
    uint32_t    ancestor;                /* always resolved, never raw (F-17) */
    size_t      len;                     /* total on-disk bytes, multiple of 8 */
};

struct zsi_term {
    uint8_t     type;
    uint64_t    spanlen;
    uint32_t    csum;
    size_t      len;                     /* 8 or 24 */
};

static bool zsi_rec_is_delete(const struct zsi_rec *r)
{
    return (r->type & ZSI_ISDELETE) != 0;
}

/* Bytes a data record will occupy, or 0 if the inputs cannot be encoded.
 *
 * The big form is chosen by key or value length only, never by the ancestor:
 * the ancestor is 4 bytes whenever it is present, and in the big forms it fits
 * inside padding the shape already carries, so HasAncestor costs nothing there
 * (F-12c).  In the short forms it adds 4 bytes. */
static size_t zsi_rec_encoded_len(size_t keylen, size_t vallen, bool isdelete,
                                  bool store_ancestor)
{
    size_t hdr, body;

    if (keylen < 1) return 0;           /* F-14: a key is at least 1 byte */

    bool big = keylen > ZSI_SHORT_KEYLEN_MAX
            || (!isdelete && vallen > ZSI_SHORT_VALLEN_MAX);

    if (isdelete) {
        hdr = big ? ZSI_HDRLEN_BIGDELETION
                  : (store_ancestor ? ZSI_HDRLEN_DELETION_ANC
                                    : ZSI_HDRLEN_DELETION);
        /* key NUL, no value at all */
        if (!zsi_add_sz(keylen, 1, &body)) return 0;
    } else {
        hdr = big ? ZSI_HDRLEN_BIGKEYVALUE
                  : (store_ancestor ? ZSI_HDRLEN_KEYVALUE_ANC
                                    : ZSI_HDRLEN_KEYVALUE);
        /* key NUL value NUL (F-13: stored lengths exclude the terminators) */
        if (!zsi_add3_sz(keylen, vallen, 2, &body)) return 0;
    }

    size_t total;
    if (!zsi_add_sz(hdr, body, &total)) return 0;
    return zsi_roundup8(total);
}

/* Which of the eight data types the given shape encodes as (F-15).  Split out so
 * the encoder and the tests agree on it by construction. */
static uint8_t zsi_rec_type_for(size_t keylen, size_t vallen, bool isdelete,
                                bool store_ancestor)
{
    bool big = keylen > ZSI_SHORT_KEYLEN_MAX
            || (!isdelete && vallen > ZSI_SHORT_VALLEN_MAX);

    if (isdelete) {
        if (big) return store_ancestor ? ZSI_BIGDELETION_ANC : ZSI_BIGDELETION;
        return store_ancestor ? ZSI_DELETION_ANC : ZSI_DELETION;
    }

    if (big) return store_ancestor ? ZSI_BIGKEYVALUE_ANC : ZSI_BIGKEYVALUE;
    return store_ancestor ? ZSI_KEYVALUE_ANC : ZSI_KEYVALUE;
}

/* Encode a data record into buf, which must hold zsi_rec_encoded_len bytes.
 *
 * val == NULL encodes a deletion (A-1); a non-NULL zero-length value encodes an
 * empty value, which is a distinct state.  store_ancestor is decided by the
 * caller per F-17 -- omit exactly when the ancestor equals the containing file's
 * start generation.
 *
 * Every pad byte is zeroed, not just the tail padding.  Canonical encoding means
 * byte-for-byte reproducibility across implementations (T-12a), and an
 * uninitialised pad byte breaks that while being invisible to every test that
 * reads back through the decoder. */
static void zsi_rec_encode(char *buf, const char *key, size_t keylen,
                           const char *val, size_t vallen,
                           bool store_ancestor, uint32_t ancestor)
{
    bool isdelete = (val == NULL);
    if (isdelete) vallen = 0;

    uint8_t type = zsi_rec_type_for(keylen, vallen, isdelete, store_ancestor);
    size_t total = zsi_rec_encoded_len(keylen, vallen, isdelete, store_ancestor);
    size_t body;

    memset(buf, 0, total);
    buf[0] = (char)type;

    if (type & ZSI_ISBIG) {
        if (isdelete) {
            /* BIGDELETION      +0 type, +1 pad(7),  +8 keylen, +16 key NUL
             * BIGDELETION_ANC  +0 type, +1 pad(3),  +4 ancestor, +8 keylen,
             *                  +16 key NUL */
            if (store_ancestor) zsi_put32(buf + 4, ancestor);
            zsi_put64(buf + 8, (uint64_t)keylen);
            body = ZSI_HDRLEN_BIGDELETION;
        } else {
            /* BIGKEYVALUE      +0 type, +1 pad(7), +8 keylen, +16 vallen,
             *                  +24 key NUL value NUL
             * BIGKEYVALUE_ANC  +0 type, +1 pad(3), +4 ancestor, +8 keylen,
             *                  +16 vallen, +24 key NUL value NUL */
            if (store_ancestor) zsi_put32(buf + 4, ancestor);
            zsi_put64(buf + 8, (uint64_t)keylen);
            zsi_put64(buf + 16, (uint64_t)vallen);
            body = ZSI_HDRLEN_BIGKEYVALUE;
        }
    } else {
        buf[1] = (char)(unsigned char)keylen;
        if (isdelete) {
            /* DELETION      +0 type, +1 keylen, +2 pad(2), +4 key NUL
             * DELETION_ANC  +0 type, +1 keylen, +2 pad(2), +4 ancestor,
             *               +8 key NUL */
            if (store_ancestor) {
                zsi_put32(buf + 4, ancestor);
                body = ZSI_HDRLEN_DELETION_ANC;
            } else {
                body = ZSI_HDRLEN_DELETION;
            }
        } else {
            /* KEYVALUE      +0 type, +1 keylen, +2 vallen, +4 key NUL value NUL
             * KEYVALUE_ANC  +0 type, +1 keylen, +2 vallen, +4 ancestor,
             *               +8 key NUL value NUL */
            zsi_put16(buf + 2, (uint16_t)vallen);
            if (store_ancestor) {
                zsi_put32(buf + 4, ancestor);
                body = ZSI_HDRLEN_KEYVALUE_ANC;
            } else {
                body = ZSI_HDRLEN_KEYVALUE;
            }
        }
    }

    /* Key and value are contiguous, separated by a NUL, with a further NUL after
     * the value, then zero padding to the next multiple of 8.  Both are
     * therefore usable in place as C strings, while the stored lengths remain
     * authoritative and may themselves contain NULs (F-13). */
    memcpy(buf + body, key, keylen);
    buf[body + keylen] = '\0';
    if (!isdelete) {
        if (vallen) memcpy(buf + body + keylen + 1, val, vallen);
        buf[body + keylen + 1 + vallen] = '\0';
    }
}

/* Decode the data record at buf[0..len), resolving its ancestor against the
 * containing file's start generation (F-17).
 *
 * Records carry no checksum of their own; the span terminator covers them
 * (F-19), so nothing is verified here.  What is checked is structure: the type
 * byte, every length bounded and overflow-free, and the total within len.
 *
 * Returns ZS_BADFORMAT for anything that does not decode.  On success out->len is
 * the record's total on-disk size, which the caller uses to advance -- and which
 * F-29 requires it verify is strictly greater than zero before doing so. */
static int zsi_rec_decode(const char *buf, size_t len, uint32_t file_start,
                          struct zsi_rec *out)
{
    if (len < 1) return ZS_BADFORMAT;

    uint8_t type = (uint8_t)buf[0];
    if (!zsi_type_valid(type)) return ZS_BADFORMAT;
    if (!(type & ZSI_HASKEY)) return ZS_BADFORMAT;   /* not a data record */

    bool isdelete = (type & ZSI_ISDELETE) != 0;
    bool big      = (type & ZSI_ISBIG) != 0;
    bool hasanc   = (type & ZSI_HASANCESTOR) != 0;

    size_t hdr, keylen = 0, vallen = 0;
    uint32_t ancestor;

    /* Read the fixed header only after confirming it is present.  Every read
     * below is inside a bound already checked. */
    if (big) {
        hdr = isdelete ? ZSI_HDRLEN_BIGDELETION : ZSI_HDRLEN_BIGKEYVALUE;
        if (len < hdr) return ZS_BADFORMAT;

        uint64_t k = zsi_get64(buf + 8);
        /* On a 32-bit host a 64-bit length may not fit in size_t at all, which
         * is a bounds failure rather than something to truncate into. */
        if (k > (uint64_t)SIZE_MAX) return ZS_BADFORMAT;
        keylen = (size_t)k;

        if (!isdelete) {
            uint64_t v = zsi_get64(buf + 16);
            if (v > (uint64_t)SIZE_MAX) return ZS_BADFORMAT;
            vallen = (size_t)v;
        }
    } else {
        hdr = isdelete ? (hasanc ? ZSI_HDRLEN_DELETION_ANC : ZSI_HDRLEN_DELETION)
                       : (hasanc ? ZSI_HDRLEN_KEYVALUE_ANC : ZSI_HDRLEN_KEYVALUE);
        if (len < hdr) return ZS_BADFORMAT;

        keylen = (size_t)(unsigned char)buf[1];
        if (!isdelete) vallen = (size_t)zsi_get16(buf + 2);
    }

    if (keylen < 1) return ZS_BADFORMAT;             /* F-14 */

    /* Note what is NOT checked here: whether the encoding is canonical (F-15).
     *
     * A big record whose lengths would have fitted the short form is
     * non-canonical, and a conforming writer never produces one -- but decoding
     * MUST still accept it.  Rejecting would be a data-loss bug, because of how
     * two rules compose: a record that fails to validate makes an unordered file
     * complete at that point (F-24), discarding everything after it, and G-3
     * forbids corruption costing *committed* data.  A peer implementation with a
     * canonicalisation bug would therefore cost us every record it wrote after
     * the first non-canonical one, silently.
     *
     * The spec puts this in check_consistency instead (T-6 says exactly that for
     * the analogous non-canonical ancestor), which reports the divergence while
     * still reading the data.  zsi_rec_is_canonical below is what that uses. */

    /* The ancestor: stored when HasAncestor, otherwise the containing file's
     * start.  The caller never sees "not stored" -- that is the whole point of
     * F-17's rule, and it is why decoding never needs to establish whether a
     * record is the first occurrence of its key (F-17a). */
    ancestor = hasanc ? zsi_get32(buf + 4) : file_start;

    /* Total size, every term overflow-checked (G-0b).  keylen + vallen + 2 is
     * exactly the expression that turns a bounds check into a bypass when it
     * wraps. */
    size_t body, total;
    if (isdelete) {
        if (!zsi_add_sz(keylen, 1, &body)) return ZS_BADFORMAT;
    } else {
        if (!zsi_add3_sz(keylen, vallen, 2, &body)) return ZS_BADFORMAT;
    }
    if (!zsi_add_sz(hdr, body, &total)) return ZS_BADFORMAT;
    total = zsi_roundup8(total);
    if (total == 0) return ZS_BADFORMAT;             /* roundup8 saturated */
    if (total > len) return ZS_BADFORMAT;

    out->type     = type;
    out->keylen   = keylen;
    out->key      = buf + hdr;
    out->ancestor = ancestor;
    out->len      = total;

    if (isdelete) {
        out->val    = NULL;
        out->vallen = 0;
    } else {
        out->val    = buf + hdr + keylen + 1;
        out->vallen = vallen;
    }

    return ZS_OK;
}

/* Bytes a terminator will occupy: 8 while the span fits in three bytes, 24
 * beyond that (F-15). */
static size_t zsi_term_encoded_len(uint64_t spanlen)
{
    return spanlen <= ZSI_SHORT_SPANLEN_MAX ? ZSI_TERMLEN_SHORT
                                            : ZSI_TERMLEN_LONG;
}

/* Encode a terminator over a span whose data bytes are spandata[0..spanlen).
 *
 * The checksum covers the span's data followed by the terminator's own bytes up
 * to the checksum field (F-19).  Because it covers both, a terminator that
 * reaches disk without its data fails validation and reads as absent: a torn
 * tail is always detectable (F-22).  Recovery depends on that, and so does
 * reading a file a writer is still appending to (C-4f) -- the checksum supplies
 * the ordering guarantee that no memory barrier can provide between independent
 * processes sharing a mapping. */
static void zsi_term_encode(char *buf, uint64_t spanlen, bool rollback,
                            const char *spandata, zs_csum *csum, unsigned csum_id)
{
    size_t len = zsi_term_encoded_len(spanlen);

    memset(buf, 0, len);

    if (len == ZSI_TERMLEN_SHORT) {
        /* +0 type, +1 span length (3 bytes), +4 checksum */
        buf[0] = (char)(rollback ? ZSI_ROLLBACK : ZSI_COMMIT);
        zsi_put24(buf + 1, (uint32_t)spanlen);
        zsi_put32(buf + 4, zsi_csum2(csum, csum_id, spandata, (size_t)spanlen,
                                     buf, ZSI_TERMLEN_SHORT - 4));
    } else {
        /* +0 type, +1 pad(7), +8 span length, +16 pad(4), +20 checksum */
        buf[0] = (char)(rollback ? ZSI_ROLLBACK_LONG : ZSI_COMMIT_LONG);
        zsi_put64(buf + 8, spanlen);
        zsi_put32(buf + 20, zsi_csum2(csum, csum_id, spandata, (size_t)spanlen,
                                      buf, ZSI_TERMLEN_LONG - 4));
    }
}

/* Decode the terminator at buf[0..len).  Does not verify the checksum -- the
 * caller has the span's data and does that (see zsi_unordered_replay).
 *
 * Terminators are only ever found by scanning forward from the header (F-20).
 * Nothing reads them backwards, because the pointer section is located by its own
 * trailer, so a long terminator needs no marker in its second half. */
static int zsi_term_decode(const char *buf, size_t len, struct zsi_term *out)
{
    if (len < 1) return ZS_BADFORMAT;

    uint8_t type = (uint8_t)buf[0];
    if (!zsi_type_valid(type)) return ZS_BADFORMAT;
    if (!(type & ZSI_SPANTERM)) return ZS_BADFORMAT;

    if (type & ZSI_ISBIG) {
        if (len < ZSI_TERMLEN_LONG) return ZS_BADFORMAT;
        out->spanlen = zsi_get64(buf + 8);
        out->csum    = zsi_get32(buf + 20);
        out->len     = ZSI_TERMLEN_LONG;

        /* A long terminator over a span that would have fitted the short form is
         * non-canonical (F-15) but decodes: see zsi_term_is_canonical, and the
         * note in zsi_rec_decode about why rejecting it here would discard
         * committed data. */
    } else {
        if (len < ZSI_TERMLEN_SHORT) return ZS_BADFORMAT;
        out->spanlen = zsi_get24(buf + 1);
        out->csum    = zsi_get32(buf + 4);
        out->len     = ZSI_TERMLEN_SHORT;
    }

    out->type = type;
    return ZS_OK;
}

static bool zsi_term_is_rollback(const struct zsi_term *t)
{
    return (t->type & ZSI_ISDELETE) != 0;
}

/* Whether a decoded record uses the encoding F-15 requires for its contents.
 *
 * Reads never consult this.  Rejecting non-canonical input on read would lose
 * committed data, because a record that fails to validate makes an unordered file
 * complete at that point (F-24) and G-3 forbids that costing committed data --
 * so a peer with a canonicalisation bug would silently cost us everything it
 * wrote after its first non-canonical record.
 *
 * zs_db_check_consistency consults it instead, which reports the divergence while
 * still reading the data.  T-6 sets that precedent explicitly for the ancestor
 * case below.
 *
 * file_start is the containing file's start generation: F-17 requires the
 * ancestor be omitted exactly when it equals that value, so a record storing an
 * ancestor equal to it is non-canonical even though it decodes identically. */
static bool zsi_rec_is_canonical(const struct zsi_rec *r, uint32_t file_start)
{
    bool isdelete = zsi_rec_is_delete(r);
    bool anc_stored = (r->type & ZSI_HASANCESTOR) != 0;

    /* the shape F-15 requires for these lengths */
    if (r->type != zsi_rec_type_for(r->keylen, r->vallen, isdelete, anc_stored))
        return false;

    /* F-17: stored exactly when it differs from the file's start */
    if (anc_stored && r->ancestor == file_start) return false;

    /* and the total must be the canonical rounded length */
    if (r->len != zsi_rec_encoded_len(r->keylen, r->vallen, isdelete, anc_stored))
        return false;

    return true;
}

/* Whether a decoded terminator uses the width F-15 requires for its span.
 * Reported, not enforced, for the same reason as records. */
static bool zsi_term_is_canonical(const struct zsi_term *t)
{
    return t->len == zsi_term_encoded_len(t->spanlen);
}

/********** FILE OBJECT *************/

/* One open, mapped data file.
 *
 * Nothing in the format depends on mmap (G-0): an implementation may read files
 * with ordinary reads and copy data out.  This one maps, because the C binding
 * promises zero-copy pointer lifetimes (A-4), but the mapping is an optimisation
 * the format permits rather than one it requires.
 *
 * Everything a reader touches through this object is immutable for its lifetime.
 * In-order files are never modified.  A non-active unordered file is never
 * appended to again.  The active file IS appended to, but only ever appended to
 * (G-1), so every byte below the snapshot boundary is stable by construction and
 * growth beyond it is simply not looked at (C-4c). */
struct zsi_file {
    char             *fname;      /* full path, owned */
    int               fd;
    const char       *base;       /* mmap, or NULL for a zero-length file */
    size_t            maplen;     /* bytes mapped; 0 when base is NULL */
    size_t            size;       /* st_size at map time */
    struct zsi_header hdr;
    zs_csum          *csum;       /* the engine this file's own header names */
    unsigned          csum_id;
    bool              hdr_valid;  /* false for the D-10 case */
    bool              needs_external_csum;  /* engine 2, no function supplied */

    /* unordered (hdr.end == 0), filled by the UNORDERED FILE section */
    size_t            complete;   /* F-24 complete point */
    struct zsi_index *index;      /* private, built by replay */

    /* in-order (hdr.end != 0), filled by the POINTER SECTION section */
    size_t            ptr_off;
    uint64_t          nptrs;
    bool              ptr_wide;
    uint32_t          records_csum;   /* from the trailer; verified on demand */
};

/* Bounds-checked access to file data (F-30).
 *
 * This is the single choke point: no other code indexes base directly.  That is
 * deliberate, and worth preserving -- it is the difference between one audited
 * check and thirty unaudited ones, and every offset reaching it is
 * corruption-controlled.  Returns NULL unless [off, off+len) lies wholly within
 * the file.
 *
 * A zero-length request is answered with a valid pointer when the offset itself
 * is in range, because an empty records region is an ordinary case (F-26g) and
 * checksumming zero bytes at a legitimate offset must not look like a failure. */
static const char *zsi_file_at(const struct zsi_file *f, size_t off, size_t len)
{
    size_t end;

    if (!zsi_add_sz(off, len, &end)) return NULL;   /* G-0b */
    if (end > f->size) return NULL;
    if (!f->base) return NULL;                      /* zero-length file */

    return f->base + off;
}

/* Forward declaration, and the one place this file's strict downward layering is
 * broken.  A struct zsi_file owns its private index, so closing the file must
 * free it -- but the index is defined further down, since building one needs span
 * replay which in turn needs this struct.  The alternative, making some caller
 * remember to free the index before closing the file, is a leak waiting to
 * happen: every early-return path in the snapshot protocol would need it. */
static void zsi_index_free(struct zsi_index **ip);

static void zsi_file_close(struct zsi_file **fp)
{
    struct zsi_file *f = *fp;
    if (!f) return;

    zsi_index_free(&f->index);
    if (f->base) munmap((void *)f->base, f->maplen);
    if (f->fd >= 0) close(f->fd);
    free(f->fname);
    free(f);
    *fp = NULL;
}

/* Open and map one data file.
 *
 * name_start is the generation parsed from the filename (D-1), used when the
 * header cannot supply one.  On a header that fails to validate, or a zero-length
 * file, this succeeds with hdr_valid == false and hdr.start taken from the name:
 * D-10 requires that state be representable, because an active file in it is
 * treated as a complete file with zero spans rather than as an error.
 *
 * The CALLER decides whether that is tolerable, and the answer differs by
 * position in the file set: for the active file, yes (D-10); for any other file,
 * it is ZS_BADFORMAT, because its records cannot be recovered and silently
 * skipping the generation would lose committed data (D-10a).  This function
 * cannot make that call, since it does not know the file set. */
static int zsi_file_open(const char *dir, const char *name,
                         uint32_t name_start, zs_csum *external_csum,
                         struct zsi_file **out)
{
    struct zsi_file *f = zsi_zmalloc(sizeof(*f));
    if (!f) return ZS_INTERNAL;

    f->fd = -1;

    size_t dlen = strlen(dir), nlen = strlen(name);
    f->fname = malloc(dlen + 1 + nlen + 1);
    if (!f->fname) { zsi_file_close(&f); return ZS_INTERNAL; }
    memcpy(f->fname, dir, dlen);
    f->fname[dlen] = '/';
    memcpy(f->fname + dlen + 1, name, nlen + 1);

    /* Read-only: a struct zsi_file is a reader's view.  Writers append through a
     * separate descriptor, so nothing can write through this mapping and G-6's
     * "nothing a reader may be reading is ever rewritten beneath it" is enforced
     * by the open mode rather than by convention. */
    f->fd = open(f->fname, O_RDONLY);
    if (f->fd < 0) {
        int r = (errno == ENOENT) ? ZS_NOTFOUND : ZS_IOERROR;
        zsi_file_close(&f);
        return r;
    }

    struct stat sb;
    if (fstat(f->fd, &sb) < 0) { zsi_file_close(&f); return ZS_IOERROR; }
    if (!S_ISREG(sb.st_mode)) { zsi_file_close(&f); return ZS_BADFORMAT; }
    f->size = (size_t)sb.st_size;

    /* A zero-length file cannot be mapped, and must not be an error: D-10 makes
     * it a legal state for the active file.  base stays NULL and zsi_file_at
     * refuses every request, which is the correct behaviour for a file with no
     * content rather than a special case anyone has to remember. */
    if (f->size > 0) {
        void *m = mmap(NULL, f->size, PROT_READ, MAP_SHARED, f->fd, 0);
        if (m == MAP_FAILED) { zsi_file_close(&f); return ZS_IOERROR; }
        f->base = (const char *)m;
        f->maplen = f->size;
    }

    /* Default the generation from the filename before attempting the header, so
     * the D-10 path has it regardless of what the header turns out to hold. */
    f->hdr.start = name_start;
    f->hdr.end = 0;

    if (f->size >= ZSI_HEADER_LEN) {
        /* The engine id comes out of the flags field as plain data first (F-5a):
         * the checksum cannot be verified until the engine is known, and the
         * engine is recorded inside the header the checksum protects. */
        unsigned id = zsi_header_engine_id(f->base);
        zs_csum *cs = zsi_csum_for_id(id, external_csum);

        /* Engine 2 with no function supplied is a CONFIGURATION error, not
         * corruption, and the two must not be conflated: D-10 tolerates a corrupt
         * active file, so without this distinction an unverifiable single-file
         * database would open as empty instead of reporting A-6. */
        if (id == ZSI_CSUM_EXTERNAL && !external_csum)
            f->needs_external_csum = true;

        /* An unknown engine, or engine 2 with no function supplied, leaves the
         * header unverifiable.  Treat it as an invalid header rather than
         * guessing: the caller's position test (D-10 vs D-10a) then decides,
         * and for a non-active file that is the error A-6 wants. */
        if (cs && zsi_header_decode(f->base, f->size, cs, &f->hdr) == ZS_OK) {
            f->hdr_valid = true;
            f->csum = cs;
            f->csum_id = id;
        }
    }

    if (!f->hdr_valid) {
        /* Restore the name-derived generation: zsi_header_decode may have written
         * fields before failing, and a partially-filled header must not be
         * mistaken for a real one. */
        memset(&f->hdr, 0, sizeof(f->hdr));
        f->hdr.start = name_start;
        f->hdr.end = 0;
        f->csum = zsi_csum_none;
        f->csum_id = ZSI_CSUM_NONE;
    }

    *out = f;
    return ZS_OK;
}

/* Which kind of file this is, from the header alone.
 *
 * The two kinds are exhaustive and distinguishable before reading anything else,
 * so a reader always knows whether a pointer section must be present (section 2).
 * An invalid header reads as unordered, which is what D-10 needs: an active file
 * with a corrupt header is a complete file with zero spans, and spans only exist
 * in unordered files. */
static bool zsi_file_is_unordered(const struct zsi_file *f)
{
    return !f->hdr_valid || zsi_header_is_unordered(&f->hdr);
}

/********** UNORDERED FILE *************/

/* From the end of an unordered file's header onwards, the file is a flat sequence
 * of spans (F-23).  Each span is zero or more data records followed by exactly
 * one terminator whose span length equals the span's data byte count and whose
 * checksum validates.  Every byte belongs to exactly one span or terminator: no
 * gaps, no nesting.
 *
 * Spans exist only in unordered files; an in-order file has none (section 4.9). */

/* Invoked for each committed record, in file order.  Records in rolled-back spans
 * are never presented (F-25).  Returning non-zero stops the replay. */
typedef int zsi_replay_cb(void *rock, const struct zsi_rec *rec, size_t off);

/* Walk the span chain, setting f->complete to the offset after the last valid
 * span (F-24) -- which may be short of f->size, and is the header length for a
 * file with no valid spans at all.
 *
 * A torn tail is not an error.  It is the ordinary outcome of a crash, and of
 * reading a file a writer is still appending to, so this returns ZS_OK and lets
 * f->complete carry the answer.  Content beyond the complete point is simply not
 * part of the database.
 *
 * nocsum skips checksum verification (F-5e).  That is the caller's choice and it
 * costs tear detection: without the checksum, a span whose data never landed is
 * accepted on the strength of its length field alone.  F-22's guarantee does not
 * hold under it, and neither does C-4f's.
 *
 * Two passes per span, deliberately.  Pass one finds the terminator and validates
 * the whole span; pass two replays its records.  Records are therefore decoded
 * twice.  The alternative is buffering an unbounded span's records in memory, and
 * the second decode costs nothing measurable because the span is already in page
 * cache from the first. */
static int zsi_unordered_replay(struct zsi_file *f, bool nocsum,
                                zsi_replay_cb *cb, void *rock)
{
    /* D-10: an active file with a corrupt header or zero length is treated as a
     * complete file with zero spans.  Nothing in it is part of the database, so
     * the complete point is 0 -- which also makes it not clean (D-9), so a writer
     * moves to a new file rather than building a chain on an untrustworthy
     * boundary (R-4). */
    if (!f->hdr_valid) {
        f->complete = 0;
        return ZS_OK;
    }

    /* An in-order file has no spans.  Calling this on one is a programming error
     * rather than a data condition, so it reports nothing rather than inventing
     * an answer. */
    if (!zsi_header_is_unordered(&f->hdr)) {
        f->complete = 0;
        return ZS_BADUSAGE;
    }

    size_t pos = ZSI_HEADER_LEN;
    f->complete = pos;

    for (;;) {
        size_t span_start = pos;
        size_t p = pos;
        struct zsi_term term;
        bool found_term = false;

        /* Pass one: walk records until a terminator, validating as we go. */
        for (;;) {
            const char *b = zsi_file_at(f, p, 1);
            if (!b) break;                      /* ran off the end */

            uint8_t type = (uint8_t)b[0];
            if (!zsi_type_valid(type)) break;

            size_t avail = f->size - p;

            if (type & ZSI_SPANTERM) {
                if (zsi_term_decode(b, avail, &term) != ZS_OK) break;
                if (!zsi_file_at(f, p, term.len)) break;
                found_term = true;
                break;
            }

            /* A pointer section cannot appear in an unordered file (section 4.9),
             * and a valid type byte that is neither a data record nor a
             * terminator can only be one.  Ends the file here. */
            if (!(type & ZSI_HASKEY)) break;

            struct zsi_rec r;
            if (zsi_rec_decode(b, avail, f->hdr.start, &r) != ZS_OK) break;

            /* F-29's progress rule: the next offset comes from this record's own
             * length fields, and must be strictly greater and within bounds.
             * Non-termination is impossible by construction, which T-3's per-case
             * timeout is the detector for.
             *
             * These four checks are DELIBERATELY REDUNDANT, not dead code.
             * zsi_rec_decode already guarantees every one of them -- it rejects a
             * saturated roundup8, so out->len is never 0, and it rejects a total
             * exceeding the length it was given, so p + len never passes f->size.
             * Mutation testing confirms removing them changes nothing observable.
             *
             * They stay for two reasons.  F-29 requires the verification at the
             * iteration site rather than somewhere it happens to be implied.  And
             * they become load-bearing the moment the decoder's contract changes,
             * which is precisely the change nobody would think to audit this walk
             * for. */
            size_t next;
            if (r.len == 0) break;
            if (!zsi_add_sz(p, r.len, &next)) break;
            if (next <= p) break;
            if (next > f->size) break;

            p = next;
        }

        if (!found_term) break;                 /* complete at span_start */

        /* The span's data byte count is exactly the distance walked, so there is
         * nothing to accumulate and nothing to overflow. */
        size_t datalen = p - span_start;
        if (term.spanlen != (uint64_t)datalen) break;

        const char *spandata = zsi_file_at(f, span_start, datalen);
        const char *termbytes = zsi_file_at(f, p, term.len);
        if (!termbytes) break;
        if (datalen && !spandata) break;

        /* F-19: the checksum covers the span's data followed by the terminator's
         * own bytes up to the checksum field.  Because it covers BOTH, a
         * terminator that reached disk without its data fails here and the span
         * reads as absent.
         *
         * This is the load-bearing check of the whole concurrency design.  It is
         * what makes a torn tail always detectable (F-22), and it supplies the
         * ordering guarantee that no memory barrier can provide between
         * independent processes sharing a mapping -- which is what permits reading
         * a live file with no lock at all (C-4f).  It looks like an ordinary
         * checksum check and is not. */
        if (!nocsum) {
            uint32_t want = zsi_csum2(f->csum, f->csum_id,
                                      spandata ? spandata : "", datalen,
                                      termbytes, term.len - 4);
            if (want != term.csum) break;
        }

        size_t after;
        if (!zsi_add_sz(p, term.len, &after)) break;
        if (after > f->size) break;

        /* Pass two: replay, unless the span was rolled back.
         *
         * F-21: a ROLLBACK is a commit that says "ignore the records in this
         * span".  F-25: visibility is per span, not a watermark -- a rolled-back
         * span may sit between two live ones, so this must skip exactly this span
         * and carry on rather than stopping or lowering a high-water mark. */
        if (cb && !zsi_term_is_rollback(&term)) {
            size_t q = span_start;
            while (q < p) {
                const char *rb = zsi_file_at(f, q, 1);
                struct zsi_rec r;
                if (!rb) break;
                if (zsi_rec_decode(rb, f->size - q, f->hdr.start, &r) != ZS_OK)
                    break;
                if (r.len == 0) break;
                if (cb(rock, &r, q) != 0) return ZS_OK;   /* caller stopped */
                q += r.len;
            }
        }

        f->complete = after;
        pos = after;
    }

    return ZS_OK;
}

/* Whether the active file may be appended to (D-9).
 *
 * "An active file is clean if it has a VALID HEADER and zero or more valid spans
 * with nothing after the last."  Both halves matter: a zero-length file has
 * complete == size == 0 and would otherwise look clean, but D-10 requires a writer
 * move to a new file rather than append to it -- so no chain is ever built on a
 * boundary that failed to validate (R-4). */
static bool zsi_unordered_is_clean(const struct zsi_file *f)
{
    return f->hdr_valid && f->complete == f->size;
}

/********** POINTER SECTION *************/

/* An in-order file always ends with a pointer section followed by a 16-byte
 * trailer; an unordered file never has either (section 4.9).  So:
 *
 *     in-order   [header][records][pointer section][trailer]
 *     unordered  [header](span)*
 *
 * An in-order file has no spans and no terminators.  Every record in it is live
 * by construction, and it is written whole under a temporary name and renamed
 * only once finished (D-21), so a commit record would assert nothing that is not
 * already guaranteed.
 *
 *     PTRS32 (0x20)                        narrow
 *       +0    1      type
 *       +1    3      pad
 *       +4    4      count (uint32)
 *       +8    4xN    record offsets (uint32)
 *             .      pad with zeroes to a multiple of 8
 *
 *     PTRS64 (0x24)                        wide
 *       +0    1      type
 *       +1    7      pad
 *       +8    8      count (uint64)
 *       +16   8xN    record offsets (uint64)
 *
 * The trailer is a FIXED 16 bytes, always, so it can be read without knowing
 * anything else about the file:
 *
 *     filesize-16   8   offset of the start of the pointer section
 *     filesize-8    4   checksum of the records region
 *     filesize-4    4   checksum of the pointer section
 */

#define ZSI_TRAILER_LEN 16

/* 72 header + 8 empty PTRS32 + 16 trailer.  A file shorter than this cannot be a
 * valid in-order file (F-26g). */
#define ZSI_INORDER_MIN (ZSI_HEADER_LEN + 8 + ZSI_TRAILER_LEN)

/* Bytes a pointer section occupies, including F-26d's padding.  Returns 0 if the
 * count cannot be represented. */
static size_t zsi_ptrs_section_len(uint64_t count, bool wide)
{
    size_t hdr = wide ? 16 : 8;
    size_t per = wide ? 8 : 4;
    size_t body, total;

    if (count > SIZE_MAX / per) return 0;
    body = (size_t)count * per;
    if (!zsi_add_sz(hdr, body, &total)) return 0;

    /* The narrow section is padded with zeroes to a multiple of 8 so the trailer
     * begins 8-aligned (F-2).  The pad is 0 or 4 bytes and the checksum covers
     * it.  The wide section is always a multiple of 8 already. */
    return zsi_roundup8(total);
}

/* The record offset at index i.  i must be < f->nptrs; the caller has already
 * bounds-checked the array as a whole in zsi_ptrs_load. */
static uint64_t zsi_ptrs_at(const struct zsi_file *f, uint64_t i)
{
    size_t hdr = f->ptr_wide ? 16 : 8;
    size_t per = f->ptr_wide ? 8 : 4;
    const char *p = f->base + f->ptr_off + hdr + (size_t)i * per;

    return f->ptr_wide ? zsi_get64(p) : (uint64_t)zsi_get32(p);
}

/* Read the trailer and pointer section and validate the file's structure.
 *
 * O(1) (F-31): validate the header, read the 16-byte trailer, verify the
 * pointer-section checksum, use the pointers.  The records region is never
 * touched here -- its checksum is verified only on demand (F-26f), because
 * opening must not be proportional to the file's size.
 *
 * Order matters, and F-26a and F-26b are what make it safe:
 *
 *   - the trailer is at a fixed size and a fixed position, so it needs no prior
 *     knowledge of the file;
 *   - the back pointer inside it is plain data, read before anything is
 *     verified, so there is no circularity -- a wrong value yields a failed
 *     checksum rather than a wrong interpretation;
 *   - the back pointer is 8 bytes wide even in a narrow file, because a
 *     variable-size trailer could not be read without first knowing which size
 *     it was.
 */
static int zsi_ptrs_load(struct zsi_file *f)
{
    if (!f->hdr_valid) return ZS_BADFORMAT;
    if (zsi_header_is_unordered(&f->hdr)) return ZS_BADUSAGE;

    /* Enough for a header and a trailer at minimum.  The 96-byte floor is
     * enforced implicitly by the back-pointer bounds below, which require at
     * least an 8-byte section between them. */
    if (f->size < ZSI_HEADER_LEN + ZSI_TRAILER_LEN) return ZS_BADFORMAT;

    const char *tr = zsi_file_at(f, f->size - ZSI_TRAILER_LEN, ZSI_TRAILER_LEN);
    if (!tr) return ZS_BADFORMAT;

    uint64_t back = zsi_get64(tr);
    uint32_t rec_csum = zsi_get32(tr + 8);
    uint32_t sec_csum = zsi_get32(tr + 12);

    /* The back pointer must land inside the file, after the header, and leave
     * room for at least the smallest section before the trailer.  It must also
     * be 8-aligned (F-2), which a corrupt value usually is not. */
    if (back < ZSI_HEADER_LEN) return ZS_BADFORMAT;
    if (back % 8 != 0) return ZS_BADFORMAT;
    if (back > (uint64_t)SIZE_MAX) return ZS_BADFORMAT;

    size_t ptr_off = (size_t)back;
    size_t sec_end;
    if (!zsi_add_sz(ptr_off, 8, &sec_end)) return ZS_BADFORMAT;
    if (sec_end > f->size - ZSI_TRAILER_LEN) return ZS_BADFORMAT;

    const char *sec = zsi_file_at(f, ptr_off, 8);
    if (!sec) return ZS_BADFORMAT;

    uint8_t type = (uint8_t)sec[0];
    bool wide;
    if (type == ZSI_PTRS32)      wide = false;
    else if (type == ZSI_PTRS64) wide = true;
    else                         return ZS_BADFORMAT;

    /* F-26b: the section checksum covers everything from the start of the
     * section up to the checksum field itself -- the section, its padding, the
     * back pointer, and the records checksum.  F-4 with no special case.
     *
     * Verified BEFORE the count is trusted, so a corrupt count cannot steer the
     * bounds arithmetic below. */
    size_t covered = (f->size - 4) - ptr_off;
    const char *cbase = zsi_file_at(f, ptr_off, covered);
    if (!cbase) return ZS_BADFORMAT;
    if (f->csum(cbase, covered) != sec_csum) return ZS_BADCHECKSUM;

    uint64_t count;
    if (wide) {
        const char *c = zsi_file_at(f, ptr_off, 16);
        if (!c) return ZS_BADFORMAT;
        count = zsi_get64(c + 8);
    } else {
        count = (uint64_t)zsi_get32(sec + 4);
    }

    size_t seclen = zsi_ptrs_section_len(count, wide);
    if (seclen == 0) return ZS_BADFORMAT;

    size_t want_end;
    if (!zsi_add_sz(ptr_off, seclen, &want_end)) return ZS_BADFORMAT;

    /* The section plus the trailer must be exactly the rest of the file.  An
     * equality rather than a bound: anything else means the count and the file
     * size disagree, which no conforming writer produces. */
    if (want_end != f->size - ZSI_TRAILER_LEN) return ZS_BADFORMAT;

    f->ptr_off = ptr_off;
    f->nptrs = count;
    f->ptr_wide = wide;
    f->records_csum = rec_csum;

    /* F-27: every pointer must be 8-aligned and lie between the header and the
     * pointer section.  With count == 0 this loop runs zero times and the
     * requirement is vacuous, which is exactly right (F-26g). */
    for (uint64_t i = 0; i < count; i++) {
        uint64_t off = zsi_ptrs_at(f, i);
        if (off < ZSI_HEADER_LEN) return ZS_BADFORMAT;
        if (off >= ptr_off) return ZS_BADFORMAT;
        if (off % 8 != 0) return ZS_BADFORMAT;
    }

    return ZS_OK;
}

/* Verify the records-region checksum (F-26e).
 *
 * Called on demand only -- by zs_db_check_consistency, or by a caller that
 * chooses to -- never on open, which stays O(1) (F-26f).  This is the only thing
 * that detects a record body corrupted in place in an in-order file: there are no
 * span terminators to notice it, so without this the corruption is invisible. */
static int zsi_ptrs_verify_records(struct zsi_file *f)
{
    size_t len = f->ptr_off - ZSI_HEADER_LEN;
    const char *p = zsi_file_at(f, ZSI_HEADER_LEN, len);

    if (!p && len) return ZS_BADFORMAT;

    /* An empty records region checksums to the engine's value for empty input,
     * not to zero (F-26g).  Passing "" rather than NULL keeps that path
     * identical to any other zero-length checksum. */
    if (f->csum(p ? p : "", len) != f->records_csum) return ZS_BADCHECKSUM;

    return ZS_OK;
}

/* Decode the record at pointer index i. */
static int zsi_ptrs_rec(struct zsi_file *f, uint64_t i, struct zsi_rec *out)
{
    uint64_t off = zsi_ptrs_at(f, i);
    const char *b = zsi_file_at(f, (size_t)off, 1);

    if (!b) return ZS_BADFORMAT;
    return zsi_rec_decode(b, f->ptr_off - (size_t)off, f->hdr.start, out);
}

/* An implementation MAY probe the first and last pointers before the rest, which
 * rejects an out-of-range key in two comparisons rather than the log2(n) a plain
 * binary search takes to walk to an end (D-14d).
 *
 * This is a search STRATEGY, not a way of avoiding the search: those two pointers
 * still have to be dereferenced and their keys compared, so it is the same kind
 * of work, just less of it.  It needs no cached metadata and cannot change the
 * answer -- which T-5a checks by running the same assertions with it compiled
 * out. */
#ifndef ZSI_PROBE_ENDS
#define ZSI_PROBE_ENDS 1
#endif

/* Binary search for key.  Sets *idx to the first index whose key is >= key, and
 * *exact to whether it matches.
 *
 * With nptrs == 0 this sets *idx = 0 and *exact = false: an ordinary case, not a
 * special one (F-26g, D-14b).  Because a repack emits exactly one record per key
 * (D-17), keys in an in-order file are unique and the array is a strict ordering,
 * so a plain lower bound is correct. */
static int zsi_ptrs_search(struct zsi_file *f, zs_compar *compar,
                           const char *key, size_t keylen,
                           uint64_t *idx, bool *exact)
{
    struct zsi_rec r;

    *idx = 0;
    *exact = false;
    if (f->nptrs == 0) return ZS_OK;

#if ZSI_PROBE_ENDS
    {
        if (zsi_ptrs_rec(f, 0, &r) != ZS_OK) return ZS_BADFORMAT;
        int c = compar(key, keylen, r.key, r.keylen);
        if (c <= 0) {
            *idx = 0;
            *exact = (c == 0);
            return ZS_OK;
        }

        if (zsi_ptrs_rec(f, f->nptrs - 1, &r) != ZS_OK) return ZS_BADFORMAT;
        c = compar(key, keylen, r.key, r.keylen);
        if (c > 0) {
            *idx = f->nptrs;            /* past every key */
            return ZS_OK;
        }
        if (c == 0) {
            *idx = f->nptrs - 1;
            *exact = true;
            return ZS_OK;
        }
    }
#endif

    uint64_t lo = 0, hi = f->nptrs;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (zsi_ptrs_rec(f, mid, &r) != ZS_OK) return ZS_BADFORMAT;
        if (compar(r.key, r.keylen, key, keylen) < 0) lo = mid + 1;
        else hi = mid;
    }

    *idx = lo;
    if (lo < f->nptrs) {
        if (zsi_ptrs_rec(f, lo, &r) != ZS_OK) return ZS_BADFORMAT;
        *exact = (compar(r.key, r.keylen, key, keylen) == 0);
    }

    return ZS_OK;
}

/* Build a pointer section and trailer for records already laid down.
 *
 * offs must be sorted by key ascending, and records_end is where the section
 * begins -- the offset just past the last record.  Returns a malloc'd buffer the
 * caller writes and frees.
 *
 * Width by F-26c: PTRS32 when every record offset fits in 32 bits, and since all
 * records precede the section, that is equivalent to the section's own offset
 * fitting.  Canonical, so two implementations produce identical bytes. */
static int zsi_ptrs_build(const uint64_t *offs, size_t n, size_t records_end,
                          uint32_t records_csum, zs_csum *csum,
                          char **out, size_t *outlen)
{
    bool wide = records_end > 0xFFFFFFFFu;
    size_t seclen = zsi_ptrs_section_len((uint64_t)n, wide);
    size_t total;

    if (seclen == 0) return ZS_INTERNAL;
    if (!zsi_add_sz(seclen, ZSI_TRAILER_LEN, &total)) return ZS_INTERNAL;

    char *buf = zsi_zmalloc(total);      /* zeroed: F-26d's padding, and F-2 */
    if (!buf) return ZS_INTERNAL;

    if (wide) {
        buf[0] = (char)ZSI_PTRS64;
        zsi_put64(buf + 8, (uint64_t)n);
        for (size_t i = 0; i < n; i++)
            zsi_put64(buf + 16 + i * 8, offs[i]);
    } else {
        buf[0] = (char)ZSI_PTRS32;
        zsi_put32(buf + 4, (uint32_t)n);
        for (size_t i = 0; i < n; i++)
            zsi_put32(buf + 8 + i * 4, (uint32_t)offs[i]);
    }

    /* Trailer: back pointer, then the records checksum, then the section
     * checksum -- which covers everything before it (F-26b). */
    zsi_put64(buf + seclen, (uint64_t)records_end);
    zsi_put32(buf + seclen + 8, records_csum);
    zsi_put32(buf + seclen + 12, csum(buf, seclen + 12));

    *out = buf;
    *outlen = total;
    return ZS_OK;
}

/********** PRIVATE INDEX *************/

/* An unordered file has no pointer section, so key order for it must be derived
 * by replaying its spans.  There is no shared index file: every process builds
 * its own, in private memory, for each unordered file in its snapshot (section
 * 5.4).  That is what makes G-6 hold without a lock -- nothing a reader may be
 * reading is ever rewritten beneath it, because nothing is shared at all.
 *
 * Structure: a sorted array of record offsets, plus a small sorted delta array
 * for records committed since the base was built.  Lookups consult the delta
 * first; traversal merges the two, preferring the delta on equal keys.
 *
 * The full rationale, and the build and lookup paths, are in the task that
 * implements them.  Only the shape and the destructor live here, because
 * zsi_file_close must be able to free an index without the caller remembering to. */
/* Above ZSI_DELTA_MAX entries the delta is merged into the base and cleared.
 *
 * The bound is what makes insertion amortised O(1) rather than O(n): a splice
 * into an array of at most this many entries is a bounded memmove, and the merge
 * that empties it is linear in the whole index but happens once per
 * ZSI_DELTA_MAX inserts.  A single sorted array with no delta would memmove the
 * entire index per commit, which for a 2MB active file under bulk load is
 * megabytes of copying per transaction. */
#define ZSI_DELTA_MAX 1024

/* Record offsets are size_t rather than uint32_t.
 *
 * uint32_t would halve the footprint and rollover_size (2MB by default) keeps
 * real unordered files far below 4GB -- but rollover_size is caller-configurable
 * and a crash can leave a larger file behind, and the failure mode of guessing
 * wrong is a database that cannot be opened.  G-3 says any state a crash can
 * produce must open; paying 4 bytes an entry to keep that unconditional is the
 * right trade.  A 2MB file of minimum-size records holds 262144 of them, so the
 * index is 2MB at worst. */
struct zsi_index {
    struct zsi_file *file;
    size_t *base;   size_t nbase;
    size_t *delta;  size_t ndelta, adelta;
};

static void zsi_index_free(struct zsi_index **ip)
{
    struct zsi_index *ix = *ip;
    if (!ix) return;

    free(ix->base);
    free(ix->delta);
    free(ix);
    *ip = NULL;
}

/* Decode the record at off far enough to reach its key.
 *
 * Every offset in an index decoded successfully during the build, so this cannot
 * fail for a well-formed index -- but it is called from comparison functions
 * where returning a wrong answer is worse than returning a stable one, so a
 * failure yields an empty key rather than reading uninitialised memory. */
static bool zsi_index_key_at(struct zsi_file *f, size_t off,
                             const char **kp, size_t *klp)
{
    const char *b = zsi_file_at(f, off, 1);
    struct zsi_rec r;

    if (!b || zsi_rec_decode(b, f->size - off, f->hdr.start, &r) != ZS_OK) {
        *kp = "";
        *klp = 0;
        return false;
    }

    *kp = r.key;
    *klp = r.keylen;
    return true;
}

/* Sort context: the comparator cannot be a global, because two threads may hold
 * handles on different databases with different comparators.  That rules out
 * plain qsort, and qsort_r's signature differs between glibc and the BSDs, so
 * this file carries its own merge sort taking an explicit context. */
struct zsi_ksort {
    struct zsi_file *f;
    zs_compar       *compar;
};

/* Order by key ascending, then by offset DESCENDING.
 *
 * The offset tie-break is what makes "newest version of a key wins within a
 * file" (D-14) fall out of the sort: equal keys end up newest-first, so keeping
 * the first of each run keeps the newest.  There is no second pass and no
 * separate notion of recency. */
static int zsi_ksort_cmp(struct zsi_ksort *ks, size_t a, size_t b)
{
    const char *ka, *kb;
    size_t la, lb;

    zsi_index_key_at(ks->f, a, &ka, &la);
    zsi_index_key_at(ks->f, b, &kb, &lb);

    int c = ks->compar(ka, la, kb, lb);
    if (c) return c;

    if (a == b) return 0;
    return a > b ? -1 : 1;              /* higher offset first */
}

static void zsi_msort(size_t *arr, size_t n, size_t *tmp, struct zsi_ksort *ks)
{
    if (n < 2) return;

    size_t mid = n / 2;
    zsi_msort(arr, mid, tmp, ks);
    zsi_msort(arr + mid, n - mid, tmp + mid, ks);

    size_t i = 0, j = mid, k = 0;
    while (i < mid && j < n)
        tmp[k++] = (zsi_ksort_cmp(ks, arr[j], arr[i]) < 0) ? arr[j++] : arr[i++];
    while (i < mid) tmp[k++] = arr[i++];
    while (j < n)   tmp[k++] = arr[j++];

    memcpy(arr, tmp, n * sizeof(*arr));
}

/* The first position in arr whose key is >= (key, keylen). */
static size_t zsi_index_lb(const size_t *arr, size_t n, struct zsi_ksort *ks,
                           const char *key, size_t keylen)
{
    size_t lo = 0, hi = n;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const char *k;
        size_t kl;
        zsi_index_key_at(ks->f, arr[mid], &k, &kl);
        if (ks->compar(k, kl, key, keylen) < 0) lo = mid + 1;
        else hi = mid;
    }

    return lo;
}

/* Whether the entry at arr[i] has exactly this key. */
static bool zsi_index_eq(const size_t *arr, size_t n, size_t i,
                         struct zsi_ksort *ks, const char *key, size_t keylen)
{
    const char *k;
    size_t kl;

    if (i >= n) return false;
    zsi_index_key_at(ks->f, arr[i], &k, &kl);
    return ks->compar(k, kl, key, keylen) == 0;
}

/* Collector for the build replay. */
struct zsi_index_build {
    size_t *offs;
    size_t  n, alloc;
    bool    oom;
};

static int zsi_index_build_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct zsi_index_build *b = rock;

    (void)rec;
    if (b->n == b->alloc) {
        size_t want = b->alloc ? b->alloc * 2 : 256;
        size_t *p = realloc(b->offs, want * sizeof(*p));
        if (!p) { b->oom = true; return 1; }
        b->offs = p;
        b->alloc = want;
    }

    b->offs[b->n++] = off;
    return 0;
}

/* Build the private index for an unordered file by replaying its spans.
 *
 * It reflects COMMITTED spans only, and for each key only its newest committed
 * record (D-13a).  Building it means replaying spans and skipping rolled-back
 * ones -- never simply walking every record, which would resurrect aborted
 * writes.  That is why this goes through zsi_unordered_replay rather than
 * scanning the file directly, and why test_index_committed_only exists.
 *
 * Sets f->complete as a side effect, since the replay establishes it. */
static int zsi_index_build(struct zsi_file *f, zs_compar *compar, bool nocsum)
{
    struct zsi_index_build b;
    struct zsi_index *ix;
    int r;

    zsi_index_free(&f->index);

    memset(&b, 0, sizeof(b));
    r = zsi_unordered_replay(f, nocsum, zsi_index_build_cb, &b);
    if (r != ZS_OK) { free(b.offs); return r; }
    if (b.oom) { free(b.offs); return ZS_INTERNAL; }

    ix = zsi_zmalloc(sizeof(*ix));
    if (!ix) { free(b.offs); return ZS_INTERNAL; }
    ix->file = f;

    if (b.n) {
        struct zsi_ksort ks = { f, compar };
        size_t *tmp = malloc(b.n * sizeof(*tmp));
        if (!tmp) { free(b.offs); free(ix); return ZS_INTERNAL; }

        zsi_msort(b.offs, b.n, tmp, &ks);
        free(tmp);

        /* Keep the first of each equal-key run, which the offset-descending
         * tie-break makes the newest (D-14). */
        size_t w = 0;
        for (size_t i = 0; i < b.n; i++) {
            if (w) {
                const char *ka, *kb;
                size_t la, lb;
                zsi_index_key_at(f, b.offs[w - 1], &ka, &la);
                zsi_index_key_at(f, b.offs[i], &kb, &lb);
                if (compar(ka, la, kb, lb) == 0) continue;
            }
            b.offs[w++] = b.offs[i];
        }
        b.n = w;
    }

    ix->base = b.offs;
    ix->nbase = b.n;
    f->index = ix;
    return ZS_OK;
}

/* Point lookup.  Consults the delta first, so a key present in both yields the
 * newer record. */
static int zsi_index_find(struct zsi_index *ix, zs_compar *compar,
                          const char *key, size_t keylen, size_t *off)
{
    struct zsi_ksort ks = { ix->file, compar };
    size_t i;

    i = zsi_index_lb(ix->delta, ix->ndelta, &ks, key, keylen);
    if (zsi_index_eq(ix->delta, ix->ndelta, i, &ks, key, keylen)) {
        *off = ix->delta[i];
        return ZS_OK;
    }

    i = zsi_index_lb(ix->base, ix->nbase, &ks, key, keylen);
    if (zsi_index_eq(ix->base, ix->nbase, i, &ks, key, keylen)) {
        *off = ix->base[i];
        return ZS_OK;
    }

    return ZS_NOTFOUND;
}

/* Ordered traversal (D-13).
 *
 * A cursor over the merged base+delta ordering.  There is deliberately no random
 * access: the merged view is not an array -- a key may appear in both sides -- so
 * a count would be an upper bound and an index-addressed at() would have to
 * re-walk the merge on every call.  The only consumer advances sequentially. */
struct zsi_index_cur { size_t bi, di; };

static void zsi_index_cur_seek_first(struct zsi_index_cur *c)
{
    c->bi = 0;
    c->di = 0;
}

static void zsi_index_cur_seek(struct zsi_index *ix, zs_compar *compar,
                               const char *key, size_t keylen,
                               struct zsi_index_cur *c)
{
    struct zsi_ksort ks = { ix->file, compar };

    /* Both sides are searched independently: the merge happens on get, not here. */
    c->bi = zsi_index_lb(ix->base, ix->nbase, &ks, key, keylen);
    c->di = zsi_index_lb(ix->delta, ix->ndelta, &ks, key, keylen);
}

/* ZS_DONE when exhausted, otherwise ZS_OK with *out filled and *off set. */
static int zsi_index_cur_get(struct zsi_index *ix, zs_compar *compar,
                             struct zsi_index_cur *c,
                             struct zsi_rec *out, size_t *off)
{
    bool have_b = c->bi < ix->nbase;
    bool have_d = c->di < ix->ndelta;
    size_t chosen;

    if (!have_b && !have_d) return ZS_DONE;

    if (have_b && have_d) {
        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[c->bi], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[c->di], &kd, &ld);
        /* Prefer the delta when the keys are equal: it is the newer record. */
        chosen = (compar(kd, ld, kb, lb) <= 0) ? ix->delta[c->di]
                                              : ix->base[c->bi];
    } else {
        chosen = have_d ? ix->delta[c->di] : ix->base[c->bi];
    }

    const char *b = zsi_file_at(ix->file, chosen, 1);
    if (!b) return ZS_DONE;
    if (zsi_rec_decode(b, ix->file->size - chosen, ix->file->hdr.start, out)
        != ZS_OK)
        return ZS_DONE;

    if (off) *off = chosen;
    return ZS_OK;
}

static void zsi_index_cur_next(struct zsi_index *ix, zs_compar *compar,
                               struct zsi_index_cur *c)
{
    bool have_b = c->bi < ix->nbase;
    bool have_d = c->di < ix->ndelta;

    if (!have_b && !have_d) return;

    if (have_b && have_d) {
        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[c->bi], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[c->di], &kd, &ld);
        int cmp = compar(kd, ld, kb, lb);

        if (cmp == 0) {
            /* Advance BOTH.  The delta's record was just yielded; leaving the
             * base's stale copy of the same key in place would surface it on the
             * following step, which breaks D-14h one level down -- a per-file
             * cursor must never yield the same key twice. */
            c->di++;
            c->bi++;
        } else if (cmp < 0) {
            c->di++;
        } else {
            c->bi++;
        }
        return;
    }

    if (have_d) c->di++;
    else        c->bi++;
}

/* Fold a newly committed record into the index (D-13b).
 *
 * A writer is a reader that also maintains the active file's index
 * incrementally: it already knows every record it appends, so it folds them in
 * at commit rather than rescanning a file it is writing. */
static int zsi_index_insert(struct zsi_index *ix, zs_compar *compar, size_t off)
{
    struct zsi_ksort ks = { ix->file, compar };
    const char *key;
    size_t keylen;

    if (!zsi_index_key_at(ix->file, off, &key, &keylen)) return ZS_BADFORMAT;

    size_t i = zsi_index_lb(ix->delta, ix->ndelta, &ks, key, keylen);

    /* Replacing an existing delta entry for this key keeps the delta at one entry
     * per key, which is what bounds it at ZSI_DELTA_MAX regardless of how often a
     * single key is rewritten. */
    if (zsi_index_eq(ix->delta, ix->ndelta, i, &ks, key, keylen)) {
        ix->delta[i] = off;
        return ZS_OK;
    }

    if (ix->ndelta == ix->adelta) {
        size_t want = ix->adelta ? ix->adelta * 2 : 64;
        size_t *p = realloc(ix->delta, want * sizeof(*p));
        if (!p) return ZS_INTERNAL;
        ix->delta = p;
        ix->adelta = want;
    }

    memmove(ix->delta + i + 1, ix->delta + i,
            (ix->ndelta - i) * sizeof(*ix->delta));
    ix->delta[i] = off;
    ix->ndelta++;

    if (ix->ndelta <= ZSI_DELTA_MAX) return ZS_OK;

    /* Merge the delta into the base and clear it: a linear pass, delta winning
     * ties because it is newer. */
    size_t *merged = malloc((ix->nbase + ix->ndelta) * sizeof(*merged));
    if (!merged) return ZS_OK;      /* the delta is still correct, just large */

    size_t bi = 0, di = 0, w = 0;
    while (bi < ix->nbase || di < ix->ndelta) {
        if (bi >= ix->nbase) { merged[w++] = ix->delta[di++]; continue; }
        if (di >= ix->ndelta) { merged[w++] = ix->base[bi++]; continue; }

        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[bi], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[di], &kd, &ld);
        int cmp = compar(kd, ld, kb, lb);

        if (cmp == 0)      { merged[w++] = ix->delta[di++]; bi++; }
        else if (cmp < 0)  { merged[w++] = ix->delta[di++]; }
        else               { merged[w++] = ix->base[bi++]; }
    }

    free(ix->base);
    ix->base = merged;
    ix->nbase = w;
    ix->ndelta = 0;

    return ZS_OK;
}

/********** PER-FILE CURSOR *************/

/* One cursor over one source, presenting the same four operations whatever the
 * source is.  This is what lets the read path (D-14, D-14e) be written once
 * rather than per file kind -- the merge does not know or care which kind it is
 * pulling from.
 *
 * Three kinds, matching D-14's table of sources:
 *
 *   ZSI_SRC_TXN        the current write transaction's uncommitted records
 *   ZSI_SRC_UNORDERED  an unordered file, via its private index (D-13)
 *   ZSI_SRC_INORDER    an in-order file, via its pointer array
 *
 * D-14h: a per-file cursor never yields the same key twice.  An in-order file
 * holds one record per key by construction (D-17), and a private index exposes
 * only the newest committed record per key (D-13a).  Duplicates therefore arise
 * only ACROSS sources, which is exactly what the merge's step 3 handles. */

enum zsi_src_kind { ZSI_SRC_INORDER, ZSI_SRC_UNORDERED, ZSI_SRC_TXN };

/* The transaction's records sort as though they had a generation above every
 * file's, giving them highest priority for equal keys without a special case in
 * the merge comparator (D-14g). */
#define ZSI_GEN_TXN UINT32_MAX

struct zsi_fcur {
    enum zsi_src_kind kind;
    struct zsi_file  *file;      /* NULL for ZSI_SRC_TXN */
    struct zs_txn    *txn;       /* NULL otherwise */
    zs_compar        *compar;
    uint32_t          gen;       /* file->hdr.start, or ZSI_GEN_TXN */
    bool              exhausted;
    struct zsi_rec    cur;       /* valid iff !exhausted */

    /* kind-specific position */
    uint64_t             pi;     /* in-order: pointer array index */
    struct zsi_index_cur ic;     /* unordered: index cursor */
    size_t               ti;     /* txn: index into its sorted pending array */
};

/* Filled in by the WRITE PATH section, which owns struct zs_txn.  Declared here
 * because the cursor must be able to present a transaction as just another
 * source; the alternative is a special case in the merge, which D-14g exists to
 * avoid. */
static int zsi_txn_cur_load(struct zsi_fcur *fc);
static void zsi_txn_cur_seek(struct zsi_fcur *fc, const char *key, size_t keylen);

/* Load the record at the cursor's current position, or mark it exhausted. */
static int zsi_fcur_load(struct zsi_fcur *fc)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER:
        if (fc->pi >= fc->file->nptrs) { fc->exhausted = true; return ZS_OK; }
        if (zsi_ptrs_rec(fc->file, fc->pi, &fc->cur) != ZS_OK) {
            fc->exhausted = true;
            return ZS_BADFORMAT;
        }
        fc->exhausted = false;
        return ZS_OK;

    case ZSI_SRC_UNORDERED: {
        /* No index means the caller skipped building one, which is a wiring
         * error rather than a data condition.  Report an exhausted source rather
         * than dereferencing NULL: G-3 requires that no state produce a crash,
         * and a cursor over a source with nothing in it is a defined answer. */
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }

        int r = zsi_index_cur_get(fc->file->index, fc->compar, &fc->ic,
                                  &fc->cur, NULL);
        fc->exhausted = (r != ZS_OK);
        return ZS_OK;
    }

    case ZSI_SRC_TXN:
        return zsi_txn_cur_load(fc);
    }

    fc->exhausted = true;
    return ZS_OK;
}

/* Position at the first record with key >= the given key, or exhaust.
 *
 * A source holding no records exhausts immediately -- an empty in-order file
 * (F-26g) or an unordered file with no committed records (F-26h) are ordinary
 * cases here, not special ones (D-14e step 1). */
static int zsi_fcur_seek(struct zsi_fcur *fc, const char *key, size_t keylen)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER: {
        uint64_t idx;
        bool exact;
        int r = zsi_ptrs_search(fc->file, fc->compar, key, keylen, &idx, &exact);
        if (r != ZS_OK) { fc->exhausted = true; return r; }
        fc->pi = idx;
        break;
    }

    case ZSI_SRC_UNORDERED:
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }
        zsi_index_cur_seek(fc->file->index, fc->compar, key, keylen, &fc->ic);
        break;

    case ZSI_SRC_TXN:
        zsi_txn_cur_seek(fc, key, keylen);
        break;
    }

    return zsi_fcur_load(fc);
}

static int zsi_fcur_seek_first(struct zsi_fcur *fc)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER:   fc->pi = 0; break;
    case ZSI_SRC_UNORDERED: zsi_index_cur_seek_first(&fc->ic); break;
    case ZSI_SRC_TXN:       fc->ti = 0; break;
    }

    return zsi_fcur_load(fc);
}

static int zsi_fcur_next(struct zsi_fcur *fc)
{
    if (fc->exhausted) return ZS_OK;

    switch (fc->kind) {
    case ZSI_SRC_INORDER:   fc->pi++; break;
    case ZSI_SRC_UNORDERED:
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }
        zsi_index_cur_next(fc->file->index, fc->compar, &fc->ic);
        break;
    case ZSI_SRC_TXN:       fc->ti++; break;
    }

    return zsi_fcur_load(fc);
}

/* Search one source for a single key, independently of any cursor state (D-14b).
 *
 * in-order   binary search the pointer array          O(log n) comparisons
 * unordered  point lookup in the private index        as that structure provides
 * txn        binary search its sorted pending array
 *
 * Both file kinds MUST report "absent" for an empty source rather than
 * misbehaving: a binary search over a zero-length array and an index for a file
 * with no committed records are both ordinary cases. */
static int zsi_fcur_find(struct zsi_fcur *fc, const char *key, size_t keylen,
                         struct zsi_rec *out)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER: {
        uint64_t idx;
        bool exact;
        int r = zsi_ptrs_search(fc->file, fc->compar, key, keylen, &idx, &exact);
        if (r != ZS_OK) return r;
        if (!exact) return ZS_NOTFOUND;
        return zsi_ptrs_rec(fc->file, idx, out);
    }

    case ZSI_SRC_UNORDERED: {
        size_t off;
        if (!fc->file->index) return ZS_NOTFOUND;
        int r = zsi_index_find(fc->file->index, fc->compar, key, keylen, &off);
        if (r != ZS_OK) return r;
        const char *b = zsi_file_at(fc->file, off, 1);
        if (!b) return ZS_BADFORMAT;
        return zsi_rec_decode(b, fc->file->size - off, fc->file->hdr.start, out);
    }

    case ZSI_SRC_TXN: {
        /* Seek a scratch cursor and check for an exact hit, so the transaction's
         * lookup shares the ordering logic rather than duplicating it. */
        struct zsi_fcur scratch = *fc;
        int r = zsi_fcur_seek(&scratch, key, keylen);
        if (r != ZS_OK) return r;
        if (scratch.exhausted) return ZS_NOTFOUND;
        if (fc->compar(scratch.cur.key, scratch.cur.keylen, key, keylen) != 0)
            return ZS_NOTFOUND;
        *out = scratch.cur;
        return ZS_OK;
    }
    }

    return ZS_NOTFOUND;
}

/* Initialise a cursor over a file.  The caller has already built the index for
 * an unordered file and loaded the pointers for an in-order one. */
static void zsi_fcur_init_file(struct zsi_fcur *fc, struct zsi_file *f,
                               zs_compar *compar)
{
    memset(fc, 0, sizeof(*fc));
    fc->kind = zsi_file_is_unordered(f) ? ZSI_SRC_UNORDERED : ZSI_SRC_INORDER;
    fc->file = f;
    fc->compar = compar;
    fc->gen = f->hdr.start;
    fc->exhausted = true;
}

/********** FILE SET *************/

/* There is no manifest.  THE DIRECTORY IS THE FILE SET (section 5.2).
 *
 * Filenames carry each file's generation range (D-1), so one readdir yields the
 * set and every range without opening a single file.  That is not a shortcut: it
 * is what makes creating a file identical to publishing it (D-8), so there is no
 * window in which a generation has been allocated but is invisible. */

struct zsi_entry {
    char     name[ZSI_NAME_MAX];
    uint32_t start, end;        /* end == 0 for unordered */
};

struct zsi_fileset {
    struct zsi_entry *all;      size_t nall;       /* every matching name */
    struct zsi_entry *resolved; size_t nresolved;  /* D-5's winners, ascending */
    zsi_uuid_t uuid;
    bool       have_uuid;
};

static void zsi_fileset_fini(struct zsi_fileset *fs)
{
    free(fs->all);
    free(fs->resolved);
    memset(fs, 0, sizeof(*fs));
}

static int zsi_entry_cmp(const void *a, const void *b)
{
    return strcmp(((const struct zsi_entry *)a)->name,
                  ((const struct zsi_entry *)b)->name);
}

/* readdir the directory, keeping the data files of one database (D-4).
 *
 * If want_uuid is NULL the UUID is DISCOVERED: parse it from each zeroskip-*
 * name and require they all agree (D-4a).  Disagreement is an error, never a
 * choice of majority -- silently adopting one would read half a database and
 * call it whole.  A directory with no data files leaves have_uuid false, which is
 * the empty case D-8a handles. */
static int zsi_fileset_scan(const char *dir, const zsi_uuid_t *want_uuid,
                            struct zsi_fileset *fs)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    size_t alloc = 0;

    memset(fs, 0, sizeof(*fs));
    if (!d) return (errno == ENOENT) ? ZS_NOTFOUND : ZS_IOERROR;

    if (want_uuid) {
        memcpy(fs->uuid, *want_uuid, 16);
        fs->have_uuid = true;
    }

    while ((de = readdir(d)) != NULL) {
        zsi_uuid_t u;
        uint32_t start, end;

        enum zsi_nametype t = zsi_name_parse(de->d_name, u, &start, &end);
        if (t == ZSI_NAME_OTHER) continue;      /* staging, lock, foreign, junk */

        if (!fs->have_uuid) {
            memcpy(fs->uuid, u, 16);
            fs->have_uuid = true;
        } else if (memcmp(fs->uuid, u, 16) != 0) {
            /* Two databases' files mixed into one directory.  If the caller named
             * a UUID this is simply someone else's file and we ignore it; if we
             * are discovering, it is corruption and must be reported. */
            if (want_uuid) continue;
            closedir(d);
            zsi_fileset_fini(fs);
            return ZS_BADFORMAT;
        }

        if (fs->nall == alloc) {
            size_t want = alloc ? alloc * 2 : 16;
            struct zsi_entry *p = realloc(fs->all, want * sizeof(*p));
            if (!p) { closedir(d); zsi_fileset_fini(fs); return ZS_INTERNAL; }
            fs->all = p;
            alloc = want;
        }

        snprintf(fs->all[fs->nall].name, ZSI_NAME_MAX, "%s", de->d_name);
        fs->all[fs->nall].start = start;
        fs->all[fs->nall].end = end;
        fs->nall++;
    }

    closedir(d);

    /* Sort lexically.  D-1's fixed-width uppercase hex makes lexical order
     * numeric order, and D-1a's no-extension rule makes an unordered name a
     * strict prefix of the in-order name for the same generation -- the two
     * properties D-5's "take the last" rule rests on, both under test in
     * test_filename_prefix_property and test_filename_lexical_order. */
    if (fs->nall)
        qsort(fs->all, fs->nall, sizeof(*fs->all), zsi_entry_cmp);

    return ZS_OK;
}

/* D-5's single sweep over the sorted names:
 *
 *   Start at the lowest generation present.  Repeatedly take the LAST file whose
 *   start equals the current generation, then set the current generation to that
 *   file's end + 1 (or start + 1 for an unordered file).  Stop when no file starts
 *   at the current generation.
 *
 * An overlap is never an error -- it is RESOLVED, not rejected.  An output is
 * renamed into place before its inputs are removed, so a scan legitimately sees
 * a repack output alongside the files it encloses.
 *
 * Taking the LAST is the whole rule, and it is correct only because of the sort:
 * fixed-width hex makes the widest end sort last among files sharing a start, and
 * the prefix property makes an unordered name sort before the in-order name for
 * the same generation.  T-9 asserts that taking the FIRST fails, so D-5b's
 * requirement is tested rather than assumed.
 *
 * Returns ZS_OK when the resolved set tiles (D-6), ZS_AGAIN when it leaves a gap
 * (D-7 -- a torn readdir, retry), or ZS_BADFORMAT for a partial overlap (D-5c). */
static int zsi_fileset_resolve(struct zsi_fileset *fs)
{
    free(fs->resolved);
    fs->resolved = NULL;
    fs->nresolved = 0;

    if (fs->nall == 0) return ZS_OK;        /* the empty case D-8a handles */

    fs->resolved = malloc(fs->nall * sizeof(*fs->resolved));
    if (!fs->resolved) return ZS_INTERNAL;

    /* The lowest generation present, over every file rather than the resolved
     * ones: a superseded file still marks where the database begins. */
    uint32_t cur = fs->all[0].start;
    for (size_t i = 1; i < fs->nall; i++)
        if (fs->all[i].start < cur) cur = fs->all[i].start;

    uint32_t highest = 0;
    for (size_t i = 0; i < fs->nall; i++) {
        uint32_t e = fs->all[i].end ? fs->all[i].end : fs->all[i].start;
        if (e > highest) highest = e;
    }

    for (;;) {
        /* The LAST file whose start equals cur.  The array is sorted by name, and
         * for a shared start that orders unordered-then-narrow-then-widest, so
         * the last is the one that encloses the others (D-5a). */
        ssize_t pick = -1;
        for (size_t i = 0; i < fs->nall; i++)
            if (fs->all[i].start == cur) pick = (ssize_t)i;

        if (pick < 0) break;

        struct zsi_entry *e = &fs->all[pick];
        fs->resolved[fs->nresolved++] = *e;

        uint32_t last = e->end ? e->end : e->start;

        /* D-5c: a PARTIAL overlap, where two ranges intersect and neither
         * contains the other, cannot arise from any legal sequence.  It is
         * corruption and MUST be reported rather than resolved -- resolving it
         * would silently pick one interpretation of a directory that has no
         * correct interpretation. */
        for (size_t i = 0; i < fs->nall; i++) {
            uint32_t s2 = fs->all[i].start;
            uint32_t e2 = fs->all[i].end ? fs->all[i].end : fs->all[i].start;
            if (s2 > cur && s2 <= last && e2 > last) return ZS_BADFORMAT;
        }

        if (last == 0xFFFFFFFFu) { cur = last; break; }   /* no next generation */
        cur = last + 1;
    }

    /* D-6: the set is complete if and only if the scan consumed every generation
     * from the lowest present through the highest.  The resolved ranges therefore
     * TILE a contiguous interval.  This is the whole completeness test -- no
     * sequence number, timestamp or publication record is needed, because tiling
     * means every generation is accounted for and so nothing committed is
     * missing (C-4a). */
    if (fs->nresolved == 0) return ZS_AGAIN;

    struct zsi_entry *lastres = &fs->resolved[fs->nresolved - 1];
    uint32_t reached = lastres->end ? lastres->end : lastres->start;
    if (reached != highest) return ZS_AGAIN;

    return ZS_OK;
}

/* One above the highest generation present in ALL files, not in the resolved set
 * (D-9b).
 *
 * A superseded file still pins its generation: it is only removed once an
 * enclosing file covers it (D-23), so the highest generation present never
 * regresses and a generation can never be reissued.  Computing this from the
 * resolved set instead would let a generation be reused while a superseded file
 * bearing that name still existed.
 *
 * D-9c: allocating past 0xFFFFFFFF fails with ZS_FULL rather than wrapping. */
static int zsi_fileset_next_gen(const struct zsi_fileset *fs, uint32_t *out)
{
    uint32_t highest = 0;

    for (size_t i = 0; i < fs->nall; i++) {
        uint32_t e = fs->all[i].end ? fs->all[i].end : fs->all[i].start;
        if (e > highest) highest = e;
    }

    if (highest == 0xFFFFFFFFu) return ZS_FULL;
    *out = highest + 1;
    return ZS_OK;
}

/********** SNAPSHOT *************/

/* C-4h: a retry happens only when the file set changed during the scan, which is
 * a structural event rather than a per-operation one.  Bounded so a pathological
 * rate of structural change surfaces as ZS_AGAIN instead of a livelock. */
#define ZSI_SNAPSHOT_RETRIES 20

struct zsi_snapshot {
    struct zsi_file **files;    /* the resolved set, sorted by start ASCENDING */
    size_t            nfiles;
    int               refcount;
};

static void zsi_snapshot_release(struct zsi_snapshot **sp)
{
    struct zsi_snapshot *s = *sp;
    if (!s) return;

    if (--s->refcount > 0) { *sp = NULL; return; }

    for (size_t i = 0; i < s->nfiles; i++)
        zsi_file_close(&s->files[i]);
    free(s->files);
    free(s);
    *sp = NULL;
}

/* The active file: the highest-generation UNORDERED file, or NULL if the newest
 * file is in-order.  The only file a writer appends to. */
static struct zsi_file *zsi_snapshot_active(struct zsi_snapshot *s)
{
    if (!s->nfiles) return NULL;

    struct zsi_file *last = s->files[s->nfiles - 1];
    return zsi_file_is_unordered(last) ? last : NULL;
}

/* C-4: take a snapshot.
 *
 *   1. readdir, keeping names matching zeroskip-<uuid>-* and parsing each
 *      generation range from its name;
 *   2. run D-5's scan.  If it leaves a gap the set is incomplete -- a torn
 *      readdir or corruption -- so restart from 1;
 *   3. open and map every file in the resolved set.  If any open fails with
 *      ENOENT, restart from 1;
 *   4. build a private index for each unordered file by replaying its spans,
 *      taking its snapshot boundary to be the end of its last valid span.
 *
 * NO LOCK IS TAKEN AT ANY POINT (C-2).  That is the single most surprising
 * property of this design and the place someone will later be tempted to add one.
 * What makes it safe:
 *
 *   - step 2's tiling check IS the completeness proof (C-4a).  Every generation
 *     in the interval is covered exactly once, so no committed data is missing,
 *     and there is nothing to compare against a published record;
 *   - a retry always converges (C-4b), because both failure modes -- a torn
 *     readdir and a file removed underneath us -- show up AS failures rather than
 *     as a set that looks complete and is not.  Removal is only ever permitted
 *     when the remaining files still tile (D-23);
 *   - everything opened is immutable from here on (C-4c).  In-order files are
 *     never modified; a non-active unordered file is never appended to again; the
 *     active file is appended to but ONLY appended to, so every byte below the
 *     boundary is stable by construction and growth beyond it is never looked at;
 *   - every index is private (C-4d), so there is no shared state to synchronise
 *     against a writer and nothing to clean up when a process dies.
 */
static int zsi_snapshot_take(const char *dir, const zsi_uuid_t *uuid,
                             zs_compar *compar, zs_csum *external_csum,
                             bool nocsum, struct zsi_snapshot **out)
{
    for (int attempt = 0; attempt < ZSI_SNAPSHOT_RETRIES; attempt++) {
        struct zsi_fileset fs;
        int r = zsi_fileset_scan(dir, uuid, &fs);
        if (r != ZS_OK) return r;

        r = zsi_fileset_resolve(&fs);
        if (r == ZS_AGAIN) { zsi_fileset_fini(&fs); continue; }
        if (r != ZS_OK) { zsi_fileset_fini(&fs); return r; }

        struct zsi_snapshot *s = zsi_zmalloc(sizeof(*s));
        if (!s) { zsi_fileset_fini(&fs); return ZS_INTERNAL; }
        s->refcount = 1;

        if (fs.nresolved) {
            s->files = zsi_zmalloc(fs.nresolved * sizeof(*s->files));
            if (!s->files) {
                free(s);
                zsi_fileset_fini(&fs);
                return ZS_INTERNAL;
            }
        }

        bool retry = false;
        for (size_t i = 0; i < fs.nresolved && !retry; i++) {
            struct zsi_file *f = NULL;
            r = zsi_file_open(dir, fs.resolved[i].name, fs.resolved[i].start,
                              external_csum, &f);

            /* A file may legitimately be unlinked between steps 2 and 3, by a
             * packer retiring an input it has already superseded.  That is not an
             * error, it is a stale scan -- restart. */
            if (r == ZS_NOTFOUND) { retry = true; break; }
            if (r != ZS_OK) {
                zsi_snapshot_release(&s);
                zsi_fileset_fini(&fs);
                return r;
            }

            s->files[s->nfiles++] = f;

            /* D-10a: a NON-ACTIVE file with an invalid header is an error -- its
             * records cannot be recovered, and silently skipping the generation
             * would lose committed data.  Only the last file may be the active
             * one, so only it gets D-10's tolerance. */
            bool is_last = (i + 1 == fs.nresolved);
            if (!f->hdr_valid && !is_last) {
                zsi_snapshot_release(&s);
                zsi_fileset_fini(&fs);
                return ZS_BADFORMAT;
            }

            if (zsi_file_is_unordered(f)) {
                /* Step 4.  The replay sets f->complete, which IS this file's
                 * snapshot boundary: growth beyond it is invisible (C-4c). */
                r = zsi_index_build(f, compar, nocsum);
                if (r != ZS_OK) {
                    zsi_snapshot_release(&s);
                    zsi_fileset_fini(&fs);
                    return r;
                }
            } else {
                r = zsi_ptrs_load(f);
                if (r != ZS_OK) {
                    zsi_snapshot_release(&s);
                    zsi_fileset_fini(&fs);
                    return r;
                }
            }
        }

        zsi_fileset_fini(&fs);

        if (retry) { zsi_snapshot_release(&s); continue; }

        *out = s;
        return ZS_OK;
    }

    /* C-4h: bounded rather than spinning, so a pathological rate of structural
     * change is reported instead of hanging. */
    return ZS_AGAIN;
}

/********** FILE LOCKING *************/

/* Three byte-range locks on zeroskip.lock (C-1):
 *
 *   byte 0  write   a write transaction: appending, creating a new active file,
 *                   converting an unordered file
 *   byte 1  repack  a whole repack, possibly long
 *   byte 2  remove  momentarily: verify completeness, then unlink
 *
 * The mechanism is EXACTLY fcntl record locking, and the byte offsets are
 * normative (C-1e).  This is interoperability surface, not an implementation
 * choice: implementations in different languages must exclude each other.
 *
 * NEVER flock.  On Linux flock occupies a separate lock space and does not
 * exclude fcntl, and it is a no-op over some network filesystems.  An
 * implementation using it would pass every single-implementation test and
 * silently fail to exclude a conforming peer -- which is precisely the false pass
 * T-13 exists to catch.
 *
 * There is deliberately NO in-process mutex here, and the absence is the
 * considered position rather than an omission.  fcntl locks are per-process
 * (C-1f), so two handles on one database within a process are excluded by
 * nothing: not by fcntl, which sees one owner, and not by a per-handle mutex,
 * which would be two different objects.  A mutex therefore buys exclusion only
 * between threads sharing a handle, which is not the guarantee anyone reads G-5 as
 * making -- so it would imply a property the format cannot enforce.  Handles are
 * not thread-safe; concurrent writers are separate processes.  An implementation
 * that genuinely needs intra-process exclusion should use F_OFD_SETLK (C-1i),
 * which is scoped to an open file description rather than a process. */

enum zsi_lock { ZSI_LOCK_WRITE = 0, ZSI_LOCK_REPACK = 1, ZSI_LOCK_REMOVE = 2 };
#define ZSI_NLOCKS 3

struct zsi_locks {
    int      fd;            /* exactly one, for the handle's lifetime (C-1g) */
    unsigned held;          /* bitmask of enum zsi_lock */
};

/* Open (creating if absent) the lock file and keep ONE descriptor for the
 * handle's lifetime.
 *
 * C-1g: fcntl locks are released by closing ANY descriptor for the file in that
 * process, so a second open followed by a close would silently drop every lock
 * this handle holds.  There is exactly one, and nothing may open another.
 *
 * D-3a: created with O_CREAT if absent, so an existing database is never
 * unopenable for want of it.  Concurrent creation is harmless -- O_CREAT on one
 * path yields one inode, so every process locks the same object.
 *
 * D-3b: it is never unlinked, by this library or anything else.  Unlinking it
 * while processes hold locks is the one way to break mutual exclusion from
 * outside: holders keep locking the removed inode while a new process creates a
 * fresh one and locks that, so TWO WRITERS each believe they hold the write lock.
 * Worth stating because an empty file named *.lock is exactly what cleanup
 * scripts delete. */
static int zsi_lock_open(struct zsi_locks *lk, const char *dir)
{
    char path[PATH_MAX];

    memset(lk, 0, sizeof(*lk));
    lk->fd = -1;

    snprintf(path, sizeof(path), "%s/%s", dir, ZSI_LOCK_NAME);
    lk->fd = open(path, O_RDWR | O_CREAT, 0600);
    if (lk->fd < 0) return (errno == ENOENT) ? ZS_NOTFOUND : ZS_IOERROR;

    return ZS_OK;
}

static void zsi_lock_close(struct zsi_locks *lk)
{
    if (lk->fd < 0) return;

    /* Closing releases every fcntl lock this process holds on the inode, which is
     * what we want on the way out -- and is why C-1g forbids a second descriptor
     * being closed early. */
    close(lk->fd);
    lk->fd = -1;
    lk->held = 0;
}

static int zsi_lock_fcntl(int fd, enum zsi_lock which, int type, bool block)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = (short)type;
    fl.l_whence = SEEK_SET;
    fl.l_start = (off_t)which;
    fl.l_len = 1;

    for (;;) {
        if (fcntl(fd, block ? F_SETLKW : F_SETLK, &fl) == 0) return ZS_OK;
        if (errno == EINTR) continue;
        if (!block && (errno == EACCES || errno == EAGAIN)) return ZS_LOCKED;
        return ZS_IOERROR;
    }
}

/* Acquire.  Blocking unless ZS_NONBLOCKING, in which case ZS_LOCKED.
 *
 * C-1f: fcntl locks are per-PROCESS, not per-thread -- two threads of one process
 * both acquire the same lock successfully, and the kernel sees a single owner.
 * G-5 therefore requires an in-process mutex as well, held across the same
 * region.  An implementation relying on fcntl alone passes every single-threaded
 * test and corrupts a database the moment two threads write, which is invisible
 * to a single-threaded suite and is why T-14 must be run per implementation.
 *
 * C-1d lock ordering: acquisition is always write -> remove or repack -> remove.
 * Nothing takes write or repack while holding remove, and nothing holds both
 * write and repack, so no cycle exists.  Asserted here, which is sound now that
 * a handle belongs to one thread of control: `held` describes the one actor
 * using it, and a violation deadlocks in production while being trivially visible
 * in development. */
static int zsi_lock_take(struct zsi_locks *lk, enum zsi_lock which, int flags)
{
    bool block = !(flags & ZS_NONBLOCKING);

    assert(lk->fd >= 0);
    assert(!(lk->held & (1u << which)));        /* not already held */

    if (which == ZSI_LOCK_WRITE || which == ZSI_LOCK_REPACK)
        assert(!(lk->held & (1u << ZSI_LOCK_REMOVE)));   /* C-1d */
    if (which == ZSI_LOCK_WRITE)
        assert(!(lk->held & (1u << ZSI_LOCK_REPACK)));
    if (which == ZSI_LOCK_REPACK)
        assert(!(lk->held & (1u << ZSI_LOCK_WRITE)));

    int r = zsi_lock_fcntl(lk->fd, which, F_WRLCK, block);
    if (r != ZS_OK) return r;

    lk->held |= (1u << which);
    return ZS_OK;
}

static int zsi_lock_release(struct zsi_locks *lk, enum zsi_lock which)
{
    if (!(lk->held & (1u << which))) return ZS_OK;

    int r = zsi_lock_fcntl(lk->fd, which, F_UNLCK, true);
    lk->held &= ~(1u << which);

    return r;
}

/********** READ PATH *************/

/********** WRITE PATH *************/

/* The transaction arm of the per-file cursor.
 *
 * A write transaction's uncommitted records are the highest-priority source in
 * D-14's table, and they are presented through the same interface as a file so
 * the merge needs no special case.  Until the transaction type exists these
 * report an exhausted source, which is exactly right for a read transaction: it
 * has no pending records, so the arm is not merely a placeholder. */
static int zsi_txn_cur_load(struct zsi_fcur *fc)
{
    if (!fc->txn) { fc->exhausted = true; return ZS_OK; }
    fc->exhausted = true;
    return ZS_OK;
}

static void zsi_txn_cur_seek(struct zsi_fcur *fc, const char *key, size_t keylen)
{
    (void)key;
    (void)keylen;
    fc->ti = 0;
}

/********** CONVERSION *************/

/********** REPACK *************/

/********** CONSISTENCY *************/

/********** OPEN AND CLOSE *************/

/* OPENING IS RECOVERY; there is no separate pass (section 7).
 *
 * R-1: scan the directory, resolve enclosures, check the tiling, map the files,
 * then replay each unordered file's spans from its start -- building the private
 * index as it goes and stopping at the first record or terminator that fails to
 * validate, which establishes that file's end.  All of that is C-4, which the
 * snapshot already does, so open is mostly configuration plus one snapshot.
 *
 * R-4: there is no in-place repair.  A file that is not clean is simply complete
 * at its last valid span, and the writer moves to a new generation.  Nothing is
 * ever appended past a boundary that failed to validate, so a spurious terminator
 * in trailing garbage -- which a checksum can never wholly exclude -- cannot
 * become the foundation of a later chain.  Generations are cheap. */

struct zs_db {
    char        *dir;
    uint32_t     flags;
    zsi_uuid_t   uuid;
    bool         have_uuid;

    zs_compar   *compar;
    char         compar_name[ZSI_COMPAR_NAME_LEN];
    zs_csum     *external_csum;
    unsigned     create_csum_id;    /* engine for files THIS handle creates */
    size_t       rollover_size;
    void       (*error)(const char *msg, const char *fmt, ...);

    bool         readonly;          /* ZS_SHARED (A-5) */
    bool         nocsum;            /* ZS_NOCSUM (F-5e) */
    bool         nosync;            /* ZS_NOSYNC (C-7c) */
    bool         nonblocking;

    struct zsi_locks     locks;
    struct zsi_snapshot *snap;      /* current snapshot for zs_db_* calls */
    struct zs_txn       *write_txn;

    /* Backing for pointers returned by the non-transactional calls, whose
     * implicit transaction has ended by the time the caller sees them (A-4). */
    char        *retbuf;
    size_t       retalloc;
};

static void zsi_default_error(const char *msg, const char *fmt, ...)
{
    (void)msg;
    (void)fmt;
}

/* Every mutating internal entry point starts here rather than scattering the
 * check.  R-3: a reader MUST NOT write, and opening a damaged database read-only
 * is side-effect-free -- no conversion, no repack, no new active file, no
 * removal.  There is no shared cache for it to update either (D-13c). */
static int zsi_check_writable(struct zs_db *db)
{
    return db->readonly ? ZS_READONLY : ZS_OK;
}

/* Create generation 1: a 72-byte header and no spans, which F-26h makes a legal
 * empty file (D-8a).
 *
 * C-6: after creating a DATA FILE the directory is fdatasync'd, otherwise the
 * name may be absent after a crash even though the file's contents are durable. */
static int zsi_create_active(struct zs_db *db, uint32_t gen)
{
    char name[ZSI_NAME_MAX], path[PATH_MAX], hdr[ZSI_HEADER_LEN];
    struct zsi_header h;

    memset(&h, 0, sizeof(h));
    h.version_read = ZSI_VERSION_READ;
    h.version_write = ZSI_VERSION_WRITE;
    h.flags = (uint16_t)db->create_csum_id;
    memcpy(h.uuid, db->uuid, 16);
    h.start = gen;
    h.end = 0;
    memcpy(h.compar_name, db->compar_name, ZSI_COMPAR_NAME_LEN);

    zs_csum *cs = zsi_csum_for_id(db->create_csum_id, db->external_csum);
    if (!cs) return ZS_BADUSAGE;
    zsi_header_encode(hdr, &h, cs);

    zsi_name_format(name, db->uuid, gen, 0);
    snprintf(path, sizeof(path), "%s/%s", db->dir, name);

    /* O_EXCL: creating a file IS publishing it (D-8), so a collision would mean
     * another writer allocated the same generation -- which D-9b makes
     * impossible, and which must therefore fail loudly rather than truncate. */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return ZS_IOERROR;

    ssize_t n = write(fd, hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) { close(fd); return ZS_IOERROR; }
    if (!db->nosync && fdatasync(fd) < 0) { close(fd); return ZS_IOERROR; }
    close(fd);

    if (!db->nosync) {
        int dfd = open(db->dir, O_RDONLY);
        if (dfd >= 0) { fdatasync(dfd); close(dfd); }
    }

    return ZS_OK;
}

/* Refresh db->snap.  Used by open and by every zs_db_* call that needs a current
 * view; a transaction holds its own snapshot for its lifetime. */
static int zsi_db_refresh(struct zs_db *db)
{
    struct zsi_snapshot *s = NULL;
    int r = zsi_snapshot_take(db->dir, db->have_uuid ? &db->uuid : NULL,
                              db->compar, db->external_csum, db->nocsum, &s);
    if (r != ZS_OK) return r;

    zsi_snapshot_release(&db->snap);
    db->snap = s;

    if (!db->have_uuid && s->nfiles) {
        memcpy(db->uuid, s->files[0]->hdr.uuid, 16);
        db->have_uuid = true;
    }

    return ZS_OK;
}

/* Every file of a database MUST carry the same UUID and the same comparator name
 * (F-11).  The comparator determines key order and hence the meaning of the
 * pointer section, so it is recorded per file -- there is no manifest to hold it.
 * Opening a database whose files disagree, or whose comparator differs from the
 * caller's, is an error rather than something to reconcile: reading a file whose
 * pointer array was built under a different order silently returns wrong
 * answers. */
static int zsi_db_check_agreement(struct zs_db *db)
{
    for (size_t i = 0; i < db->snap->nfiles; i++) {
        struct zsi_file *f = db->snap->files[i];

        /* A-6 first, because it applies to files whose header did not validate:
         * unverifiable for want of the caller's engine is a usage error, and must
         * not be swallowed by D-10's tolerance of a corrupt ACTIVE file. */
        if (f->needs_external_csum) return ZS_BADUSAGE;

        if (!f->hdr_valid) continue;        /* the D-10 active-file case */

        if (memcmp(f->hdr.uuid, db->uuid, 16) != 0) return ZS_BADFORMAT;
        if (memcmp(f->hdr.compar_name, db->compar_name,
                   ZSI_COMPAR_NAME_LEN) != 0)
            return ZS_BADFORMAT;

        /* A-6: a file recording engine 2 is readable only by a caller supplying
         * the same function.  zsi_file_open leaves such a header invalid when it
         * cannot resolve the engine, so this catches the case where the file is
         * otherwise fine. */
        if ((f->hdr.flags & ZSI_CSUM_MASK) == ZSI_CSUM_EXTERNAL
            && !db->external_csum)
            return ZS_BADUSAGE;

        /* F-7: refuse to WRITE above our write version, while still allowing the
         * file to be read.  The read gate is in zsi_header_decode. */
        if (!db->readonly && f->hdr.version_write > ZSI_VERSION_WRITE)
            return ZS_READONLY;
    }

    return ZS_OK;
}

static int zsi_db_open(const char *dir, struct zs_open_data *setup,
                       const char *uuid_str, struct zs_db **dbp)
{
    struct zs_open_data defaults = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db;
    int r;

    if (!dir || !dbp) return ZS_BADUSAGE;
    if (!setup) setup = &defaults;
    *dbp = NULL;

    db = zsi_zmalloc(sizeof(*db));
    if (!db) return ZS_INTERNAL;

    db->locks.fd = -1;
    db->flags = setup->flags;
    db->readonly = (setup->flags & ZS_SHARED) != 0;
    db->nocsum = (setup->flags & ZS_NOCSUM) != 0;
    db->nosync = (setup->flags & ZS_NOSYNC) != 0;
    db->nonblocking = (setup->flags & ZS_NONBLOCKING) != 0;
    db->rollover_size = setup->rollover_size ? setup->rollover_size
                                             : ZSI_DEFAULT_ROLLOVER;
    db->error = setup->error ? setup->error : zsi_default_error;
    db->external_csum = setup->csum;
    db->create_csum_id = zsi_csum_id_for_flags(setup->flags);

    /* A-6: engine 2 needs a function, or files this handle creates cannot be
     * verified by anyone including us. */
    if (db->create_csum_id == ZSI_CSUM_EXTERNAL && !db->external_csum) {
        free(db);
        return ZS_BADUSAGE;
    }

    /* F-11a/F-11b: the default comparator is named "memcmp"; a caller supplying
     * its own MUST supply a name, and names are compared byte for byte. */
    if (setup->compar) {
        if (!zsi_compar_name_valid(setup->compar_name)) {
            free(db);
            return ZS_BADUSAGE;
        }
        db->compar = setup->compar;
        snprintf(db->compar_name, sizeof(db->compar_name) + 0, "%s",
                 setup->compar_name);
        /* snprintf NUL-terminates within 16; the field is NUL-PADDED, and
         * zsi_zmalloc already zeroed it, so the padding is correct. */
    } else {
        db->compar = zsi_compar_default;
        memcpy(db->compar_name, "memcmp", 6);
    }

    db->dir = strdup(dir);
    if (!db->dir) { free(db); return ZS_INTERNAL; }

    /* Is there anything here?  A directory that does not exist, or holds no data
     * files, is the empty case D-8a handles. */
    struct zsi_fileset probe;
    r = zsi_fileset_scan(dir, NULL, &probe);
    bool empty = (r == ZS_NOTFOUND) || (r == ZS_OK && probe.nall == 0);
    bool discovered = (r == ZS_OK && probe.nall > 0);

    if (discovered) {
        memcpy(db->uuid, probe.uuid, 16);
        db->have_uuid = true;
    }
    if (r == ZS_OK) zsi_fileset_fini(&probe);
    else if (r != ZS_NOTFOUND) { zs_db_close(&db); return r; }

    if (empty) {
        if (!(setup->flags & ZS_CREATE)) { zs_db_close(&db); return ZS_NOTFOUND; }
        if (db->readonly) { zs_db_close(&db); return ZS_READONLY; }

        if (mkdir(dir, 0700) && errno != EEXIST) { zs_db_close(&db); return ZS_IOERROR; }

        if (uuid_str) {
            if (zsi_uuid_parse(uuid_str, db->uuid) != 0) {
                zs_db_close(&db);
                return ZS_BADUSAGE;
            }
        } else {
            zsi_uuid_generate(db->uuid);
        }
        db->have_uuid = true;
    }

    /* D-3a: the lock file is created with the database, and created on open if
     * absent, so an existing database is never unopenable for want of it.
     *
     * A read-only handle skips it entirely.  Readers take no lock (C-2), so they
     * have no use for it -- and creating it would be a write, which R-3 forbids:
     * opening a damaged database read-only must be side-effect-free, and "it only
     * creates one small file" is exactly the kind of exception that makes a
     * read-only mount fail or a forensic copy differ from its original. */
    if (!db->readonly) {
        r = zsi_lock_open(&db->locks, dir);
        if (r != ZS_OK) { zs_db_close(&db); return r; }
    }

    if (empty) {
        r = zsi_create_active(db, 1);
        if (r != ZS_OK) { zs_db_close(&db); return r; }
    }

    /* R-1: the snapshot IS the recovery pass. */
    r = zsi_db_refresh(db);
    if (r != ZS_OK) { zs_db_close(&db); return r; }

    r = zsi_db_check_agreement(db);
    if (r != ZS_OK) { zs_db_close(&db); return r; }

    *dbp = db;
    return ZS_OK;
}

int zs_db_open(const char *dir, struct zs_open_data *setup, struct zs_db **dbp)
{
    return zsi_db_open(dir, setup, NULL, dbp);
}

int zs_db_open_with_uuid(const char *dir, struct zs_open_data *setup,
                         const char *uuid_str, struct zs_db **dbp)
{
    return zsi_db_open(dir, setup, uuid_str, dbp);
}

int zs_db_close(struct zs_db **dbp)
{
    struct zs_db *db = *dbp;
    if (!db) return ZS_OK;

    zsi_snapshot_release(&db->snap);
    zsi_lock_close(&db->locks);
    free(db->retbuf);
    free(db->dir);
    free(db);
    *dbp = NULL;

    return ZS_OK;
}

/********** PUBLIC API *************/

const char *zs_strerror(int r)
{
    switch (r) {
    case ZS_OK:           return "success";
    case ZS_DONE:         return "iteration complete";
    case ZS_EXISTS:       return "record already exists";
    case ZS_IOERROR:      return "I/O error";
    case ZS_INTERNAL:     return "internal error";
    case ZS_LOCKED:       return "database is locked";
    case ZS_NOTFOUND:     return "record not found";
    case ZS_READONLY:     return "database is read-only";
    case ZS_BADFORMAT:    return "bad file format";
    case ZS_BADUSAGE:     return "bad usage";
    case ZS_BADCHECKSUM:  return "checksum mismatch";
    case ZS_FULL:         return "generation space exhausted";
    case ZS_AGAIN:        return "file set changed, retry";
    }

    return "unknown error";
}
