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
#define XXH_NO_INLINE_HINTS 1   /* callers without the SSE2 target attribute
                                   cannot inline always_inline SSE2 helpers */
#include "xxhash.h"

/* Syscall interposition for crash and sync-failure injection (T-8, T-8a).
 *
 * A test build routes the four syscalls that can leave a database in an
 * intermediate state through function pointers, so a test can count them and fail
 * or abort at call N.  Compiled out entirely otherwise -- ZS_TEST_HOOKS is defined
 * only by the crash-test target.
 *
 * Function pointers rather than LD_PRELOAD because LD_PRELOAD does not exist on
 * macOS, and rather than weak symbols because those interpose the whole process
 * including the harness's own writes.  This way only the library's calls are
 * counted, which is what makes "abort at call N" reproducible. */
#ifdef ZS_TEST_HOOKS
ssize_t (*zs_hook_write)(int, const void *, size_t) = NULL;
int     (*zs_hook_fdatasync)(int) = NULL;
int     (*zs_hook_rename)(const char *, const char *) = NULL;
int     (*zs_hook_unlink)(const char *) = NULL;

#define ZS_WRITE(fd, buf, n)  (zs_hook_write ? zs_hook_write((fd), (buf), (n)) \
                                             : write((fd), (buf), (n)))
#define ZS_FDATASYNC(fd)      (zs_hook_fdatasync ? zs_hook_fdatasync(fd) \
                                                 : fdatasync(fd))
#define ZS_RENAME(a, b)       (zs_hook_rename ? zs_hook_rename((a), (b)) \
                                              : rename((a), (b)))
#define ZS_UNLINK(p)          (zs_hook_unlink ? zs_hook_unlink(p) : unlink(p))

/* A pause point between C-4's step 2 (resolve) and step 3 (open), so a test can
 * force the interleaving C-4b is about -- a file removed after the scan saw it.
 * Racing for that window does not reliably hit it: a first attempt raced 300 times
 * and never once took the retry path, which is a test that reads as coverage
 * without providing any. */
void (*zs_hook_snapshot_gap)(const char *dir) = NULL;
#define ZS_SNAPSHOT_GAP(dir)  do { if (zs_hook_snapshot_gap) \
                                       zs_hook_snapshot_gap(dir); } while (0)
#else
#define ZS_WRITE(fd, buf, n)  write((fd), (buf), (n))
#define ZS_FDATASYNC(fd)      fdatasync(fd)
#define ZS_RENAME(a, b)       rename((a), (b))
#define ZS_UNLINK(p)          unlink(p)
#define ZS_SNAPSHOT_GAP(dir)  do { } while (0)
#endif

/********** TUNING *************/

/* A writer moves to a new file when the active file exceeds this (D-9a).
 * It bounds two other things as a side effect: how much a snapshot must replay
 * to build a private index (D-13d), and how much work one conversion does
 * (D-12d). */
#define ZSI_DEFAULT_ROLLOVER (2 * 1024 * 1024)

/* How far past the last published pointer table the active file may grow before
 * another is published (P-13).  Measured, not guessed -- zsbench's "publish
 * threshold" table, 16000 single-store transactions over a 2MB active file:
 *
 *     no cache  26.8s      every commit's snapshot refresh replays the whole file
 *     1          6.5s      the whole table rewritten on every commit
 *     4096       2.7s   <- flat minimum
 *     32768      2.8s   <-
 *     262144     6.5s      refresh replay growing again
 *     1048576   18.6s
 *
 * BOTH ends cost, and the expensive one is the high end.  A write transaction
 * refreshes its snapshot at begin (C-4), and that refresh replays from the last
 * published point -- so a threshold that publishes too rarely leaves the replay
 * unbounded and a one-store-per-transaction load quadratic.  Publishing too
 * often costs an O(records) merge and rewrite per commit, which is real but
 * cheaper, because those writes are never synced (P-14).
 *
 * An absolute byte count rather than a fraction of rollover_size, because the
 * knee is set by how much data a replay has to walk, which has nothing to do
 * with how large the caller lets a generation grow. */
#define ZSI_DEFAULT_INDEX_THRESHOLD (32 * 1024)

/* The streaming writer's append buffer (C-8): stores batch into it, so a
 * store is a memcpy rather than a syscall, and it flushes whenever a read
 * needs bytes it still holds and at every terminator. */
#define ZSI_TXN_CHUNK (64 * 1024)

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
#define ZSI_CACHE_DIR_NAME  "zeroskip.cache"   /* P-2b, in the metadata namespace (D-2) */

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
#define ZSI_VERSION_READ  2
#define ZSI_VERSION_WRITE 2

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

    /* Version 1 was never released (F-7): records below version 2 carry no
     * checksum, and pretending to read them would serve unverifiable data.
     * The clean break is spec'd, not an accident of this reader. */
    if (vread < 2) return ZS_BADFORMAT;

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
    const char *base;   /* record start; csum covers [base, base+len-4) */
    uint32_t    csum;    /* the stored trailing checksum (F-32) */
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
 * (F-12c).  In the short forms it adds 4 bytes.
 *
 * The body also carries the record's own trailing 4-byte checksum (F-32),
 * inside the roundup: it is the last 4 bytes of the padded record, not an
 * addition on top of it. */
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
        /* key NUL, no value at all, F-32's trailing checksum */
        if (!zsi_add_sz(keylen, 1 + 4, &body)) return 0;
    } else {
        hdr = big ? ZSI_HDRLEN_BIGKEYVALUE
                  : (store_ancestor ? ZSI_HDRLEN_KEYVALUE_ANC
                                    : ZSI_HDRLEN_KEYVALUE);
        /* key NUL value NUL (F-13: stored lengths exclude the terminators),
         * F-32's trailing checksum */
        if (!zsi_add3_sz(keylen, vallen, 2 + 4, &body)) return 0;
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
 * reads back through the decoder.
 *
 * csum is the CONTAINING FILE's engine function (A-6/F-5a), not the handle's --
 * the writer path already holds the right one for span checksums. */
static void zsi_rec_encode(char *buf, zs_csum *csum, const char *key,
                           size_t keylen, const char *val, size_t vallen,
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

    /* F-32: the trailing checksum, over everything before it -- fixed
     * fields, key, value, NULs, and the padding.  The engine is the
     * CONTAINING FILE's (F-5a), which is why it is a parameter: the
     * handle's engine is the same trap here as in A-6. */
    zsi_put32(buf + total - 4, csum(buf, total - 4));
}

/* Decode the data record at buf[0..len), resolving its ancestor against the
 * containing file's start generation (F-17).
 *
 * Every record now carries its own trailing checksum (F-32), but it is
 * deliberately NOT verified here.  The span terminator's checksum is what
 * covers replay (F-19); verifying a record's own checksum during replay
 * would put the check inside F-24's completion logic, where a single
 * flipped byte would cost every record after it rather than just the one
 * (F-32b).  What is checked here is structure: the type byte, every length
 * bounded and overflow-free, and the total within len.  The checksum itself
 * is read out into out->csum, alongside out->base, for the caller to verify
 * on demand at materialization (F-32a).
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
     * wraps.  The +4 is F-32's trailing checksum, counted here exactly as
     * zsi_rec_encoded_len counts it. */
    size_t body, total;
    if (isdelete) {
        if (!zsi_add_sz(keylen, 1 + 4, &body)) return ZS_BADFORMAT;
    } else {
        if (!zsi_add3_sz(keylen, vallen, 2 + 4, &body)) return ZS_BADFORMAT;
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
    out->base     = buf;
    out->csum     = zsi_get32(buf + total - 4);

    if (isdelete) {
        out->val    = NULL;
        out->vallen = 0;
    } else {
        out->val    = buf + hdr + keylen + 1;
        out->vallen = vallen;
    }

    return ZS_OK;
}

/* Verify a record's trailing checksum (F-32a).
 *
 * The engine is the caller's to supply, because a record does not know its
 * file -- and it is always the CONTAINING file's engine (F-5a).  Engine 0
 * computes zero over anything and stores zero, so it passes with no special
 * case.
 *
 * Callers are the MATERIALIZATION points only: the lookup return, the cursor
 * yield, check_consistency, salvage.  Wiring this into decode or replay is
 * the data-loss bug F-32b names: replay completes a file at its first invalid
 * record (F-24), so a verifying replay converts one flipped byte into the
 * silent loss of every record after it.
 *
 * Only records DECODED FROM A FILE qualify.  The transaction arm synthesizes
 * records over its pending buffers with base == NULL and no checksum, and the
 * merge gates on the arm's file pointer before calling here. */
static int zsi_rec_verify(zs_csum *csum, const struct zsi_rec *r)
{
    if (r->len < 4) return ZS_BADFORMAT;
    if (csum(r->base, r->len - 4) != r->csum) return ZS_BADCHECKSUM;
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

    /* The span boundary at `complete`, and the checksum the terminator ending
     * there carries (P-10).  Recorded by the replay so publishing a pointer
     * table does not need a second walk -- terminators are only ever found by
     * scanning forward (F-20), so there is no cheap way to recover them later. */
    size_t            last_term_off;
    uint32_t          last_term_csum;

    /* The valid_upto of the pointer table this file's index was seeded from, or
     * ZSI_HEADER_LEN if none was.  P-13 measures the publishing threshold from
     * here. */
    size_t            cached_upto;

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

/* Re-stat and re-map after appending, so bytes written through a separate
 * descriptor become visible through this object.
 *
 * Needed because a writer maintains the active file's index incrementally
 * (D-13b) and the index holds offsets that must be readable.  Only ever called
 * on a file nobody else is reading -- see the refcount guard at the call site --
 * because replacing a mapping under a live reader is exactly what G-6 forbids. */
static int zsi_file_remap(struct zsi_file *f)
{
    struct stat sb;

    if (fstat(f->fd, &sb) < 0) return ZS_IOERROR;
    if ((size_t)sb.st_size == f->size) return ZS_OK;

    if (f->base) { munmap((void *)f->base, f->maplen); f->base = NULL; f->maplen = 0; }
    f->size = (size_t)sb.st_size;

    if (f->size) {
        void *m = mmap(NULL, f->size, PROT_READ, MAP_SHARED, f->fd, 0);
        if (m == MAP_FAILED) { f->size = 0; return ZS_IOERROR; }
        f->base = (const char *)m;
        f->maplen = f->size;
    }

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
static int zsi_unordered_replay(struct zsi_file *f, size_t from, bool nocsum,
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

    /* P-12: a span is self-delimiting and self-validating, so a walk may begin
     * at ANY span boundary.  A caller seeding an index from a pointer table
     * passes that table's valid_upto; everyone else passes ZSI_HEADER_LEN.
     *
     * An out-of-range or non-boundary value is not trusted and is not an error:
     * the walk simply finds no valid span there and reports the file complete at
     * that point, which is F-24's ordinary outcome rather than a special case.
     * That is what makes it safe to hand this a value read out of a file. */
    size_t pos = from < ZSI_HEADER_LEN ? ZSI_HEADER_LEN : from;
    if (pos > f->size) pos = ZSI_HEADER_LEN;

    f->complete = pos;

    /* Recorded so a publisher has the boundary's terminator without a second
     * walk (P-10).  A caller seeding from a table overwrites these when the
     * walk finds nothing new -- see zsi_index_build_cached. */
    f->last_term_off  = pos;
    f->last_term_csum = 0;

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
        f->last_term_off  = p;
        f->last_term_csum = term.csum;
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

/* Folding a newly known record into an index; defined below, forward-declared
 * because the seeded build path uses it and it is easier to read after the
 * lookup and traversal it shares its invariants with. */
static int zsi_index_insert(struct zsi_index *ix, zs_compar *compar, size_t off);

/* Build the private index for an unordered file by replaying its spans,
 * optionally SEEDED.
 *
 * It reflects COMMITTED spans only, and for each key only its newest committed
 * record (D-13a).  Building it means replaying spans and skipping rolled-back
 * ones -- never simply walking every record, which would resurrect aborted
 * writes.  That is why this goes through zsi_unordered_replay rather than
 * scanning the file directly, and why test_index_committed_only exists.
 *
 * base/nbase, when given, is an already-sorted, already-deduplicated array of
 * record offsets covering everything committed below `from` -- exactly what a
 * pointer table holds (P-9).  Ownership transfers to the index, INCLUDING on
 * every failure path, so a caller that has just loaded a table never has to
 * decide who frees it.  The replay then starts at `from` and the suffix goes in
 * one record at a time through zsi_index_insert, which is the same path a writer
 * uses at commit (D-13b): one insertion path rather than two.
 *
 * With base NULL and from ZSI_HEADER_LEN this is the plain full build.
 *
 * Sets f->complete as a side effect, since the replay establishes it. */
static int zsi_index_build_from(struct zsi_file *f, zs_compar *compar,
                                bool nocsum, size_t *base, size_t nbase,
                                size_t from)
{
    struct zsi_index_build b;
    struct zsi_index *ix;
    int r;

    zsi_index_free(&f->index);

    memset(&b, 0, sizeof(b));
    r = zsi_unordered_replay(f, from, nocsum, zsi_index_build_cb, &b);
    if (r != ZS_OK) { free(b.offs); free(base); return r; }
    if (b.oom) { free(b.offs); free(base); return ZS_INTERNAL; }

    ix = zsi_zmalloc(sizeof(*ix));
    if (!ix) { free(b.offs); free(base); return ZS_INTERNAL; }
    ix->file = f;

    if (base) {
        ix->base = base;
        ix->nbase = nbase;
        f->index = ix;

        for (size_t i = 0; i < b.n; i++) {
            r = zsi_index_insert(ix, compar, b.offs[i]);
            if (r != ZS_OK) {
                free(b.offs);
                zsi_index_free(&f->index);
                return r;
            }
        }

        free(b.offs);
        return ZS_OK;
    }

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

/* The plain full build: no seed, from the top of the records region. */
static int zsi_index_build(struct zsi_file *f, zs_compar *compar, bool nocsum)
{
    f->cached_upto = ZSI_HEADER_LEN;
    return zsi_index_build_from(f, compar, nocsum, NULL, 0, ZSI_HEADER_LEN);
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

/* Reverse twins (D-14k).  A reverse position is a COUNT: the candidate on
 * each side is [pos-1], and 0 means that side is spent -- nothing underflows.
 * The tie rules are the forward ones verbatim: equal keys prefer the delta
 * (newer), and stepping past a tie consumes BOTH sides, or the base's stale
 * copy would surface on the following step and break D-14h one level down. */
static void zsi_index_cur_seek_last(struct zsi_index *ix,
                                    struct zsi_index_cur *c)
{
    c->bi = ix->nbase;
    c->di = ix->ndelta;
}

/* Position on the largest key <= (inclusive) or < (exclusive) the given key.
 * A lower bound counts the elements strictly below the key, which IS the
 * exclusive reverse position; inclusive adds the exact match when present. */
static void zsi_index_cur_seek_rev(struct zsi_index *ix, zs_compar *compar,
                                   const char *key, size_t keylen,
                                   bool inclusive, struct zsi_index_cur *c)
{
    struct zsi_ksort ks = { ix->file, compar };

    c->bi = zsi_index_lb(ix->base, ix->nbase, &ks, key, keylen);
    if (inclusive && zsi_index_eq(ix->base, ix->nbase, c->bi, &ks, key, keylen))
        c->bi++;
    c->di = zsi_index_lb(ix->delta, ix->ndelta, &ks, key, keylen);
    if (inclusive && zsi_index_eq(ix->delta, ix->ndelta, c->di, &ks, key, keylen))
        c->di++;
}

static int zsi_index_cur_get_rev(struct zsi_index *ix, zs_compar *compar,
                                 struct zsi_index_cur *c,
                                 struct zsi_rec *out, size_t *off)
{
    bool have_b = c->bi > 0;
    bool have_d = c->di > 0;
    size_t chosen;

    if (!have_b && !have_d) return ZS_DONE;

    if (have_b && have_d) {
        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[c->bi - 1], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[c->di - 1], &kd, &ld);
        /* The LARGER key is next; equal keys prefer the delta, the newer. */
        chosen = (compar(kd, ld, kb, lb) >= 0) ? ix->delta[c->di - 1]
                                              : ix->base[c->bi - 1];
    } else {
        chosen = have_d ? ix->delta[c->di - 1] : ix->base[c->bi - 1];
    }

    const char *b = zsi_file_at(ix->file, chosen, 1);
    if (!b) return ZS_DONE;
    if (zsi_rec_decode(b, ix->file->size - chosen, ix->file->hdr.start, out)
        != ZS_OK)
        return ZS_DONE;

    if (off) *off = chosen;
    return ZS_OK;
}

static void zsi_index_cur_prev(struct zsi_index *ix, zs_compar *compar,
                               struct zsi_index_cur *c)
{
    bool have_b = c->bi > 0;
    bool have_d = c->di > 0;

    if (!have_b && !have_d) return;

    if (have_b && have_d) {
        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[c->bi - 1], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[c->di - 1], &kd, &ld);
        int cmp = compar(kd, ld, kb, lb);

        if (cmp == 0) {
            /* Step past BOTH -- the D-14h reason zsi_index_cur_next gives. */
            c->di--;
            c->bi--;
        } else if (cmp > 0) {
            c->di--;
        } else {
            c->bi--;
        }
        return;
    }

    if (have_d) c->di--;
    else        c->bi--;
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

/* The merged base+delta ordering as one freshly allocated array (P-9).
 *
 * Deliberately does NOT mutate the index, even though merging in place would be
 * cheaper and would compact it as a useful side effect.  An index may be shared
 * with a live struct zsi_index_cur holding positions into BOTH arrays, and
 * rewriting them underneath it is exactly the in-place mutation of something a
 * reader is reading that G-6 forbids.  The only caller publishes a pointer
 * table, which is not worth that hazard. */
static int zsi_index_flatten(struct zsi_index *ix, zs_compar *compar,
                             size_t **out, size_t *nout)
{
    size_t total;

    if (!zsi_add_sz(ix->nbase, ix->ndelta, &total)) return ZS_INTERNAL;

    size_t *merged = malloc(total ? total * sizeof(*merged) : 1);
    if (!merged) return ZS_INTERNAL;

    size_t bi = 0, di = 0, w = 0;
    while (bi < ix->nbase || di < ix->ndelta) {
        if (bi >= ix->nbase) { merged[w++] = ix->delta[di++]; continue; }
        if (di >= ix->ndelta) { merged[w++] = ix->base[bi++]; continue; }

        const char *kb, *kd;
        size_t lb, ld;
        zsi_index_key_at(ix->file, ix->base[bi], &kb, &lb);
        zsi_index_key_at(ix->file, ix->delta[di], &kd, &ld);
        int cmp = compar(kd, ld, kb, lb);

        /* Delta wins ties: it is the newer record (D-14). */
        if (cmp == 0)      { merged[w++] = ix->delta[di++]; bi++; }
        else if (cmp < 0)  { merged[w++] = ix->delta[di++]; }
        else               { merged[w++] = ix->base[bi++]; }
    }

    *out = merged;
    *nout = w;
    return ZS_OK;
}

/********** POINTER TABLE CACHE *************/

/* Spec section 8.  An unordered file has no pointer section, so key order for it
 * is derived by replaying its spans (D-13a) -- bounded by rollover_size but paid
 * on every open.  A POINTER TABLE is that replay's result, published into a
 * cache directory the CALLER names, so another process can load it and replay
 * only the suffix beyond it.
 *
 * Three properties keep this from weakening anything:
 *
 *   - the cache directory is NOT the database (P-2), so R-3 still holds.  A
 *     reader writes nothing the database's correctness depends on, which is why
 *     a read-only handle may publish;
 *   - a table is published by rename and never modified (P-4), so G-6's "nothing
 *     a reader may be reading is ever rewritten beneath it" is unchanged;
 *   - a table is self-validating, and EVERY failure means "ignore it and replay"
 *     (P-11).  Nothing here can turn a readable database into an unreadable one.
 *
 * That last one is why nothing in this section reports corruption.  A rejected
 * table is ZS_NOTFOUND, identical to no table at all.
 *
 * Layering: this section may call upwards into PRIVATE INDEX and FILE OBJECT.
 * SNAPSHOT and WRITE PATH call into it. */

/* The same construction as the data-file magic (F-6), for the same reasons --
 * high bit set so no text file is mistaken for one and an eighth-bit-stripping
 * transfer is detected, invalid UTF-8 at byte 0, a CR-LF trap, a DOS
 * end-of-file, a bare LF, NUL padding -- and DELIBERATELY different bytes, so a
 * table and a data file are distinguishable by content as well as by name.
 * A reader validates all 16 (P-6). */
#define ZSI_IDX_MAGIC_LEN 16

static const unsigned char zsi_idx_magic[ZSI_IDX_MAGIC_LEN] = {
    0x89, 0x7A, 0x73, 0x69, 0x6E, 0x64, 0x65, 0x78,
    0x31, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00
};

/* Field offsets within the 96-byte header (P-5).  Spelled out rather than
 * derived by summing sizes, so the table in the spec maps to a table here. */
#define ZSI_IDX_HEADER_LEN        96
#define ZSI_IDX_OFF_MAGIC          0   /* 16 */
#define ZSI_IDX_OFF_VREAD         16   /*  1 */
#define ZSI_IDX_OFF_VWRITE        17   /*  1 */
#define ZSI_IDX_OFF_FLAGS         18   /*  2 */
#define ZSI_IDX_OFF_RESERVED1     20   /*  4 */
#define ZSI_IDX_OFF_UUID          24   /* 16 */
#define ZSI_IDX_OFF_START         40   /*  4 */
#define ZSI_IDX_OFF_RESERVED2     44   /*  4 */
#define ZSI_IDX_OFF_COMPAR        48   /* 16 */
#define ZSI_IDX_OFF_VALID_UPTO    64   /*  8 */
#define ZSI_IDX_OFF_TERM_OFF      72   /*  8 */
#define ZSI_IDX_OFF_NPTRS         80   /*  8 */
#define ZSI_IDX_OFF_TERM_CSUM     88   /*  4 */
#define ZSI_IDX_OFF_CSUM          92   /*  4, covers [0, 92) */

#define ZSI_IDX_VERSION_READ  1
#define ZSI_IDX_VERSION_WRITE 1

/* Bit 4 of flags: the index was built with checksum verification.  A table built
 * under ZS_NOCSUM may hold records a verifying reader would reject, so it must
 * not be handed to one (P-11).  The low 4 bits are the engine, exactly as in a
 * data file header. */
#define ZSI_IDX_FLAG_CSUM_VERIFIED 0x0010

/* A ceiling on a table's size, so a corrupt nptrs cannot become a huge
 * allocation before the exact-size check runs.  Deliberately generous rather
 * than tight: rollover_size is caller-configurable and a crash can leave a
 * larger file behind, and this is a sanity bound, not a policy. */
#define ZSI_IDX_MAX_BYTES ((size_t)1 << 31)

struct zsi_idxhdr {
    uint8_t     version_read;
    uint8_t     version_write;
    uint16_t    flags;
    zsi_uuid_t  uuid;
    uint32_t    start;
    char        compar_name[ZSI_COMPAR_NAME_LEN];  /* NUL-padded, as in a file */
    uint64_t    valid_upto;
    uint64_t    term_off;
    uint64_t    nptrs;
    uint32_t    term_csum;
};

static void zsi_idxhdr_encode(char *buf, const struct zsi_idxhdr *h,
                              zs_csum *csum)
{
    memset(buf, 0, ZSI_IDX_HEADER_LEN);

    memcpy(buf + ZSI_IDX_OFF_MAGIC, zsi_idx_magic, ZSI_IDX_MAGIC_LEN);
    buf[ZSI_IDX_OFF_VREAD]  = (char)h->version_read;
    buf[ZSI_IDX_OFF_VWRITE] = (char)h->version_write;
    zsi_put16(buf + ZSI_IDX_OFF_FLAGS, h->flags);
    /* RESERVED1 and RESERVED2 stay zero: written as zero, ignored on read.  The
     * memset above is what writes them. */
    memcpy(buf + ZSI_IDX_OFF_UUID, h->uuid, 16);
    zsi_put32(buf + ZSI_IDX_OFF_START, h->start);
    memcpy(buf + ZSI_IDX_OFF_COMPAR, h->compar_name, ZSI_COMPAR_NAME_LEN);
    zsi_put64(buf + ZSI_IDX_OFF_VALID_UPTO, h->valid_upto);
    zsi_put64(buf + ZSI_IDX_OFF_TERM_OFF, h->term_off);
    zsi_put64(buf + ZSI_IDX_OFF_NPTRS, h->nptrs);
    zsi_put32(buf + ZSI_IDX_OFF_TERM_CSUM, h->term_csum);

    /* Last 4 bytes, covering everything before them, exactly as F-4 has it for a
     * data file header.  No field-zeroing anywhere. */
    zsi_put32(buf + ZSI_IDX_OFF_CSUM, csum(buf, ZSI_IDX_OFF_CSUM));
}

/* Decode and validate the header ALONE.  The cross-checks against the data file
 * -- uuid, generation, comparator, offset ranges, the terminator binding --
 * belong to the loader, because they need the file. */
static int zsi_idxhdr_decode(const char *buf, size_t len, zs_csum *csum,
                             struct zsi_idxhdr *out)
{
    if (len < ZSI_IDX_HEADER_LEN) return ZS_BADFORMAT;

    if (memcmp(buf + ZSI_IDX_OFF_MAGIC, zsi_idx_magic, ZSI_IDX_MAGIC_LEN) != 0)
        return ZS_BADFORMAT;

    if (zsi_get32(buf + ZSI_IDX_OFF_CSUM) != csum(buf, ZSI_IDX_OFF_CSUM))
        return ZS_BADCHECKSUM;

    uint8_t vread = (uint8_t)buf[ZSI_IDX_OFF_VREAD];
    if (vread > ZSI_IDX_VERSION_READ) return ZS_BADFORMAT;

    out->version_read  = vread;
    out->version_write = (uint8_t)buf[ZSI_IDX_OFF_VWRITE];
    out->flags         = zsi_get16(buf + ZSI_IDX_OFF_FLAGS);
    memcpy(out->uuid, buf + ZSI_IDX_OFF_UUID, 16);
    out->start         = zsi_get32(buf + ZSI_IDX_OFF_START);
    memcpy(out->compar_name, buf + ZSI_IDX_OFF_COMPAR, ZSI_COMPAR_NAME_LEN);
    out->valid_upto    = zsi_get64(buf + ZSI_IDX_OFF_VALID_UPTO);
    out->term_off      = zsi_get64(buf + ZSI_IDX_OFF_TERM_OFF);
    out->nptrs         = zsi_get64(buf + ZSI_IDX_OFF_NPTRS);
    out->term_csum     = zsi_get32(buf + ZSI_IDX_OFF_TERM_CSUM);

    /* F-9 again: generations start at 1, so a start of 0 is never legitimate. */
    if (out->start == 0) return ZS_BADFORMAT;

    return ZS_OK;
}

/* The engine id, read as plain data before any verification -- exactly as
 * zsi_header_engine_id does, and for the same reason (F-5a): the checksum cannot
 * be verified until the engine is known, and the engine is recorded inside the
 * header the checksum protects.  A wrong value yields a failed checksum, not a
 * wrong interpretation.
 *
 * Requires len >= ZSI_IDX_HEADER_LEN; the caller checks that first. */
static unsigned zsi_idxhdr_engine_id(const char *buf)
{
    return (unsigned)(zsi_get16(buf + ZSI_IDX_OFF_FLAGS) & ZSI_CSUM_MASK);
}

/* P-3.  The `zeroskip.` prefix is the metadata namespace (D-2), so a table can
 * never be parsed as a data file even if someone points a cache directory at a
 * database.  The uuid and generation forms are D-0's and D-1's, so a table's
 * name reads naturally beside the file it describes. */
#define ZSI_IDX_NAME_PREFIX "zeroskip.index-"

static void zsi_name_format_index(char *out, const zsi_uuid_t uuid,
                                  uint32_t gen)
{
    char ustr[ZSI_UUID_STR_LEN];
    zsi_uuid_unparse(uuid, ustr);

    snprintf(out, ZSI_NAME_MAX, "%s%s-%08X", ZSI_IDX_NAME_PREFIX, ustr, gen);
}

/* Where a handle's tables live, and how far past the last one it lets the file
 * grow before publishing another (P-13).  dir NULL disables the cache.
 *
 * dir is the RESOLVED per-database directory -- <root>/<uuid> for a configured
 * root (P-2a), or <dbdir>/zeroskip.cache under ZS_INDEX_LOCAL (P-2b) --
 * resolved once at open, so publish, load, sweep and dump all take it as
 * given.  `local` marks the P-2b case, whose sweep may also remove
 * foreign-uuid tables: the directory serves exactly one database, so a table
 * carrying any other uuid there is garbage by construction. */
struct zsi_idxcfg {
    const char *dir;
    size_t      threshold;
    bool        local;
};

/* Load and fully validate the pointer table for this file (P-11).
 *
 * EVERY failure is ZS_NOTFOUND: a missing file, a short read, an allocation
 * failure, a rule violation.  That uniformity is the point, not laziness.  A
 * table is an optimisation living outside the database, and the only correct
 * response to any doubt about one is to ignore it and replay.  Returning an
 * error instead would let a corrupt file in a directory the database does not
 * depend on turn a readable database into an unreadable one, which is precisely
 * what P-11 forbids and what G-3 is about.
 *
 * On ZS_OK, *base is an owned array of *nbase offsets in key order. */
static int zsi_idx_load(struct zsi_file *f, const struct zsi_idxcfg *cfg,
                        const char *compar_name, bool nocsum,
                        size_t **base, size_t *nbase, size_t *valid_upto,
                        size_t *term_off, uint32_t *term_csum)
{
    char name[ZSI_NAME_MAX], path[PATH_MAX];
    struct zsi_idxhdr h;
    struct stat sb;
    char *buf = NULL;
    size_t len, want, arrlen;
    size_t *offs = NULL;
    int fd = -1, rc = ZS_NOTFOUND;

    *base = NULL;
    *nbase = 0;
    *valid_upto = ZSI_HEADER_LEN;
    *term_off = ZSI_HEADER_LEN;
    *term_csum = 0;

    if (!cfg || !cfg->dir) return ZS_NOTFOUND;

    /* D-10: a file with an invalid header has no records to index and no
     * generation we would trust a table against. */
    if (!f->hdr_valid || !f->csum) return ZS_NOTFOUND;

    zsi_name_format_index(name, f->hdr.uuid, f->hdr.start);
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, name)
        >= sizeof(path))
        return ZS_NOTFOUND;

    fd = open(path, O_RDONLY);
    if (fd < 0) return ZS_NOTFOUND;
    if (fstat(fd, &sb) < 0 || !S_ISREG(sb.st_mode)) goto out;

    len = (size_t)sb.st_size;
    if (len < ZSI_IDX_HEADER_LEN + 4) goto out;
    if (len > ZSI_IDX_MAX_BYTES) goto out;

    buf = malloc(len);
    if (!buf) goto out;
    if (read(fd, buf, len) != (ssize_t)len) goto out;

    /* P-7: the engine is the DATA FILE's, never ours.  Resolving it from the
     * file and then REQUIRING the table to agree is what closes the hole F-5a
     * closes for data files -- otherwise a table could name an engine the file
     * does not use and still validate under it. */
    if (zsi_idxhdr_engine_id(buf) != f->csum_id) goto out;
    if (zsi_idxhdr_decode(buf, len, f->csum, &h) != ZS_OK) goto out;

    if (memcmp(h.uuid, f->hdr.uuid, 16) != 0) goto out;
    if (h.start != f->hdr.start) goto out;

    /* Both comparator names: the file's, because a table sorted under a
     * different order is meaningless against it, and OUR OWN, because a handle
     * may legitimately open a file whose recorded comparator it does not
     * implement. */
    if (memcmp(h.compar_name, f->hdr.compar_name, ZSI_COMPAR_NAME_LEN) != 0)
        goto out;
    if (memcmp(h.compar_name, compar_name, ZSI_COMPAR_NAME_LEN) != 0) goto out;

    /* An index built without verification may hold records a verifying reader
     * would reject, so it must not be handed to one.  The converse is fine. */
    if (!nocsum && !(h.flags & ZSI_IDX_FLAG_CSUM_VERIFIED)) goto out;

    /* Exact size, so a truncated or padded table is rejected rather than read
     * short.  The division guards the multiply. */
    if (h.nptrs > (ZSI_IDX_MAX_BYTES - ZSI_IDX_HEADER_LEN - 4) / 8) goto out;
    arrlen = (size_t)h.nptrs * 8;
    if (!zsi_add_sz(arrlen, ZSI_IDX_HEADER_LEN + 4, &want)) goto out;
    if (want != len) goto out;

    if (zsi_get32(buf + len - 4)
        != f->csum(buf + ZSI_IDX_HEADER_LEN, arrlen))
        goto out;

    if (h.valid_upto < ZSI_HEADER_LEN || h.valid_upto > f->size) goto out;

    /* P-10's binding, in O(1).  The table names the terminator's offset because
     * terminators are only ever found by scanning forward (F-20): a reader given
     * valid_upto alone could not locate it without the replay this whole
     * mechanism exists to avoid.
     *
     * What this catches is a data file whose covered prefix is not the one the
     * table was built over -- which the format itself cannot produce, since
     * files are append-only and generations are never reissued (D-9b), so it is
     * there for out-of-band events such as a restore from backup under a
     * surviving cache directory.  It examines one span, so P-17 states the
     * limit plainly rather than implying more. */
    if (h.valid_upto == ZSI_HEADER_LEN) {
        if (h.term_off != ZSI_HEADER_LEN || h.term_csum != 0 || h.nptrs != 0)
            goto out;
    } else {
        struct zsi_term term;
        const char *tb;
        size_t after;

        if (h.term_off < ZSI_HEADER_LEN || h.term_off >= h.valid_upto) goto out;
        tb = zsi_file_at(f, (size_t)h.term_off, 1);
        if (!tb) goto out;
        if (zsi_term_decode(tb, f->size - (size_t)h.term_off, &term) != ZS_OK)
            goto out;
        if (!zsi_add_sz((size_t)h.term_off, term.len, &after)) goto out;
        if (after != (size_t)h.valid_upto) goto out;
        if (term.csum != h.term_csum) goto out;
    }

    offs = malloc(h.nptrs ? (size_t)h.nptrs * sizeof(*offs) : 1);
    if (!offs) goto out;

    for (uint64_t i = 0; i < h.nptrs; i++) {
        uint64_t v = zsi_get64(buf + ZSI_IDX_HEADER_LEN + i * 8);

        /* Every offset must address a record inside the covered prefix.  This
         * is O(n) over the array but touches no data-file page, so it costs
         * nothing the cache is trying to save.
         *
         * It deliberately does NOT verify that the offsets are in key order:
         * that needs a record decode per entry, which is exactly the work being
         * avoided, and the array checksum already stands behind the ordering. */
        if (v < ZSI_HEADER_LEN || v >= h.valid_upto) goto out;
        if (v > (uint64_t)SIZE_MAX) goto out;
        offs[i] = (size_t)v;
    }

    *base = offs;
    *nbase = (size_t)h.nptrs;
    *valid_upto = (size_t)h.valid_upto;
    *term_off = (size_t)h.term_off;
    *term_csum = h.term_csum;
    offs = NULL;
    rc = ZS_OK;

out:
    free(offs);
    free(buf);
    if (fd >= 0) close(fd);
    return rc;
}

/* Build the private index, seeded from a table if one is usable (P-12). */
static int zsi_index_build_cached(struct zsi_file *f, zs_compar *compar,
                                  const char *compar_name, bool nocsum,
                                  const struct zsi_idxcfg *cfg)
{
    size_t *base = NULL, nbase = 0;
    size_t vu = ZSI_HEADER_LEN, to = ZSI_HEADER_LEN;
    uint32_t tc = 0;
    int r;

    if (zsi_idx_load(f, cfg, compar_name, nocsum,
                     &base, &nbase, &vu, &to, &tc) != ZS_OK)
        return zsi_index_build(f, compar, nocsum);

    f->cached_upto = vu;
    r = zsi_index_build_from(f, compar, nocsum, base, nbase, vu);
    if (r != ZS_OK) return r;

    /* A replay that found no new span leaves last_term_* at the values it
     * started with, so seed them from the table.  Without this, publishing after
     * a no-op catch-up would record a terminator that is not the one at the
     * complete point, and every later reader would reject the result. */
    if (f->complete == vu) {
        f->last_term_off  = to;
        f->last_term_csum = tc;
    }

    return ZS_OK;
}

/* Publish a table covering this file's complete point (P-4, P-13).
 *
 * ZS_DONE means the threshold was not reached and nothing was written, which is
 * the common case and not a failure.  Any other non-OK is a real problem with
 * the cache directory, and the caller is REQUIRED to swallow it (P-15): the data
 * is already durable, and failing an operation over a rebuildable cache would be
 * a regression rather than a safety measure.
 *
 * There is deliberately NO fsync (P-14).  A table is rebuildable, a torn or
 * zero-filled file after a crash is rejected by P-11's checksums, and syncing
 * would put a third sync on a commit path C-7 defines as exactly two -- which is
 * the cost this whole mechanism exists to reduce. */
static int zsi_idx_publish(struct zsi_file *f, const struct zsi_idxcfg *cfg,
                           zs_compar *compar, bool nocsum)
{
    char name[ZSI_NAME_MAX], path[PATH_MAX], tmp[PATH_MAX];
    unsigned char rnd[4];
    struct zsi_idxhdr h;
    char hdr[ZSI_IDX_HEADER_LEN], csbuf[4];
    size_t *offs = NULL, n = 0, arrlen;
    char *arr = NULL;
    int fd = -1, rc = ZS_INTERNAL;

    if (!cfg || !cfg->dir) return ZS_DONE;
    if (!f->hdr_valid || !f->csum || !f->index) return ZS_DONE;
    if (!zsi_file_is_unordered(f)) return ZS_DONE;          /* P-1 */

    /* P-13.  cached_upto is the valid_upto of the table this index was seeded
     * from, or the header length if none was.  Publishing below the threshold is
     * what would make a bulk load quadratic: a table is rewritten whole, so one
     * publication per commit costs O(records in the file) of I/O per commit. */
    if (f->complete < f->cached_upto) return ZS_DONE;
    if (f->complete - f->cached_upto < cfg->threshold) return ZS_DONE;

    if (zsi_index_flatten(f->index, compar, &offs, &n) != ZS_OK)
        return ZS_INTERNAL;

    if (n > (ZSI_IDX_MAX_BYTES - ZSI_IDX_HEADER_LEN - 4) / 8) goto out;
    arrlen = n * 8;

    arr = malloc(arrlen ? arrlen : 1);
    if (!arr) goto out;
    for (size_t i = 0; i < n; i++)
        zsi_put64(arr + i * 8, (uint64_t)offs[i]);

    memset(&h, 0, sizeof(h));
    h.version_read  = ZSI_IDX_VERSION_READ;
    h.version_write = ZSI_IDX_VERSION_WRITE;
    /* P-7: the FILE's engine, never the handle's.  Using the handle's would
     * produce tables a conforming peer must reject -- the same silent failure
     * that using the handle's engine to append to an existing file causes
     * (A-6, F-5a). */
    h.flags = (uint16_t)(f->csum_id & ZSI_CSUM_MASK);
    if (!nocsum) h.flags |= ZSI_IDX_FLAG_CSUM_VERIFIED;
    memcpy(h.uuid, f->hdr.uuid, 16);
    h.start = f->hdr.start;
    memcpy(h.compar_name, f->hdr.compar_name, ZSI_COMPAR_NAME_LEN);
    h.valid_upto = (uint64_t)f->complete;
    h.term_off   = (uint64_t)f->last_term_off;
    h.nptrs      = (uint64_t)n;
    h.term_csum  = f->last_term_csum;

    zsi_idxhdr_encode(hdr, &h, f->csum);

    if (!zsi_random_bytes(rnd, sizeof(rnd))) {
        uint64_t e = zsi_weak_entropy();
        memcpy(rnd, &e, sizeof(rnd));
    }

    /* Random digits as well as the pid: two processes sharing a cache directory
     * across a network filesystem can have the same pid, and two of them writing
     * one staging name would interleave into a file that is then renamed into
     * place.  P-11 would reject the result, but at the cost of both processes'
     * work and with nothing to explain why. */
    if ((size_t)snprintf(tmp, sizeof(tmp), "%s/%s%d.%02x%02x%02x%02x",
                         cfg->dir, ZSI_STAGING_PREFIX, (int)getpid(),
                         rnd[0], rnd[1], rnd[2], rnd[3]) >= sizeof(tmp))
        goto out;

    zsi_name_format_index(name, f->hdr.uuid, f->hdr.start);
    if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, name)
        >= sizeof(path))
        goto out;

    fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { rc = ZS_IOERROR; goto out; }

    rc = ZS_IOERROR;
    if (ZS_WRITE(fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) goto out_unlink;
    if (arrlen && ZS_WRITE(fd, arr, arrlen) != (ssize_t)arrlen) goto out_unlink;

    /* Note the empty case: f->csum(arr, 0) must be the ENGINE's value for empty
     * input, not zero.  zsi_csum_xxhash has no empty short-circuit for exactly
     * this reason (F-26g), so passing a zero length here is correct. */
    zsi_put32(csbuf, f->csum(arr, arrlen));
    if (ZS_WRITE(fd, csbuf, 4) != 4) goto out_unlink;

    if (close(fd) < 0) { fd = -1; goto out_unlink; }
    fd = -1;

    if (ZS_RENAME(tmp, path) < 0) goto out_unlink;

    f->cached_upto = f->complete;
    rc = ZS_OK;
    goto out;

out_unlink:
    if (fd >= 0) { close(fd); fd = -1; }
    ZS_UNLINK(tmp);

out:
    if (fd >= 0) close(fd);
    free(arr);
    free(offs);
    return rc;
}

/* P-16.  Unlink tables for this database whose generation is no longer an
 * unordered file.
 *
 * Takes the live generations as a plain array rather than a struct zsi_fileset,
 * because FILE SET is defined below this section and the layering runs one way.
 *
 * Safe against a concurrent reader: a descriptor already open survives the
 * unlink, and a reader that misses a table replays instead.  Every error is
 * ignored, because a sweep that cannot run costs disk space, not correctness. */
static void zsi_idx_sweep(const struct zsi_idxcfg *cfg, const zsi_uuid_t uuid,
                          const uint32_t *live, size_t nlive)
{
    char want[ZSI_UUID_STR_LEN];
    size_t plen = strlen(ZSI_IDX_NAME_PREFIX);
    struct dirent *de;
    DIR *d;

    if (!cfg || !cfg->dir) return;

    zsi_uuid_unparse(uuid, want);

    d = opendir(cfg->dir);
    if (!d) return;

    while ((de = readdir(d))) {
        const char *nm = de->d_name;
        char path[PATH_MAX];
        uint32_t gen;
        bool alive = false;

        if (strncmp(nm, ZSI_IDX_NAME_PREFIX, plen) != 0) continue;
        nm += plen;

        /* Another database's tables are not ours to remove -- except inside
         * zeroskip.cache (P-2b), which serves exactly one database, where a
         * foreign uuid is garbage by construction. */
        if (!cfg->local && strncmp(nm, want, 36) != 0) continue;
        nm += 36;
        if (*nm != '-') continue;
        nm++;

        if (zsi_parse_gen8(nm, &gen) != 8) continue;
        if (nm[8] != '\0') continue;

        for (size_t i = 0; i < nlive; i++)
            if (live[i] == gen) { alive = true; break; }
        if (alive) continue;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s", cfg->dir, de->d_name)
            < sizeof(path))
            ZS_UNLINK(path);
    }

    closedir(d);
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

    /* D-14k: travel toward smaller keys.  Fixed when the owning cursor is
     * opened.  Reverse positions are COUNTS -- the in-order candidate is
     * pi-1, the index cursor's are bi-1/di-1, and 0 is exhausted -- so no
     * unsigned position ever underflows. */
    bool              reverse;
    struct zsi_rec    cur;       /* valid iff !exhausted */

    /* kind-specific position */
    uint64_t             pi;     /* in-order: pointer array index */
    struct zsi_index_cur ic;     /* unordered: index cursor */

    /* txn: the KEY reached, not an index into the pending array (D-14j-a).
     *
     * An index is the trap.  The pending array is sorted and a write during the
     * traversal inserts into it, shifting every element from the insertion point
     * onward -- so an index stops referring to the record it referred to, and
     * the cursor re-yields a key it has already returned.  A key survives that,
     * costs one binary search per step over the transaction's own pending set,
     * and makes a write ahead of the cursor visible, which is what A-1a and
     * cyrusdb both want.  In reverse, NULL means "after the last" instead, and
     * texclusive records whether a seek bound was < rather than <= -- it only
     * matters until the first record is consumed (tstarted), after which the
     * position is always strictly below tkey. */
    char                *tkey;   /* owned; NULL means "before the first" */
    size_t               tkeylen;
    bool                 tstarted;
    bool                 texclusive;
};

/* Filled in by the WRITE PATH section, which owns struct zs_txn.  Declared here
 * because the cursor must be able to present a transaction as just another
 * source; the alternative is a special case in the merge, which D-14g exists to
 * avoid. */
static int zsi_txn_cur_load(struct zsi_fcur *fc);
static void zsi_txn_cur_seek(struct zsi_fcur *fc, const char *key, size_t keylen);
static void zsi_txn_cur_seek_rev(struct zsi_fcur *fc, const char *key,
                                 size_t keylen, bool inclusive);

/* struct zs_txn belongs to WRITE PATH, below, so the cursor reaches it through
 * these rather than through its fields -- the same arrangement the two above
 * already use, and the reason the merge needs no special case for a
 * transaction (D-14g). */
struct zsi_snapshot;    /* SNAPSHOT, below */
static unsigned long zsi_txn_seq(struct zs_txn *txn);
static void zsi_txn_set_snapshot(struct zs_txn *txn, struct zsi_snapshot *snap);

/* Load the record at the cursor's current position, or mark it exhausted. */
static int zsi_fcur_load(struct zsi_fcur *fc)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER: {
        uint64_t at = fc->pi;
        if (fc->reverse) {
            if (fc->pi == 0) { fc->exhausted = true; return ZS_OK; }
            at = fc->pi - 1;
        } else if (fc->pi >= fc->file->nptrs) {
            fc->exhausted = true;
            return ZS_OK;
        }
        if (zsi_ptrs_rec(fc->file, at, &fc->cur) != ZS_OK) {
            fc->exhausted = true;
            return ZS_BADFORMAT;
        }
        fc->exhausted = false;
        return ZS_OK;
    }

    case ZSI_SRC_UNORDERED: {
        /* No index means the caller skipped building one, which is a wiring
         * error rather than a data condition.  Report an exhausted source rather
         * than dereferencing NULL: G-3 requires that no state produce a crash,
         * and a cursor over a source with nothing in it is a defined answer. */
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }

        int r = fc->reverse
              ? zsi_index_cur_get_rev(fc->file->index, fc->compar, &fc->ic,
                                      &fc->cur, NULL)
              : zsi_index_cur_get(fc->file->index, fc->compar, &fc->ic,
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
    case ZSI_SRC_TXN:
        free(fc->tkey);
        fc->tkey = NULL;
        fc->tkeylen = 0;
        fc->tstarted = false;
        break;
    }

    return zsi_fcur_load(fc);
}

/* D-14k step 1: position on the largest key <= (inclusive) or < (exclusive)
 * the given key, or exhaust when nothing lies below the bound. */
static int zsi_fcur_seek_rev(struct zsi_fcur *fc, const char *key,
                             size_t keylen, bool inclusive)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER: {
        uint64_t idx;
        bool exact;
        int r = zsi_ptrs_search(fc->file, fc->compar, key, keylen, &idx, &exact);
        if (r != ZS_OK) { fc->exhausted = true; return r; }
        /* idx counts the pointers strictly below the key -- the exclusive
         * reverse position; inclusive keeps an exact match. */
        fc->pi = idx + ((inclusive && exact) ? 1 : 0);
        break;
    }

    case ZSI_SRC_UNORDERED:
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }
        zsi_index_cur_seek_rev(fc->file->index, fc->compar, key, keylen,
                               inclusive, &fc->ic);
        break;

    case ZSI_SRC_TXN:
        zsi_txn_cur_seek_rev(fc, key, keylen, inclusive);
        break;
    }

    return zsi_fcur_load(fc);
}

/* D-14k's empty start: the last key each source holds. */
static int zsi_fcur_seek_last(struct zsi_fcur *fc)
{
    switch (fc->kind) {
    case ZSI_SRC_INORDER:
        fc->pi = fc->file->nptrs;
        break;
    case ZSI_SRC_UNORDERED:
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }
        zsi_index_cur_seek_last(fc->file->index, &fc->ic);
        break;
    case ZSI_SRC_TXN:
        free(fc->tkey);
        fc->tkey = NULL;
        fc->tkeylen = 0;
        fc->tstarted = false;
        fc->texclusive = false;
        break;
    }

    return zsi_fcur_load(fc);
}

/* Advance one step IN THE CURSOR'S DIRECTION -- the merge above never knows
 * which way an arm travels (D-14k). */
static int zsi_fcur_next(struct zsi_fcur *fc)
{
    if (fc->exhausted) return ZS_OK;

    switch (fc->kind) {
    case ZSI_SRC_INORDER:
        if (fc->reverse) fc->pi--;
        else             fc->pi++;
        break;
    case ZSI_SRC_UNORDERED:
        if (!fc->file->index) { fc->exhausted = true; return ZS_OK; }
        if (fc->reverse)
            zsi_index_cur_prev(fc->file->index, fc->compar, &fc->ic);
        else
            zsi_index_cur_next(fc->file->index, fc->compar, &fc->ic);
        break;
    case ZSI_SRC_TXN: {
        /* Remember the key just yielded, so the next load finds its successor
         * in the array as it stands THEN -- which is the whole point (D-14j-a).
         * Copied, because the pending array may be reallocated by a write made
         * from the callback before the next step. */
        char *k = fc->cur.keylen ? malloc(fc->cur.keylen) : NULL;
        if (fc->cur.keylen && !k) { fc->exhausted = true; return ZS_INTERNAL; }
        if (k) memcpy(k, fc->cur.key, fc->cur.keylen);
        free(fc->tkey);
        fc->tkey = k;
        fc->tkeylen = fc->cur.keylen;
        fc->tstarted = true;
        break;
    }
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
/* Release anything a per-file cursor owns.
 *
 * Only the transaction arm owns anything -- the key it holds instead of an index
 * (D-14j-a) -- but every arm goes through this, so a STACK-allocated fcur is
 * cleaned up the same way as one inside a cursor.  Those are easy to miss:
 * zsi_lookup builds one per source on the stack, and the key copy leaked from
 * exactly there until `make leaks` said so. */
static void zsi_fcur_fini(struct zsi_fcur *fc)
{
    free(fc->tkey);
    fc->tkey = NULL;
    fc->tkeylen = 0;
}

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
         * lookup shares the ordering logic rather than duplicating it.
         *
         * The scratch is a COPY, and since D-14j-a a transaction arm owns its
         * position key -- so the copy owns one too, and it has to be released on
         * every exit from here.  A struct that was trivially copyable and then
         * grows an owned pointer is exactly where this goes wrong; `make leaks`
         * is what caught it. */
        struct zsi_fcur scratch = *fc;
        int r = zsi_fcur_seek(&scratch, key, keylen);

        if (r == ZS_OK) {
            if (scratch.exhausted
                || fc->compar(scratch.cur.key, scratch.cur.keylen,
                              key, keylen) != 0)
                r = ZS_NOTFOUND;
            else
                *out = scratch.cur;
        }

        zsi_fcur_fini(&scratch);
        return r;
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

        /* The length is already bounded: zsi_name_parse returned a data-file
         * type above, which requires the name to be exactly
         * "zeroskip-" + 36 + "-" + 8 [+ "-" + 8] with nothing trailing -- 63
         * characters at most, against ZSI_NAME_MAX of 80.
         *
         * Copied with an EXPLICIT length rather than snprintf'd, because the
         * compiler cannot see that reasoning: `d_name` is NAME_MAX-sized, so
         * -Wformat-truncation flags the snprintf, and Cyrus builds -Werror.
         * Suppressing the warning would leave the bound implicit; this states
         * it where it is relied on.  The check is unreachable for the same
         * reason it is here, in the manner of F-29's progress checks. */
        size_t nlen = strlen(de->d_name);
        if (nlen >= ZSI_NAME_MAX) continue;

        memcpy(fs->all[fs->nall].name, de->d_name, nlen + 1);
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
                             zs_compar *compar, const char *compar_name,
                             zs_csum *external_csum,
                             bool nocsum, const struct zsi_idxcfg *idxcfg,
                             void (*report)(const char *, const char *, ...),
                             struct zsi_snapshot **out)
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

        /* Between step 2 and step 3.  Nothing in a shipped build. */
        ZS_SNAPSHOT_GAP(dir);

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

            /* D-10a: a NON-ACTIVE file with an invalid header is REPORTED and
             * treated as holding no records.  It is deliberately NOT fatal, and
             * D-10b records why: D-10 directs a writer to move on from an unclean
             * active file, and the instant it does that file becomes non-active --
             * so a fatal rule would turn the first write after an ordinary crash
             * into a permanently unopenable database, which G-3 forbids.
             *
             * Tolerating it costs nothing that was not already lost, since an
             * invalid header means no record in that file is recoverable either
             * way, while refusing to open costs every other file too.  "Silently"
             * is the hazard, and the report addresses it. */
            bool is_last = (i + 1 == fs.nresolved);
            if (!f->hdr_valid && !is_last && report)
                report("non-active file has an invalid header",
                       "file=<%s>", f->fname);

            if (zsi_file_is_unordered(f)) {
                /* Step 4.  The replay sets f->complete, which IS this file's
                 * snapshot boundary: growth beyond it is invisible (C-4c).
                 *
                 * Seeded from a published pointer table when one is usable
                 * (P-12), which turns the replay from "the whole file" into
                 * "whatever has been appended since somebody last published". */
                r = zsi_index_build_cached(f, compar, compar_name, nocsum,
                                           idxcfg);
                if (r != ZS_OK) {
                    zsi_snapshot_release(&s);
                    zsi_fileset_fini(&fs);
                    return r;
                }

                /* P-13.  A reader publishes on exactly the same rule as a
                 * writer: whoever builds the pointers and has moved far enough
                 * past the last published table writes it out.  Safe from either
                 * side, because publication is a rename (P-4), and never fatal
                 * (P-15) -- which is why the result is discarded here rather
                 * than reported.  The write path, which has a handle to warn
                 * through, does report the first failure. */
                if (idxcfg && idxcfg->dir)
                    (void)zsi_idx_publish(f, idxcfg, compar, nocsum);
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

        /* P-16.  Done here rather than inside the publish, because this is the
         * one place that knows the whole file set.  A table whose generation is
         * still an unordered file is kept, so a stranded unconverted file does
         * not lose its own.  Every failure inside is ignored: a sweep that
         * cannot run costs disk space, not correctness. */
        if (idxcfg && idxcfg->dir && s->nfiles) {
            const zsi_uuid_t *u = NULL;
            uint32_t stackbuf[16];
            uint32_t *live = stackbuf;
            size_t nlive = 0;

            for (size_t i = 0; i < s->nfiles; i++)
                if (s->files[i]->hdr_valid) { u = &s->files[i]->hdr.uuid; break; }

            if (u) {
                if (s->nfiles > sizeof(stackbuf) / sizeof(stackbuf[0]))
                    live = malloc(s->nfiles * sizeof(*live));

                if (live) {
                    for (size_t i = 0; i < s->nfiles; i++)
                        if (zsi_file_is_unordered(s->files[i]))
                            live[nlive++] = s->files[i]->hdr.start;

                    zsi_idx_sweep(idxcfg, *u, live, nlive);
                    if (live != stackbuf) free(live);
                }
            }
        }

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
 * C-1d lock ordering: the locks form ONE TOTAL ORDER, repack -> write -> remove.
 * A holder may take a lock later in that order and must not take one earlier, so
 * no cycle exists.  Most operations use a sub-chain: a writer takes
 * write -> remove, a repacker repack -> remove, and the two never contend
 * (C-1a).  Only compaction (D-26) holds both repack and write, which is why the
 * order is a chain rather than two disjoint pairs.
 *
 * Asserted here, which is sound now that a handle belongs to one thread of
 * control: `held` describes the one actor using it, and a violation deadlocks in
 * production while being trivially visible in development. */
static int zsi_lock_take(struct zsi_locks *lk, enum zsi_lock which, int flags)
{
    bool block = !(flags & ZS_NONBLOCKING);

    assert(lk->fd >= 0);
    assert(!(lk->held & (1u << which)));        /* not already held */

    /* C-1d, as a chain: taking a lock is legal only while holding locks strictly
     * EARLIER in repack -> write -> remove. */
    if (which == ZSI_LOCK_WRITE || which == ZSI_LOCK_REPACK)
        assert(!(lk->held & (1u << ZSI_LOCK_REMOVE)));
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

/********** DATABASE HANDLE *************/

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

    /* Pointer table cache (spec section 8).  index_dir NULL disables it.
     * Resolved at open to the PER-DATABASE directory: <root>/<uuid> for a
     * configured root (P-2a), <dbdir>/zeroskip.cache under ZS_INDEX_LOCAL
     * (P-2b, index_local set). */
    char        *index_dir;          /* owned */
    size_t       index_threshold;    /* P-13 */
    bool         index_local;        /* A-8a */
    bool         idx_publish_warned; /* P-15: report the first failure only */

    bool         readonly;          /* ZS_SHARED (A-5) */
    bool         nocsum;            /* ZS_NOCSUM (F-5e) */
    bool         nosync;            /* ZS_NOSYNC (C-7c) */
    bool         nonblocking;

    struct zsi_locks     locks;
    struct zsi_snapshot *snap;      /* current snapshot for zs_db_* calls */
    struct zs_txn       *write_txn;

    /* C-4i freshness baseline: the raw sorted name set of the last probe's
     * directory scan, NUL-joined.  Taken BEFORE the snapshot it vouches for,
     * so a file appearing between the two scans makes the NEXT probe fire a
     * spurious refresh rather than ever missing one -- the safe direction. */
    char        *probe_names;
    size_t       probe_names_len;

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

    ssize_t n = ZS_WRITE(fd, hdr, sizeof(hdr));
    if (n != (ssize_t)sizeof(hdr)) { close(fd); return ZS_IOERROR; }
    if (!db->nosync && ZS_FDATASYNC(fd) < 0) { close(fd); return ZS_IOERROR; }
    close(fd);

    /* C-6: fdatasync the DIRECTORY after creating a data file, or the name may be
     * absent after a crash even though the contents are durable.
     *
     * A failure here is REPORTED rather than fatal.  The file exists and its
     * contents are durable; only the name's durability is in doubt, and if it were
     * lost the result is a generation that never appeared -- which is
     * indistinguishable from the transaction not having happened, and recoverable
     * (C-6a).  Failing the create would be worse: it would leave a file the caller
     * has been told does not exist. */
    if (!db->nosync) {
        int dfd = open(db->dir, O_RDONLY);
        if (dfd >= 0) {
            if (ZS_FDATASYNC(dfd) < 0)
                db->error("directory sync failed; the new file's name may not "
                          "be durable", "dir=<%s>", db->dir);
            close(dfd);
        }
    }

    return ZS_OK;
}

/* Refresh db->snap.  Used by open and by every zs_db_* call that needs a current
 * view; a transaction holds its own snapshot for its lifetime. */
static int zsi_db_refresh(struct zs_db *db)
{
    struct zsi_snapshot *s = NULL;
    struct zsi_idxcfg idxcfg = { db->index_dir, db->index_threshold,
                                 db->index_local };
    int r = zsi_snapshot_take(db->dir, db->have_uuid ? &db->uuid : NULL,
                              db->compar, db->compar_name, db->external_csum,
                              db->nocsum, &idxcfg, db->error, &s);
    if (r != ZS_OK) return r;

    zsi_snapshot_release(&db->snap);
    db->snap = s;

    if (!db->have_uuid && s->nfiles) {
        memcpy(db->uuid, s->files[0]->hdr.uuid, 16);
        db->have_uuid = true;
    }

    return ZS_OK;
}

static int zsi_entry_name_order(const void *va, const void *vb)
{
    const struct zsi_entry *a = va, *b = vb;
    return strcmp(a->name, b->name);
}

/* C-4i: refresh the handle's snapshot, unless a probe proves nothing has
 * committed since it was taken.
 *
 * If the active file is the same and hasn't changed size, it's always safe --
 * what remains is noticing a NEW active file, and the name set is what does
 * that.  The probe is therefore EXACT, not a heuristic: every commit either
 * appends to the active file -- its size grows past the snapshot's -- or
 * publishes a file by rename (rollover, conversion, repack), which changes
 * the directory's name set.  Timestamps play no part, so filesystem
 * timestamp granularity cannot fake freshness.
 *
 * The baseline name set is the probe's own scan, captured BEFORE the refresh
 * it triggers.  A file that appears between this scan and the refresh's own
 * is in the snapshot but missing from the baseline, so the NEXT probe fires a
 * refresh for nothing -- a spurious refresh, never a missed commit.  That is
 * the safe direction, and it converges the probe after.
 *
 * This is what makes a read on an hours-old handle as current as one on a
 * handle opened for the call, at a few syscalls rather than the snapshot
 * rebuild zsi_db_refresh pays: readers take no lock (C-2), so nothing
 * announces another process's commit -- the only way to see it is to look. */
static int zsi_db_freshen(struct zs_db *db)
{
    struct zsi_fileset fs;
    char *names = NULL;
    size_t len = 0;
    bool stale;
    int r;

    r = zsi_fileset_scan(db->dir, db->have_uuid ? &db->uuid : NULL, &fs);
    if (r != ZS_OK) return r;

    /* The raw name set, sorted and NUL-joined into one comparable blob.
     * Raw rather than resolved: after a repack, a superseded input lingers
     * beside its output until D-23 removes it, and a resolved-set baseline
     * would read that removal window as "changed" on every probe. */
    qsort(fs.all, fs.nall, sizeof(*fs.all), zsi_entry_name_order);
    for (size_t i = 0; i < fs.nall; i++) len += strlen(fs.all[i].name) + 1;

    names = malloc(len ? len : 1);
    if (!names) { zsi_fileset_fini(&fs); return ZS_INTERNAL; }
    {
        size_t off = 0;
        for (size_t i = 0; i < fs.nall; i++) {
            size_t n = strlen(fs.all[i].name) + 1;
            memcpy(names + off, fs.all[i].name, n);
            off += n;
        }
    }
    zsi_fileset_fini(&fs);

    stale = !db->probe_names
         || db->probe_names_len != len
         || memcmp(db->probe_names, names, len) != 0;

    /* Names unchanged: the set is the same, so the only possible commit is an
     * append to the snapshot's active file.  No active file means the newest
     * file is in-order and any commit would have had to CREATE one -- which
     * the name set would have caught -- so there is nothing left to check. */
    if (!stale) {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        if (act) {
            struct stat sb;
            /* fname is already the full path, as zsi_file_open stored it. */
            if (stat(act->fname, &sb) != 0) stale = true;
            else if ((size_t)sb.st_size != act->size) stale = true;
        }
    }

    if (!stale) { free(names); return ZS_OK; }

    free(db->probe_names);
    db->probe_names = names;
    db->probe_names_len = len;

    return zsi_db_refresh(db);
}

/********** READ PATH *************/

/* Every read draws on the same set of SOURCES, ordered newest to oldest (D-14):
 *
 *   highest   the current write transaction's uncommitted records
 *   then      each data file, by start generation DESCENDING
 *
 * Within a file the newest version of a key wins; across sources, the first
 * record found in that order wins.  If that record is a deletion, the key does not
 * exist.
 *
 * D-14a: point lookups, cursors and range scans ALL resolve visibility by this
 * one rule, which is what makes it impossible for them to disagree (G-7).  Both
 * live here, over the same per-file cursors, for that reason -- a second
 * resolution path would be a second set of bugs. */

/* D-14d, point lookup.  Walk the sources in priority order; in each, search for
 * the key by D-14b; stop at the first source that HAS it.  That record decides the
 * answer -- its value, or absence if it is a deletion.
 *
 * A source that does not have the key is skipped, and NO SOURCE MAY BE SKIPPED FOR
 * ANY OTHER REASON.  No bloom filter, no cached key range, nothing: the cost is
 * proportional to the number of files, which is why keeping that count low is the
 * point of the repack policy (D-16) rather than something to optimise around here.
 *
 * D-14c: ancestors are not consulted.  They exist solely for repacking, so a
 * lookup never follows a chain. */
static int zsi_lookup(struct zs_db *db, struct zsi_snapshot *snap,
                      struct zs_txn *txn, const char *key, size_t keylen,
                      struct zsi_rec *out)
{
    struct zsi_fcur fc;

    if (keylen < 1) return ZS_BADUSAGE;         /* F-14 */

    /* The transaction first: its writes are visible to its own reads and to
     * nothing else until commit (A-1a). */
    if (txn) {
        memset(&fc, 0, sizeof(fc));
        fc.kind = ZSI_SRC_TXN;
        fc.txn = txn;
        fc.compar = db->compar;
        fc.gen = ZSI_GEN_TXN;

        int r = zsi_fcur_find(&fc, key, keylen, out);
        zsi_fcur_fini(&fc);
        if (r == ZS_OK) return zsi_rec_is_delete(out) ? ZS_NOTFOUND : ZS_OK;
        if (r != ZS_NOTFOUND) return r;
    }

    /* Then each file, newest first.  The snapshot is sorted ascending, so this
     * walks it backwards. */
    for (size_t i = snap->nfiles; i-- > 0; ) {
        zsi_fcur_init_file(&fc, snap->files[i], db->compar);

        int r = zsi_fcur_find(&fc, key, keylen, out);
        zsi_fcur_fini(&fc);
        /* F-32a: verify the record being materialized -- tombstones included,
         * since a corrupt tombstone is about to decide this key is absent. */
        if (r == ZS_OK && !db->nocsum) {
            int vr = zsi_rec_verify(snap->files[i]->csum, out);
            if (vr != ZS_OK) return vr;
        }
        if (r == ZS_OK) return zsi_rec_is_delete(out) ? ZS_NOTFOUND : ZS_OK;
        if (r != ZS_NOTFOUND) return r;
    }

    return ZS_NOTFOUND;
}

/* D-14e, iteration.  A cursor holds one per-file cursor per source, each
 * traversable in comparator order, held in an array kept sorted by:
 *
 *     current key ascending, then generation descending.
 *
 * The tie-break is not decoration.  Because equal keys sort by generation
 * descending, every cursor positioned on the emitted key is CONTIGUOUS FROM THE
 * FRONT of the array and element 0 is the newest -- which is what makes step 3 a
 * complete treatment of duplicates rather than a heuristic (D-14f). */
struct zs_cursor {
    struct zs_db     *db;
    struct zs_txn    *txn;          /* owning txn, or NULL for an implicit one */
    struct zsi_snapshot *snap;
    bool              owns_txn;

    struct zsi_fcur  *cur;
    size_t            ncur;

    char             *prefix;
    size_t            prefixlen;

    /* ZS_SKIPROOT needs the start key kept, since the prefix buffer is only
     * populated for ZS_CURSOR_PREFIX and the two flags are independent. */
    char             *skiproot_key;
    size_t            skiproot_keylen;

    uint32_t          flags;

    bool              started;
    bool              done;

    /* the last record handed out, and its owning source */
    struct zsi_rec    emitted;
    bool              have_emitted;

    /* D-14j liveness.  A cursor caches each arm's current record, so it must
     * notice when the world underneath changed:
     *
     *   - the transaction's pending array, by a counter (free);
     *   - this handle's file set, by comparing our snapshot against the
     *     handle's current one -- also free, because a commit through the same
     *     handle already replaces it.
     *
     * last_key is an owned copy of the key most recently yielded, because after
     * a refresh the record it came from may have been unmapped.  It is what
     * D-14j-b's "resume strictly after" is measured from. */
    unsigned long     txn_seq;
    char             *last_key;
    size_t            last_keylen;

    /* The key this cursor was opened at, kept for the whole of its life.
     *
     * A refresh re-seeks every arm, and before the first record has been
     * emitted there is no last_key to resume from -- so without this it would
     * restart from the beginning and yield records BEFORE the start key.  Under
     * ZS_CURSOR_PREFIX that restart lands outside the prefix and ends the scan
     * immediately, turning a refresh into a silently empty result. */
    char             *start_key;
    size_t            start_keylen;

    /* D-14k.  Direction is fixed at open.  For a reverse prefix scan the
     * start position is the last key carrying the prefix, found by an
     * exclusive seek at the prefix's byte-successor -- computed ONCE, here,
     * so a refresh re-derives the same bound the open used rather than
     * trusting a stale arm position.  rev_succ NULL with rev_succ_none set
     * means the prefix was all 0xFF: no successor, seek from the end. */
    bool              reverse;
    char             *rev_succ;
    size_t            rev_succlen;
    bool              rev_succ_none;

    /* Whether this cursor may observe the HANDLE's later commits.
     *
     * True for the non-transactional forms, whose enclosing transaction is one
     * the library made for them; false inside a caller's explicit transaction,
     * where G-4 promises a fixed file set.  It cannot be derived from c->txn,
     * because a read-only implicit transaction is passed to the cursor as NULL
     * -- so the wrapper that created it says so. */
    bool              handle_live;
};

/* Order two per-file cursors: exhausted last, then key ascending, then generation
 * descending.  D-14g gives the transaction ZSI_GEN_TXN so its records win equal
 * keys with no special case here. */
static int zsi_cur_order(struct zs_cursor *c, const struct zsi_fcur *a,
                         const struct zsi_fcur *b)
{
    if (a->exhausted && b->exhausted) return 0;
    if (a->exhausted) return 1;
    if (b->exhausted) return -1;

    int r = c->db->compar(a->cur.key, a->cur.keylen, b->cur.key, b->cur.keylen);
    /* D-14k: reverse flips the KEY order only.  The generation tie-break is
     * direction-blind -- equal keys must stay newest-first however the walk
     * travels, or step 3 would suppress the wrong duplicate. */
    if (r) return c->reverse ? -r : r;

    if (a->gen == b->gen) return 0;
    return a->gen > b->gen ? -1 : 1;        /* higher generation first */
}

static void zsi_cur_sort(struct zs_cursor *c)
{
    /* Insertion sort: the array is small by design (D-16 keeps the file count
     * low), and it is almost always already sorted. */
    for (size_t i = 1; i < c->ncur; i++) {
        struct zsi_fcur t = c->cur[i];
        size_t j = i;
        while (j > 0 && zsi_cur_order(c, &c->cur[j - 1], &t) > 0) {
            c->cur[j] = c->cur[j - 1];
            j--;
        }
        c->cur[j] = t;
    }
}

/* D-14e step 5: advance element 0, then move it down to its new position.
 *
 * Its key only ever moves in the direction of travel, so this is a single
 * insertion into an already-sorted array, and it stops as soon as it is no
 * longer greater than its neighbour under the cursor's order.  O(k) worst
 * case, and usually O(1) because the advanced cursor stays at or near the
 * front (D-14i). */
static void zsi_cur_resort_head(struct zs_cursor *c)
{
    if (c->ncur < 2) return;

    struct zsi_fcur t = c->cur[0];
    size_t j = 0;
    while (j + 1 < c->ncur && zsi_cur_order(c, &t, &c->cur[j + 1]) > 0) {
        c->cur[j] = c->cur[j + 1];
        j++;
    }
    c->cur[j] = t;
}

static void zsi_cursor_free(struct zs_cursor *c)
{
    if (!c) return;
    for (size_t i = 0; i < c->ncur; i++) zsi_fcur_fini(&c->cur[i]);
    zsi_snapshot_release(&c->snap);
    free(c->start_key);
    free(c->last_key);
    free(c->rev_succ);
    free(c->cur);
    free(c->prefix);
    free(c->skiproot_key);
    free(c);
}

/* D-14e step 1: seek every per-file cursor to the start point, so its current
 * record is the first with key >= the start key -- or mark it exhausted, which is
 * immediately the case for a source holding no records.  Then sort. */
/* `flags` is uint32_t here and `int` in the public binding, which is where the
 * three casts at the call sites come from.  The binding uses int because that is
 * what C callers expect of a flags argument; internally the flag space is one
 * 32-bit set of bits and unsigned is the honest type for it.  Every public flag
 * value fits well inside 30 bits, so the conversion is value-preserving -- but it
 * is written out rather than left implicit, because Cyrus builds -Wconversion. */
/* D-14k step 1, one arm.  Forward: lower bound of the start key, or the
 * beginning.  Reverse: the largest key <= the start key; under
 * ZS_CURSOR_PREFIX the start key IS the prefix and the scan begins at the
 * LAST key carrying it -- an exclusive seek at the prefix's byte-successor,
 * computed once at open.  Shared by open and by the pre-first-emit reseek,
 * so a refresh cannot diverge from the open (the zsi_cursor_reseek_arm
 * lesson, again). */
static int zsi_cursor_seek_arm_start(struct zs_cursor *c, struct zsi_fcur *fc)
{
    if (!c->reverse)
        return c->start_key ? zsi_fcur_seek(fc, c->start_key, c->start_keylen)
                            : zsi_fcur_seek_first(fc);

    if ((c->flags & ZS_CURSOR_PREFIX) && c->prefixlen) {
        if (c->rev_succ_none) return zsi_fcur_seek_last(fc);
        return zsi_fcur_seek_rev(fc, c->rev_succ, c->rev_succlen, false);
    }

    return c->start_key ? zsi_fcur_seek_rev(fc, c->start_key,
                                            c->start_keylen, true)
                        : zsi_fcur_seek_last(fc);
}

static int zsi_cursor_open(struct zs_db *db, struct zs_txn *txn,
                           struct zsi_snapshot *snap,
                           const char *key, size_t keylen,
                           uint32_t flags, struct zs_cursor **out)
{
    /* A-13: reverse composed with LIVE is rejected, not half-supported. */
    if ((flags & ZS_REVERSE) && (flags & ZS_CURSOR_LIVE)) return ZS_BADUSAGE;

    struct zs_cursor *c = zsi_zmalloc(sizeof(*c));
    if (!c) return ZS_INTERNAL;

    c->db = db;
    c->txn = txn;
    c->reverse = (flags & ZS_REVERSE) != 0;

    /* The cursor takes its OWN reference rather than borrowing the
     * transaction's.  It used to borrow, which was fine while a cursor's
     * snapshot never changed -- but D-14j lets a handle-live cursor swap to a
     * newer one, and releasing a borrowed reference frees a snapshot the
     * transaction is still pointing at. */
    c->snap = snap;
    c->snap->refcount++;
    c->flags = flags;

    /* Take the transaction's change counter NOW.  Starting from zero makes the
     * first step see a false change whenever the transaction already holds a
     * pending write -- and a spurious refresh before anything is emitted is
     * exactly the case that loses the start key. */
    c->txn_seq = zsi_txn_seq(txn);

    if (key && keylen) {
        c->start_key = malloc(keylen);
        if (!c->start_key) { zsi_cursor_free(c); return ZS_INTERNAL; }
        memcpy(c->start_key, key, keylen);
        c->start_keylen = keylen;
    }

    size_t n = snap->nfiles + (txn ? 1 : 0);
    if (n) {
        c->cur = zsi_zmalloc(n * sizeof(*c->cur));
        if (!c->cur) { zsi_cursor_free(c); return ZS_INTERNAL; }
    }

    for (size_t i = 0; i < snap->nfiles; i++)
        zsi_fcur_init_file(&c->cur[c->ncur++], snap->files[i], db->compar);

    if (txn) {
        struct zsi_fcur *fc = &c->cur[c->ncur++];
        memset(fc, 0, sizeof(*fc));
        fc->kind = ZSI_SRC_TXN;
        fc->txn = txn;
        fc->compar = db->compar;
        fc->gen = ZSI_GEN_TXN;
    }

    if ((flags & ZS_SKIPROOT) && key && keylen) {
        c->skiproot_key = malloc(keylen);
        if (!c->skiproot_key) { zsi_cursor_free(c); return ZS_INTERNAL; }
        memcpy(c->skiproot_key, key, keylen);
        c->skiproot_keylen = keylen;
    }

    if (flags & ZS_CURSOR_PREFIX) {
        if (keylen) {
            c->prefix = malloc(keylen);
            if (!c->prefix) { zsi_cursor_free(c); return ZS_INTERNAL; }
            memcpy(c->prefix, key, keylen);
        }
        c->prefixlen = keylen;
    }

    /* D-14k: the byte-successor of the prefix -- increment the last
     * non-0xFF byte and discard everything after it.  All 0xFF has no
     * successor and means "from the end".  Seeking at the prefix ITSELF
     * would be the bug here: the largest key <= the prefix is below every
     * key carrying it (F-11a orders the bare prefix first), so the scan
     * would end before it began unless the bare prefix happened to exist. */
    if (c->reverse && (flags & ZS_CURSOR_PREFIX) && c->prefixlen) {
        size_t n = c->prefixlen;
        while (n && (unsigned char)c->prefix[n - 1] == 0xFF) n--;
        if (!n) {
            c->rev_succ_none = true;
        } else {
            c->rev_succ = malloc(n);
            if (!c->rev_succ) { zsi_cursor_free(c); return ZS_INTERNAL; }
            memcpy(c->rev_succ, c->prefix, n);
            c->rev_succ[n - 1] = (char)((unsigned char)c->rev_succ[n - 1] + 1);
            c->rev_succlen = n;
        }
    }

    for (size_t i = 0; i < c->ncur; i++) {
        c->cur[i].reverse = c->reverse;
        int r = zsi_cursor_seek_arm_start(c, &c->cur[i]);
        if (r != ZS_OK) { zsi_cursor_free(c); return r; }
    }

    zsi_cur_sort(c);

    *out = c;
    return ZS_OK;
}

/* One turn of D-14e's steps 2 through 6.  Returns ZS_OK with a record, or ZS_DONE. */
static int zsi_cursor_step(struct zs_cursor *c, struct zsi_rec *out, bool *emit)
{
    *emit = false;

    /* Step 2, take: the next record is always element 0.  O(1), with no
     * comparison needed to find it. */
    if (!c->ncur || c->cur[0].exhausted) return ZS_DONE;

    struct zsi_rec rec = c->cur[0].cur;

    /* F-32a: verify at the yield, on the record actually being consumed --
     * element 0 is the newest version, the one whose bytes the caller (or the
     * tombstone filter below) will act on.  The stale duplicates step 3 skips
     * are never verified: a corrupt SHADOWED version must not fail a read
     * whose answer does not depend on it.  The file gate also excludes the
     * transaction arm, whose records are synthesized over pending buffers
     * with no base and no checksum. */
    if (c->cur[0].file && !c->db->nocsum) {
        int vr = zsi_rec_verify(c->cur[0].file->csum, &rec);
        if (vr != ZS_OK) return vr;
    }

    /* Step 6, bound: for a prefix scan, stop when the emitted key leaves the
     * prefix.  Checked before emitting, so the first out-of-prefix key ends the
     * scan rather than being returned. */
    if ((c->flags & ZS_CURSOR_PREFIX) && c->prefixlen) {
        if (rec.keylen < c->prefixlen
            || memcmp(rec.key, c->prefix, c->prefixlen) != 0)
            return ZS_DONE;
    }

    /* Step 3, skip stale duplicates.  Every cursor positioned on this key is
     * contiguous from the front and element 0 is the newest, so advancing
     * elements 1, 2, ... while their key matches is a COMPLETE treatment.
     *
     * D-14f: advancing only element 0 would leave the same key at another
     * cursor's head, to be emitted again from an OLDER version -- the value
     * before the newest one, which is worse than a duplicate. */
    size_t nstale = 0;
    for (size_t i = 1; i < c->ncur; i++) {
        if (c->cur[i].exhausted) break;
        if (c->db->compar(c->cur[i].cur.key, c->cur[i].cur.keylen,
                          rec.key, rec.keylen) != 0)
            break;
        nstale++;
    }

    for (size_t i = 1; i <= nstale; i++) {
        int r = zsi_fcur_next(&c->cur[i]);
        if (r != ZS_OK) return r;
    }

    /* Step 5, advance element 0 and re-sort.  Done before the caller sees the
     * record, so the cursor is always positioned for the next call -- and the
     * record's key and value point into the mapping, which outlives the step. */
    int r = zsi_fcur_next(&c->cur[0]);
    if (r != ZS_OK) return r;

    if (nstale) zsi_cur_sort(c);            /* several moved: full re-sort */
    else        zsi_cur_resort_head(c);     /* just element 0 */

    /* Step 4, filter: if the record is a deletion, emit nothing.  The key is
     * consumed either way, which is what makes a tombstone hide older versions in
     * other files rather than merely being skipped. */
    if (zsi_rec_is_delete(&rec)) return ZS_OK;

    *out = rec;
    *emit = true;
    return ZS_OK;
}

/* Re-seek one arm to just after the last key yielded, against whatever its
 * source holds now.  D-14j-b: resume strictly after, never re-yielding and never
 * skipping a key that was already there.
 *
 * The resume point is the CURSOR's, never the arm's.  An arm has its own notion
 * of position -- the last key consumed from it -- and that position LAGS the
 * merge: an arm that was exhausted at open has consumed nothing at all.
 * Re-positioning an arm from its own state therefore resurfaces any key written
 * into the gap between the two, and the merge hands it out BEHIND the last key
 * yielded (found downstream: a store made mid-walk at a key already passed was
 * yielded out of order, shifting the rest of the traversal by one). */
static int zsi_cursor_reseek_arm(struct zs_cursor *c, struct zsi_fcur *fc)
{
    int r;

    /* Before anything has been yielded, the resume point is the OPEN
     * position -- re-derived by the same helper the open used, which for a
     * reverse prefix scan means the byte-successor bound rather than any
     * stale arm state. */
    if (!c->last_key) return zsi_cursor_seek_arm_start(c, fc);

    /* Resume from the last key yielded.  A seek lands ON the key when it is
     * still present, and it has been yielded, so step once past it -- in the
     * direction of travel (D-14j-b, D-14k). */
    if (c->reverse)
        r = zsi_fcur_seek_rev(fc, c->last_key, c->last_keylen, true);
    else
        r = zsi_fcur_seek(fc, c->last_key, c->last_keylen);
    if (r != ZS_OK) return r;

    if (!fc->exhausted
        && c->db->compar(fc->cur.key, fc->cur.keylen,
                         c->last_key, c->last_keylen) == 0)
        return zsi_fcur_next(fc);

    return ZS_OK;
}

static int zsi_cursor_reseek(struct zs_cursor *c)
{
    for (size_t i = 0; i < c->ncur; i++) {
        int r = zsi_cursor_reseek_arm(c, &c->cur[i]);
        if (r != ZS_OK) return r;
    }

    zsi_cur_sort(c);
    return ZS_OK;
}

/* A pending write moved only the transaction's own records, so only that arm
 * needs re-seeking; the files cannot have moved.  An in-order file is never
 * modified, a non-active unordered file is never appended to again, and the
 * active file only ever grows by a COMMIT -- which replaces the handle's
 * snapshot and is the other refresh case entirely.
 *
 * That distinction is the difference between one binary search of the pending
 * array and a search through every file per record written mid-walk, which on a
 * large traversal is the whole cost of the liveness mechanism. */
static int zsi_cursor_reseek_txn(struct zs_cursor *c)
{
    for (size_t i = 0; i < c->ncur; i++) {
        if (c->cur[i].kind != ZSI_SRC_TXN) continue;
        int r = zsi_cursor_reseek_arm(c, &c->cur[i]);
        if (r != ZS_OK) return r;
    }

    zsi_cur_sort(c);
    return ZS_OK;
}

/* D-14j: has anything this cursor is allowed to observe changed since the last
 * step?  Both checks are a comparison, not a syscall -- ZS_CURSOR_LIVE is the
 * only case that costs, and it is opt-in. */
static int zsi_cursor_refresh(struct zs_cursor *c)
{
    bool files_moved = false, txn_moved = false;

    /* The transaction's own pending records, whether the transaction is ours or
     * the caller's: A-1a makes a write on it visible to a traversal already in
     * progress on it. */
    if (c->txn && zsi_txn_seq(c->txn) != c->txn_seq) {
        c->txn_seq = zsi_txn_seq(c->txn);
        txn_moved = true;
    }

    /* The file set, but only for a cursor from a DATABASE HANDLE.  Inside an
     * explicit transaction the file set is fixed for the cursor's lifetime
     * (G-4), which is what a transactional read promises. */
    if (c->handle_live) {
        if (c->flags & ZS_CURSOR_LIVE) {
            /* Still the case that costs, and still a flag -- but C-4i's probe
             * makes each step a few syscalls, not the full snapshot rebuild
             * this used to pay: readers take no lock (C-2), so another
             * process's commit cannot announce itself and the only way to see
             * it is to look. */
            int r = zsi_db_freshen(c->db);
            if (r != ZS_OK) return r;
        }

        if (c->snap != c->db->snap) {
            struct zsi_snapshot *old = c->snap;

            c->db->snap->refcount++;
            c->snap = c->db->snap;
            if (c->txn) zsi_txn_set_snapshot(c->txn, c->snap);
            zsi_snapshot_release(&old);

            /* The file arms point into the old snapshot's files, so they must be
             * rebuilt rather than re-seeked. */
            for (size_t i = 0; i < c->ncur; i++) zsi_fcur_fini(&c->cur[i]);
            free(c->cur);
            c->cur = NULL;
            c->ncur = 0;

            {
                size_t n = c->snap->nfiles + (c->txn ? 1 : 0);
                if (n) {
                    c->cur = zsi_zmalloc(n * sizeof(*c->cur));
                    if (!c->cur) return ZS_INTERNAL;
                }
                for (size_t i = 0; i < c->snap->nfiles; i++)
                    zsi_fcur_init_file(&c->cur[c->ncur++], c->snap->files[i],
                                       c->db->compar);
                if (c->txn) {
                    struct zsi_fcur *fc = &c->cur[c->ncur++];
                    memset(fc, 0, sizeof(*fc));
                    fc->kind = ZSI_SRC_TXN;
                    fc->txn = c->txn;
                    fc->compar = c->db->compar;
                    fc->gen = ZSI_GEN_TXN;
                }
                /* Rebuilt arms travel the way the cursor does (D-14k). */
                for (size_t i = 0; i < c->ncur; i++)
                    c->cur[i].reverse = c->reverse;
            }
            files_moved = true;
        }
    }

    /* A snapshot change subsumes a pending-write change: the full re-seek
     * repositions every arm, transaction included. */
    if (files_moved) return zsi_cursor_reseek(c);
    if (txn_moved)   return zsi_cursor_reseek_txn(c);

    return ZS_OK;
}

static int zsi_cursor_next(struct zs_cursor *c, struct zsi_rec *out)
{
    int rr = zsi_cursor_refresh(c);
    if (rr != ZS_OK) return rr;

    for (;;) {
        bool emit;
        int r = zsi_cursor_step(c, out, &emit);
        if (r != ZS_OK) return r;
        if (emit) {
            /* ZS_SKIPROOT: skip the first record if it matches the start key
             * exactly.  Only the first, and only an exact match. */
            if (!c->started && (c->flags & ZS_SKIPROOT)
                && c->skiproot_key
                && c->db->compar(out->key, out->keylen,
                                 c->skiproot_key, c->skiproot_keylen) == 0) {
                c->started = true;
                continue;
            }
            c->started = true;
            c->emitted = *out;
            c->have_emitted = true;

            /* Owned, because a refresh before the next step may unmap the
             * record this key points into (D-14j-b). */
            {
                char *k = out->keylen ? malloc(out->keylen) : NULL;
                if (out->keylen && !k) return ZS_INTERNAL;
                if (k) memcpy(k, out->key, out->keylen);
                free(c->last_key);
                c->last_key = k;
                c->last_keylen = out->keylen;
            }
            return ZS_OK;
        }
        c->started = true;
    }
}

/********** WRITE PATH *************/

/* Defined further down this section; the streaming store needs them first. */
static int zsi_write_all(int fd, const char *buf, size_t len);
static int zsi_writer_active(struct zs_db *db, int *fdp, uint32_t *genp);
static uint32_t zsi_ancestor_for(struct zs_db *db, struct zsi_snapshot *snap,
                                 uint32_t active_start,
                                 const char *key, size_t keylen);

/* A transaction's own uncommitted records: a sorted KEY -> OFFSET index over
 * records already streamed into the active file (C-8's shape).  The key is an
 * owned copy -- the sorted index and D-14j-a's key-based cursor positions need
 * it -- but the VALUE lives in the file: transaction memory is O(keys), never
 * O(bytes written), so a terabyte transaction does not eat a terabyte of RAM.
 *
 * They are the highest-priority source in D-14's table, visible to subsequent
 * reads on the same transaction and to nothing else until commit (A-1a).
 * A same-key overwrite appends a new record and repoints `off`; the old
 * record becomes a shadowed version in the span, exactly like a shadowed
 * version across spans, and vanishes at the file's next conversion. */
struct zsi_pending {
    char   *key;  size_t keylen;
    size_t  off;                      /* record offset in the active file */
    size_t  len;                      /* its encoded length */
};

/* One read-only mapping of the active file.  A transaction accumulates these
 * and unmaps NOTHING until it ends: that is what keeps every pointer any of
 * its reads returned valid for its whole lifetime (A-4) with no copies. */
struct zsi_txnmap {
    char  *base;
    size_t len;
};

struct zs_txn {
    struct zs_db        *db;
    struct zsi_snapshot *snap;        /* the transaction's fixed snapshot */
    bool                 readonly;
    bool                 holds_write_lock;

    /* Created by a zs_db_* wrapper rather than by the caller (A-0), which is
     * what makes its cursors observe this handle's later commits (D-14j). */
    bool                 implicit;

    struct zsi_pending  *pend;
    size_t               npend, apend;

    /* Bumped on every change to `pend`.  A cursor over this transaction caches
     * the arm's current record, so it needs to know when that cache is stale --
     * and comparing a counter is free, where re-reading the array every step
     * would not be (D-14j). */
    unsigned long        pend_seq;

    /* Streaming state (C-8).  The active file is chosen at the FIRST store --
     * the write lock is held from begin, so D-9's rules apply identically,
     * only the moment moves -- and records are appended as they are stored,
     * through a small chunk buffer that is flushed whenever a read needs
     * bytes it still holds.
     *
     * wsize is the file's logical size including the unflushed chunk;
     * flushed is what has reached the fd.  A read maps the file on demand,
     * each new mapping at least DOUBLING the last (and mapping ahead of EOF,
     * which MAP_SHARED fills in as the file grows), so a transaction holds
     * O(log bytes) mappings and unmaps none of them until it ends (A-4). */
    int                  wfd;         /* -1 until the first store */
    bool                 broken;      /* a stream failure poisoned the span:
                                         commit MUST refuse (see zsi_pend_set) */
    uint32_t             wgen;        /* generation being appended to */
    zs_csum             *wcs;         /* the FILE's engine (A-6, F-5a) */
    unsigned             wcsum_id;
    size_t               span_base;   /* where this transaction's span begins */
    size_t               wsize;
    size_t               flushed;
    char                *chunk;
    size_t               chunklen, chunkcap;
    struct zsi_txnmap   *maps;
    size_t               nmaps, amaps;
};

static int zsi_pend_search(struct zs_txn *txn, const char *key, size_t keylen,
                           size_t *pos)
{
    size_t lo = 0, hi = txn->npend;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = txn->db->compar(txn->pend[mid].key, txn->pend[mid].keylen,
                                key, keylen);
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }

    *pos = lo;
    if (lo < txn->npend
        && txn->db->compar(txn->pend[lo].key, txn->pend[lo].keylen,
                           key, keylen) == 0)
        return ZS_OK;

    return ZS_NOTFOUND;
}

static void zsi_pend_clear(struct zs_txn *txn)
{
    for (size_t i = 0; i < txn->npend; i++)
        free(txn->pend[i].key);
    txn->npend = 0;
}

/* Flush the chunk buffer to the fd.  Called before any read that needs bytes
 * the buffer still holds, and at every terminator. */
static int zsi_txn_flush(struct zs_txn *txn)
{
    if (!txn->chunklen) return ZS_OK;
    int r = zsi_write_all(txn->wfd, txn->chunk, txn->chunklen);
    if (r != ZS_OK) return r;
    txn->flushed += txn->chunklen;
    txn->chunklen = 0;
    return ZS_OK;
}

/* Append bytes to the transaction's stream, batching small records so a
 * store is not a syscall. */
static int zsi_txn_stream(struct zs_txn *txn, const char *buf, size_t len)
{
    if (txn->chunklen + len > txn->chunkcap) {
        int r = zsi_txn_flush(txn);
        if (r != ZS_OK) return r;
        if (len >= txn->chunkcap) {
            r = zsi_write_all(txn->wfd, buf, len);
            if (r != ZS_OK) return r;
            txn->flushed += len;
            txn->wsize += len;
            return ZS_OK;
        }
    }
    memcpy(txn->chunk + txn->chunklen, buf, len);
    txn->chunklen += len;
    txn->wsize += len;
    return ZS_OK;
}

/* Bytes [off, off+len) of the active file, valid until the TRANSACTION ends.
 *
 * This is A-4's mechanism for the transaction's own records: mappings
 * accumulate -- each new one at least doubles the last and may extend past
 * the current EOF, which MAP_SHARED fills in as the file grows -- and none
 * is unmapped until the transaction ends, so no pointer a read returned is
 * ever invalidated.  O(log bytes) mappings, total address space at most a
 * small multiple of the final size, zero copies.  Never dereferences past
 * wsize, so the beyond-EOF tail of a mapping is never touched. */
static const char *zsi_txn_at(struct zs_txn *txn, size_t off, size_t len)
{
    size_t need = off + len;

    if (need < off) return NULL;                    /* overflow */
    if (need > txn->wsize) return NULL;

    /* Flush BEFORE the covering-mapping early return: a record still in the
     * chunk buffer is not in the file yet, and a mapping large enough to
     * cover its offset happily shows the stale bytes there instead. */
    if (need > txn->flushed && zsi_txn_flush(txn) != ZS_OK) return NULL;

    if (txn->nmaps && need <= txn->maps[txn->nmaps - 1].len)
        return txn->maps[txn->nmaps - 1].base + off;

    size_t want = need;
    if (txn->nmaps && want < txn->maps[txn->nmaps - 1].len * 2)
        want = txn->maps[txn->nmaps - 1].len * 2;
    if (want < (size_t)1 << 20) want = (size_t)1 << 20;

    if (txn->nmaps == txn->amaps) {
        size_t grow = txn->amaps ? txn->amaps * 2 : 8;
        struct zsi_txnmap *p = realloc(txn->maps, grow * sizeof(*p));
        if (!p) return NULL;
        txn->maps = p;
        txn->amaps = grow;
    }

    void *m = mmap(NULL, want, PROT_READ, MAP_SHARED, txn->wfd, 0);
    if (m == MAP_FAILED) {
        /* Mapping ahead of EOF can be refused; the exact size cannot, short
         * of a real failure. */
        want = need;
        m = mmap(NULL, want, PROT_READ, MAP_SHARED, txn->wfd, 0);
        if (m == MAP_FAILED) return NULL;
    }

    txn->maps[txn->nmaps].base = (char *)m;
    txn->maps[txn->nmaps].len = want;
    txn->nmaps++;

    return txn->maps[txn->nmaps - 1].base + off;
}

/* First store: choose the file this transaction streams into (D-9 applied at
 * the first store rather than at commit -- the write lock is held from begin,
 * so only the moment moves), pin the snapshot the rollover may have replaced,
 * and record the FILE's checksum engine (A-6, F-5a): using the handle's
 * engine to checksum a span appended to a file recording a different one is
 * silent data loss -- the terminator validates under neither, so the next
 * reader rejects the whole span (F-22 doing its job). */
static int zsi_txn_stream_begin(struct zs_txn *txn)
{
    struct zs_db *db = txn->db;

    if (txn->wfd >= 0) return ZS_OK;

    int fd;
    uint32_t gen;
    int r = zsi_writer_active(db, &fd, &gen);
    if (r != ZS_OK) return r;

    /* The snapshot may have been replaced by a rollover, and both the
     * ancestor search and this transaction's own reads must see the file set
     * as it is now. */
    zsi_snapshot_release(&txn->snap);
    txn->snap = db->snap;
    txn->snap->refcount++;

    struct zsi_file *act = zsi_snapshot_active(db->snap);
    zs_csum *cs = act && act->hdr_valid
                ? act->csum
                : zsi_csum_for_id(db->create_csum_id, db->external_csum);
    unsigned csum_id = act && act->hdr_valid ? act->csum_id
                                             : db->create_csum_id;
    if (!cs) { close(fd); return ZS_BADUSAGE; }

    struct stat sb;
    if (fstat(fd, &sb) < 0) { close(fd); return ZS_IOERROR; }

    if (!txn->chunk) {
        txn->chunk = malloc(ZSI_TXN_CHUNK);
        if (!txn->chunk) { close(fd); return ZS_INTERNAL; }
        txn->chunkcap = ZSI_TXN_CHUNK;
    }

    txn->wfd = fd;
    txn->wgen = gen;
    txn->wcs = cs;
    txn->wcsum_id = csum_id;
    txn->span_base = (size_t)sb.st_size;
    txn->wsize = (size_t)sb.st_size;
    txn->flushed = (size_t)sb.st_size;

    return ZS_OK;
}

/* Record a write.  val == NULL is a deletion; a non-NULL zero-length value is an
 * empty value, and the two are distinct states (A-1, F-14).
 *
 * The record is encoded and STREAMED here (C-8); only the key and the record's
 * offset are kept.  The ancestor is computed now, against the transaction's
 * snapshot: repack can change the file SET mid-transaction, but never which
 * generation range holds a key's previous version -- an output covers its
 * inputs' ranges (D-5a) and ancestors resolve by range (F-16c).  An intra-
 * transaction overwrite needs no special case: the previous version is in the
 * active file, so the ancestor is the active generation and F-17 omits it --
 * the same answer zsi_ancestor_for gives for a create. */
static int zsi_pend_set(struct zs_txn *txn, const char *key, size_t keylen,
                        const char *val, size_t vallen)
{
    size_t pos;
    bool found = (zsi_pend_search(txn, key, keylen, &pos) == ZS_OK);

    /* Bumped up front so every early return below still counts as a change:
     * an over-count costs one wasted reload, an under-count costs a cursor that
     * misses a write, and only one of those is a bug (D-14j). */
    txn->pend_seq++;

    int r = zsi_txn_stream_begin(txn);
    if (r != ZS_OK) return r;

    uint32_t anc = zsi_ancestor_for(txn->db, txn->snap, txn->wgen,
                                    key, keylen);
    bool store_anc = (anc != txn->wgen);            /* F-17 */

    size_t n = zsi_rec_encoded_len(keylen, vallen, val == NULL, store_anc);
    if (!n) return ZS_BADUSAGE;

    /* One record's transient encode buffer -- the only per-store allocation
     * proportional to the VALUE, gone before this returns. */
    char *rec = malloc(n);
    if (!rec) return ZS_INTERNAL;
    zsi_rec_encode(rec, txn->wcs, key, keylen, val, vallen, store_anc, anc);

    /* EVERYTHING fallible happens before the stream: a streamed record MUST
     * be indexed, or this handle's fold at commit would disagree with every
     * other reader's replay of the same span. */
    char *kcopy = NULL;
    if (!found) {
        kcopy = malloc(keylen);
        if (!kcopy) { free(rec); return ZS_INTERNAL; }
        memcpy(kcopy, key, keylen);

        if (txn->npend == txn->apend) {
            size_t want = txn->apend ? txn->apend * 2 : 16;
            struct zsi_pending *p = realloc(txn->pend, want * sizeof(*p));
            if (!p) { free(kcopy); free(rec); return ZS_INTERNAL; }
            txn->pend = p;
            txn->apend = want;
        }
    }

    size_t off = txn->wsize;
    r = zsi_txn_stream(txn, rec, n);
    free(rec);
    if (r != ZS_OK) {
        /* A short write may have left a torn record inside the span.  The
         * transaction is POISONED: commit must refuse, because a COMMIT
         * terminator over a torn record makes replay complete the file at
         * the tear (F-24) and lose the span. */
        txn->broken = true;
        free(kcopy);
        return r;
    }

    if (found) {
        txn->pend[pos].off = off;
        txn->pend[pos].len = n;
        return ZS_OK;
    }

    memmove(txn->pend + pos + 1, txn->pend + pos,
            (txn->npend - pos) * sizeof(*txn->pend));
    txn->pend[pos].key = kcopy;
    txn->pend[pos].keylen = keylen;
    txn->pend[pos].off = off;
    txn->pend[pos].len = n;
    txn->npend++;

    return ZS_OK;
}

/* The transaction arm of the per-file cursor (declared in PER-FILE CURSOR).
 *
 * Presenting pending records through the same interface as a file is what lets
 * D-14g work by sorting rather than by a special case in the merge. */
/* Resolve the transaction arm's key position to an index, NOW.
 *
 * Done on every load rather than once, because the pending array may have moved
 * under us since the last step: that is the whole point of holding a key
 * (D-14j-a).  Before the first step tkey is NULL, meaning "start at the
 * beginning"; afterwards it is the last key yielded and the next record is the
 * first strictly after it (D-14j-b). */
static size_t zsi_txn_cur_index(struct zsi_fcur *fc)
{
    size_t pos = 0;

    if (!fc->txn || !fc->tkey) return 0;        /* from the beginning */

    /* An exact hit means the array still holds the key we are positioned on.
     * After a step that key has been yielded, so the next record is the one
     * after it (D-14j-b); after a seek it has not, so it is the answer. */
    if (zsi_pend_search(fc->txn, fc->tkey, fc->tkeylen, &pos) == ZS_OK
        && fc->tstarted)
        pos++;

    return pos;
}

static unsigned long zsi_txn_seq(struct zs_txn *txn)
{
    return txn ? txn->pend_seq : 0;
}

static void zsi_txn_set_snapshot(struct zs_txn *txn, struct zsi_snapshot *snap)
{
    if (!txn) return;
    zsi_snapshot_release(&txn->snap);
    txn->snap = snap;
    txn->snap->refcount++;
}

/* The reverse mirror of zsi_txn_cur_index, D-14j-a intact: the position is a
 * key, resolved against the array as it stands NOW, so a store below the
 * position mid-walk is seen and one at or above it is already passed.
 * Returns false when the arm is exhausted. */
static bool zsi_txn_cur_index_rev(struct zsi_fcur *fc, size_t *ti)
{
    struct zs_txn *txn = fc->txn;
    size_t pos;

    if (!txn->npend) return false;

    if (!fc->tkey) {                            /* "after the last": the top */
        *ti = txn->npend - 1;
        return true;
    }

    bool exact = (zsi_pend_search(txn, fc->tkey, fc->tkeylen, &pos) == ZS_OK);

    /* An inclusive seek that landed on a present key answers with it; after a
     * yield (tstarted), or under an exclusive bound, the answer is strictly
     * below -- pos counts the keys strictly below tkey either way. */
    if (exact && !fc->tstarted && !fc->texclusive) {
        *ti = pos;
        return true;
    }
    if (pos == 0) return false;
    *ti = pos - 1;
    return true;
}

static int zsi_txn_cur_load(struct zsi_fcur *fc)
{
    struct zs_txn *txn = fc->txn;
    size_t ti;

    if (!txn) { fc->exhausted = true; return ZS_OK; }

    if (fc->reverse) {
        if (!zsi_txn_cur_index_rev(fc, &ti)) {
            fc->exhausted = true;
            return ZS_OK;
        }
    } else {
        ti = zsi_txn_cur_index(fc);
    }
    if (ti >= txn->npend) { fc->exhausted = true; return ZS_OK; }

    /* The record was streamed to the active file when it was stored (C-8);
     * decode it back through the transaction's accumulated mappings, whose
     * pointers live until the transaction ends (A-4). */
    struct zsi_pending *p = &txn->pend[ti];
    const char *b = zsi_txn_at(txn, p->off, p->len);
    if (!b
        || zsi_rec_decode(b, p->len, txn->wgen, &fc->cur) != ZS_OK) {
        fc->exhausted = true;
        return ZS_IOERROR;
    }
    fc->exhausted = false;

    return ZS_OK;
}

/* Seek to the first pending record with key >= the given key.
 *
 * Recorded as "the key before this one" rather than as a position: tstarted
 * stays false so the next load begins at the search result, which
 * zsi_txn_cur_index recomputes against the array as it is then. */
static void zsi_txn_cur_seek(struct zsi_fcur *fc, const char *key, size_t keylen)
{
    free(fc->tkey);
    fc->tkey = NULL;
    fc->tkeylen = 0;
    fc->tstarted = false;

    if (!fc->txn || !key || !keylen) return;

    fc->tkey = malloc(keylen);
    if (!fc->tkey) return;              /* degrades to "from the beginning" */
    memcpy(fc->tkey, key, keylen);
    fc->tkeylen = keylen;

    /* A seek lands ON the key if present, so the first load must NOT skip it:
     * tstarted false makes zsi_txn_cur_index return the search position rather
     * than one past it. */
}

/* Reverse seek: largest pending key <= (inclusive) or < the given key.  An
 * empty key means "after the last" here, since a reverse walk begins at the
 * top (D-14k). */
static void zsi_txn_cur_seek_rev(struct zsi_fcur *fc, const char *key,
                                 size_t keylen, bool inclusive)
{
    zsi_txn_cur_seek(fc, key, keylen);
    fc->texclusive = !inclusive;
}

/* Defined in CONVERSION.  D-12 requires a writer convert any non-active unordered
 * file BEFORE IT FINISHES, so the call belongs at the end of a commit -- which
 * means this one forward declaration, since conversion needs the write path's file
 * writer in turn. */
static int zsi_convert_pending(struct zs_db *db);
static int zsi_convert_one(struct zs_db *db, struct zsi_file *f);

/* Append bytes to a file descriptor, retrying a short write. */
static int zsi_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = ZS_WRITE(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return ZS_IOERROR;
        }
        if (n == 0) return ZS_IOERROR;
        off += (size_t)n;
    }

    return ZS_OK;
}

/* Decide the ancestor for a record about to be written (F-16, F-17).
 *
 * The ancestor is the START of the range of the file holding the superseded
 * record (F-16a) -- not its end.  Since start <= end, D-19's containment test then
 * errs toward RETAINING a tombstone rather than dropping one, so an imprecise
 * ancestor costs disk space and not correctness.
 *
 * F-17: it is omitted exactly when it equals the containing file's start.  That
 * single rule covers every row of the table -- a new key, a key rewritten in the
 * same file, and a key last written in an older file -- and F-17a notes why the
 * decoder never needs to know which case applied.
 *
 * Returns the ancestor generation.  The caller omits it when it equals active. */
static uint32_t zsi_ancestor_for(struct zs_db *db, struct zsi_snapshot *snap,
                                 uint32_t active_start,
                                 const char *key, size_t keylen)
{
    /* Search newest to oldest for the file that currently holds this key.  The
     * active file included: a key rewritten within it takes the active start,
     * which F-17 then omits. */
    for (size_t i = snap->nfiles; i-- > 0; ) {
        struct zsi_fcur fc;
        struct zsi_rec r;

        zsi_fcur_init_file(&fc, snap->files[i], db->compar);
        {
            int found = (zsi_fcur_find(&fc, key, keylen, &r) == ZS_OK);
            zsi_fcur_fini(&fc);
            if (found) return snap->files[i]->hdr.start;
        }
    }

    /* A key nobody holds is a create: its ancestor is its own generation, which
     * is the active file's start, and so is omitted. */
    return active_start;
}

/* Choose the file a write transaction will append to (D-9).
 *
 * While holding the write lock, a writer MUST either append to a CLEAN active
 * file, or create a new unordered file whose generation is exactly one higher,
 * write a valid header, and append there.
 *
 * D-9a: it moves on when the active file is not clean, or when it exceeds
 * rollover_size.  Rollover is cheap -- a new header and nothing else, since the
 * writer never appends a pointer section to an unordered file (D-11).
 *
 * D-10 and R-4: an unclean file is not repaired.  Because it is not clean the
 * writer moves on, so no chain is ever built on an untrustworthy boundary. */
static int zsi_writer_active(struct zs_db *db, int *fdp, uint32_t *genp)
{
    struct zsi_file *act = zsi_snapshot_active(db->snap);
    char name[ZSI_NAME_MAX], path[PATH_MAX];

    if (act && zsi_unordered_is_clean(act)
            && act->size < db->rollover_size) {
        snprintf(path, sizeof(path), "%s", act->fname);
        int fd = open(path, O_RDWR | O_APPEND);   /* RDWR: the streaming reads mmap this fd (A-4) */
        if (fd < 0) return ZS_IOERROR;
        *fdp = fd;
        *genp = act->hdr.start;
        return ZS_OK;
    }

    /* A new generation, one above the highest PRESENT (D-9b), so a superseded
     * file's generation is never reissued. */
    struct zsi_fileset fs;
    int r = zsi_fileset_scan(db->dir, &db->uuid, &fs);
    if (r != ZS_OK) return r;

    uint32_t next;
    r = zsi_fileset_next_gen(&fs, &next);
    zsi_fileset_fini(&fs);
    if (r != ZS_OK) return r;           /* ZS_FULL past 0xFFFFFFFF (D-9c) */

    r = zsi_create_active(db, next);
    if (r != ZS_OK) return r;

    r = zsi_db_refresh(db);
    if (r != ZS_OK) return r;

    zsi_name_format(name, db->uuid, next, 0);
    snprintf(path, sizeof(path), "%s/%s", db->dir, name);
    int fd = open(path, O_RDWR | O_APPEND);   /* RDWR: the streaming reads mmap this fd (A-4) */
    if (fd < 0) return ZS_IOERROR;

    *fdp = fd;
    *genp = next;
    return ZS_OK;
}

/* Commit: two gates (C-7).
 *
 *   1. append the span's data records, then fdatasync;
 *   2. append the terminator, then fdatasync again.
 *
 * fdatasync rather than fsync because appending changes only the metadata needed
 * to read the data back, which fdatasync is required to flush.
 *
 * Together the gates make "a valid terminator implies its data is durable" a
 * guarantee of the filesystem (C-7a).  The cost is two syncs per TRANSACTION, not
 * per record, which is the reason zs_txn_* exists alongside the single-operation
 * zs_db_* calls (C-7b).
 *
 * C-7c: ZS_NOSYNC omits BOTH gates.  Atomicity survives, because a torn tail is
 * still detectable (F-22); durability does not, and neither does C-7a's ordering
 * guarantee. */
static int zsi_txn_terminate(struct zs_txn *txn, bool rollback,
                             size_t *term_off_out, uint32_t *term_csum_out)
{
    struct zs_db *db = txn->db;
    size_t spanlen = txn->wsize - txn->span_base;

    int r = zsi_txn_flush(txn);
    if (r != ZS_OK) return r;

    /* The terminator's checksum covers the span's BYTES plus the terminator
     * (C-4f), the engine is one-shot -- and engine 2 is the caller's, so no
     * incremental variant is possible -- so the streamed span is re-read
     * through the mapping: one warm pass, where the buffered writer paid the
     * same pass over RAM.  A ROLLBACK's checksum matters as much as a
     * COMMIT's: an invalid rolled-back span completes the file early (F-24)
     * and costs everything after it. */
    const char *span = zsi_txn_at(txn, txn->span_base, spanlen);
    if (!span) return ZS_IOERROR;

    /* Gate 1: the data records, then a sync.  If this fails, no terminator
     * was written, so the transaction plainly did not happen (C-7a).  C-8: an
     * abort syncs NEITHER gate -- the next commit's own gate 1 is what orders
     * the ROLLBACK ahead of the next COMMIT terminator (C-8a). */
    if (!db->nosync && !rollback) {
        if (ZS_FDATASYNC(txn->wfd) < 0) return ZS_IOERROR;
    }

    char term[ZSI_TERMLEN_LONG];
    size_t termlen = zsi_term_encoded_len((uint64_t)spanlen);

    /* The engine is the ACTIVE FILE'S, not this handle's (A-6, F-5a) --
     * recorded at the first store, where the file was chosen. */
    zsi_term_encode(term, (uint64_t)spanlen, rollback, span,
                    txn->wcs, txn->wcsum_id);

    r = zsi_write_all(txn->wfd, term, termlen);
    if (r != ZS_OK) return r;
    txn->flushed += termlen;
    txn->wsize += termlen;

    /* Gate 2: the terminator, then a sync.  If THIS fails the terminator may
     * or may not be durable -- and either outcome is correct, so the error is
     * reported and nothing else is done.
     *
     * An implementation MUST NOT retry a failed sync and treat success as
     * evidence the data survived: a second call can succeed after the dirty
     * pages were discarded.  Retrying a failed syscall is the reflex, which
     * is why this says so at the call site rather than only in the spec. */
    if (!db->nosync && !rollback) {
        if (ZS_FDATASYNC(txn->wfd) < 0) return ZS_IOERROR;
    }

    /* Where the terminator landed and what checksum it carries (P-10).  Handed
     * back rather than recovered later because terminators are only ever found
     * by scanning FORWARD (F-20): a caller that skipped the replay -- which is
     * the whole point of the incremental index path -- has no other way to know,
     * and re-walking the span it just wrote would checksum it twice. */
    if (term_off_out) *term_off_out = txn->span_base + spanlen;
    if (term_csum_out) *term_csum_out = zsi_get32(term + termlen - 4);

    return ZS_OK;
}

static void zsi_txn_free(struct zs_txn *txn)
{
    if (!txn) return;

    zsi_pend_clear(txn);
    free(txn->pend);

    /* The mappings outlive every pointer this transaction's reads returned
     * (A-4), so HERE -- the end of the transaction -- is the first moment any
     * of them may go. */
    for (size_t i = 0; i < txn->nmaps; i++)
        munmap(txn->maps[i].base, txn->maps[i].len);
    free(txn->maps);
    free(txn->chunk);
    if (txn->wfd >= 0) close(txn->wfd);

    zsi_snapshot_release(&txn->snap);
    free(txn);
}

static int zsi_txn_begin(struct zs_db *db, bool shared, struct zs_txn **out)
{
    struct zs_txn *txn;
    int r;

    if (!shared) {
        r = zsi_check_writable(db);
        if (r != ZS_OK) return r;
        if (db->write_txn) return ZS_BADUSAGE;      /* one at a time */
    }

    txn = zsi_zmalloc(sizeof(*txn));
    if (!txn) return ZS_INTERNAL;
    txn->db = db;
    txn->readonly = shared;
    txn->wfd = -1;              /* no stream until the first store */

    if (!shared) {
        /* The write lock is held for the whole transaction (C-1).  A read
         * transaction takes NO lock (C-2, G-4). */
        r = zsi_lock_take(&db->locks, ZSI_LOCK_WRITE,
                          db->nonblocking ? ZS_NONBLOCKING : 0);
        if (r != ZS_OK) { free(txn); return r; }
        txn->holds_write_lock = true;

        /* Refresh only after the lock: the set may have changed while we waited,
         * and appending to a stale view of the active file would append to a file
         * another writer has already moved past. */
        r = zsi_db_refresh(db);
        if (r != ZS_OK) {
            zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
            free(txn);
            return r;
        }
    } else {
        /* C-4i: freshness is a property of BEGIN, not of open.  Without this
         * a long-lived handle reads the world as of its own last write --
         * found downstream, where one process created a mailbox and another,
         * holding the database open since before that commit, answered that
         * it did not exist.  Snapshot isolation (G-4) starts here and holds
         * for the transaction's lifetime, never the handle's. */
        r = zsi_db_freshen(db);
        if (r != ZS_OK) { free(txn); return r; }
    }

    txn->snap = db->snap;
    txn->snap->refcount++;

    if (!shared) db->write_txn = txn;

    *out = txn;
    return ZS_OK;
}

static int zsi_txn_commit(struct zs_txn *txn)
{
    struct zs_db *db = txn->db;
    int r;
    uint32_t gen = 0;
    size_t *offs = NULL, noffs = 0;
    size_t term_off = 0;
    uint32_t term_csum = 0;

    if (txn->readonly) { zsi_txn_free(txn); return ZS_OK; }

    /* Nothing streamed: no span, no syncs, no trace.  An empty transaction is
     * not an error.  (An open stream with nothing appended is the same case:
     * a failed first store may have chosen the file and then written
     * nothing, and the file is exactly as it was.) */
    if (txn->wfd < 0 || txn->wsize == txn->span_base) {
        db->write_txn = NULL;
        zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
        zsi_txn_free(txn);
        return ZS_OK;
    }

    /* A stream failure left a possibly-torn record inside the span.  A
     * COMMIT terminator over it would make replay complete the file at the
     * tear (F-24) and lose the whole span -- so refuse, and void the span
     * the way an abort does (C-8). */
    if (txn->broken) {
        (void)zsi_txn_terminate(txn, true, NULL, NULL);
        r = ZS_IOERROR;
        goto out;
    }

    /* The records are already on disk (C-8): everything between the two
     * gates is the terminator's. */
    r = zsi_txn_terminate(txn, false, &term_off, &term_csum);
    if (r != ZS_OK) goto out;

    gen = txn->wgen;
    struct zsi_file *act = zsi_snapshot_active(db->snap);

    /* The D-13b fold takes its offsets from the pending index: the FINAL
     * record per key, which is exactly what D-13a wants folded -- a version
     * this transaction itself overwrote is shadowed inside the span, and
     * replay resolves it by offset order (D-17b) just as the fold does by
     * skipping it here. */
    offs = malloc(txn->npend * sizeof(*offs));
    if (offs) {
        for (size_t i = 0; i < txn->npend; i++) offs[i] = txn->pend[i].off;
        noffs = txn->npend;
    }

    /* D-13b: a writer is a reader that also maintains the active file's index
     * INCREMENTALLY.  It already knows every record it appended, so it folds them
     * in rather than rescanning a file it is writing -- which matters because a
     * rescan is O(active file) per commit, making a bulk load quadratic.
     *
     * Only when nothing else holds the snapshot.  A cursor opened from the
     * same handle shares the object, and replacing its mapping or mutating its
     * index underneath would be exactly the in-place mutation of something a
     * reader is reading that G-6 forbids.  With a sharer present the fallback
     * is a full refresh, which builds a NEW snapshot and leaves the old one
     * untouched.
     *
     * TWO references are this commit's own: db->snap itself, and txn->snap,
     * re-pointed at the same object above.  Counting "sole holder" as one made
     * this branch dead code from the first commit -- every commit fell back to
     * a full refresh, and with no cache directory to seed it that refresh
     * replays the whole active file, making a bulk load exactly the quadratic
     * thing this branch exists to prevent.  Found by a mutant that targeted
     * the branch and could not be caught. */
    if (act && offs && db->snap->refcount == 2
        && act->hdr.start == gen && act->index) {
        r = zsi_file_remap(act);
        if (r == ZS_OK) {
            act->complete = act->size;
            /* The span just written ends the file, so its terminator is the one
             * at the complete point (P-10).  Taken from the writer rather than
             * rediscovered, since skipping the replay is the point of this
             * branch and terminators are only found by scanning forward. */
            act->last_term_off  = term_off;
            act->last_term_csum = term_csum;
            for (size_t i = 0; i < noffs && r == ZS_OK; i++)
                r = zsi_index_insert(act->index, db->compar, offs[i]);
        }
        /* A failure here is not fatal to the commit -- the data is on disk and
         * durable.  Fall back to a rebuild so this handle's view is correct. */
        if (r != ZS_OK) r = zsi_db_refresh(db);

        /* P-13, P-15.  The writer publishes on the same threshold rule a reader
         * uses, and a failure never fails the commit: the records are already
         * durable, and a cache is not something a caller can act on.  Reported
         * once per handle so a broken cache directory is visible without
         * becoming noise on every commit.
         *
         * D-25e: no table for a file this commit is about to seal.  A table
         * covers only unordered files (P-1), so it would be born stale and
         * swept at the next opportunity (P-16). */
        bool sealing = act->size >= db->rollover_size;
        if (r == ZS_OK && db->index_dir && !sealing) {
            struct zsi_idxcfg cfg = { db->index_dir, db->index_threshold,
                                      db->index_local };
            int pr = zsi_idx_publish(act, &cfg, db->compar, db->nocsum);

            if (pr != ZS_OK && pr != ZS_DONE && !db->idx_publish_warned) {
                db->idx_publish_warned = true;
                db->error("could not publish a pointer table; continuing "
                          "without the index cache", "dir=<%s>", db->index_dir);
            }
        }
    } else {
        r = zsi_db_refresh(db);
    }

    /* D-12: a writer that finds a non-active unordered file MUST convert it before
     * it finishes.  This is what holds the steady state at exactly one unordered
     * file (D-12a), so a snapshot normally replays only the active one.
     *
     * A conversion failure does not fail the commit: the records are already
     * durable, and an unconverted file is a performance matter that the next
     * writer retries.  Reporting it would turn a successful commit into an error
     * the caller cannot act on. */
    if (r == ZS_OK) {
        int cr = zsi_convert_pending(db);
        (void)cr;
    }

    /* D-25d: a commit that grew the active file past rollover_size seals it
     * now, while the write lock is still held, so the conversion cost lands on
     * the transaction that incurred it rather than on the next writer -- the
     * one case D-12d's bound cannot cover, since a span is never split across
     * files and a bulk load writes its whole transaction as one span.  After
     * zsi_convert_pending, so D-12b's oldest-first order holds.  Never fatal:
     * the records are durable, and an unsealed oversized file is exactly what
     * D-9a's rollover already recovers at the next commit. */
    if (r == ZS_OK) {
        struct zsi_file *oversized = zsi_snapshot_active(db->snap);
        if (oversized && oversized->hdr_valid
            && oversized->size >= db->rollover_size
            && oversized->complete > ZSI_HEADER_LEN) {
            int sr = zsi_convert_one(db, oversized);
            if (sr == ZS_OK) (void)zsi_db_refresh(db);
        }
    }

out:
    free(offs);
    db->write_txn = NULL;
    zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
    zsi_txn_free(txn);
    return r;
}

/* Abort (C-8, F-21).
 *
 * The records were streamed to the active file as they were stored, so the
 * abort MUST void them: without a ROLLBACK terminator, a later commit's span
 * would begin before the aborted records and enclose them, making them live.
 * The ROLLBACK syncs neither gate (C-8) -- the next commit's own first gate
 * orders it ahead of the next COMMIT terminator (C-8a), and a crash that
 * loses it leaves a torn span F-22 already discards, which is the same
 * outcome reached the slower way.
 *
 * A failure writing the ROLLBACK is likewise left as a torn span; it is not
 * reported, because the caller is already abandoning the transaction and
 * there is nothing they could do differently. */
static int zsi_txn_abort(struct zs_txn *txn)
{
    struct zs_db *db = txn->db;

    if (txn->readonly) { zsi_txn_free(txn); return ZS_OK; }

    if (txn->wfd >= 0 && txn->wsize > txn->span_base)
        (void)zsi_txn_terminate(txn, true, NULL, NULL);

    db->write_txn = NULL;
    zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
    zsi_txn_free(txn);
    return ZS_OK;
}

/********** CONVERSION *************/

/* D-12, immediate conversion.
 *
 * A writer that finds a NON-ACTIVE unordered file MUST convert it to its
 * single-generation in-order form -- <uuid>-N becomes <uuid>-N-N -- before it
 * finishes, oldest first, and MUST NOT go further: it does not merge in-order
 * files, which is the repacker's job (D-16).
 *
 * D-12a: this is what keeps the steady state at EXACTLY ONE unordered file, the
 * active one, so a snapshot normally replays only that file and nothing else.  A
 * non-active unordered file exists only transiently -- between a rollover and the
 * next writer's conversion, or after a crash left an unclean file behind.
 *
 * D-12d: each conversion is bounded by rollover_size, so a writer's extra cost is
 * bounded and predictable rather than proportional to the database. */

/* D-20a: a staging file MUST be created with O_CREAT|O_EXCL, advancing <n> until
 * it succeeds.
 *
 * A process identifier is NOT unique on shared storage -- two hosts readily have
 * the same pid -- and two processes writing one staging file would produce an
 * interleaved output that is then renamed into place as though complete.  O_EXCL
 * costs nothing and removes the case. */
static int zsi_staging_open(struct zs_db *db, char *name_out, int *fdp)
{
    for (unsigned n = 0; n < 10000; n++) {
        char path[PATH_MAX];

        zsi_staging_name(name_out, n);
        snprintf(path, sizeof(path), "%s/%s", db->dir, name_out);

        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) { *fdp = fd; return ZS_OK; }
        if (errno != EEXIST) return ZS_IOERROR;
    }

    return ZS_IOERROR;
}

/* Remove a data file (D-23).
 *
 * MUST be done holding the REMOVE lock, and only after verifying that a complete
 * set of files exists WITHOUT it.  Verification and removal happen under one
 * unbroken hold, so the set cannot change in between.
 *
 * If verification fails the file is left alone: leaking a file costs disk space,
 * removing a needed one costs the database.  That asymmetry is the whole reason
 * this is not just an unlink.
 *
 * C-6a: no directory sync after unlink.  If a removed name reappears after a
 * crash it is a file an enclosing range already supersedes, which readers ignore
 * and a later pass removes again. */
static int zsi_remove_file(struct zs_db *db, const char *name)
{
    struct zsi_fileset fs;
    int r;

    r = zsi_lock_take(&db->locks, ZSI_LOCK_REMOVE,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    r = zsi_fileset_scan(db->dir, &db->uuid, &fs);
    if (r != ZS_OK) goto out;

    /* The interval the database currently covers.  Tiling ALONE is not a
     * sufficient precondition, which is worth spelling out because it is a
     * tempting reading of D-23: D-6 measures completeness "from the oldest
     * SURVIVING generation", so deleting the oldest file merely raises that floor
     * and the remainder tiles perfectly while the data is gone.  With files
     * {1-2, 3}, removing 1-2 leaves {3}, which tiles.
     *
     * So the real precondition is that the candidate be SUPERSEDED: the set
     * without it must tile AND still span the same interval.  That is what "a
     * complete set of files exists without it" has to mean, since a set covering
     * less is not the same set. */
    uint32_t lowest = 0, highest = 0;
    for (size_t i = 0; i < fs.nall; i++) {
        uint32_t s0 = fs.all[i].start;
        uint32_t e0 = fs.all[i].end ? fs.all[i].end : fs.all[i].start;
        if (i == 0 || s0 < lowest) lowest = s0;
        if (e0 > highest) highest = e0;
    }

    /* Drop the candidate, then ask.  Doing it in this order -- rather than
     * unlinking and re-scanning -- is what makes a failed verification harmless. */
    size_t w = 0;
    for (size_t i = 0; i < fs.nall; i++)
        if (strcmp(fs.all[i].name, name) != 0) fs.all[w++] = fs.all[i];

    if (w == fs.nall) { r = ZS_NOTFOUND; zsi_fileset_fini(&fs); goto out; }
    fs.nall = w;

    r = zsi_fileset_resolve(&fs);

    if (r == ZS_OK) {
        uint32_t nlow = 0, nhigh = 0;
        for (size_t i = 0; i < fs.nall; i++) {
            uint32_t s0 = fs.all[i].start;
            uint32_t e0 = fs.all[i].end ? fs.all[i].end : fs.all[i].start;
            if (i == 0 || s0 < nlow) nlow = s0;
            if (e0 > nhigh) nhigh = e0;
        }
        if (fs.nall == 0 || nlow != lowest || nhigh != highest) r = ZS_AGAIN;
    }

    zsi_fileset_fini(&fs);

    if (r != ZS_OK) {
        /* Still needed.  Leave it: a leaked file is reclaimed by a later pass. */
        r = ZS_AGAIN;
        goto out;
    }

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", db->dir, name);
    if (ZS_UNLINK(path) < 0 && errno != ENOENT) r = ZS_IOERROR;

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_REMOVE);
    return r;
}

/* Write an in-order file holding the records at the given offsets of src, in the
 * order given, and rename it into place (D-21, C-3).
 *
 * The output has NO spans and NO terminators: every record in it is live by
 * construction, and it is written whole under a temporary name and renamed only
 * once finished, so a commit record would assert nothing not already guaranteed
 * (section 4.9, F-24a).
 *
 * Ancestors are copied through VERBATIM.  For a single-generation conversion the
 * containing file's start is unchanged (N becomes N-N), so a record whose ancestor
 * was omitted stays omitted and one that stored an ancestor keeps it -- F-17 is
 * satisfied without recomputation (F-16c, D-17a). */
static int zsi_write_inorder(struct zs_db *db, struct zsi_file *src,
                             const size_t *offs, size_t n,
                             uint32_t start, uint32_t end)
{
    char sname[ZSI_NAME_MAX], fname[ZSI_NAME_MAX];
    char spath[PATH_MAX], fpath[PATH_MAX];
    char hdr[ZSI_HEADER_LEN];
    struct zsi_header h;
    uint64_t *ptrs = NULL;
    char *recs = NULL;
    size_t recslen = 0, alloc = 0;
    int fd = -1, r;

    zs_csum *cs = zsi_csum_for_id(db->create_csum_id, db->external_csum);
    if (!cs) return ZS_BADUSAGE;

    if (n) {
        ptrs = malloc(n * sizeof(*ptrs));
        if (!ptrs) return ZS_INTERNAL;
    }

    /* F-32c: a record's checksum is under its FILE's engine.  While the
     * output engine matches the source's, a byte-for-byte copy carries a
     * valid checksum with it; when the two differ -- an engine-0 file sealed
     * by an engine-1 handle, say -- the copied checksum validates for
     * nobody, so every record is re-encoded under the output engine instead.
     * D-20b verified the source before this ran.  Re-encoding canonicalises
     * the form (F-15) but copies the ancestor DECISION verbatim (F-17,
     * D-17a): stored stays stored with its stored value, omitted stays
     * omitted, nothing is renumbered. */
    bool reencode = (src->csum_id != db->create_csum_id);

    /* Lay the records out contiguously, recording each one's offset in the
     * output.  A record is copied byte for byte from the source, which is what
     * makes ancestors verbatim rather than recomputed. */
    for (size_t i = 0; i < n; i++) {
        const char *b = zsi_file_at(src, offs[i], 1);
        struct zsi_rec rec;
        size_t outlen;

        if (!b) { r = ZS_BADFORMAT; goto fail; }
        if (zsi_rec_decode(b, src->size - offs[i], src->hdr.start, &rec)
            != ZS_OK) { r = ZS_BADFORMAT; goto fail; }

        bool store_anc = (rec.type & ZSI_HASANCESTOR) != 0;

        outlen = rec.len;
        if (reencode) {
            outlen = zsi_rec_encoded_len(rec.keylen, rec.vallen,
                                         rec.val == NULL, store_anc);
            if (!outlen) { r = ZS_BADFORMAT; goto fail; }
        }

        if (recslen + outlen > alloc) {
            size_t want = alloc ? alloc * 2 : 8192;
            while (want < recslen + outlen) want *= 2;
            char *q = realloc(recs, want);
            if (!q) { r = ZS_INTERNAL; goto fail; }
            recs = q;
            alloc = want;
        }

        ptrs[i] = (uint64_t)(ZSI_HEADER_LEN + recslen);
        if (reencode)
            zsi_rec_encode(recs + recslen, cs, rec.key, rec.keylen,
                           rec.val, rec.vallen, store_anc, rec.ancestor);
        else
            memcpy(recs + recslen, b, rec.len);
        recslen += outlen;
    }

    memset(&h, 0, sizeof(h));
    h.version_read = ZSI_VERSION_READ;
    h.version_write = ZSI_VERSION_WRITE;
    h.flags = (uint16_t)db->create_csum_id;
    memcpy(h.uuid, db->uuid, 16);
    h.start = start;
    h.end = end;
    memcpy(h.compar_name, db->compar_name, ZSI_COMPAR_NAME_LEN);
    zsi_header_encode(hdr, &h, cs);

    char *sec = NULL;
    size_t seclen = 0;
    uint32_t rc = cs(recs ? recs : "", recslen);
    r = zsi_ptrs_build(ptrs, n, ZSI_HEADER_LEN + recslen, rc, cs, &sec, &seclen);
    if (r != ZS_OK) goto fail;

    r = zsi_staging_open(db, sname, &fd);
    if (r != ZS_OK) { free(sec); goto fail; }

    r = zsi_write_all(fd, hdr, sizeof(hdr));
    if (r == ZS_OK && recslen) r = zsi_write_all(fd, recs, recslen);
    if (r == ZS_OK) r = zsi_write_all(fd, sec, seclen);
    free(sec);

    /* The file's contents durable BEFORE the rename, or the name could exist
     * pointing at a partial file after a crash. */
    if (r == ZS_OK && !db->nosync && ZS_FDATASYNC(fd) < 0) r = ZS_IOERROR;
    close(fd);
    fd = -1;

    snprintf(spath, sizeof(spath), "%s/%s", db->dir, sname);
    if (r != ZS_OK) { unlink(spath); goto fail; }

    /* C-3: a file is published by writing it under a staging name, then renaming.
     * Readers see it only once complete, and the rename is the instant it joins
     * the file set.  C-1b: publishing needs NO LOCK -- rename is atomic, and a
     * conversion's output [c..c] and a repack's output [a..b] with c > b are
     * disjoint, so they cannot interfere in either order. */
    zsi_name_format(fname, db->uuid, start, end);
    snprintf(fpath, sizeof(fpath), "%s/%s", db->dir, fname);
    if (ZS_RENAME(spath, fpath) < 0) { ZS_UNLINK(spath); r = ZS_IOERROR; goto fail; }

    /* C-6: fdatasync the DIRECTORY after renaming an output into place, otherwise
     * the name may be absent after a crash even though the contents are durable.
     * Reported rather than fatal: a lost name leaves the inputs in place, which
     * still tile, so a later pass simply converts again (C-6a, R-5). */
    if (!db->nosync) {
        int dfd = open(db->dir, O_RDONLY);
        if (dfd >= 0) {
            if (ZS_FDATASYNC(dfd) < 0)
                db->error("directory sync failed after publishing a conversion",
                          "dir=<%s>", db->dir);
            close(dfd);
        }
    }

    free(recs);
    free(ptrs);
    return ZS_OK;

fail:
    if (fd >= 0) close(fd);
    free(recs);
    free(ptrs);
    return r;
}

/* Convert one non-active unordered file to its single-generation in-order form. */
static int zsi_convert_one(struct zs_db *db, struct zsi_file *f)
{
    struct zsi_index_cur ic;
    struct zsi_rec rec;
    size_t *offs = NULL, n = 0, alloc = 0;
    int r;

    if (!f->index) return ZS_INTERNAL;

    /* D-20b: verify the span chain over everything about to be copied, with
     * checksums ON regardless of db->nocsum -- that flag is scoped to reads
     * (F-5e), and this is a write: the output gets a fresh records checksum
     * computed over the copy, so converting an unverified span would launder
     * corruption into a file that validates.  Replayed on a SCRATCH copy,
     * because the walk records its progress into the struct it is given
     * (complete, the last terminator) and this walk's answer must not replace
     * the one the handle's readers are using; nothing in the copy is owned or
     * released.  A verified walk stopping short of f->complete means a span
     * this handle admitted fails its checksum. */
    {
        struct zsi_file scratch = *f;
        r = zsi_unordered_replay(&scratch, ZSI_HEADER_LEN, false, NULL, NULL);
        if (r != ZS_OK) return r;
        if (scratch.complete < f->complete) {
            db->error("unordered file fails a span checksum; not converted",
                      "file=<%s>", f->fname);
            return ZS_BADCHECKSUM;
        }
    }

    /* The index already holds exactly the live records, newest per key, in key
     * order (D-13a) -- which is precisely what the output needs (D-20: inputs are
     * iterated in key order, from the same private index any reader builds; there
     * is nothing conversion-specific about it). */
    zsi_index_cur_seek_first(&ic);
    for (;;) {
        size_t off;
        if (zsi_index_cur_get(f->index, db->compar, &ic, &rec, &off) != ZS_OK)
            break;

        if (n == alloc) {
            size_t want = alloc ? alloc * 2 : 256;
            size_t *p = realloc(offs, want * sizeof(*p));
            if (!p) { free(offs); return ZS_INTERNAL; }
            offs = p;
            alloc = want;
        }
        offs[n++] = off;

        zsi_index_cur_next(f->index, db->compar, &ic);
    }

    r = zsi_write_inorder(db, f, offs, n, f->hdr.start, f->hdr.start);
    free(offs);
    if (r != ZS_OK) return r;

    /* Retire the input under the remove lock (D-23).  D-12c: conversion never
     * takes the repack lock -- it renames its output in without any lock and holds
     * remove only momentarily, so a writer never waits on a repack. */
    char iname[ZSI_NAME_MAX];
    zsi_name_format(iname, db->uuid, f->hdr.start, 0);
    r = zsi_remove_file(db, iname);

    /* ZS_AGAIN means the set still needs it, which cannot happen here -- the
     * output covers exactly its range -- but a leaked input is harmless and a
     * later pass removes it, so this is not fatal. */
    if (r == ZS_AGAIN) r = ZS_OK;

    return r;
}

/* Convert every non-active unordered file, OLDEST FIRST (D-12b).
 *
 * Oldest first keeps the generation range split into a prefix of in-order files
 * followed by a suffix of unordered ones, the last of which is the active file.
 * Converting out of order would leave an unordered file stranded between in-order
 * ones, and since only adjacent files may be merged (D-19), that hole would block
 * the repacker's cascade until it was filled (D-12b, D-16c). */
static int zsi_convert_pending(struct zs_db *db)
{
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    for (;;) {
        struct zsi_file *act = zsi_snapshot_active(db->snap);
        struct zsi_file *victim = NULL;

        /* The oldest unordered file that is not the active one. */
        for (size_t i = 0; i < db->snap->nfiles; i++) {
            struct zsi_file *f = db->snap->files[i];
            if (f == act) continue;
            if (!zsi_file_is_unordered(f)) continue;
            victim = f;
            break;
        }

        if (!victim) return ZS_OK;

        /* A file with an invalid header cannot be converted -- there is nothing
         * to read.  D-10a already makes that an error at open for a non-active
         * file, so reaching here means the active file, which we skip. */
        if (!victim->hdr_valid) return ZS_OK;

        r = zsi_convert_one(db, victim);
        if (r != ZS_OK) return r;

        r = zsi_db_refresh(db);
        if (r != ZS_OK) return r;
    }
}

/* D-25: convert the ACTIVE file, on demand, under the write lock.
 *
 * D-12 skips the active file because another writer may be appending to it.  A
 * caller holding the write lock has excluded exactly that, so the exception does
 * not apply -- which is both why this is safe and why it must never be reachable
 * without the lock.
 *
 * NO replacement active file is created (D-25a).  A conversion output covers its
 * input's range (D-5a), so afterwards the newest file is in-order,
 * zsi_snapshot_active returns NULL -- a state every reader already handles -- and
 * the next write creates a new generation (D-9b).  Rolling over first and
 * converting second reaches the same layout while consuming a generation per
 * seal, and generations are finite (D-9c); test_seal_creates_no_new_generation
 * is what holds that apart. */
static int zsi_seal(struct zs_db *db)
{
    struct zsi_file *act;
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    /* One writer at a time: sealing under an open write transaction would
     * convert a file that transaction is about to append to. */
    if (db->write_txn) return ZS_BADUSAGE;

    r = zsi_lock_take(&db->locks, ZSI_LOCK_WRITE,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    /* Refresh under the lock: another writer may have rolled over while we
     * waited, and sealing a stale view would convert a file already superseded. */
    r = zsi_db_refresh(db);
    if (r != ZS_OK) goto out;

    act = zsi_snapshot_active(db->snap);

    /* D-25b's three no-ops, none of them errors. */
    if (!act) goto out;                             /* already sealed */

    if (!act->hdr_valid) {                          /* D-10 */
        db->error("active file has an invalid header; nothing to seal",
                  "file=<%s>", act->fname);
        goto out;
    }

    if (act->complete <= ZSI_HEADER_LEN) goto out;  /* no valid spans */

    r = zsi_convert_one(db, act);
    if (r != ZS_OK) goto out;

    r = zsi_db_refresh(db);

    /* A seal is a natural moment to catch up any stranded unordered file, and
     * costs nothing when there is none (D-12).  Not fatal for the same reason it
     * is not fatal at commit: the data is already durable and an unconverted
     * file is a performance matter. */
    if (r == ZS_OK) (void)zsi_convert_pending(db);

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_WRITE);
    return r;
}

/********** REPACK *************/

/* D-15/D-16: the repacker works ONLY on in-order files.  It never touches the
 * active file, and never touches an unordered file at all -- converting those is
 * the writer's job (D-12).
 *
 * D-16a: the two jobs divide by whether a file has an `end`, which is what makes
 * them independent (C-1a).  A writer's conversion is bounded by rollover_size and
 * runs inline; the repacker's cascade is unbounded and runs out of band.  A file
 * becomes visible to the repacker precisely when the writer has finished with it,
 * so a writer never waits on a repack. */

/* Input selection (D-16):
 *
 *   1. start from the newest in-order file;
 *   2. if the result would be larger than the next lowest in-order file, include
 *      that file too and repeat;
 *   3. stop when every file is included or the next lowest in-order file is
 *      larger.
 *
 * This yields geometrically sized in-order files and amortised O(log n) rewrites
 * per record.  D-16c: because conversion keeps in-order files as a contiguous
 * prefix, the inputs are always adjacent and the cascade is never blocked by an
 * unordered file sitting between two candidates.
 *
 * Returns the number selected, filling *first with the index of the lowest.  The
 * selection is always a contiguous run ending at the newest in-order file. */
static size_t zsi_repack_select(struct zsi_snapshot *snap, size_t *first)
{
    /* The in-order prefix: files [0, nio). */
    size_t nio = 0;
    while (nio < snap->nfiles && !zsi_file_is_unordered(snap->files[nio])) nio++;

    if (nio < 2) { *first = 0; return 0; }      /* nothing to merge */

    size_t lo = nio - 1;                        /* the newest in-order file */
    size_t total = snap->files[lo]->size;

    while (lo > 0) {
        size_t next = snap->files[lo - 1]->size;

        /* At least as large, NOT strictly larger (D-16d).  Rollover produces files
         * of near-identical size, so equality is the common case: with a strict
         * comparison neither the include rule nor the stop rule fires, nothing ever
         * merges, and the file count grows without bound -- which defeats the
         * policy entirely.  Merging equals is also what produces the geometric
         * progression, two files of size s becoming one of 2s. */
        if (total < next) break;
        lo--;
        total += next;
    }

    *first = lo;
    return nio - lo;
}

/* D-24: whether D-16 currently has work. */
static bool zsi_should_repack(struct zsi_snapshot *snap)
{
    size_t first;
    return zsi_repack_select(snap, &first) >= 2;
}

/* One key's versions, gathered across the inputs in D-17b's total order.
 *
 * D-17b: a repack MUST consider the versions of a key oldest to newest --
 *
 *   1. across files, by increasing start generation.  The tiling invariant (D-6)
 *      means ranges never overlap, so this is total;
 *   2. within one unordered file, by increasing offset among committed spans;
 *   3. an in-order file holds one record per key, so there is nothing to order.
 *
 * V1 is the first version in that order and V3 the last.  The emitted record takes
 * V3's VALUE and V1's ANCESTOR -- from those records specifically, and by no other
 * route.  Getting this wrong silently emits the wrong value or the wrong ancestor,
 * which is why T-7 tests the ordering directly rather than only its consequences. */
struct zsi_merge_key {
    struct zsi_rec v1;          /* oldest version: its ancestor is emitted */
    struct zsi_rec v3;          /* newest version: its value is emitted */
    bool           have;
};

/* Merge the selected inputs into one in-order file (D-17).
 *
 * The output holds EXACTLY ONE record per key, built from the live records of all
 * inputs.  Since the repacker only ever merges in-order files, and each holds one
 * record per key, "within one file" ordering never arises -- but the code walks
 * the inputs in increasing start generation regardless, because that is what makes
 * the order total and what a future pairwise merge would still need. */
static int zsi_repack_merge(struct zs_db *db, struct zsi_snapshot *snap,
                            size_t first, size_t count)
{
    struct zsi_fcur *fc = NULL;
    char *recs = NULL;
    uint64_t *ptrs = NULL;
    size_t recslen = 0, alloc = 0, nptrs = 0, aptrs = 0;
    char sname[ZSI_NAME_MAX], fname[ZSI_NAME_MAX];
    char spath[PATH_MAX], fpath[PATH_MAX];
    char hdr[ZSI_HEADER_LEN];
    struct zsi_header h;
    int fd = -1, r = ZS_OK;

    uint32_t out_start = snap->files[first]->hdr.start;
    uint32_t out_end   = snap->files[first + count - 1]->hdr.end;
    if (out_end == 0) return ZS_INTERNAL;       /* never an unordered input */

    zs_csum *cs = zsi_csum_for_id(db->create_csum_id, db->external_csum);
    if (!cs) return ZS_BADUSAGE;

    /* D-20b: verify every input's records-region checksum before copying a
     * byte of it.  NOT gated on db->nocsum -- that flag is scoped to reads
     * (F-5e), and this is a write: the output gets a fresh checksum computed
     * over whatever is copied, and D-23 then removes the inputs, so copying
     * an unverified body would launder corruption into a file that validates
     * while destroying the only evidence.  Failing here costs nothing: the
     * inputs stay, the database still reads, salvage still works. */
    for (size_t i = 0; i < count; i++) {
        r = zsi_ptrs_verify_records(snap->files[first + i]);
        if (r != ZS_OK) {
            db->error("repack input fails its records checksum; not merged",
                      "file=<%s>", snap->files[first + i]->fname);
            return r;
        }
    }

    fc = zsi_zmalloc(count * sizeof(*fc));
    if (!fc) return ZS_INTERNAL;

    /* Inputs are iterated in key order, from the pointer section (D-20). */
    for (size_t i = 0; i < count; i++) {
        zsi_fcur_init_file(&fc[i], snap->files[first + i], db->compar);
        r = zsi_fcur_seek_first(&fc[i]);
        if (r != ZS_OK) goto out;
    }

    for (;;) {
        /* The least key still present across the inputs. */
        const char *bestk = NULL;
        size_t bestkl = 0;
        for (size_t i = 0; i < count; i++) {
            if (fc[i].exhausted) continue;
            if (!bestk || db->compar(fc[i].cur.key, fc[i].cur.keylen,
                                     bestk, bestkl) < 0) {
                bestk = fc[i].cur.key;
                bestkl = fc[i].cur.keylen;
            }
        }
        if (!bestk) break;

        /* Gather this key's versions in D-17b's order.  The inputs are indexed by
         * increasing start generation, so ascending i IS oldest to newest. */
        struct zsi_merge_key mk;
        memset(&mk, 0, sizeof(mk));

        for (size_t i = 0; i < count; i++) {
            if (fc[i].exhausted) continue;
            if (db->compar(fc[i].cur.key, fc[i].cur.keylen, bestk, bestkl) != 0)
                continue;

            if (!mk.have) { mk.v1 = fc[i].cur; mk.have = true; }
            mk.v3 = fc[i].cur;

            r = zsi_fcur_next(&fc[i]);
            if (r != ZS_OK) goto out;
        }

        if (!mk.have) break;

        /* D-19: a key is removed entirely IF AND ONLY IF its latest version is a
         * deletion AND V1's ancestor lies inside the output range -- its whole
         * lifespan from create through update to delete is contained.
         *
         * Otherwise the tombstone MUST be retained, because an older file may
         * still hold the key and dropping it would resurrect the value.
         *
         * D-19a: the record is written even when a NEWER file already shadows the
         * key.  Being shadowed does not permit dropping it; only D-19 does.  The
         * retained record carries the chain's reach, which no other file records.
         * This looks like a missed optimisation and is not -- T-7 constructs the
         * resurrection that follows from removing it. */
        bool v3_delete = zsi_rec_is_delete(&mk.v3);
        bool contained = (mk.v1.ancestor >= out_start);

        if (v3_delete && contained) continue;   /* drop the key */

        /* D-18's table: the ancestor-omitting form when the chain begins inside
         * the output range, the ancestor-storing form otherwise, carrying V1's
         * ancestor -- copied verbatim, never renumbered (D-17a). */
        bool store_anc = !contained;
        uint32_t anc = mk.v1.ancestor;

        const char *val = mk.v3.val;
        size_t vallen = mk.v3.vallen;
        size_t n = zsi_rec_encoded_len(mk.v3.keylen, vallen, val == NULL,
                                       store_anc);
        if (!n) { r = ZS_INTERNAL; goto out; }

        if (recslen + n > alloc) {
            size_t want = alloc ? alloc * 2 : 8192;
            while (want < recslen + n) want *= 2;
            char *q = realloc(recs, want);
            if (!q) { r = ZS_INTERNAL; goto out; }
            recs = q;
            alloc = want;
        }
        if (nptrs == aptrs) {
            size_t want = aptrs ? aptrs * 2 : 256;
            uint64_t *q = realloc(ptrs, want * sizeof(*q));
            if (!q) { r = ZS_INTERNAL; goto out; }
            ptrs = q;
            aptrs = want;
        }

        ptrs[nptrs++] = (uint64_t)(ZSI_HEADER_LEN + recslen);
        zsi_rec_encode(recs + recslen, cs, mk.v3.key, mk.v3.keylen, val,
                       vallen, store_anc, anc);
        recslen += n;
    }

    /* D-22: the output may legitimately contain ZERO records, in F-26g's form --
     * every key deleted, or a create and its delete repacked together.  It MUST
     * still be written, so the generation range stays tiled (D-6).  It is cheap and
     * short-lived: an empty file violates D-16's size relation maximally, so the
     * next repack absorbs it. */

    memset(&h, 0, sizeof(h));
    h.version_read = ZSI_VERSION_READ;
    h.version_write = ZSI_VERSION_WRITE;
    h.flags = (uint16_t)db->create_csum_id;
    memcpy(h.uuid, db->uuid, 16);
    h.start = out_start;
    h.end = out_end;
    memcpy(h.compar_name, db->compar_name, ZSI_COMPAR_NAME_LEN);
    zsi_header_encode(hdr, &h, cs);

    char *sec = NULL;
    size_t seclen = 0;
    uint32_t rc = cs(recs ? recs : "", recslen);
    r = zsi_ptrs_build(ptrs, nptrs, ZSI_HEADER_LEN + recslen, rc, cs,
                       &sec, &seclen);
    if (r != ZS_OK) goto out;

    r = zsi_staging_open(db, sname, &fd);
    if (r != ZS_OK) { free(sec); goto out; }

    r = zsi_write_all(fd, hdr, sizeof(hdr));
    if (r == ZS_OK && recslen) r = zsi_write_all(fd, recs, recslen);
    if (r == ZS_OK) r = zsi_write_all(fd, sec, seclen);
    free(sec);
    if (r == ZS_OK && !db->nosync && ZS_FDATASYNC(fd) < 0) r = ZS_IOERROR;
    close(fd);
    fd = -1;

    snprintf(spath, sizeof(spath), "%s/%s", db->dir, sname);
    if (r != ZS_OK) { unlink(spath); goto out; }

    /* D-21: renamed to a name covering the ENTIRE range of every input, only once
     * complete.  C-1b: publishing needs no lock. */
    zsi_name_format(fname, db->uuid, out_start, out_end);
    snprintf(fpath, sizeof(fpath), "%s/%s", db->dir, fname);
    if (ZS_RENAME(spath, fpath) < 0) { ZS_UNLINK(spath); r = ZS_IOERROR; goto out; }

    if (!db->nosync) {                          /* C-6 */
        int dfd = open(db->dir, O_RDONLY);
        if (dfd >= 0) {
            if (ZS_FDATASYNC(dfd) < 0)
                db->error("directory sync failed after publishing a repack output",
                          "dir=<%s>", db->dir);
            close(dfd);
        }
    }

out:
    if (fd >= 0) close(fd);
    free(recs);
    free(ptrs);
    free(fc);
    return r;
}

/* One repack: select, merge, publish, retire the inputs.
 *
 * D-16b: a cascade writes ONE output for the whole selected set, not one per pair.
 * A single invocation is therefore unbounded in duration, which the spec records
 * as an open item -- writing continues throughout regardless, because the repack
 * lock and the write lock never contend (C-1a). */
/* Merge snap->files[first .. first+count) into one, retire the inputs, refresh.
 *
 * THE single merge entry point.  Both zsi_repack and zsi_compact reach the merge
 * through here, so D-17 to D-23 are implemented once: a second call site for
 * zsi_repack_merge is exactly how two sets of retention rules would drift apart,
 * and D-19's tombstone rule is the one nobody would notice diverging.
 *
 * The caller holds the repack lock and has refreshed under it. */
static int zsi_repack_run(struct zs_db *db, size_t first, size_t count)
{
    int r;

    /* Remember the input names before the merge: the snapshot is replaced by the
     * refresh below, and the names are what D-23 needs. */
    char (*names)[ZSI_NAME_MAX] = malloc(count * sizeof(*names));
    if (!names) return ZS_INTERNAL;
    for (size_t i = 0; i < count; i++)
        zsi_name_format(names[i], db->uuid,
                        db->snap->files[first + i]->hdr.start,
                        db->snap->files[first + i]->hdr.end);

    r = zsi_repack_merge(db, db->snap, first, count);
    if (r != ZS_OK) { free(names); return r; }

    r = zsi_db_refresh(db);
    if (r != ZS_OK) { free(names); return r; }

    /* Retire the inputs (D-23).  Each is now superseded by the output, so each
     * verification succeeds -- but a failure is not fatal: a leaked file costs
     * disk space and a later pass removes it. */
    for (size_t i = 0; i < count; i++) {
        int rr = zsi_remove_file(db, names[i]);
        (void)rr;
    }

    free(names);
    return zsi_db_refresh(db);
}

static int zsi_repack(struct zs_db *db)
{
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    r = zsi_lock_take(&db->locks, ZSI_LOCK_REPACK,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    /* Refresh under the lock: another process may have repacked while we waited. */
    r = zsi_db_refresh(db);
    if (r != ZS_OK) goto out;

    size_t first, count;
    count = zsi_repack_select(db->snap, &first);
    if (count < 2) { r = ZS_OK; goto out; }     /* nothing to do */

    r = zsi_repack_run(db, first, count);

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_REPACK);
    return r;
}

/* D-26: the whole database into one file.
 *
 * Unbounded by design (D-29), holding the repack lock throughout while writers
 * continue -- the same shape zs_db_repack already has, and spec open item 1's
 * unboundedness now a deliberate API entry point rather than an emergent
 * property of D-16's cascade.
 *
 * Lock order is REPACK then WRITE (C-1d), and zsi_seal releases the write lock
 * before returning, so the merge runs holding repack alone and a long compaction
 * does not block writers throughout.  C-1d had to be AMENDED for this: it
 * previously said nothing holds both write and repack.  The order is still
 * acyclic -- repack, write, remove is a chain -- and every other operation uses
 * a sub-chain of it. */
static int zsi_compact(struct zs_db *db)
{
    size_t first, count;
    int r = zsi_check_writable(db);
    if (r != ZS_OK) return r;

    r = zsi_lock_take(&db->locks, ZSI_LOCK_REPACK,
                      db->nonblocking ? ZS_NONBLOCKING : 0);
    if (r != ZS_OK) return r;

    /* Steps 1 and 2: seal the active generation, and convert any straggler.
     * zsi_seal takes and releases the write lock itself, and converts pending
     * files on its way out. */
    r = zsi_seal(db);
    if (r != ZS_OK) goto out;

    r = zsi_db_refresh(db);
    if (r != ZS_OK) goto out;

    /* Step 3: merge every maximal RUN of adjacent in-order files, largest job
     * first, until none of two or more remains.
     *
     * Runs, not the in-order prefix (D-26a).  D-16's geometric selection does not
     * apply -- it exists to keep a repack amortised and compaction is explicitly
     * the unamortised case -- but adjacency still does (D-19), so a file nothing
     * can convert splits the set into runs that must each be merged separately.
     * Taking only the prefix would merge NOTHING when such a file sits second,
     * which is exactly the damaged database where D-28's "everything mergeable"
     * has to mean something.
     *
     * Terminates because each pass reduces nfiles by at least one, and
     * zsi_repack_run refreshes, so the next scan sees the new set. */
    for (;;) {
        size_t i = 0;
        bool found = false;

        while (i < db->snap->nfiles) {
            if (zsi_file_is_unordered(db->snap->files[i])) { i++; continue; }

            first = i;
            while (i < db->snap->nfiles
                   && !zsi_file_is_unordered(db->snap->files[i])) i++;
            count = i - first;

            if (count >= 2) { found = true; break; }
        }

        if (!found) break;

        r = zsi_repack_run(db, first, count);
        if (r != ZS_OK) goto out;
    }

    /* D-28: strict in reporting, having already done everything it could.  What
     * blocks a single file is a non-active file with an invalid header -- D-10a
     * tolerates it, and it can be neither converted nor merged, so it sits in
     * the middle of the range and stops the in-order prefix there. */
    if (db->snap->nfiles != 1) {
        for (size_t i = 0; i < db->snap->nfiles; i++)
            if (!db->snap->files[i]->hdr_valid)
                db->error("file cannot be merged; compaction left it in place",
                          "file=<%s>", db->snap->files[i]->fname);
        r = ZS_BADFORMAT;
    }

out:
    zsi_lock_release(&db->locks, ZSI_LOCK_REPACK);
    return r;
}

int zs_db_repack(struct zs_db *db)
{
    if (!db) return ZS_BADUSAGE;
    return zsi_repack(db);
}

int zs_db_seal(struct zs_db *db)
{
    if (!db) return ZS_BADUSAGE;
    return zsi_seal(db);
}

int zs_db_compact(struct zs_db *db)
{
    if (!db) return ZS_BADUSAGE;
    return zsi_compact(db);
}

bool zs_db_should_repack(struct zs_db *db)
{
    if (!db) return false;
    return zsi_should_repack(db->snap);
}

/********** CONSISTENCY *************/

/* Checks a reader deliberately does NOT perform, gathered here.
 *
 * Two categories, and the distinction matters:
 *
 *   - things too expensive for open, which must stay O(1) (F-31): the records
 *     region checksum (F-26f), and the pointer array's key ordering;
 *   - things that are non-canonical rather than invalid (F-15, F-17): a big form
 *     whose lengths would have fitted the short one, an ancestor stored when it
 *     equals the file's own start.  A conforming writer never produces these, but
 *     REJECTING them on read would discard committed data -- a record that fails to
 *     validate makes an unordered file complete at that point (F-24), and G-3
 *     forbids that costing committed data.  So they are reported here and read
 *     normally there, which is the division T-6 sets out.
 */

static void zsi_report(struct zs_db *db, const char *what, const char *fname,
                       unsigned long long detail)
{
    db->error(what, "file=<%s> at=<%llu>", fname, detail);
}

/* F-28: an in-order file's pointer array MUST be strictly increasing by key.
 *
 * That confirms the sort AND catches a repack that emitted a key twice (D-17) --
 * two failures a binary search cannot detect, because it would simply return wrong
 * answers over a misordered array rather than noticing. */
static int zsi_check_inorder(struct zs_db *db, struct zsi_file *f)
{
    struct zsi_rec prev, cur;
    int r = ZS_OK;

    for (uint64_t i = 0; i < f->nptrs; i++) {
        if (zsi_ptrs_rec(f, i, &cur) != ZS_OK) {
            zsi_report(db, "record does not decode", f->fname, i);
            return ZS_BADFORMAT;
        }

        if (i > 0) {
            int c = db->compar(prev.key, prev.keylen, cur.key, cur.keylen);
            if (c == 0) {
                zsi_report(db, "duplicate key in pointer array", f->fname, i);
                r = ZS_BADFORMAT;
            } else if (c > 0) {
                zsi_report(db, "pointer array not sorted by key", f->fname, i);
                r = ZS_BADFORMAT;
            }
        }

        /* F-15/F-17 non-canonical encodings, reported not rejected. */
        if (!zsi_rec_is_canonical(&cur, f->hdr.start)) {
            zsi_report(db, "non-canonical record encoding", f->fname, i);
            r = ZS_BADFORMAT;
        }

        /* F-32a: the record's own checksum.  The region check below sees the
         * same bytes but cannot name the record; this names it. */
        if (!db->nocsum && zsi_rec_verify(f->csum, &cur) != ZS_OK) {
            zsi_report(db, "record checksum mismatch", f->fname, i);
            r = ZS_BADCHECKSUM;
        }

        prev = cur;
    }

    /* F-26e: the records region checksum, which is the only thing that detects a
     * record body corrupted in place in an in-order file. */
    if (!db->nocsum && zsi_ptrs_verify_records(f) != ZS_OK) {
        zsi_report(db, "records region checksum mismatch", f->fname, 0);
        r = ZS_BADCHECKSUM;
    }

    return r;
}

struct zsi_check_unordered {
    struct zs_db    *db;
    struct zsi_file *f;
    int              result;
};

static int zsi_check_rec_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct zsi_check_unordered *c = rock;

    if (!zsi_rec_is_canonical(rec, c->f->hdr.start)) {
        zsi_report(c->db, "non-canonical record encoding", c->f->fname, off);
        c->result = ZS_BADFORMAT;
    }

    /* F-32a.  This callback only runs inside a span the replay just
     * validated, and the span checksum covers the record checksum field --
     * so a mismatch here cannot be in-place corruption (that would have
     * failed the span first).  It means the record was WRITTEN wrong: a
     * peer's encoder bug, reported-not-rejected exactly like a
     * non-canonical encoding (T-6). */
    if (!c->db->nocsum && zsi_rec_verify(c->f->csum, rec) != ZS_OK) {
        zsi_report(c->db, "record checksum mismatch", c->f->fname, off);
        c->result = ZS_BADCHECKSUM;
    }

    return 0;
}

static int zsi_check_unordered_file(struct zs_db *db, struct zsi_file *f)
{
    struct zsi_check_unordered ctx = { db, f, ZS_OK };
    int r;

    if (!f->hdr_valid) {
        /* D-10: legal for the active file, and already an error for any other
         * (D-10a), so nothing to report here. */
        return ZS_OK;
    }

    /* The replay validates every span's checksum as it goes, so a torn or
     * corrupted span shows up as a complete point short of the file's end. */
    r = zsi_unordered_replay(f, ZSI_HEADER_LEN, db->nocsum,
                             zsi_check_rec_cb, &ctx);
    if (r != ZS_OK) return r;

    if (f->complete != f->size) {
        /* Not an error: F-24 makes this an ordinary state, and D-9 has the writer
         * move on rather than repair.  Reported so a tool can say so. */
        zsi_report(db, "content after the last valid span", f->fname,
                   (unsigned long long)f->complete);
    }

    /* A terminator whose width is not the canonical one for its span (F-15). */
    size_t pos = ZSI_HEADER_LEN;
    while (pos < f->complete) {
        const char *b = zsi_file_at(f, pos, 1);
        if (!b) break;

        uint8_t type = (uint8_t)b[0];
        if (type & ZSI_SPANTERM) {
            struct zsi_term t;
            if (zsi_term_decode(b, f->size - pos, &t) != ZS_OK) break;
            if (!zsi_term_is_canonical(&t)) {
                zsi_report(db, "non-canonical terminator width", f->fname, pos);
                ctx.result = ZS_BADFORMAT;
            }
            pos += t.len;
            continue;
        }

        struct zsi_rec rec;
        if (zsi_rec_decode(b, f->size - pos, f->hdr.start, &rec) != ZS_OK) break;
        if (rec.len == 0) break;
        pos += rec.len;
    }

    return ctx.result;
}

int zs_db_check_consistency(struct zs_db *db)
{
    int result = ZS_OK;

    if (!db) return ZS_BADUSAGE;

    for (size_t i = 0; i < db->snap->nfiles; i++) {
        struct zsi_file *f = db->snap->files[i];
        int r = zsi_file_is_unordered(f) ? zsi_check_unordered_file(db, f)
                                        : zsi_check_inorder(db, f);
        if (r != ZS_OK) result = r;
    }

    /* D-6: the resolved ranges must tile.  Checked last, because a per-file
     * problem is more specific and more useful to report. */
    struct zsi_fileset fs;
    int r = zsi_fileset_scan(db->dir, &db->uuid, &fs);
    if (r == ZS_OK) {
        r = zsi_fileset_resolve(&fs);
        zsi_fileset_fini(&fs);
        if (r != ZS_OK) {
            zsi_report(db, "file set does not tile", db->dir, 0);
            result = r;
        }
    }

    return result;
}

/* zs_db_dump: print structure.
 *
 * The line format is a compatibility surface, because it is what T-0a's `dump`
 * subcommand emits and what the interop runner compares as text.  Changing it
 * breaks the runner, not just a human's expectations.
 *
 *   FILE <name> kind=<unordered|inorder> start=<N> end=<N> csum=<id> size=<N>
 *   SPAN <off> len=<N> term=<COMMIT|ROLLBACK> records=<N>
 *   REC  <off> type=<0xNN> keylen=<N> vallen=<N|-> anc=<N> key=<hex>
 *   PTRS <off> width=<32|64> count=<N>
 */
static void zsi_dump_hex(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", (unsigned char)p[i]);
}

static void zsi_dump_rec(const struct zsi_rec *r, size_t off, int detail)
{
    printf("REC  %zu type=0x%02X keylen=%zu ", off, r->type, r->keylen);
    if (r->val) printf("vallen=%zu ", r->vallen);
    else        printf("vallen=- ");
    printf("anc=%u key=", r->ancestor);
    zsi_dump_hex(r->key, r->keylen);
    if (detail > 1 && r->val) {
        printf(" val=");
        zsi_dump_hex(r->val, r->vallen);
    }
    printf("\n");
}

struct zsi_dump_ctx { int detail; size_t nrecs; };

static int zsi_dump_cb(void *rock, const struct zsi_rec *rec, size_t off)
{
    struct zsi_dump_ctx *c = rock;
    c->nrecs++;
    if (c->detail > 0) zsi_dump_rec(rec, off, c->detail);
    return 0;
}

int zs_db_dump(struct zs_db *db, int detail)
{
    if (!db) return ZS_BADUSAGE;

    for (size_t i = 0; i < db->snap->nfiles; i++) {
        struct zsi_file *f = db->snap->files[i];
        const char *base = strrchr(f->fname, '/');
        base = base ? base + 1 : f->fname;

        printf("FILE %s kind=%s start=%u end=%u csum=%u size=%zu\n",
               base, zsi_file_is_unordered(f) ? "unordered" : "inorder",
               f->hdr.start, f->hdr.end, f->csum_id, f->size);

        if (zsi_file_is_unordered(f)) {
            /* Walk the spans directly, so a ROLLBACK is reported rather than
             * silently skipped -- the whole point of a dump is to show what is
             * there, including what a reader ignores. */
            size_t pos = ZSI_HEADER_LEN;
            while (pos < f->complete) {
                size_t span_start = pos;
                size_t nrecs = 0;

                for (;;) {
                    const char *b = zsi_file_at(f, pos, 1);
                    if (!b) break;
                    uint8_t type = (uint8_t)b[0];

                    if (type & ZSI_SPANTERM) {
                        struct zsi_term t;
                        if (zsi_term_decode(b, f->size - pos, &t) != ZS_OK) break;
                        printf("SPAN %zu len=%llu term=%s records=%zu\n",
                               span_start, (unsigned long long)t.spanlen,
                               zsi_term_is_rollback(&t) ? "ROLLBACK" : "COMMIT",
                               nrecs);
                        pos += t.len;
                        break;
                    }

                    struct zsi_rec rec;
                    if (zsi_rec_decode(b, f->size - pos, f->hdr.start, &rec)
                        != ZS_OK) break;
                    if (rec.len == 0) break;
                    if (detail > 0) zsi_dump_rec(&rec, pos, detail);
                    nrecs++;
                    pos += rec.len;
                }

                if (pos <= span_start) break;
            }
        } else {
            printf("PTRS %zu width=%d count=%llu\n", f->ptr_off,
                   f->ptr_wide ? 64 : 32, (unsigned long long)f->nptrs);
            if (detail > 0) {
                for (uint64_t j = 0; j < f->nptrs; j++) {
                    struct zsi_rec rec;
                    if (zsi_ptrs_rec(f, j, &rec) != ZS_OK) break;
                    zsi_dump_rec(&rec, (size_t)zsi_ptrs_at(f, j), detail);
                }
            }
        }
    }

    (void)zsi_dump_cb;
    return ZS_OK;
}

/* T-0a: the pointer table, as text a runner can compare byte for byte.
 *
 * Validation goes through the same loader the read path uses, so what this
 * reports is the real acceptance decision (P-11) rather than a second parser
 * that could disagree with it -- which is the failure mode a separate dumper
 * would introduce, and nobody would notice until two implementations argued.
 *
 * Never an error: no cache directory, no table, and a rejected table are all
 * states to report, because a table is never required. */
int zs_db_index_dump(struct zs_db *db)
{
    struct zsi_idxcfg cfg;

    if (!db) return ZS_BADUSAGE;

    cfg.dir = db->index_dir;
    cfg.threshold = db->index_threshold;
    cfg.local = db->index_local;

    if (!db->index_dir) {
        printf("INDEXDIR none\n");
        return ZS_OK;
    }

    printf("INDEXDIR set threshold=%zu\n", db->index_threshold);

    for (size_t i = 0; i < db->snap->nfiles; i++) {
        struct zsi_file *f = db->snap->files[i];
        char name[ZSI_NAME_MAX];
        size_t *base = NULL, nbase = 0, vu = 0, to = 0;
        uint32_t tc = 0;

        /* P-1: only unordered files have tables. */
        if (!zsi_file_is_unordered(f) || !f->hdr_valid) continue;

        zsi_name_format_index(name, f->hdr.uuid, f->hdr.start);

        if (zsi_idx_load(f, &cfg, db->compar_name, db->nocsum,
                         &base, &nbase, &vu, &to, &tc) != ZS_OK) {
            printf("TABLE %s state=absent\n", name);
            continue;
        }

        printf("TABLE %s state=usable generation=%08X valid_upto=%zu"
               " term_off=%zu term_csum=%08X nptrs=%zu\n",
               name, f->hdr.start, vu, to, tc, nbase);
        free(base);
    }

    return ZS_OK;
}

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

    db->index_local = (setup->flags & ZS_INDEX_LOCAL) != 0;

    /* A-8a: the two name different locations for the same tables, so setting
     * both is ambiguous, not a preference order. */
    if (db->index_local && setup->index_dir) {
        free(db->dir);
        free(db);
        return ZS_BADUSAGE;
    }

    if (setup->index_dir) {
        /* A-8/P-2, the cheap half: identical strings, caught before anything is
         * created.  The resolved-path half has to wait until the database
         * directory exists -- see below. */
        if (strcmp(dir, setup->index_dir) == 0) {
            free(db->dir);
            free(db);
            return ZS_BADUSAGE;
        }

        db->index_dir = strdup(setup->index_dir);
        if (!db->index_dir) { free(db->dir); free(db); return ZS_INTERNAL; }
    }

    if (setup->index_dir || db->index_local) {
        /* A-9.  Zero means the measured default, so a caller that sets index_dir
         * and nothing else still gets bounded publishing.
         *
         * Capped at a quarter of rollover_size, so a caller using small
         * generations still publishes several times within one rather than
         * never -- the default is an absolute figure, and a rollover smaller
         * than it would otherwise make the cache useless exactly where opens are
         * most frequent.
         *
         * Never zero in the end: a threshold of zero publishes on every commit,
         * which P-13 exists to bound. */
        if (setup->index_threshold) {
            db->index_threshold = setup->index_threshold;
        } else {
            size_t quarter = db->rollover_size / 4;
            db->index_threshold = quarter < ZSI_DEFAULT_INDEX_THRESHOLD
                                ? quarter : (size_t)ZSI_DEFAULT_INDEX_THRESHOLD;
        }
        if (db->index_threshold == 0) db->index_threshold = 1;
    }

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

    /* A-8/P-2.  The cache directory must not be the database directory: writing
     * a table there would be a write to the database, and the R-3 amendment
     * permits a read-only handle to publish only because a cache directory is
     * somewhere else.
     *
     * Checked HERE rather than at the top of this function, because with
     * ZS_CREATE the database directory does not exist until the mkdir above and
     * realpath cannot resolve a directory that is not there yet.  Checking early
     * would let the very first open -- the one that creates the database -- slip
     * through with the two directories identical.
     *
     * This is the half that catches "." and a trailing slash.  The identical-
     * string half already ran, before the mkdir. */
    if (db->index_dir) {
        char rd[PATH_MAX], rc[PATH_MAX];

        if (realpath(dir, rd) && realpath(db->index_dir, rc)
            && strcmp(rd, rc) == 0) {
            zs_db_close(&db);
            return ZS_BADUSAGE;
        }
    }

    /* A-8/A-8a: resolve the EFFECTIVE cache directory once, here, where the
     * uuid is known and the database directory exists, so publish, load,
     * sweep and dump all take a per-database path as given and none of them
     * changes.  Every failure disables the cache for this handle rather than
     * failing the open (P-15): the cache is never load-bearing. */
    if (db->index_local) {
        char path[PATH_MAX];
        struct stat st;

        if ((size_t)snprintf(path, sizeof(path), "%s/%s",
                             dir, ZSI_CACHE_DIR_NAME) >= sizeof(path)) {
            db->index_local = false;
        } else {
            /* P-2b/R-3: only a writable handle creates it.  A read-only
             * handle uses it if present and is otherwise simply without a
             * cache -- creating a directory inside the database is a visible
             * side effect R-3 forbids. */
            if (!db->readonly && mkdir(path, 0700) != 0 && errno != EEXIST)
                db->error("could not create the cache directory; continuing "
                          "without the index cache", "dir=<%s>", path);
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                db->index_dir = strdup(path);
                if (!db->index_dir) { zs_db_close(&db); return ZS_INTERNAL; }
            } else {
                db->index_local = false;
            }
        }
    } else if (db->index_dir) {
        char uu[ZSI_UUID_STR_LEN], path[PATH_MAX];
        struct stat st;

        zsi_uuid_unparse(db->uuid, uu);

        /* P-2a: the per-uuid level is created as needed, by ANY handle -- it
         * is outside the database, so R-3 is untouched.  The ROOT is never
         * created (A-8): a missing root fails this mkdir with ENOENT, the
         * stat below fails too, and the cache is disabled for the handle. */
        free(db->index_dir);
        db->index_dir = NULL;
        if ((size_t)snprintf(path, sizeof(path), "%s/%s",
                             setup->index_dir, uu) < sizeof(path)) {
            (void)mkdir(path, 0700);
            if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
                db->index_dir = strdup(path);
                if (!db->index_dir) { zs_db_close(&db); return ZS_INTERNAL; }
            }
        }
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
    free(db->probe_names);
    free(db->retbuf);
    free(db->index_dir);
    free(db->dir);
    free(db);
    *dbp = NULL;

    return ZS_OK;
}

/********** PUBLIC API *************/

/* A-0: every read and write entry point exists in three forms -- on the database,
 * on a transaction, and via a cursor -- and all three take flags.  The zs_db_*
 * forms are convenience wrappers that open an implicit single-operation
 * transaction, implemented LITERALLY as wrappers so the wrapped path is the only
 * path and there is no operation reachable one way but not another.
 *
 * A-2: there is no yield call and no yield flags, because readers hold no lock and
 * so have nothing to yield.  A-3: there is no MVCC flag, because snapshot
 * isolation is the only read mode.  twom has both; porting them here would be
 * copying an answer to a question this design does not ask. */

/* A-4: pointers returned by a zs_db_* call stay valid until the next call on that
 * handle.  Records read from a mapped file are stable for the snapshot's life, so
 * those are returned directly; a record from a transaction's pending array dies
 * with the implicit transaction, so it is copied into handle-owned scratch.  This
 * is the only case that needs a buffer. */
static int zsi_db_retain(struct zs_db *db, const struct zsi_rec *r,
                         const char **keyp, size_t *keylenp,
                         const char **valp, size_t *vallenp)
{
    size_t need = r->keylen + (r->val ? r->vallen : 0) + 2;

    if (need > db->retalloc) {
        char *p = realloc(db->retbuf, need);
        if (!p) return ZS_INTERNAL;
        db->retbuf = p;
        db->retalloc = need;
    }

    memcpy(db->retbuf, r->key, r->keylen);
    db->retbuf[r->keylen] = '\0';
    if (r->val && r->vallen)
        memcpy(db->retbuf + r->keylen + 1, r->val, r->vallen);
    db->retbuf[r->keylen + 1 + (r->val ? r->vallen : 0)] = '\0';

    if (keyp) *keyp = db->retbuf;
    if (keylenp) *keylenp = r->keylen;
    if (valp) *valp = db->retbuf + r->keylen + 1;
    if (vallenp) *vallenp = r->val ? r->vallen : 0;

    return ZS_OK;
}

/* transactions */

int zs_db_begin_txn(struct zs_db *db, int shared, struct zs_txn **txnp)
{
    if (!db || !txnp) return ZS_BADUSAGE;
    *txnp = NULL;
    return zsi_txn_begin(db, shared != 0, txnp);
}

int zs_txn_commit(struct zs_txn **txnp)
{
    struct zs_txn *txn;

    if (!txnp || !*txnp) return ZS_BADUSAGE;
    txn = *txnp;
    *txnp = NULL;
    return zsi_txn_commit(txn);
}

int zs_txn_abort(struct zs_txn **txnp)
{
    struct zs_txn *txn;

    if (!txnp || !*txnp) return ZS_BADUSAGE;
    txn = *txnp;
    *txnp = NULL;
    return zsi_txn_abort(txn);
}

/* fetch */

int zs_txn_fetch(struct zs_txn *txn, const char *key, size_t keylen,
                 const char **keyp, size_t *keylenp,
                 const char **valp, size_t *vallenp, int flags)
{
    struct zsi_rec r;
    int rc;

    if (!txn || !key) return ZS_BADUSAGE;
    if (keylen < 1) return ZS_BADUSAGE;

    if ((flags & ZS_FETCHNEXT) && (flags & ZS_FETCHPREV))
        return ZS_BADUSAGE;                                 /* A-12 */

    /* ZS_FETCHNEXT: return the record AFTER the given key.  A cursor seeked to
     * the key with ZS_SKIPROOT is exactly that, so it shares the merge rather than
     * reimplementing "next".
     *
     * ZS_FETCHPREV: the largest key <= (or, with ZS_SKIPROOT, <) the given
     * key.  A REVERSE cursor seeked at the key is exactly that (D-14l), so it
     * shares the merge too -- the fetch family stays two shapes, not three,
     * and predecessor lookup cannot disagree with reverse iteration (G-7). */
    if (flags & (ZS_FETCHNEXT | ZS_FETCHPREV)) {
        struct zs_cursor *c = NULL;
        uint32_t cflags = (flags & ZS_FETCHNEXT)
                        ? ZS_SKIPROOT
                        : ZS_REVERSE | ((uint32_t)flags & ZS_SKIPROOT);
        rc = zsi_cursor_open(txn->db, txn->readonly ? NULL : txn, txn->snap,
                             key, keylen, cflags, &c);
        if (rc != ZS_OK) return rc;
        rc = zsi_cursor_next(c, &r);
        if (rc == ZS_OK) {
            if (keyp) *keyp = r.key;
            if (keylenp) *keylenp = r.keylen;
            if (valp) *valp = r.val;
            if (vallenp) *vallenp = r.vallen;
        } else {
            rc = ZS_NOTFOUND;
        }
        zsi_cursor_free(c);
        return rc;
    }

    rc = zsi_lookup(txn->db, txn->snap, txn->readonly ? NULL : txn,
                    key, keylen, &r);
    if (rc != ZS_OK) return rc;

    if (keyp) *keyp = r.key;
    if (keylenp) *keylenp = r.keylen;
    if (valp) *valp = r.val;
    if (vallenp) *vallenp = r.vallen;
    return ZS_OK;
}

int zs_db_fetch(struct zs_db *db, const char *key, size_t keylen,
                const char **keyp, size_t *keylenp,
                const char **valp, size_t *vallenp, int flags)
{
    struct zs_txn *txn = NULL;
    struct zsi_rec r;
    int rc;

    if (!db || !key) return ZS_BADUSAGE;

    rc = zs_db_begin_txn(db, 1, &txn);
    if (rc != ZS_OK) return rc;
    txn->implicit = true;   /* D-14j: its cursors follow this handle */

    const char *k = NULL, *v = NULL;
    size_t kl = 0, vl = 0;
    rc = zs_txn_fetch(txn, key, keylen, &k, &kl, &v, &vl, flags);

    if (rc == ZS_OK) {
        r.key = k; r.keylen = kl;
        r.val = v; r.vallen = vl;
        r.type = ZSI_KEYVALUE;
        rc = zsi_db_retain(db, &r, keyp, keylenp, valp, vallenp);
    }

    zs_txn_abort(&txn);
    return rc;
}

/* store */

int zs_txn_store(struct zs_txn *txn, const char *key, size_t keylen,
                 const char *val, size_t vallen, int flags)
{
    struct zsi_rec r;
    int rc;

    if (!txn || !key) return ZS_BADUSAGE;
    if (keylen < 1) return ZS_BADUSAGE;             /* F-14 */
    if (txn->readonly) return ZS_READONLY;          /* A-5 */

    /* ZS_IFNOTEXIST / ZS_IFEXIST are evaluated against the transaction's own
     * view, so they compose with earlier writes in the same transaction (A-1a). */
    if (flags & (ZS_IFNOTEXIST | ZS_IFEXIST)) {
        rc = zsi_lookup(txn->db, txn->snap, txn, key, keylen, &r);
        if ((flags & ZS_IFNOTEXIST) && rc == ZS_OK) return ZS_EXISTS;
        if ((flags & ZS_IFEXIST) && rc == ZS_NOTFOUND) return ZS_NOTFOUND;
        if (rc != ZS_OK && rc != ZS_NOTFOUND) return rc;
    }

    return zsi_pend_set(txn, key, keylen, val, vallen);
}

int zs_db_store(struct zs_db *db, const char *key, size_t keylen,
                const char *val, size_t vallen, int flags)
{
    struct zs_txn *txn = NULL;
    int rc;

    if (!db || !key) return ZS_BADUSAGE;

    rc = zs_db_begin_txn(db, 0, &txn);
    if (rc != ZS_OK) return rc;
    txn->implicit = true;   /* D-14j: its cursors follow this handle */

    rc = zs_txn_store(txn, key, keylen, val, vallen, flags);
    if (rc != ZS_OK) {
        zs_txn_abort(&txn);
        return rc;
    }

    return zs_txn_commit(&txn);
}

/* foreach */

int zs_txn_foreach(struct zs_txn *txn, const char *start, size_t startlen,
                   zs_cb *p, zs_cb *cb, void *rock, int flags)
{
    struct zs_cursor *c = NULL;
    struct zsi_rec r;
    int rc;

    if (!txn || !cb) return ZS_BADUSAGE;

    /* A-13: no reverse foreach.  Nothing needs it -- a consumer walking
     * backwards drives a cursor -- and a rejected flag is cheaper than an
     * untested promise. */
    if (flags & ZS_REVERSE) return ZS_BADUSAGE;

    rc = zsi_cursor_open(txn->db, txn->readonly ? NULL : txn, txn->snap,
                         startlen ? start : NULL, startlen,
                         (uint32_t)flags, &c);
    if (rc != ZS_OK) return rc;
    c->handle_live = txn->implicit;

    while ((rc = zsi_cursor_next(c, &r)) == ZS_OK) {
        /* p is a predicate: when supplied, cb runs only where it returns
         * non-zero.  Matches the cyrusdb convention the other backends use. */
        if (p && !p(rock, r.key, r.keylen, r.val, r.vallen)) continue;
        int cr = cb(rock, r.key, r.keylen, r.val, r.vallen);
        if (cr) { rc = cr; goto out;  }
    }

    if (rc == ZS_DONE) rc = ZS_OK;

out:
    zsi_cursor_free(c);
    return rc;
}

int zs_db_foreach(struct zs_db *db, const char *start, size_t startlen,
                  zs_cb *p, zs_cb *cb, void *rock, int flags)
{
    struct zs_txn *txn = NULL;
    int rc;

    if (!db || !cb) return ZS_BADUSAGE;

    rc = zs_db_begin_txn(db, 1, &txn);
    if (rc != ZS_OK) return rc;
    txn->implicit = true;   /* D-14j: its cursors follow this handle */

    rc = zs_txn_foreach(txn, start, startlen, p, cb, rock, flags);
    zs_txn_abort(&txn);
    return rc;
}

/* cursors */

int zs_txn_begin_cursor(struct zs_txn *txn, const char *key, size_t keylen,
                        struct zs_cursor **curp, int flags)
{
    if (!txn || !curp) return ZS_BADUSAGE;
    *curp = NULL;
    {
        int r = zsi_cursor_open(txn->db, txn->readonly ? NULL : txn, txn->snap,
                                keylen ? key : NULL, keylen,
                                (uint32_t)flags, curp);
        if (r == ZS_OK) (*curp)->handle_live = txn->implicit;
        return r;
    }
}

int zs_db_begin_cursor(struct zs_db *db, const char *key, size_t keylen,
                       struct zs_cursor **curp, int flags)
{
    struct zs_txn *txn = NULL;
    int rc;

    if (!db || !curp) return ZS_BADUSAGE;
    *curp = NULL;

    /* An implicit transaction, owned by the cursor: a cursor from a database
     * needs somewhere for its snapshot to live, and zs_cursor_commit/abort is
     * what ends it. */
    rc = zs_db_begin_txn(db, (flags & ZS_SHARED) ? 1 : 0, &txn);
    if (rc != ZS_OK) return rc;

    rc = zsi_cursor_open(db, txn->readonly ? NULL : txn, txn->snap,
                         keylen ? key : NULL, keylen,
                         (uint32_t)flags, curp);
    if (rc != ZS_OK) { zs_txn_abort(&txn); return rc; }

    (*curp)->txn = txn;
    (*curp)->owns_txn = true;
    (*curp)->handle_live = true;    /* D-14j: opened from a database handle */
    return ZS_OK;
}

int zs_cursor_next(struct zs_cursor *cur,
                   const char **keyp, size_t *keylenp,
                   const char **valp, size_t *vallenp)
{
    struct zsi_rec r;
    int rc;

    if (!cur) return ZS_BADUSAGE;

    rc = zsi_cursor_next(cur, &r);
    if (rc != ZS_OK) return rc;

    if (keyp) *keyp = r.key;
    if (keylenp) *keylenp = r.keylen;
    if (valp) *valp = r.val;
    if (vallenp) *vallenp = r.vallen;
    return ZS_OK;
}

int zs_cursor_replace(struct zs_cursor *cur, const char *val, size_t vallen,
                      int flags)
{
    if (!cur) return ZS_BADUSAGE;
    if (!cur->have_emitted) return ZS_BADUSAGE;     /* nothing to replace */
    if (!cur->txn || cur->txn->readonly) return ZS_READONLY;

    return zs_txn_store(cur->txn, cur->emitted.key, cur->emitted.keylen,
                        val, vallen, flags);
}

int zs_cursor_commit(struct zs_cursor **curp)
{
    struct zs_cursor *c;
    int rc = ZS_OK;

    if (!curp || !*curp) return ZS_BADUSAGE;
    c = *curp;
    *curp = NULL;

    if (c->owns_txn && c->txn) {
        struct zs_txn *txn = c->txn;
        c->txn = NULL;
        rc = zsi_txn_commit(txn);
    }

    zsi_cursor_free(c);
    return rc;
}

int zs_cursor_abort(struct zs_cursor **curp)
{
    struct zs_cursor *c;
    int rc = ZS_OK;

    if (!curp || !*curp) return ZS_BADUSAGE;
    c = *curp;
    *curp = NULL;

    if (c->owns_txn && c->txn) {
        struct zs_txn *txn = c->txn;
        c->txn = NULL;
        rc = zsi_txn_abort(txn);
    }

    zsi_cursor_free(c);
    return rc;
}

/* Release a cursor inside a caller-owned transaction, without touching it. */
void zs_cursor_fini(struct zs_cursor **curp)
{
    if (!curp || !*curp) return;

    if ((*curp)->owns_txn && (*curp)->txn) {
        struct zs_txn *txn = (*curp)->txn;
        (*curp)->txn = NULL;
        zsi_txn_abort(txn);
    }

    zsi_cursor_free(*curp);
    *curp = NULL;
}

/* utility */

int zs_db_sync(struct zs_db *db)
{
    struct zsi_file *act;

    if (!db) return ZS_BADUSAGE;

    act = zsi_snapshot_active(db->snap);
    if (!act) return ZS_OK;

    int fd = open(act->fname, O_WRONLY);
    if (fd < 0) return ZS_IOERROR;
    int r = ZS_FDATASYNC(fd) < 0 ? ZS_IOERROR : ZS_OK;
    close(fd);
    return r;
}

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

/********** SALVAGE *************/

/* Spec section 9.  Rebuild what is readable out of a DAMAGED directory into a
 * new database.
 *
 * This section deliberately does NOT share the read path, and must not.  Its
 * whole purpose is to read structures sections 5 and 7 refuse: a file set with a
 * gap, a header that does not validate, a pointer section that will not load,
 * spans sitting after a bad one.  Routing it through zsi_snapshot_take would
 * make it refuse exactly the databases it exists for.
 *
 * The source is never written to, never locked, and never unlinked from (S-1).
 * That is what lets this guess and improvise without risking the only copy of
 * the data it is trying to save, and it is why R-4 needs no repair exception. */

/* How far a resynchronisation scan will look before giving up on a file.  Not a
 * correctness bound -- the scan stops at end of file regardless -- but a guard
 * against spending unbounded time on a file that is mostly noise. */
#define ZSI_SALVAGE_SCAN_MAX (64u * 1024u * 1024u)

struct zsi_salvage_key { size_t off, len; };

struct zsi_salvage_ctx {
    struct zs_salvage_data *setup;
    struct zs_db           *out;        /* the destination, open for writing */
    const char             *fname;      /* the file being processed, for events */
    uint32_t                gen;

    /* S-10: the earliest point at which anything was lost, as (generation,
     * offset).  A key whose winning record is older than this MAY have been
     * superseded by something in the lost bytes. */
    bool                    any_loss;
    uint32_t                loss_gen;
    size_t                  loss_off;

    unsigned long long      nkeys, nstale, nunverified, nlost;
    int                     stopped;    /* the callback asked us to stop */

    /* Keys applied at or after the first loss (S-10).
     *
     * Files are scanned oldest first and positions ascend within a file, so the
     * first loss is DISCOVERED before any record beyond it is applied.  A key is
     * therefore stale exactly when its last application preceded that loss --
     * which makes "safe" a set that can be built as we go, with no second pass
     * and no need to know the loss point in advance.
     *
     * Keys are packed into one blob so a growable array of offsets can index
     * them without a separate allocation per key. */
    char                   *safe;       size_t safelen, safealloc;
    struct zsi_salvage_key *safekeys;   size_t nsafe, safekeysalloc;
};

static void zsi_salvage_emit(struct zsi_salvage_ctx *ctx, int kind,
                             size_t off, size_t len,
                             const char *key, size_t keylen)
{
    struct zs_salvage_event ev;

    if (!ctx->setup->report || ctx->stopped) return;

    memset(&ev, 0, sizeof(ev));
    ev.kind = kind;
    ev.file = ctx->fname;
    ev.generation = ctx->gen;
    ev.offset = off;
    ev.length = len;
    ev.key = key;
    ev.keylen = keylen;

    if (ctx->setup->report(ctx->setup->rock, &ev) != 0) ctx->stopped = 1;
}

/* Note that data was lost at this point (S-10), keeping the earliest. */
static void zsi_salvage_loss(struct zsi_salvage_ctx *ctx, size_t off, size_t len)
{
    ctx->nlost++;

    if (!ctx->any_loss || ctx->gen < ctx->loss_gen
        || (ctx->gen == ctx->loss_gen && off < ctx->loss_off)) {
        ctx->any_loss = true;
        ctx->loss_gen = ctx->gen;
        ctx->loss_off = off;
    }

    zsi_salvage_emit(ctx, ZS_SALVAGE_SPAN_LOST, off, len, NULL, 0);
}

/* S-5: determine a file's checksum engine when its header did not validate.
 *
 * Tries each engine and keeps the one under which the FIRST span validates.
 * Engine 0 is tried last and matches only where a stored checksum really is
 * zero, so it is a genuine signal rather than a catch-all that would accept
 * anything -- which is the whole risk of guessing at all.
 *
 * A file with no valid span under any engine yields ZS_NOTFOUND; the caller
 * still reports it, because "nothing recoverable here" is worth saying. */
static int zsi_salvage_engine(struct zsi_file *f, zs_csum *external,
                              zs_csum **out, unsigned *id_out)
{
    static const unsigned order[] = { ZSI_CSUM_XXHASH, ZSI_CSUM_EXTERNAL,
                                      ZSI_CSUM_NONE };

    for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        zs_csum *cs = zsi_csum_for_id(order[i], external);
        size_t p = ZSI_HEADER_LEN;
        struct zsi_term term;

        if (!cs) continue;                      /* engine 2 with no function */

        /* Walk records to the first terminator and see whether it validates. */
        for (;;) {
            const char *b = zsi_file_at(f, p, 1);
            struct zsi_rec r;

            if (!b) break;
            if (!zsi_type_valid((uint8_t)b[0])) break;

            if ((uint8_t)b[0] & ZSI_SPANTERM) {
                const char *spandata, *termbytes;
                size_t datalen = p - ZSI_HEADER_LEN;

                if (zsi_term_decode(b, f->size - p, &term) != ZS_OK) break;
                if (!zsi_file_at(f, p, term.len)) break;
                if (term.spanlen != (uint64_t)datalen) break;

                spandata = zsi_file_at(f, ZSI_HEADER_LEN, datalen);
                termbytes = zsi_file_at(f, p, term.len);
                if (!termbytes || (datalen && !spandata)) break;

                if (zsi_csum2(cs, order[i], spandata ? spandata : "", datalen,
                              termbytes, term.len - 4) == term.csum) {
                    *out = cs;
                    *id_out = order[i];
                    return ZS_OK;
                }
                break;
            }

            if (!((uint8_t)b[0] & ZSI_HASKEY)) break;
            if (zsi_rec_decode(b, f->size - p, f->hdr.start, &r) != ZS_OK) break;
            if (r.len == 0) break;
            if (!zsi_add_sz(p, r.len, &p)) break;
            if (p > f->size) break;
        }
    }

    return ZS_NOTFOUND;
}

/* S-7: find the next VERIFIED span at or after `from`.
 *
 * This is the only place salvage guesses, and the guess is always confirmed
 * before it is believed.  A terminator carries its span's length (F-19), so a
 * candidate terminator implies exactly where its span began -- and that span can
 * then be checksummed.  A match is proof, not a heuristic.
 *
 * Steps 8 bytes at a time because every record and terminator begins on an
 * 8-multiple (F-2), so nothing valid is skipped.
 *
 * Returns ZS_OK with *span_start, *term_off and *term filled, or ZS_NOTFOUND. */
static int zsi_salvage_resync(struct zsi_file *f, zs_csum *cs, unsigned csum_id,
                              size_t from, size_t floor,
                              size_t *span_start, size_t *term_off,
                              struct zsi_term *term)
{
    size_t scanned = 0;

    for (size_t p = (from + 7u) & ~(size_t)7; p < f->size; p += 8) {
        const char *b, *spandata, *termbytes;
        struct zsi_term t;
        size_t start, after;

        if (++scanned > ZSI_SALVAGE_SCAN_MAX / 8) break;

        b = zsi_file_at(f, p, 1);
        if (!b) break;

        if (zsi_term_decode(b, f->size - p, &t) != ZS_OK) continue;
        if (!zsi_file_at(f, p, t.len)) continue;

        /* The span it claims must lie inside the region we are still willing to
         * attribute to this file: at or after the last known good boundary, and
         * ending exactly at this terminator. */
        if (t.spanlen > (uint64_t)(p - floor)) continue;
        start = p - (size_t)t.spanlen;
        if (start < floor) continue;
        if (!zsi_add_sz(p, t.len, &after)) continue;
        if (after > f->size) continue;

        spandata = zsi_file_at(f, start, (size_t)t.spanlen);
        termbytes = zsi_file_at(f, p, t.len);
        if (!termbytes) continue;
        if (t.spanlen && !spandata) continue;

        /* The proof. */
        if (zsi_csum2(cs, csum_id, spandata ? spandata : "",
                      (size_t)t.spanlen, termbytes, t.len - 4) != t.csum)
            continue;

        *span_start = start;
        *term_off = p;
        *term = t;
        return ZS_OK;
    }

    return ZS_NOTFOUND;
}

/* Apply one recovered record to the destination.
 *
 * Records arrive oldest first (S-3), so the newest surviving version of a key
 * wins simply by being applied last -- there is no recency pass, and none is
 * possible anyway once some of the ordering evidence has been lost.
 *
 * A deletion is applied as a deletion, so a recovered tombstone still removes a
 * key and a key whose newest surviving version is a deletion ends absent.  That
 * is correct rather than a loss: it is what the database said. */
static int zsi_salvage_apply(struct zsi_salvage_ctx *ctx,
                             const struct zsi_rec *rec, bool verified)
{
    int r;

    if (rec->type & ZSI_ISDELETE)
        r = zs_db_store(ctx->out, rec->key, rec->keylen, NULL, 0, 0);
    else
        r = zs_db_store(ctx->out, rec->key, rec->keylen, rec->val, rec->vallen, 0);

    if (r != ZS_OK) return r;

    ctx->nkeys++;

    /* S-10: once anything has been lost, every key applied from here on has a
     * version no older than the loss, so it cannot be stale. */
    if (ctx->any_loss) {
        if (ctx->safelen + rec->keylen > ctx->safealloc) {
            size_t want = ctx->safealloc ? ctx->safealloc * 2 : 4096;
            char *p;
            while (want < ctx->safelen + rec->keylen) want *= 2;
            p = realloc(ctx->safe, want);
            if (!p) return ZS_INTERNAL;
            ctx->safe = p;
            ctx->safealloc = want;
        }
        if (ctx->nsafe == ctx->safekeysalloc) {
            size_t want = ctx->safekeysalloc ? ctx->safekeysalloc * 2 : 256;
            struct zsi_salvage_key *p =
                realloc(ctx->safekeys, want * sizeof(*p));
            if (!p) return ZS_INTERNAL;
            ctx->safekeys = p;
            ctx->safekeysalloc = want;
        }
        memcpy(ctx->safe + ctx->safelen, rec->key, rec->keylen);
        ctx->safekeys[ctx->nsafe].off = ctx->safelen;
        ctx->safekeys[ctx->nsafe].len = rec->keylen;
        ctx->nsafe++;
        ctx->safelen += rec->keylen;
    }

    if (!verified) {
        ctx->nunverified++;
        zsi_salvage_emit(ctx, ZS_SALVAGE_KEY_UNVERIFIED, 0, 0,
                         rec->key, rec->keylen);
    }

    return ZS_OK;
}

/* Walk a span's records, applying each.
 *
 * A record's own checksum (F-32) is deliberately NOT consulted here.  It
 * proves the record's BYTES; "verified" in this walk means the span's
 * terminator proved the records were COMMITTED (S-8), and a torn tail with
 * pristine record checksums was still never acknowledged to anyone.  The
 * two proofs answer different questions, and only in-order salvage -- where
 * publication by rename already implies commitment (D-21) -- may substitute
 * one for the other. */
static int zsi_salvage_span(struct zsi_salvage_ctx *ctx, struct zsi_file *f,
                            size_t from, size_t to, bool verified)
{
    size_t q = from;

    while (q < to && !ctx->stopped) {
        const char *b = zsi_file_at(f, q, 1);
        struct zsi_rec r;
        int rc;

        if (!b) break;
        if (zsi_rec_decode(b, f->size - q, f->hdr.start, &r) != ZS_OK) break;
        if (r.len == 0) break;

        rc = zsi_salvage_apply(ctx, &r, verified);
        if (rc != ZS_OK) return rc;

        q += r.len;
    }

    return ZS_OK;
}

/* One unordered file: walk its spans, resynchronising past damage (S-7). */
static int zsi_salvage_unordered(struct zsi_salvage_ctx *ctx,
                                 struct zsi_file *f,
                                 zs_csum *cs, unsigned csum_id)
{
    bool want_unverified =
        (ctx->setup->flags & ZS_SALVAGE_UNVERIFIED) != 0;
    size_t pos = ZSI_HEADER_LEN;
    int r;

    while (pos < f->size && !ctx->stopped) {
        size_t span_start, term_off, after;
        struct zsi_term term;

        if (zsi_salvage_resync(f, cs, csum_id, pos, pos,
                               &span_start, &term_off, &term) != ZS_OK) {
            /* Nothing verifiable remains.  Whatever is left is a torn tail: it
             * was very likely never acknowledged to anyone, so it is recovered
             * only on request (S-8) and always reported. */
            if (pos < f->size) {
                zsi_salvage_loss(ctx, pos, f->size - pos);
                if (want_unverified) {
                    r = zsi_salvage_span(ctx, f, pos, f->size, false);
                    if (r != ZS_OK) return r;
                }
            }
            return ZS_OK;
        }

        /* Bytes between where we were and where the next verified span begins
         * are a span whose terminator did not survive.  Its own records cannot
         * be proved -- the terminator is what would prove them (S-8). */
        if (span_start > pos) {
            zsi_salvage_loss(ctx, pos, span_start - pos);
            if (want_unverified) {
                r = zsi_salvage_span(ctx, f, pos, span_start, false);
                if (r != ZS_OK) return r;
            }
            zsi_salvage_emit(ctx, ZS_SALVAGE_RESYNC, span_start,
                             term_off - span_start, NULL, 0);
        }

        /* S-9: a rolled-back span is deliberately aborted.  Never recovered,
         * with or without the flag -- no conforming reader has ever shown its
         * records, and producing them would resurrect a transaction that did
         * not happen. */
        if (zsi_term_is_rollback(&term)) {
            zsi_salvage_emit(ctx, ZS_SALVAGE_SPAN_ROLLBACK, span_start,
                             term_off - span_start, NULL, 0);
        } else {
            r = zsi_salvage_span(ctx, f, span_start, term_off, true);
            if (r != ZS_OK) return r;
        }

        if (!zsi_add_sz(term_off, term.len, &after)) break;
        pos = after;
    }

    return ZS_OK;
}

/* One in-order file: walk the records region directly.
 *
 * S-6: the pointer section is IGNORED entirely, whether or not it loads.  A
 * pointer section that fails makes the file unreadable under section 7 while its
 * records may be perfect, and re-deriving order costs nothing here because the
 * destination sorts anyway.  An in-order file has no spans (F-23), so this is a
 * flat record walk. */
static int zsi_salvage_inorder(struct zsi_salvage_ctx *ctx, struct zsi_file *f,
                               zs_csum *cs, unsigned csum_id)
{
    size_t pos = ZSI_HEADER_LEN;

    zsi_salvage_emit(ctx, ZS_SALVAGE_PTRS_IGNORED, 0, 0, NULL, 0);

    while (pos < f->size && !ctx->stopped) {
        const char *b = zsi_file_at(f, pos, 1);
        struct zsi_rec r;
        int rc;

        if (!b) break;

        /* The records region ends where the pointer section begins, which a
         * type byte that is not a data record marks just as well -- and does so
         * without trusting a trailer we have already decided to ignore. */
        if (!zsi_type_valid((uint8_t)b[0])) break;
        if (!((uint8_t)b[0] & ZSI_HASKEY)) break;

        if (zsi_rec_decode(b, f->size - pos, f->hdr.start, &r) != ZS_OK) {
            zsi_salvage_loss(ctx, pos, f->size - pos);
            break;
        }
        if (r.len == 0) break;

        /* F-32 gives every record its own proof, so "verified" is a
         * per-record fact here rather than a blanket claim -- an in-order
         * file has no spans, and the trailer that could vouch for the region
         * is exactly what salvage refuses to trust (S-6).  Under engine 0
         * nothing can be proved and every key reports unverified, which is
         * the honest version of what this path used to assert. */
        rc = zsi_salvage_apply(ctx, &r,
                               csum_id != 0
                               && zsi_rec_verify(cs, &r) == ZS_OK);
        if (rc != ZS_OK) return rc;

        pos += r.len;
    }

    return ZS_OK;
}

/* Order for S-3: oldest first, by start ascending; for equal start the NARROWER
 * range first, since a wider one is a repack output derived from it and is
 * therefore newer. */
static int zsi_salvage_order(const void *va, const void *vb)
{
    const struct zsi_entry *a = va, *b = vb;

    if (a->start != b->start) return a->start < b->start ? -1 : 1;

    {
        uint32_t ae = a->end ? a->end : a->start;
        uint32_t be = b->end ? b->end : b->start;
        if (ae != be) return ae < be ? -1 : 1;
    }

    return 0;
}

/* S-10: report every surviving key whose recovered record predates the first
 * loss, and no others.
 *
 * Deliberately conservative rather than exact: it reports what COULD have been
 * superseded by the lost bytes, because determining what actually was would need
 * the key set of those bytes, which is precisely what has been lost.  Reporting
 * only keys older than the loss is what keeps it small enough to act on -- the
 * alternative, "everything might be stale", is true and useless. */
static int zsi_salvage_report_stale(struct zsi_salvage_ctx *ctx)
{
    struct zs_cursor *c = NULL;
    zs_compar *cmp = ctx->out->compar;
    const char *k, *v;
    size_t kl, vl;
    int r;

    if (!ctx->any_loss || !ctx->setup->report) return ZS_OK;

    /* Sort the safe keys so each output key costs one binary search rather than
     * a scan; duplicates are harmless to a lower-bound search. */
    if (ctx->nsafe > 1) {
        /* Insertion into a sorted array via the merge sort already in this file
         * would need a context, so a simple qsort over a temporary of resolved
         * pointers is clearer here -- and this runs once, over recovered keys
         * only, on a path that has already read the whole database. */
        for (size_t i = 1; i < ctx->nsafe; i++) {
            struct zsi_salvage_key key = ctx->safekeys[i];
            size_t j = i;
            while (j > 0
                   && cmp(ctx->safe + ctx->safekeys[j - 1].off,
                          ctx->safekeys[j - 1].len,
                          ctx->safe + key.off, key.len) > 0) {
                ctx->safekeys[j] = ctx->safekeys[j - 1];
                j--;
            }
            ctx->safekeys[j] = key;
        }
    }

    r = zs_db_begin_cursor(ctx->out, NULL, 0, &c, 0);
    if (r != ZS_OK) return r;

    while ((r = zs_cursor_next(c, &k, &kl, &v, &vl)) == ZS_OK && !ctx->stopped) {
        size_t lo = 0, hi = ctx->nsafe;
        bool safe = false;

        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int d = cmp(ctx->safe + ctx->safekeys[mid].off,
                        ctx->safekeys[mid].len, k, kl);
            if (d == 0) { safe = true; break; }
            if (d < 0) lo = mid + 1; else hi = mid;
        }

        if (!safe) {
            ctx->nstale++;
            ctx->fname = NULL;
            ctx->gen = 0;
            zsi_salvage_emit(ctx, ZS_SALVAGE_KEY_MAYBE_STALE, 0, 0, k, kl);
        }
    }

    zs_cursor_fini(&c);
    return (r == ZS_DONE || r == ZS_OK) ? ZS_OK : r;
}

int zs_db_salvage(const char *from, const char *to,
                  struct zs_salvage_data *setup)
{
    struct zs_salvage_data defaults = ZS_SALVAGE_DATA_INITIALIZER;
    struct zs_open_data outsetup = ZS_OPEN_DATA_INITIALIZER;
    struct zsi_salvage_ctx ctx;
    struct zsi_fileset fs;
    struct zs_db *out = NULL;
    uint32_t expect_gen = 0;
    int r;

    if (!from || !to) return ZS_BADUSAGE;
    if (!setup) setup = &defaults;

    memset(&ctx, 0, sizeof(ctx));
    ctx.setup = setup;

    /* S-2: scan for names only.  zsi_fileset_scan parses D-1 names and does NOT
     * resolve overlaps or check tiling -- that is zsi_fileset_resolve, which is
     * deliberately not called here.  A gap is something to report and step over,
     * not something to fail on. */
    r = zsi_fileset_scan(from, NULL, &fs);
    if (r == ZS_NOTFOUND) return ZS_NOTFOUND;
    if (r != ZS_OK) return r;

    if (fs.nall == 0) { zsi_fileset_fini(&fs); return ZS_NOTFOUND; }

    qsort(fs.all, fs.nall, sizeof(*fs.all), zsi_salvage_order);

    outsetup.flags = ZS_CREATE;
    outsetup.compar = setup->compar;
    outsetup.compar_name = setup->compar_name;
    outsetup.csum = setup->csum;
    outsetup.error = setup->error;
    r = zs_db_open(to, &outsetup, &out);
    if (r != ZS_OK) { zsi_fileset_fini(&fs); return r; }
    ctx.out = out;

    for (size_t i = 0; i < fs.nall && !ctx.stopped; i++) {
        struct zsi_file *f = NULL;
        zs_csum *cs;
        unsigned csum_id;

        ctx.fname = fs.all[i].name;
        ctx.gen = fs.all[i].start;

        /* S-2 again: a generation absent from the set is reported, and the walk
         * simply carries on.  Nothing is reconstructed (S-12). */
        if (expect_gen && fs.all[i].start > expect_gen)
            zsi_salvage_emit(&ctx, ZS_SALVAGE_GAP, 0,
                             fs.all[i].start - expect_gen, NULL, 0);
        {
            uint32_t end = fs.all[i].end ? fs.all[i].end : fs.all[i].start;
            if (end + 1 > expect_gen) expect_gen = end + 1;
        }

        if (zsi_file_open(from, fs.all[i].name, fs.all[i].start,
                          setup->csum, &f) != ZS_OK) {
            zsi_salvage_emit(&ctx, ZS_SALVAGE_FILE_UNREADABLE, 0, 0, NULL, 0);
            continue;
        }

        if (f->hdr_valid) {
            cs = f->csum;
            csum_id = f->csum_id;

            /* S-4: the source's comparator does not affect the output, which is
             * ordered by the caller's -- but a mismatch means the source was
             * built under an order we are not reproducing, and that is worth
             * saying rather than discovering later. */
            if (out->compar_name[0]
                && memcmp(f->hdr.compar_name, out->compar_name,
                          ZSI_COMPAR_NAME_LEN) != 0)
                setup->error ? setup->error(
                    "source comparator differs from the one salvage was given",
                    "file=<%s>", f->fname) : (void)0;
        } else {
            /* S-5: the header did not validate.  The generation still comes
             * from the filename; only the engine is genuinely unknown. */
            zsi_salvage_emit(&ctx, ZS_SALVAGE_HEADER_INVALID, 0, 0, NULL, 0);

            if (zsi_salvage_engine(f, setup->csum, &cs, &csum_id) != ZS_OK) {
                zsi_salvage_emit(&ctx, ZS_SALVAGE_FILE_UNREADABLE, 0, 0,
                                 NULL, 0);
                zsi_file_close(&f);
                continue;
            }
            zsi_salvage_emit(&ctx, ZS_SALVAGE_ENGINE_GUESSED, csum_id, 0,
                             NULL, 0);
        }

        /* The KIND comes from the filename, not the header: a file whose header
         * is unreadable still has its range in its name (D-1), and that is what
         * says whether to expect spans or a records region. */
        if (fs.all[i].end == 0)
            r = zsi_salvage_unordered(&ctx, f, cs, csum_id);
        else
            r = zsi_salvage_inorder(&ctx, f, cs, csum_id);

        zsi_file_close(&f);
        if (r != ZS_OK) goto out;
    }

    if (r == ZS_OK) r = zsi_salvage_report_stale(&ctx);
    if (r == ZS_OK && ctx.stopped) r = ZS_DONE;

out:
    free(ctx.safe);
    free(ctx.safekeys);
    zsi_fileset_fini(&fs);
    zs_db_close(&out);
    return r;
}
