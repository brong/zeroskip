# zeroskip C Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A clean-room C implementation of the zeroskip append-only ordered
key-value store, conforming to `doc/specification.md`.

**Architecture:** One translation unit (`zeroskip.c`) organised in labelled
sections that layer strictly bottom-up: byte-level primitives, then the two
file kinds, then the file set, then the read and write paths, then repacking,
then the public API. The two file kinds — unordered (append-only spans, no
pointers) and in-order (sorted records, pointer section) — are unified behind
one *per-file cursor* interface, so the read path is written once rather than
per kind. Everything a reader touches is immutable; the only mutable state is
private to a process.

**Tech Stack:** C99, POSIX (`mmap`, `fcntl` record locking, `/dev/urandom`),
vendored `xxhash.h`. No external libraries, no build system beyond a
hand-written `Makefile`.

**Reference implementation for style:** `../twom/` — the sibling library. Match
its conventions: single TU with `/*** SECTION ***/` markers, `enum` return
codes with `ZS_OK = 0`, output through pointer parameters, opaque public
structs, a `zstest` harness with `ASSERT_*` macros and a substring filter.
Poach `xxhash.h`, the UUID generator, the `/dev/urandom` wrapper, and the test
harness scaffolding directly from `twom.c` / `twomtest.c`.

## Global Constraints

Every task's requirements implicitly include these. Values are copied verbatim
from the spec; where a task restates one, the spec wins on any discrepancy.

- **Endianness (F-1, G-0a):** every integer in every structure is little-endian,
  including lengths, counts, generations and offsets. Never rely on host order.
- **Overflow (G-0b):** any arithmetic on a length, count or offset **read from a
  file** MUST be overflow-checked before use. `keylen + vallen + 2` and
  `offset + record_length` are corruption-controlled.
- **Alignment (F-2):** every record begins at an offset that is a multiple of 8
  and occupies a whole multiple of 8 bytes. Padding bytes MUST be zero.
- **Offsets (F-3):** every offset and pointer is an **absolute** byte offset from
  the file start. Nothing is relative to a record, a section or a mapping base.
- **Checksum placement (F-4):** the checksum field is always the **last 4 bytes**
  of the structure it protects, covering every byte from the start of the
  protected region up to (not including) the field. No field-zeroing anywhere.
- **Bounds (F-30):** every length, offset and pointer MUST be bounds-checked
  against the file size before any dereference.
- **Progress (F-29):** iteration computes the next offset from the current
  record's own length fields and MUST verify it is strictly greater than the
  current offset and within bounds.
- **Canonical encoding (F-15, F-26c):** short form whenever `keylen <= 255` and
  `vallen <= 65535`; short terminator whenever the span is `<= 0xFFFFFF` bytes;
  ancestor stored exactly per F-17; `PTRS32` when every record offset fits in 32
  bits, else `PTRS64`. Two implementations MUST produce identical bytes.
- **No mutation (G-1):** nothing is ever written except by appending to a file or
  creating a new one. No file is modified in place or truncated. A file is
  renamed exactly once, from staging name to final name.
- **Magic (F-6), 16 bytes, validated in full:**
  `89 7A 65 72 6F 73 6B 69 70 31 0D 0A 1A 0A 00 00`
- **Checksum engine 1 (F-5b):** `XXH3_64bits` with **seed 0**, stored value is
  `(uint32_t)(h & 0xFFFFFFFF)`, written little-endian.
- **Default comparator name (F-11b):** the ASCII string `memcmp`, NUL-padded to
  16 bytes.
- **Filenames (D-0, D-1):** UUID is the 36-character **lowercase** hyphenated
  RFC 4122 form; generations are **uppercase hexadecimal, zero-padded to 8
  digits**. Data files carry **no extension** (D-1a).
- **Locks (C-1, C-1e):** exactly `fcntl` record locking — `F_SETLK` or
  `F_SETLKW`, `l_type = F_WRLCK`, `l_whence = SEEK_SET`, `l_len = 1`, on
  `zeroskip.lock` byte 0 (write), 1 (repack), 2 (remove). Never `flock`.
- **Default `rollover_size`:** 2MB.
- **C99, no external libraries.** Builds on Linux, macOS and the BSDs.

### Internal naming conventions

- Public API: `zs_db_*`, `zs_txn_*`, `zs_cursor_*`; public types `struct zs_db`,
  `struct zs_txn`, `struct zs_cursor` (opaque in the header).
- Internal types and functions: `zsi_` prefix, all `static`.
- Macros: UPPERCASE.
- Errors: every internal function returning a status returns `enum zs_ret`.

### Section layout of `zeroskip.c`

Written in this order; each section may only call downwards into earlier ones.

```
TUNING              constants, limits
LIBRARY SUPPORT     zmalloc, random bytes, UUID, LE load/store, roundup8,
                    overflow-checked arithmetic
COMPARATORS         F-11a total order
CHECKSUMS           three engines
FILENAMES           D-0/D-1 format and parse
FILE HEADER         §4.3 encode/decode/validate
RECORDS             §4.4-4.7 encode/decode/validate, ancestors
POINTER SECTION     §4.9 section + trailer, binary search
FILE OBJECT         open, mmap, kind detection, bounds
UNORDERED FILE      §4.8 span chain replay, complete-at
PRIVATE INDEX       §5.4 base+delta ordered index
PER-FILE CURSOR     uniform seek/next over both kinds
FILE SET            §5.1-5.2 readdir, D-5 resolution, D-6 tiling
SNAPSHOT            C-4 protocol
FILE LOCKING        §6 three byte locks + in-process mutex
READ PATH           §5.5 D-14 lookup, D-14e merge cursor
WRITE PATH          §5.3 spans, commit, two durability gates
CONVERSION          D-12 unordered -> in-order
REPACK              §5.6 selection and merge
CONSISTENCY         F-28, F-26f, dump
OPEN AND CLOSE      §7
PUBLIC API          §8
```

---

## Task 1: Scaffolding — build, header, test harness

**Files:**
- Create: `Makefile`, `zeroskip.h`, `zeroskip.c`, `zstest.c`, `CLAUDE.md`
- Copy: `xxhash.h` from `../twom/xxhash.h` verbatim
- Modify: `README.md` (rewrite for the new layout)

**Requirements:** §8 (API surface declaration only), §9 harness shape.

**Interfaces produced:**
- `zeroskip.h`: the complete public surface from §8 — `enum zs_ret`, `enum
  zs_flagspec`, `zs_cb`, `zs_compar`, `zs_csum`, `struct zs_open_data`, all
  function prototypes, the three `*_delete` macros. Declared, not implemented.
- `zstest.c`: `ASSERT`, `ASSERT_EQ`, `ASSERT_OK`, `ASSERT_NULL`,
  `ASSERT_NOT_NULL`, `ASSERT_STR_EQ`, `ASSERT_MEM_EQ`, `CB_ASSERT`,
  `CB_ASSERT_EQ`, `CB_ASSERT_OK`, `SKIP`; `static int setup(void)` /
  `static int teardown(void)` giving each test a fresh temp directory under
  `$TMPDIR` or `/tmp`; `struct test_entry { const char *name; void (*func)(void); }`
  and a `tests[]` table; `main` taking an optional substring filter.
- Globals for tests: `static char *basedir` (the temp dir) and
  `static char *dbdir` (`$basedir/db`, **not** created by setup — most tests
  want `ZS_CREATE` to make it).

**Error codes** — following twom's numbering, with the three the spec pins:

```c
enum zs_ret {
    ZS_OK           = 0,
    ZS_DONE         = 1,
    ZS_EXISTS       = -1,
    ZS_IOERROR      = -2,
    ZS_INTERNAL     = -3,
    ZS_LOCKED       = -4,   /* pinned by §8 */
    ZS_NOTFOUND     = -5,   /* pinned by §8 */
    ZS_READONLY     = -6,
    ZS_BADFORMAT    = -7,   /* pinned by §8 */
    ZS_BADUSAGE     = -8,
    ZS_BADCHECKSUM  = -9,
    ZS_FULL         = -10,
    ZS_AGAIN        = -11,
};
```

**Flags** — one 32-bit space, never reused across calls (§8):

```c
enum zs_flagspec {
    ZS_CREATE        = 1<<0,
    ZS_SHARED        = 1<<1,
    ZS_NOCSUM        = 1<<2,
    ZS_NOSYNC        = 1<<3,
    ZS_NONBLOCKING   = 1<<4,
    ZS_IFNOTEXIST    = 1<<11,
    ZS_IFEXIST       = 1<<12,
    ZS_FETCHNEXT     = 1<<13,
    ZS_SKIPROOT      = 1<<14,
    ZS_CURSOR_PREFIX = 1<<16,
    ZS_CSUM_NONE     = 1<<27,
    ZS_CSUM_XXHASH   = 1<<28,
    ZS_CSUM_EXTERNAL = 1<<29,
};
```

- [ ] **Step 1: `Makefile`**

Adapt `../twom/Makefile`. Targets: `libzeroskip.a`, the versioned shared
library with the same Darwin/ELF split, `zstool`, `zstest`, `zsbench`. Plus:

```make
CFLAGS ?= -Wall -Wextra -g -O2 -fno-strict-aliasing -std=c99 -D_GNU_SOURCE
check: zstest
	./zstest
test: check
asan: CFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -O1
asan: clean zstest
	./zstest
```

`-fno-strict-aliasing` for the same reason as twom: `zeroskip.c` reads the
mmap'd `char *` through casts to fixed-width integer types. Note in a comment
that the LE accessors of Task 2 use `memcpy` and so do not actually require it,
but it stays as belt and braces.

- [ ] **Step 2: `zeroskip.h`**

Transcribe §8's declarations exactly, with the enums above. Include
`<stdbool.h>`, `<stdlib.h>`, `<stdint.h>`. Guard `INCLUDED_ZEROSKIP_H`. Add
`#define ZS_OPEN_DATA_INITIALIZER { 0, NULL, NULL, NULL, 0, NULL }` matching
`struct zs_open_data`'s six members in order.

- [ ] **Step 3: `zeroskip.c` skeleton**

The `/*** SECTION ***/` markers from the layout above, in order, each empty.
Includes, and a `zs_strerror` returning a string per code — the one function
implemented in this task.

- [ ] **Step 4: `zstest.c` harness**

Port the framework from `../twom/twomtest.c` lines 40–120 and 240–290 and
5665–5780, renaming `TWOM_`→`ZS_` and `twom`→`zs`. One registered test,
`test_strerror`, asserting `zs_strerror(ZS_OK)` is non-NULL and that two
distinct codes give distinct strings.

- [ ] **Step 5: `CLAUDE.md` and `README.md`**

`CLAUDE.md` modelled on `../twom/CLAUDE.md`: build commands, source layout, the
section list, naming conventions, the pointer to the spec. `README.md`
rewritten — what zeroskip is, how to build, where the spec lives.

- [ ] **Step 6: Verify and commit**

```bash
make clean && make && make check
```
Expected: builds with no warnings, one test passes.

```bash
git add -A && git commit -m "build: scaffolding, public header, and test harness"
```

---

## Task 2: Library support, comparators, checksums

**Files:** `zeroskip.c` (LIBRARY SUPPORT, COMPARATORS, CHECKSUMS), `zstest.c`

**Requirements:** G-0a, G-0b, F-2, F-5, F-5a–F-5e, F-11a, F-11b, A-7, T-2c

**Interfaces produced:**

```c
typedef unsigned char zsi_uuid_t[16];
#define ZSI_UUID_STR_LEN 37          /* 36 + NUL */

static void *zsi_zmalloc(size_t bytes);
static bool  zsi_random_bytes(void *buf, size_t len);
static uint64_t zsi_weak_entropy(void);      /* fallback when no /dev/urandom */
static void  zsi_uuid_generate(zsi_uuid_t uuid);
static void  zsi_uuid_unparse(const zsi_uuid_t uuid, char *out);  /* lowercase, D-0 */
static int   zsi_uuid_parse(const char *in, zsi_uuid_t out);      /* 0 on success */

/* little-endian accessors -- memcpy based, no alignment or host-order assumption */
static uint16_t zsi_get16(const char *p);
static uint32_t zsi_get32(const char *p);
static uint64_t zsi_get64(const char *p);
static void     zsi_put16(char *p, uint16_t v);
static void     zsi_put32(char *p, uint32_t v);
static void     zsi_put64(char *p, uint64_t v);

/* a 24-bit LE field, for the short terminator's span length */
static uint32_t zsi_get24(const char *p);
static void     zsi_put24(char *p, uint32_t v);

static size_t zsi_roundup8(size_t n);   /* saturating: returns 0 on overflow */

/* overflow-checked arithmetic (G-0b).  Each returns false on overflow and
 * leaves *out untouched. */
static bool zsi_add_sz(size_t a, size_t b, size_t *out);
static bool zsi_add3_sz(size_t a, size_t b, size_t c, size_t *out);

/* F-11a: unsigned octet compare over min(alen,blen), shorter sorts first */
static int zsi_compar_default(const char *a, size_t alen,
                              const char *b, size_t blen);
/* F-11b: rejects an empty name and any name longer than 16 bytes. */
static bool zsi_compar_name_valid(const char *name);

/* F-5 engines.  id is 0, 1 or 2. */
static uint32_t zsi_csum_none(const char *buf, size_t len);
static uint32_t zsi_csum_xxhash(const char *buf, size_t len);
#define ZSI_CSUM_NONE     0
#define ZSI_CSUM_XXHASH   1
#define ZSI_CSUM_EXTERNAL 2

/* Resolve an engine id to its function, or NULL for an unknown id.  Engine 2
 * yields db's caller-supplied csum, which may itself be NULL (A-6). */
static zs_csum *zsi_csum_for_id(struct zs_db *db, unsigned id);
/* Map the ZS_CSUM_* open flags to an engine id.  Defaults to 1 (F-5). */
static unsigned zsi_csum_id_for_flags(uint32_t flags);
```

- [ ] **Step 1: LIBRARY SUPPORT**

Port `zsi_zmalloc`, `zsi_random_bytes`, `zsi_weak_entropy`, `zsi_uuid_generate`
and `zsi_uuid_unparse` from `../twom/twom.c` lines 234–320 with the rename.
Write `zsi_uuid_parse` fresh: it MUST accept only the exact 36-character
lowercase hyphenated form with hyphens at positions 8, 13, 18, 23 — reject
uppercase, reject braces, reject a missing hyphen — because D-0 pins the
spelling and a lenient parser lets two implementations disagree about which
files belong to a database.

Write the LE accessors with `memcpy` into a local of the right width, then
byte-assemble; do **not** cast the pointer. `zsi_get24` reads three bytes.

`zsi_roundup8` must saturate rather than wrap: if `n > SIZE_MAX - 7` return 0,
and every caller treats 0 as "invalid length". This is G-0b at its most
load-bearing — `roundup8` is applied to lengths straight out of a file.

- [ ] **Step 2: COMPARATORS**

`zsi_compar_default` per F-11a. Compare `min(alen, blen)` bytes as
**`unsigned char`**; if they differ that decides; otherwise shorter sorts
first. Do not call `memcmp` — F-11a forbids delegating, because `memcmp` only
promises a sign and says nothing about differing lengths. Add a comment saying
so, since the body looks like something an optimiser-minded reader would
"simplify" back to `memcmp`.

Also `static int zsi_compar_name_valid(const char *name)` — rejects an empty
name (F-11b) and any name longer than 16 bytes.

- [ ] **Step 3: CHECKSUMS**

`zsi_csum_none` returns 0. `zsi_csum_xxhash` returns
`(uint32_t)XXH3_64bits(buf, len)` — note that unlike twom's `csum_xxh64` there
is **no** `if (!len) return 0;` short-circuit: F-26g explicitly requires the
engine's value for empty input, and an in-order file with zero records depends
on it. Add a comment marking that difference, since it is exactly the kind of
thing copied across from twom by reflex.

A resolver `static zs_csum *zsi_csum_for_id(struct zs_db *db, unsigned id)`
returning the engine function or NULL for an unknown id, and
`static unsigned zsi_csum_id_for_flags(uint32_t flags)` mapping the
`ZS_CSUM_*` open flags to an id (default 1).

- [ ] **Step 4: Tests — T-2c interoperability constants**

Register `test_interop_constants`. Every assertion is against a **literal**,
never against the implementation's own computation — that is the whole point of
T-2c.

- `zsi_csum_xxhash` over: the empty input; `"a"`; `"abc"`; the 16 magic bytes;
  and a 1024-byte buffer of `0x00..0xFF` repeated. Five hard-coded `uint32_t`
  expectations. **Generate these once by hand** (a throwaway program against
  the vendored `xxhash.h`), paste them in, and never regenerate them from the
  implementation again — if the implementation changes, the literal is the
  thing that must not move.
- `zsi_compar_default` over a table that includes: a key and its own prefix
  (`"ab"` vs `"abc"` — shorter first); two keys differing only above `0x7F`
  (`"\x7f"` vs `"\x80"` — the case a signed-`char` compare gets backwards);
  the empty-versus-one-byte case; and equal keys. Assert the **sign**, not the
  magnitude.
- The exact 16 bytes of the `memcmp` comparator name field:
  `"memcmp\0\0\0\0\0\0\0\0\0\0"`, asserted with `ASSERT_MEM_EQ` over 16 bytes.
- `zsi_uuid_unparse` on a fixed 16-byte array, asserted character for character
  against a lowercase literal; then `zsi_uuid_parse` of that string round-trips
  to the same 16 bytes; then `zsi_uuid_parse` rejects the uppercase form.

Register `test_overflow_guards`: `zsi_roundup8(SIZE_MAX)` and
`zsi_roundup8(SIZE_MAX - 3)` return 0; `zsi_add_sz(SIZE_MAX, 1, &out)` returns
false and leaves `out` untouched.

- [ ] **Step 5: Verify and commit**

```bash
make check && make asan
```

```bash
git commit -am "feat: library support, default comparator, and checksum engines"
```

---

## Task 3: File header

**Files:** `zeroskip.c` (FILE HEADER), `zstest.c`

**Requirements:** F-6, F-6a, F-7, F-8, F-9, F-10, F-11, §4.2, §4.3, T-2

**Interfaces produced:**

```c
#define ZSI_MAGIC_LEN   16
#define ZSI_HEADER_LEN  72
#define ZSI_VERSION_READ  1
#define ZSI_VERSION_WRITE 1
static const unsigned char zsi_magic[ZSI_MAGIC_LEN];

struct zsi_header {
    uint8_t     version_read;
    uint8_t     version_write;
    uint16_t    flags;            /* low 4 bits are the csum engine id */
    zsi_uuid_t  uuid;
    uint32_t    start;
    uint32_t    end;              /* 0 == unordered (F-9) */
    char        compar_name[16];  /* NUL-padded, not NUL-terminated */
};

/* Decode and fully validate.  buf must hold at least len bytes; returns
 * ZS_BADFORMAT if len < ZSI_HEADER_LEN, the magic is wrong, the header
 * checksum fails, or version_read exceeds ours.  Reserved fields are IGNORED,
 * never rejected (F-8).  version_write is recorded, not enforced -- that gate
 * belongs to the writer (F-7). */
static int zsi_header_decode(const char *buf, size_t len,
                             zs_csum *csum, struct zsi_header *out);

/* Encode into a ZSI_HEADER_LEN buffer, computing the checksum with csum. */
static void zsi_header_encode(char *buf, const struct zsi_header *hdr,
                              zs_csum *csum);

static bool zsi_header_is_unordered(const struct zsi_header *h); /* h->end == 0 */
```

- [ ] **Step 1: Implement**

Field offsets exactly per §4.3's table: magic 0, read version 16, write version
17, flags 18, reserved 20 (4), uuid 24 (16), start 40, end 44, comparator name
48 (16), reserved 64 (4), checksum 68 covering `[0, 68)`.

`zsi_header_decode` order of operations matters. Read the flags — and hence the
engine id — as **plain data before any verification** (F-5a), because the
checksum cannot be verified until the engine is known; there is no
bootstrapping problem only if the id is read first. Then: magic (all 16 bytes,
F-6), then checksum, then versions, then reserved fields.

Reserved fields: F-8 says write zero and **ignore on read**. So `zsi_header_decode`
MUST NOT reject a nonzero reserved field — the checksum already covers them, and
rejecting would make a future extension unreadable by this version, which is
precisely what the version fields are for. Compatibility decisions belong to the
version fields, not to reserved bytes.

Version check per F-7: refuse to read above `ZSI_VERSION_READ`. The *write*
version gate belongs to the writer, not the decoder, so `zsi_header_decode`
records `version_write` and leaves the decision to the caller — this is what
lets a file be opened read-only when it is readable but not writable.

- [ ] **Step 2: Tests — T-2**

`test_magic`: assert the 16 literal bytes; assert every one of the 16 single-byte
mutations is rejected (loop over position × a differing value); the four designed
corruptions each rejected — eighth bit stripped from byte 0 (`0x09`), `0D 0A`
collapsed to `0A` (shifting the tail), `0A` expanded to `0D 0A`, and byte 0
replaced by `EF BF BD`; and a hand-written UTF-8 validator run over the 16 bytes
asserting they are **not** valid UTF-8 (F-6a), so the property is proven rather
than believed.

`test_header_roundtrip`: encode/decode round-trips every field, for an unordered
header (`end == 0`) and an in-order one (`end == 5`), under engines 0 and 1.
`zsi_header_is_unordered` agrees.

`test_header_versions`: a header with `version_read = ZSI_VERSION_READ + 1`
rejected; one with `version_write` above ours but `version_read` at ours decoded
successfully with `version_write` reported, so the caller can open it read-only.

`test_header_checksum`: flipping any byte in `[0, 68)` is caught under engine 1;
under engine 0 the checksum field is zero and nothing is verified (F-5c).

`test_header_reserved`: a nonzero reserved field is **accepted** and ignored
(F-8), and the encoder always writes zeros there.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: 72-byte file header with magic and version gates"
```

---

## Task 4: Records and terminators

**Files:** `zeroskip.c` (RECORDS), `zstest.c`

**Requirements:** §4.4–§4.7 in full — F-12, F-12a–c, F-13, F-14, F-15, F-16,
F-16a–c, F-17, F-17a, F-19, F-20, F-21, F-22, T-2b, T-4 (encoding boundaries)

This is the densest task in the plan. The 14 type bytes and their eight data
layouts are transcribed from §4.4 and §4.5; get them wrong and every later task
inherits the error.

**Interfaces produced:**

```c
/* type bits (F-12a) */
#define ZSI_HASKEY      0x01
#define ZSI_ISDELETE    0x02
#define ZSI_ISBIG       0x04
#define ZSI_HASANCESTOR 0x08
#define ZSI_SPANTERM    0x10
#define ZSI_POINTERS    0x20

/* the 14 legal type bytes (F-12) */
#define ZSI_KEYVALUE         0x01
#define ZSI_DELETION         0x03
#define ZSI_BIGKEYVALUE      0x05
#define ZSI_BIGDELETION      0x07
#define ZSI_KEYVALUE_ANC     0x09
#define ZSI_DELETION_ANC     0x0B
#define ZSI_BIGKEYVALUE_ANC  0x0D
#define ZSI_BIGDELETION_ANC  0x0F
#define ZSI_COMMIT           0x10
#define ZSI_ROLLBACK         0x12
#define ZSI_COMMIT_LONG      0x14
#define ZSI_ROLLBACK_LONG    0x16
#define ZSI_PTRS32           0x20
#define ZSI_PTRS64           0x24

/* True for exactly the 14 above and nothing else, including 0x00. */
static bool zsi_type_valid(uint8_t type);

struct zsi_rec {
    uint8_t     type;
    const char *key;    size_t keylen;
    const char *val;    size_t vallen;   /* val == NULL for a deletion */
    uint32_t    ancestor;               /* always resolved, never raw (F-17) */
    size_t      len;                    /* total on-disk bytes, multiple of 8 */
};

struct zsi_term {
    uint8_t     type;
    uint64_t    spanlen;
    uint32_t    csum;
    size_t      len;                    /* 8 or 24 */
};

/* Decode the record at buf[0..len).  file_start is the containing file's
 * start generation, used to resolve an omitted ancestor (F-17).  Validates
 * the type byte, bounds-checks every length, and enforces F-14 (keylen >= 1).
 * Does NOT verify a checksum -- records have none of their own; the span
 * terminator covers them (F-19). */
static int zsi_rec_decode(const char *buf, size_t len, uint32_t file_start,
                          struct zsi_rec *out);

/* Bytes this record will occupy.  Returns 0 if the inputs cannot be encoded. */
static size_t zsi_rec_encoded_len(size_t keylen, size_t vallen, bool isdelete,
                                  bool store_ancestor);

/* Encode into buf, which must hold zsi_rec_encoded_len bytes.  Chooses the
 * canonical form per F-15.  store_ancestor is decided by the caller per F-17. */
static void zsi_rec_encode(char *buf, const char *key, size_t keylen,
                           const char *val, size_t vallen,
                           bool store_ancestor, uint32_t ancestor);

static int zsi_term_decode(const char *buf, size_t len, struct zsi_term *out);
static size_t zsi_term_encoded_len(uint64_t spanlen);   /* 8 or 24 */
static void zsi_term_encode(char *buf, uint64_t spanlen, bool rollback,
                            const char *spandata, zs_csum *csum);
```

F-12b is worth internalising before writing any of this: **each data shape has
exactly two forms, one storing an ancestor and one omitting it, and nothing
distinguishes a "create" at the record level** — because nothing needs to (F-17).
There is no `CREATE` type and no create bit. An implementer coming from a
write-ahead-log background will look for one and must not invent it.

- [ ] **Step 1: `zsi_type_valid`**

Write it as an explicit `switch` over the 14 values, not as a bit-property
computation. F-12's table is normative and a computed predicate is a second
specification that can drift from it. A comment should say why.

- [ ] **Step 2: Decode**

Eight data layouts, transcribed from §4.5. Every one: bounds-check the fixed
header first, then read the lengths, then `zsi_add3_sz(header, keylen + 1,
vallen + 1)` overflow-checked, then `zsi_roundup8`, then check the total against
`len`. Only then set `out->key` and `out->val`.

`vallen` is absent from the deletion forms; set `val = NULL, vallen = 0`.
Lengths are authoritative and exclude the NUL terminators (F-13); a key of
length 0 is invalid (F-14) but a value of length 0 is legal.

Ancestor resolution (F-17): if `type & ZSI_HASANCESTOR`, read the stored
generation; otherwise `out->ancestor = file_start`. The caller never sees "not
stored" — that is the whole point of the rule.

`zsi_term_decode`: 8-byte form reads a 24-bit span length via `zsi_get24`;
24-byte form reads 64 bits at +8 and the checksum at +20. F-20 — terminators are
only ever found by scanning forward, so nothing here may search backwards.

- [ ] **Step 3: Encode**

`zsi_rec_encoded_len` picks the form: big when `keylen > 255 || vallen > 65535`
(F-15), and note explicitly that the ancestor never influences that choice —
it is 4 bytes whenever present, and in the big forms it fits in existing padding
(F-12c), so `BIGKEYVALUE` and `BIGKEYVALUE_ANC` are both 24-byte headers.

Zero **all** padding, including the pad between fields, not just the tail
padding (F-2). Canonical encoding means byte-for-byte reproducibility (T-12a);
an uninitialised pad byte breaks it and is invisible to every test that reads
back through the decoder.

`zsi_term_encode` computes the checksum over the span's data bytes **followed by
the terminator's own bytes up to the checksum field** (F-19). Implement by
writing the terminator's non-checksum bytes into `buf` first, then running the
engine over `spandata[0..spanlen)` and `buf[0..len-4)` — which needs a
two-region checksum. Since `zs_csum` takes one contiguous buffer, the terminator
is small: copy the span tail is wrong (the span may be large), so instead give
the engine the two regions via a helper that the xxHash streaming API supports:

```c
/* Checksum two regions as though concatenated.  Engine 1 uses XXH3's
 * streaming state; engines 0 and 2 fall back to a temporary join, which is
 * acceptable because engine 0 ignores its input and engine 2 is caller-supplied
 * and out of the conformance corpus (F-5d). */
static uint32_t zsi_csum2(zs_csum *csum, unsigned id,
                          const char *a, size_t alen,
                          const char *b, size_t blen);
```

For engine 2 the join allocates; document that as the cost of a caller-supplied
engine. For engine 1 use `XXH3_64bits_reset` / `_update` / `_digest` and assert
in a test that it agrees with the one-shot over a joined buffer.

- [ ] **Step 4: Tests — T-2b, and encoding**

`test_type_byte_validity`: loop all 256 values through `zsi_type_valid`,
asserting exactly the 14 in F-12's table are accepted and the other 242
rejected. Then assert specifically that the near-misses are rejected: two family
bits at once (`0x11`, `0x21`, `0x31`), `HasAncestor` without `HasKey` (`0x08`,
`0x18`), `IsDelete` with `Pointers` (`0x22`, `0x26`), either reserved bit set
(`0x41`, `0x81`), and `0x00`.

`test_record_roundtrip`: every one of the eight data shapes encodes and decodes
back to the same key, value, deletion-ness and ancestor. Assert `len` is a
multiple of 8 and that all pad bytes are zero.

`test_record_canonical`: the form chosen for each boundary — `keylen` 255 vs
256, `vallen` 65535 vs 65536 — asserting the exact type byte, so a wrong
threshold fails here rather than as a cross-implementation diff much later.
Assert an ancestor never promotes a record to the big form.

`test_record_embedded_nul`: keys and values containing NUL bytes round-trip,
with the lengths authoritative (F-13). A key that is entirely NULs.

`test_record_bounds`: for each shape, decoding with `len` one byte short of the
true length is rejected; a record claiming `keylen = SIZE_MAX` is rejected
rather than overflowing; a `keylen` of 0 is rejected (F-14).

`test_terminator`: short form for a span of `0xFFFFFF` bytes and long form for
`0x1000000` (F-15's boundary); checksum covers span-then-terminator (F-19),
verified by mutating one byte of the span and one byte of the terminator and
asserting both are caught; `zsi_csum2` agrees with a one-shot over a joined
buffer.

- [ ] **Step 5: Verify and commit**

```bash
make check && make asan
git commit -am "feat: record and terminator encoding, all fourteen type bytes"
```

---

## Task 5: Filenames

**Files:** `zeroskip.c` (FILENAMES), `zstest.c`

**Requirements:** D-0, D-1, D-1a, D-2, D-4, D-20a, T-2c, part of T-9

**Interfaces produced:**

```c
#define ZSI_NAME_MAX 128

enum zsi_nametype {
    ZSI_NAME_OTHER = 0,      /* not ours -- ignore */
    ZSI_NAME_UNORDERED,      /* zeroskip-<uuid>-<gen> */
    ZSI_NAME_INORDER,        /* zeroskip-<uuid>-<start>-<end> */
};

/* Parse a directory entry.  Returns the kind; fills uuid/start/end when it is
 * a data file.  For ZSI_NAME_UNORDERED, *end is set to 0 (F-9). */
static enum zsi_nametype zsi_name_parse(const char *name, zsi_uuid_t uuid,
                                        uint32_t *start, uint32_t *end);

/* Format into out, which must hold ZSI_NAME_MAX bytes.  end == 0 gives the
 * unordered form. */
static void zsi_name_format(char *out, const zsi_uuid_t uuid,
                            uint32_t start, uint32_t end);

#define ZSI_LOCK_NAME "zeroskip.lock"
static void zsi_staging_name(char *out, unsigned n);   /* zeroskip.tmp.<pid>.<n> */
```

- [ ] **Step 1: Implement**

`zsi_name_format`: `"zeroskip-%s-%08X"` or `"zeroskip-%s-%08X-%08X"` with the
lowercase unparsed UUID. Uppercase hex for generations (D-1), lowercase UUID
(D-0) — the contrast is deliberate, note it in a comment.

`zsi_name_parse` must be strict: exactly 8 hex digits per generation, uppercase
only, no leading `0x`, nothing trailing. A data file carries **no extension**
(D-1a) so a trailing `.zs` or `.tmp` makes it `ZSI_NAME_OTHER`. Reject
generation 0 as a `start` (F-9). Anything beginning `zeroskip.` is metadata, not
data (D-2), and returns `ZSI_NAME_OTHER`.

- [ ] **Step 2: Tests**

`test_filenames`: T-2c's generated-filename assertion — a known UUID and
generation range formatted character for character against a literal, both
forms. Round-trip parse. Reject: lowercase hex generations, 7 or 9 digits, a
`.zs` suffix, a trailing `-`, a missing UUID hyphen, `zeroskip.tmp.1.0`,
`zeroskip.lock`, and unrelated names.

`test_filename_prefix_property`: **the D-1a property asserted directly on
generated names** — for a range of generations, `strncmp(unordered_name,
inorder_name, strlen(unordered_name)) == 0` and `strcmp(unordered_name,
inorder_name) < 0`. D-5's whole resolution rule rests on this, and T-9 requires
it be a test so that adding an extension later breaks a test rather than the
database.

`test_filename_lexical_order`: for a set of ranges, lexical order of the
formatted names matches numeric order of `(start, end)` — the property fixed
width buys (D-1).

- [ ] **Step 3: Verify and commit**

```bash
make check
git commit -am "feat: data file naming, with the D-1a prefix property under test"
```

---

## Task 6: File object — open, map, kind detection

**Files:** `zeroskip.c` (FILE OBJECT), `zstest.c`

**Requirements:** F-31, F-30, G-0, D-10, D-10a, part of T-6

**Interfaces produced:**

```c
struct zsi_file {
    char             *fname;       /* full path, owned */
    int               fd;
    const char       *base;        /* mmap, or NULL for a zero-length file */
    size_t            size;        /* st_size at map time */
    struct zsi_header hdr;
    zs_csum          *csum;        /* resolved from hdr.flags (F-5a) */
    unsigned          csum_id;
    int               hdr_valid;   /* 0 for the D-10 case */

    /* unordered (hdr.end == 0) */
    size_t            complete;    /* F-24 complete point; 0 until replayed */
    struct zsi_index *index;

    /* in-order (hdr.end != 0) */
    size_t            ptr_off;
    uint64_t          nptrs;
    int               ptr_wide;
};

/* Open and map.  Decodes and validates the header.  If the header is invalid
 * or the file is zero length, sets hdr_valid = 0 and leaves hdr.start taken
 * from the caller-supplied name_start (D-10) -- the caller decides whether that
 * is tolerable (active file: yes; any other file: ZS_BADFORMAT, D-10a). */
static int zsi_file_open(struct zs_db *db, const char *dir, const char *name,
                         uint32_t name_start, struct zsi_file **out);
static void zsi_file_close(struct zsi_file **fp);

/* Bounds-checked access: returns NULL unless [off, off+len) lies within the
 * file (F-30).  Every dereference of file data goes through this. */
static const char *zsi_file_at(const struct zsi_file *f, size_t off, size_t len);
```

- [ ] **Step 1: Implement**

`open` read-only, `fstat` for the size, `mmap` `PROT_READ, MAP_SHARED`. A
zero-length file cannot be mapped — handle it as `base = NULL, size = 0`, which
D-10 requires be a legal state rather than an error.

`zsi_file_at` is the single choke point for bounds checking. Use
`zsi_add_sz(off, len, &end)` then `end <= f->size`. Everything downstream calls
it; no other code indexes `base` directly. Say so in a comment — it is the
difference between one audited check and thirty unaudited ones.

Resolve the checksum engine from `hdr.flags & 0x0F` (F-5a). If the id is 2 and
the caller supplied no `csum`, that is an error (A-6) — but raise it at the
*database* level in Task 14, not here, so that a tool can still inspect the
file.

- [ ] **Step 2: Tests**

`test_file_open_kinds`: hand-build a 72-byte unordered header file and a
96-byte empty in-order file (F-26g's exact layout) on disk; assert both open,
that the kind is determined from `hdr.end` alone without reading further, and
that `zsi_file_at` refuses one byte past the end, a length that overflows, and
an offset past the end.

`test_file_zero_length`: an empty file opens with `hdr_valid == 0` and does not
crash (D-10).

`test_file_garbage_header`: 72 bytes of `0xFF` opens with `hdr_valid == 0`.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: file object with a single bounds-checked accessor"
```

---

## Task 7: Unordered files — the span chain

**Files:** `zeroskip.c` (UNORDERED FILE), `zstest.c`

**Requirements:** §4.8 — F-23, F-24, F-25, F-26h, F-29, F-22, R-2, R-4, D-9, D-10

**Interfaces produced:**

```c
/* Callback invoked for each committed record, in file order, during replay.
 * Rolled-back spans are never presented (F-25).  Return non-zero to stop. */
typedef int zsi_replay_cb(void *rock, const struct zsi_rec *rec, size_t off);

/* Walk the span chain from the end of the header.  Sets f->complete to the
 * offset after the last valid span (F-24) -- which may be short of f->size,
 * and is the header length for a file with no valid spans.  Never fails on a
 * torn tail; that is an ordinary outcome, not an error. */
static int zsi_unordered_replay(struct zsi_file *f, zsi_replay_cb *cb, void *rock);

/* True iff f->complete == f->size: nothing follows the last valid span (D-9). */
static bool zsi_unordered_is_clean(const struct zsi_file *f);
```

- [ ] **Step 1: Implement the two-pass span walk**

A span is "zero or more data records followed by exactly one terminator" (F-23),
and a span's records are only live once its terminator validates. So the walk
per span is:

1. From `pos`, scan records forward until a terminator type is met, accumulating
   the data byte count. Any record that fails to decode ends the file at the
   span's start — not at the record — because the span was never terminated.
2. Decode the terminator. If its `spanlen` does not equal the accumulated data
   byte count, the file ends at the span's start.
3. Verify the checksum over span-data-then-terminator (F-19) via `zsi_csum2`.
   Failure ends the file at the span's start. **This is the mechanism that makes
   a torn tail detectable (F-22) and that lets a reader safely scan a file a
   writer is still appending to (C-4f)** — comment it as such, because it looks
   like an ordinary checksum check and is in fact the load-bearing one.
4. If the terminator is a `COMMIT`, replay the span's records through `cb`. If
   it is a `ROLLBACK`, skip them (F-21, F-25). Either way advance `pos` past the
   terminator and set `f->complete = pos`.

F-25 is why this cannot be a watermark: a rolled-back span may sit between two
live ones, so the walk must be per span all the way to the end.

F-29: every step forward must be strictly greater than the current offset and
within bounds. `zsi_rec_decode` returning `len == 0`, or a `len` that would not
advance, terminates the walk. Non-termination must be impossible by
construction, and T-3's per-case timeout is the detector for a mistake here.

Note the two-pass shape: pass 1 finds the terminator and validates, pass 2
replays. Records are decoded twice per span. That is deliberate — the
alternative is buffering an unbounded span's records — and it costs nothing
measurable because the span is already in page cache from pass 1.

- [ ] **Step 2: Tests**

`test_span_basic`: build files by hand (a small helper that appends records and
a terminator, computing the checksum) — one commit span of three records
replays all three; a rollback span replays none; commit, rollback, commit
replays only the first and third **and in that order** (F-25).

`test_span_torn_tail`: a file whose last span is missing its terminator —
`complete` is the offset before that span, `is_clean` is false, and the earlier
spans still replay. Then the same file with the terminator present but the last
data byte flipped: the checksum fails and the outcome is identical (F-22).

`test_span_terminator_without_data`: a terminator written where its span's data
never landed — reads as absent, which is exactly F-22's guarantee.

`test_span_empty_file`: a file that is only a header — `complete` is 72,
`is_clean` is true, zero records (F-26h).

`test_span_all_rolled_back`: every span rolled back — clean, zero records, not
an error (F-26h).

`test_span_progress`: a hand-built record whose length field would make the next
offset equal to or less than the current one — the walk terminates rather than
looping. Run under a wall-clock alarm so a regression hangs the test rather than
the suite.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: span chain replay, with the terminator checksum as the tear detector"
```

---

## Task 8: The private index

**Files:** `zeroskip.c` (PRIVATE INDEX), `zstest.c`

**Requirements:** §5.4 — D-13, D-13a, D-13b, D-13c, D-13d

**Design.** An unordered file has no pointer section, so key order is derived by
replaying spans. The index must support point lookup, lower-bound seek **and
ordered traversal** (D-13) — a hash table is explicitly insufficient.

Structure: a **sorted array of record offsets** into the file, plus a small
sorted **delta** array for records committed since the base was built. The base
is built once (sort at the end of replay) and the delta absorbs the writer's
incremental commits (D-13b), merging into the base when it exceeds
`ZSI_DELTA_MAX`. Lookup consults the delta first, then the base. Traversal
merges the two, preferring the delta on equal keys.

Why not a single array with splice-insert: the active file holds up to
`rollover_size` of records, so a bulk load would memmove megabytes per commit.
Why not a tree: a sorted array is cache-friendly, the base is immutable once
built, and the delta bound keeps insertion cheap. D-14i already argues the same
shape for the merge cursor.

**Interfaces produced:**

```c
#define ZSI_DELTA_MAX 1024

struct zsi_index {
    struct zsi_file *file;
    uint32_t *base;   size_t nbase;
    uint32_t *delta;  size_t ndelta, adelta;
};

/* Build by replaying committed spans, keeping only the newest record per key
 * (D-13a).  Never walks every record blindly -- that would resurrect aborted
 * writes. */
static int zsi_index_build(struct zsi_file *f);
static void zsi_index_free(struct zsi_index **ip);

/* Point lookup.  Sets *off to the record offset; ZS_NOTFOUND if absent.
 * Consults the delta first, so a key present in both yields the newer. */
static int zsi_index_find(struct zsi_index *ix, struct zs_db *db,
                          const char *key, size_t keylen, size_t *off);

/* Ordered traversal (D-13).  A cursor over the merged base+delta ordering.
 * There is deliberately no random access: the merged view is not an array, and
 * the only consumer -- the per-file cursor of Task 10 -- advances
 * sequentially. */
struct zsi_index_cur { size_t bi, di; };

/* Position at the first entry with key >= the given key. */
static void zsi_index_cur_seek(struct zsi_index *ix, struct zs_db *db,
                               const char *key, size_t keylen,
                               struct zsi_index_cur *c);
static void zsi_index_cur_seek_first(struct zsi_index *ix,
                                     struct zsi_index_cur *c);
/* ZS_DONE when exhausted, ZS_OK with *out filled otherwise. */
static int  zsi_index_cur_get(struct zsi_index *ix, struct zsi_index_cur *c,
                              struct zsi_rec *out);
static void zsi_index_cur_next(struct zsi_index *ix, struct zs_db *db,
                               struct zsi_index_cur *c);

/* Writer's incremental maintenance (D-13b). */
static int zsi_index_insert(struct zsi_index *ix, struct zs_db *db, size_t off);
```

- [ ] **Step 1: Implement build**

Replay via `zsi_unordered_replay`, collecting every committed record's offset.
Sort by key using the database comparator, with ties — the same key appearing
more than once — broken by **offset descending**, then keep only the first of
each run. That is "newest wins within a file" (D-14) implemented as a sort
property rather than a separate pass.

Comparison during the sort reads keys through `zsi_rec_decode` at each offset.
That is a decode per comparison; acceptable at `rollover_size` scale and much
simpler than materialising keys. Note the tradeoff in a comment so a future
optimiser knows it was a choice.

- [ ] **Step 2: Implement lookup, traversal and insert**

`zsi_index_find` binary-searches the delta, then the base, and returns the first
hit — delta first, so a key present in both yields the newer record. Both
searches decode the record at each probe to read its key.

`zsi_index_cur_seek` binary-searches **both** arrays independently, leaving `bi`
and `di` at each side's lower bound. `zsi_index_cur_get` returns whichever side's
current key is smaller, preferring the **delta** on a tie since it is newer.
`zsi_index_cur_next` advances whichever side was just yielded, and when the two
keys were equal advances **both** — otherwise the base's stale copy of the key
surfaces on the following step, which is D-14h's guarantee broken one level down.

There is deliberately no random access into the merged view. A count would be an
upper bound only (a key may appear in both arrays), and an index-addressed `at()`
would have to re-walk the merge from the start on every call. The sequential
cursor is both cheaper and the only thing Task 10 needs.

`zsi_index_insert` splices into the delta (small, so memmove is cheap), replacing
an existing delta entry for the same key. On exceeding `ZSI_DELTA_MAX`, merge
delta into base — a linear merge, delta winning ties — and clear the delta.

- [ ] **Step 3: Tests**

`test_index_committed_only`: a file with a rolled-back span containing key `k`
and no other record for `k` — the index does not contain `k` (D-13a). This is
the test that catches "walk every record" as an implementation shortcut.

`test_index_newest_per_key`: key `k` written three times in one file at
increasing offsets — the index yields exactly one entry for `k`, the one at the
highest offset.

`test_index_ordered_traversal`: keys inserted in random order traverse in
comparator order; lower-bound seek to a key present, to a key absent between two
present, to a key before all, and to a key after all.

`test_index_delta`: build a base, insert `ZSI_DELTA_MAX + 10` records one at a
time asserting lookup and traversal stay correct across the merge boundary;
insert a key that already exists in base and assert the delta's version wins in
both lookup and traversal.

`test_index_empty`: an index over a file with no committed records — lookup
returns `ZS_NOTFOUND`, the cursor is immediately exhausted (F-26h). D-14b
requires this be an ordinary case, not a special one.

- [ ] **Step 4: Verify and commit**

```bash
make check && make asan
git commit -am "feat: private ordered index over unordered files"
```

---

## Task 9: In-order files — pointer section and trailer

**Files:** `zeroskip.c` (POINTER SECTION), `zstest.c`

**Requirements:** §4.9 — F-26, F-26a–h, F-27, F-28, F-31, D-21, T-2a, part of T-6

**Interfaces produced:**

```c
#define ZSI_TRAILER_LEN 16
#define ZSI_INORDER_MIN 96      /* 72 header + 8 empty PTRS32 + 16 trailer */

/* Read the trailer and pointer section, verify the section's checksum, and
 * populate f->ptr_off / f->nptrs / f->ptr_wide.  O(1): does not touch the
 * records region (F-31).  The records checksum is NOT verified here (F-26f). */
static int zsi_ptrs_load(struct zsi_file *f);

/* The record offset at index i.  i must be < f->nptrs. */
static uint64_t zsi_ptrs_at(const struct zsi_file *f, uint64_t i);

/* Binary search for key.  Sets *idx to the first index whose key is >= key,
 * and *exact to whether it matches.  With nptrs == 0, sets *idx = 0 and
 * *exact = false -- an ordinary case (F-26g), not a special one. */
static int zsi_ptrs_search(struct zsi_file *f, struct zs_db *db,
                           const char *key, size_t keylen,
                           uint64_t *idx, bool *exact);

/* Verify the records-region checksum (F-26e).  Called on demand only. */
static int zsi_ptrs_verify_records(struct zsi_file *f);

/* Writer side: emit a pointer section and trailer for records already written.
 * offs must be sorted by key ascending.  Chooses PTRS32/PTRS64 per F-26c. */
static int zsi_ptrs_write(int fd, size_t records_end, const uint64_t *offs,
                          size_t n, uint32_t records_csum, zs_csum *csum,
                          unsigned csum_id);
```

- [ ] **Step 1: Implement load**

Order of operations, all of which F-26a/F-26b make safe:

1. `f->size >= ZSI_HEADER_LEN + ZSI_TRAILER_LEN`, else `ZS_BADFORMAT`.
2. Read the 16-byte trailer at `f->size - 16` — fixed size and fixed position,
   so it needs no prior knowledge of the file (F-26a).
3. Back pointer is plain data at `size-16`, 8 bytes wide **even in a narrow
   file** (F-26a). Bounds-check it: `>= ZSI_HEADER_LEN`, `< size - 16`, and
   8-aligned.
4. Read the section type at the back pointer. `PTRS32` or `PTRS64`, nothing
   else.
5. Verify the pointer-section checksum over `[ptr_off, size-4)` — section,
   padding, back pointer and records checksum, per F-26b with no special case.
   The back pointer was read as plain data first, so there is no circularity.
6. Read `count` at the width the type states, bounds-check
   `ptr_off + header + width*count <= size - 16`.

Verify each pointer is 8-aligned and lies between the header and the section
(F-27). With `count == 0` that loop runs zero times and the requirement is
vacuous.

`zsi_ptrs_verify_records` checksums `[ZSI_HEADER_LEN, ptr_off)` and compares
with the trailer's field. Never called from open (F-26f) — open stays O(1).

- [ ] **Step 2: Implement search and write**

Binary search comparing the key at each probe via `zsi_rec_decode`. Because a
repack emits one record per key (D-17), the array is a strict ordering, so
plain lower-bound is correct.

Implement D-14d's first-and-last probe as an optional fast path behind a
compile-time `ZSI_PROBE_ENDS` (default on), because T-5a requires results be
compared with it disabled to confirm it changes nothing.

`zsi_ptrs_write`: pad the narrow section with zeroes to a multiple of 8 (F-26d —
0 or 4 bytes, and the checksum covers it), then the 16-byte trailer. Width by
F-26c: `PTRS32` iff every offset fits in 32 bits, equivalently iff the section's
own offset fits, since all records precede it.

- [ ] **Step 3: Tests — T-2a and T-6**

`test_inorder_empty`: an in-order file with zero records round-trips as
**exactly 96 bytes**, is written as `PTRS32`, and carries the engine's
empty-input checksum for its records region — **not zero** (F-26g). Assert the
byte count with a literal 96.

`test_trailer_negatives`: each rejected rather than read — a back pointer past
the end of the file; before the header; not 8-aligned; pointing at a byte that
is neither `PTRS32` nor `PTRS64`; a file shorter than header plus trailer; a
corrupted pad byte (caught by the section checksum, F-26d).

`test_records_checksum_on_demand`: a file whose record body is corrupted in
place opens successfully and reads records, but `zsi_ptrs_verify_records`
reports the corruption (F-26e, F-26f). Nothing else in an in-order file would
detect it — that is why the test matters.

`test_ptrs_widths`: a normally written file whose offsets all fit is `PTRS32`
(F-26c); a **hand-constructed** `PTRS64` file is honoured for the width it
states, so the wide form is covered without writing 4GB; both padding cases
(0 and 4 bytes) round-trip.

`test_ptrs_search`: search over 0, 1, 2 and many records; keys before the first
and after the last; exact and inexact hits; and with `ZSI_PROBE_ENDS` compiled
out, identical results throughout.

`test_ptrs_strictly_increasing`: a hand-built file with two pointers out of key
order, and one with a duplicated key, both reported (F-28) — the check itself
lands in Task 20, so here assert the primitive that Task 20 will call.

- [ ] **Step 4: Verify and commit**

```bash
make check && make asan
git commit -am "feat: pointer section and trailer, with O(1) in-order open"
```

---

## Task 10: The per-file cursor

**Files:** `zeroskip.c` (PER-FILE CURSOR), `zstest.c`

**Requirements:** D-14b, D-14e (step 1), D-14h, F-26g, F-26h

This is the abstraction that lets the read path be written once. Both file
kinds, and the write transaction's own map, present the same four operations.

**Interfaces produced:**

```c
enum zsi_src_kind { ZSI_SRC_INORDER, ZSI_SRC_UNORDERED, ZSI_SRC_TXN };

struct zsi_fcur {
    enum zsi_src_kind kind;
    struct zsi_file  *file;      /* NULL for ZSI_SRC_TXN */
    struct zs_txn    *txn;       /* NULL otherwise */
    uint32_t          gen;       /* file->hdr.start; UINT32_MAX for the txn (D-14g) */
    bool              exhausted;
    struct zsi_rec    cur;       /* valid iff !exhausted */
    /* kind-specific position */
    uint64_t              pi;    /* in-order: pointer array index */
    struct zsi_index_cur  ic;    /* unordered: index cursor */
    size_t                ti;    /* txn: index into its sorted map */
};

/* Position at the first record with key >= the given key, or exhaust.  A
 * source holding no records exhausts immediately -- ordinary, not an error. */
static int zsi_fcur_seek(struct zsi_fcur *fc, struct zs_db *db,
                         const char *key, size_t keylen);
static int zsi_fcur_seek_first(struct zsi_fcur *fc, struct zs_db *db);
static int zsi_fcur_next(struct zsi_fcur *fc, struct zs_db *db);

/* Single-key search within one source (D-14b), independent of cursor state. */
static int zsi_fcur_find(struct zsi_fcur *fc, struct zs_db *db,
                         const char *key, size_t keylen, struct zsi_rec *out);
```

- [ ] **Step 1: Implement**

In-order: `zsi_ptrs_search` for seek, `pi++` for next, decode at
`zsi_ptrs_at(pi)`. Unordered: `zsi_index_cur_*`. Txn: binary search then
sequential walk over the transaction's sorted map (defined in Task 16; until
then the `ZSI_SRC_TXN` arm is a stub returning exhausted, and Task 16 fills it
in — note that explicitly so the implementer does not block).

D-14h: a per-file cursor never yields the same key twice. In-order files hold
one record per key by construction (D-17); the private index exposes only the
newest committed record per key (D-13a). Assert this in a debug build.

`gen` for an in-order file is `hdr.start`. D-14g gives the transaction
`UINT32_MAX` so its records sort above every file's for equal keys without a
special case in the merge comparator — which is why the field exists at all.

- [ ] **Step 2: Tests**

`test_fcur_uniform`: the same assertions driven against all three kinds — seek
to a key present, absent-between-two, before all, after all; walk to
exhaustion; seek on an empty source exhausts immediately (F-26g for an empty
in-order file, F-26h for an unordered one with no committed records).

`test_fcur_no_duplicate_keys`: for a file with a key written three times, the
cursor yields it once (D-14h).

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: uniform per-file cursor over both file kinds"
```

---

## Task 11: The file set

**Files:** `zeroskip.c` (FILE SET), `zstest.c`

**Requirements:** §5.2 — D-4, D-4a, D-5, D-5a, D-5b, D-5c, D-6, D-7, D-9b, and
most of T-9

**Interfaces produced:**

```c
struct zsi_entry {
    char     name[ZSI_NAME_MAX];
    uint32_t start, end;      /* end == 0 for unordered */
};

struct zsi_fileset {
    struct zsi_entry *all;      size_t nall;      /* every matching name */
    struct zsi_entry *resolved; size_t nresolved; /* D-5's winners, ascending */
    zsi_uuid_t uuid;
    bool       have_uuid;
};

/* readdir the directory, keep names matching zeroskip-<uuid>-* (D-4).  If
 * want_uuid is NULL the UUID is discovered and all files must agree (D-4a). */
static int zsi_fileset_scan(struct zs_db *db, const char *dir,
                            const zsi_uuid_t *want_uuid,
                            struct zsi_fileset *out);
static void zsi_fileset_fini(struct zsi_fileset *fs);

/* D-5's single sweep over the sorted names.  Returns ZS_OK when the resolved
 * set tiles (D-6), ZS_AGAIN when it leaves a gap (D-7 -- retry), or
 * ZS_BADFORMAT for a partial overlap (D-5c). */
static int zsi_fileset_resolve(struct zsi_fileset *fs);

/* One above the highest generation present in `all` -- not in `resolved`
 * (D-9b).  ZS_FULL past 0xFFFFFFFF (D-9c). */
static int zsi_fileset_next_gen(const struct zsi_fileset *fs, uint32_t *out);
```

- [ ] **Step 1: Implement scan**

`readdir`, `zsi_name_parse` each entry, keep data files whose UUID matches. When
discovering (D-4a): parse the UUID from each `zeroskip-*` name and require they
all agree; **disagreement is `ZS_BADFORMAT`, never a majority vote** — silently
adopting one would read half a database and call it whole. A directory with no
data files leaves `have_uuid = false`, the empty case D-8a handles.

Sort `all` by name, lexically. D-1's fixed-width hex makes lexical order numeric
and D-1a makes the unordered name a prefix of the in-order one — the two
properties D-5's rule rests on, both under test from Task 5.

- [ ] **Step 2: Implement resolve**

D-5 verbatim:

> Start at the lowest generation present. Repeatedly take the **last** file
> whose `start` equals the current generation, then set the current generation
> to that file's `end + 1` (or `start + 1` for an unordered file). Stop when no
> file starts at the current generation.

Taking the **last** is the whole rule, and it is correct only because of the
sort — D-5a's table is the proof, and T-9 asserts that taking the *first*
fails, so the error is caught rather than rediscovered.

After the sweep, D-6's completeness test: the resolved ranges must tile a
contiguous interval from the lowest generation present through the highest.
Any gap → `ZS_AGAIN` (a torn `readdir`, D-7, or genuine loss).

D-5c: a **partial** overlap — two ranges that intersect where neither contains
the other — cannot arise from any legal sequence and is `ZS_BADFORMAT`, not
something to resolve. Detect it while sweeping: if a file's range starts inside
the previously taken range but ends beyond it, that is partial.

`zsi_fileset_next_gen` scans `all`, not `resolved`, because a superseded file
still pins the generation (D-9b) — the highest generation present must never
regress.

- [ ] **Step 3: Tests — T-9**

These need only *names*, so most cases are driven by `touch`ing files with the
right names into a temp directory. Add a helper `seed_names(const char *dir,
const char *const *names)`.

`test_fileset_derives_from_names`: the set and every range derived from
filenames alone, asserted by seeding names with **empty** files — if anything
opened them it would fail.

`test_fileset_overlap_table`: each row of D-5a — a repack output `[1-4]` with
inputs `[1-1]`..`[4-4]`, asserting `[1-4]` wins and the contained files are
ignored for reading; a conversion output `5-5` with its input `5`, asserting the
in-order file wins; all three of `5`, `5-5`, `5-9` at once, asserting `[5-9]`.
The same with some inputs already unlinked, asserting the set still tiles.

`test_fileset_first_vs_last`: taking the **first** file at each step asserted to
produce a wrong set, so D-5b's requirement is under test rather than assumed.

`test_fileset_gaps`: rejected and reported `ZS_AGAIN` — a missing middle
generation; a gap at the bottom. And `ZS_BADFORMAT` for two files claiming
overlapping ranges that are not nested (D-5c).

`test_fileset_uuid_disagreement`: two UUIDs in one directory → `ZS_BADFORMAT`,
not a majority choice (D-4a).

`test_fileset_ignores_foreign`: staging names (`zeroskip.tmp.123.0`), the lock
file, another database's UUID, and unrelated files are all ignored (D-4, D-2).

`test_fileset_next_gen`: one above the highest present, **including after files
have been removed** — seed `1-4` and `5`, remove `5`, assert the next generation
is still computed from what is present (D-9b). And `ZS_FULL` when the highest is
`0xFFFFFFFF` (D-9c).

`test_fileset_mid_conversion_stable`: a directory left mid-conversion (`5` and
`5-5` both present) is judged complete, and leaving it that way indefinitely —
as a writer death would — does not make readers retry forever.

- [ ] **Step 4: Verify and commit**

```bash
make check && make asan
git commit -am "feat: file set discovery and D-5 overlap resolution"
```

---

## Task 12: Snapshots

**Files:** `zeroskip.c` (SNAPSHOT), `zstest.c`

**Requirements:** C-4, C-4a–C-4h, G-4, G-6, A-3, R-1

**Interfaces produced:**

```c
#define ZSI_SNAPSHOT_RETRIES 20

struct zsi_snapshot {
    struct zsi_file **files;   /* resolved set, sorted by start ASCENDING */
    size_t            nfiles;
    int               refcount;
};

/* C-4 in full: scan, resolve, open and map, build indexes.  Retries on a
 * non-tiling set or an ENOENT, up to ZSI_SNAPSHOT_RETRIES, then ZS_AGAIN
 * (C-4h) rather than spinning. */
static int zsi_snapshot_take(struct zs_db *db, struct zsi_snapshot **out);
static void zsi_snapshot_release(struct zsi_snapshot **sp);

/* The active file: the highest-generation unordered file, or NULL if the
 * newest file is in-order. */
static struct zsi_file *zsi_snapshot_active(struct zsi_snapshot *s);
```

- [ ] **Step 1: Implement**

C-4's four steps exactly. Retry from step 1 on `ZS_AGAIN` from resolve, or on
any `open` failing with `ENOENT`. C-4b is the argument that a retry suffices:
both failures show up as a failure, never as a set that looks complete but is
not, because removal is only permitted when the remaining files still tile
(D-23).

Step 4 builds a private index per unordered file, **taking its snapshot boundary
to be the end of its last valid span** — `f->complete` from Task 7. C-4c is why
this is safe: the active file is appended to, but only appended to, so every
byte below the boundary is immutable and growth above it is invisible.

Bound the retries and return `ZS_AGAIN` (C-4h) so a pathological rate of
structural change surfaces as an error rather than a livelock.

C-4d: every index is private (D-13c), so a snapshot needs **no synchronisation
against a writer** beyond what C-4c already provides. There is no shared cache to
invalidate and no reference table to update — which is also why nothing needs
cleaning up when a process dies.

No lock is taken at any point (C-2). Say so in a comment at the top of the
function, because it is the single most surprising property of the design and
the place someone will later be tempted to add one.

- [ ] **Step 2: Tests**

`test_snapshot_basic`: a database of several files snapshots with the resolved
set in ascending order; `zsi_snapshot_active` finds the unordered file, and
returns NULL when the newest file is in-order.

`test_snapshot_retry_on_gap`: a directory seeded to not tile causes retries and
eventually `ZS_AGAIN`, in bounded time. Assert the retry count is bounded by
running under an alarm.

`test_snapshot_boundary`: an unordered file with trailing garbage after its last
valid span — the snapshot's boundary is `complete`, and records in the garbage
are invisible.

Concurrency cases (a repack completing in the gap, a file unlinked between steps
2 and 3, bytes below the boundary never changing) need real processes and land
in Task 25's T-10b.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: the lock-free snapshot protocol"
```

---

## Task 13: File locking

**Files:** `zeroskip.c` (FILE LOCKING), `zstest.c`

**Requirements:** §6 — C-1, C-1c–C-1i, D-3, D-3a, D-3b, D-3c, G-5, and T-14

**Interfaces produced:**

```c
enum zsi_lock { ZSI_LOCK_WRITE = 0, ZSI_LOCK_REPACK = 1, ZSI_LOCK_REMOVE = 2 };

/* Open (creating with O_CREAT if absent, D-3a) zeroskip.lock and keep the one
 * descriptor for the handle's lifetime.  MUST NOT open a second (C-1g). */
static int zsi_lock_open(struct zs_db *db, const char *dir);

/* Acquire.  Blocking unless ZS_NONBLOCKING, in which case ZS_LOCKED.
 * Takes the in-process mutex as well as the fcntl lock (C-1f). */
static int zsi_lock_take(struct zs_db *db, enum zsi_lock which, int flags);
static int zsi_lock_release(struct zs_db *db, enum zsi_lock which);
```

- [ ] **Step 1: Implement**

`fcntl` exactly as C-1 specifies: `F_SETLK` (non-blocking) or `F_SETLKW`,
`l_type = F_WRLCK`, `l_whence = SEEK_SET`, `l_start = which`, `l_len = 1`.
Release with `F_UNLCK` over the same byte. Retry on `EINTR`.

**Never `flock`** (C-1e). Add a comment: on Linux `flock` occupies a separate
lock space and does not exclude `fcntl`, so an implementation using it would
appear to pass every single-implementation test and silently fail to exclude a
conforming peer. T-13 exists to catch exactly that.

C-1f: `fcntl` locks are per-process, so two threads of one process both acquire
successfully. Hold a `pthread_mutex_t` per lock byte across the same region.
This means linking `-lpthread` where the platform requires it; add it to
`LDLIBS` conditionally in the Makefile.

C-1g: exactly one descriptor for `zeroskip.lock` for the handle's lifetime, and
never a second — closing *any* descriptor for the file drops the process's locks
on it. Assert `db->lockfd` is opened once, in a debug build.

C-1d lock ordering: write → remove, or repack → remove. Nothing takes write or
repack while holding remove, and nothing holds both write and repack. Enforce
with an assertion on a per-handle bitmask of held locks, so a violation fails
loudly during development rather than deadlocking in production.

D-3b: the lock file is never unlinked by the library. Note in a comment that
unlinking it while processes hold locks silently breaks mutual exclusion — the
holders keep locking the removed inode while a new process creates and locks a
fresh one — which is the one way to get two writers. Worth stating because an
empty `*.lock` file is exactly what cleanup scripts delete.

Do not implement `F_OFD_SETLK` (C-1i permits it but only after verifying
platform behaviour; not worth the conditional here).

C-1h: C-1d orders the locks **within one database**. The library cannot see
across two, so a caller holding locks on several while writing must impose its
own consistent order. That is a documentation obligation, not code — record it in
`CLAUDE.md` and in `zeroskip.h` above `zs_db_begin_txn`, since a Cyrus-style
caller with several databases open is exactly who will hit it.

- [ ] **Step 2: Tests**

`test_lock_basic`: take and release each of the three bytes; the three do not
conflict with each other.

`test_lock_nonblocking`: with a second descriptor from a **forked child**
holding byte 0, the parent's `ZS_NONBLOCKING` take returns `ZS_LOCKED` and its
blocking take waits until the child exits.

`test_lock_dies_with_process`: a forked child takes the write lock and is
`SIGKILL`ed; the parent then acquires it with no intervention (G-5) — the kernel
releases `fcntl` locks on process death, so no lock state outlives a process.

`test_lock_threads` (T-14): two threads of **one process** each attempting a
write-lock take, asserting they exclude each other. This fails for an
implementation relying on `fcntl` alone (C-1f) and is invisible to a
single-threaded test, so it must exist.

`test_lock_file_recreated`: opening a database whose lock file is absent
succeeds and recreates it (D-3a).

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: three fcntl byte-range locks plus in-process mutexes"
```

---

## Task 14: Open, create and close

**Files:** `zeroskip.c` (OPEN AND CLOSE), `zstest.c`

**Requirements:** §7 — R-1, R-3, R-5; D-8a, D-8; F-11, A-5, A-6; G-3

**Interfaces produced:** the public `zs_db_open` and `zs_db_close`, plus:

```c
struct zs_db {
    char        *dir;
    uint32_t     flags;
    zsi_uuid_t   uuid;
    zs_compar   *compar;
    char         compar_name[16];
    zs_csum     *external_csum;
    unsigned     create_csum_id;   /* engine for files THIS handle creates (A-6) */
    size_t       rollover_size;
    void       (*error)(const char *msg, const char *fmt, ...);
    int          lockfd;
    unsigned     held;             /* bitmask of zsi_lock */
    pthread_mutex_t mutex[3];
    bool         readonly;         /* ZS_SHARED */
    bool         nocsum;
    bool         nosync;
    struct zsi_snapshot *snap;     /* current snapshot for zs_db_* calls */
    struct zs_txn *write_txn;
};
```

- [ ] **Step 1: Implement open**

Opening **is** recovery; there is no separate pass (§7). The sequence:

1. Resolve setup: comparator (default `zsi_compar_default`, name `memcmp`),
   `rollover_size` (0 → 2MB), error callback, create-time checksum engine from
   the `ZS_CSUM_*` flags.
2. `zsi_lock_open` — creating `zeroskip.lock` with `O_CREAT` if absent (D-3a).
   Concurrent creation is harmless: `O_CREAT` on one path yields one inode.
3. Scan the directory. If it holds no data files: with `ZS_CREATE`, create the
   directory if needed, generate a UUID, and create generation 1 as the active
   file — a 72-byte header and no spans, legal per F-26h — then `fdatasync` the
   **directory** (C-6). Without `ZS_CREATE`, `ZS_NOTFOUND` (D-8a).
4. Take a snapshot (Task 12), which validates and maps everything and builds the
   indexes (R-1).
5. Check agreement across the resolved set: every file MUST carry the same UUID
   and the same comparator name, and the comparator name MUST match the
   caller's (F-11). Disagreement is an error. If any file records engine 2 and
   no `csum` was supplied, error (A-6).
6. D-10a: a **non-active** file with an invalid header is `ZS_BADFORMAT` — its
   records cannot be recovered and skipping the generation would lose committed
   data. Only the active file may be invalid (D-10), and then it is treated as
   a complete file with zero spans.

R-3: `ZS_SHARED` is read-only and MUST NOT write — no conversion, no repack, no
new active file, no removal. Opening a damaged database read-only is
side-effect-free. Enforce with a single `if (db->readonly) return ZS_READONLY;`
at the head of every mutating internal entry point, not scattered checks.

G-3: any state a crash can produce MUST open in bounded time and expose the
committed data.

- [ ] **Step 2: Tests**

`test_open_create`: `ZS_CREATE` on a nonexistent directory creates it, the lock
file and generation 1; the active file is exactly 72 bytes; reopening finds the
same UUID. Without `ZS_CREATE`, `ZS_NOTFOUND`.

`test_open_comparator_mismatch`: a database written with the default comparator
opened with a caller comparator named `custom` → error (F-11).

`test_open_engine_external_missing`: files recording engine 2 opened without
`csum` → error (A-6).

`test_open_readonly_no_side_effects`: snapshot the directory listing, open a
database with an unclean active file under `ZS_SHARED`, close, and assert the
listing is byte-identical — no conversion, no new generation, no removal (R-3).

`test_open_bad_nonactive_header`: an in-order file with a corrupt header →
`ZS_BADFORMAT` (D-10a). The same corruption in the **active** file opens fine
with zero spans (D-10).

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: open is recovery -- create, validate, snapshot"
```

---

## Task 15: The read path

**Files:** `zeroskip.c` (READ PATH), `zstest.c`

**Requirements:** §5.5 — D-14, D-14a–D-14i; G-7; and T-5, T-5a, T-5b

**Interfaces produced:**

```c
/* The merge cursor: an array of per-file cursors kept sorted by
 * current key ascending, then generation descending (D-14e). */
struct zs_cursor {
    struct zs_db     *db;
    struct zs_txn    *txn;            /* owning txn, or an implicit one */
    struct zsi_fcur  *cur;  size_t ncur;
    char             *prefix; size_t prefixlen;
    uint32_t          flags;
    bool              started;
    struct zsi_rec    emitted;        /* last record handed out */
};

/* D-14d point lookup: walk the sources newest to oldest, stop at the first
 * that has the key.  A deletion means the key does not exist. */
static int zsi_lookup(struct zs_db *db, struct zsi_snapshot *snap,
                      struct zs_txn *txn,
                      const char *key, size_t keylen,
                      struct zsi_rec *out);

static int zsi_cursor_open(struct zs_db *db, struct zs_txn *txn,
                           const char *key, size_t keylen,
                           uint32_t flags, struct zs_cursor **out);
static int zsi_cursor_next(struct zs_cursor *cur,
                           const char **keyp, size_t *keylenp,
                           const char **valp, size_t *vallenp);
```

- [ ] **Step 1: Implement point lookup**

Sources in priority order (D-14's table): the write transaction's uncommitted
records first, then each data file by `start` **descending**. In each, search by
D-14b via `zsi_fcur_find`. Stop at the first source that has the key; that
record decides the answer — its value, or absence if it is a deletion.

A source that does not have the key is skipped, and **no source may be skipped
for any other reason** — no bloom filter, no cached key range, nothing. D-14c:
ancestors are not consulted by any read; they exist solely for repacking, so a
lookup never follows a chain.

- [ ] **Step 2: Implement the merge cursor**

D-14e's six steps, literally:

1. **Seek** every per-file cursor to the start point, or mark it exhausted —
   immediately the case for a source holding no records. Then sort the array.
2. **Take** element 0. O(1), no comparison needed.
3. **Skip stale duplicates.** Because equal keys sort by generation descending,
   every cursor on the emitted key is contiguous from the front and element 0 is
   the newest. Advance elements 1, 2, … while their current key equals the
   emitted one. D-14f: advancing only element 0 would leave the same key at
   another cursor's head, to be emitted again from an *older* version — the
   exact bug T-5b constructs.
4. **Filter.** If element 0's record is a deletion, emit nothing; the key is
   consumed either way.
5. **Advance and re-sort.** Advance element 0, then move it down to its new
   position — a single insertion into an already-sorted array, stopping as soon
   as it is no longer greater than its neighbour, since its key only increases.
6. **Bound.** For a prefix scan, stop when the emitted key leaves the prefix.

Sort comparator: key ascending via `db->compar`, then `gen` **descending**.
Exhausted cursors sort last unconditionally. D-14g: the transaction's records
carry `gen = UINT32_MAX`, giving them highest priority for equal keys with no
special case.

A sorted array rather than a heap, per D-14i: *k* is small by design and the
advanced cursor usually stays at or near the front, so the insertion terminates
immediately.

The sources are fixed for the cursor's lifetime, so no file can appear or vanish
mid-scan — the snapshot is refcounted for exactly this.

- [ ] **Step 3: Implement `ZS_SKIPROOT`, `ZS_CURSOR_PREFIX`, `ZS_FETCHNEXT`**

`ZS_SKIPROOT`: skip the first record if it matches the start key exactly.
`ZS_CURSOR_PREFIX`: step 6's bound. `ZS_FETCHNEXT` on a fetch: return the record
*after* the given key — a cursor seeked to the key with `ZS_SKIPROOT`.

- [ ] **Step 4: Tests — T-5b, T-5a, T-5**

`test_cursor_invariant` (T-5b): after **every** step, assert the array is
ordered by key ascending then generation descending; that cursors on equal keys
are contiguous from the front with the newest first; and that element 0 is the
correct next record. Expose a debug hook for the test to inspect the array.

`test_cursor_d14f` (T-5b): the same key present in **three** files at once —
asserting it is emitted once from the newest, **and** that a variant build which
advances only element 0 would emit it three times. Construct the failure
directly, so the rule cannot be removed as an optimisation without a test
failing.

`test_cursor_exhaustion` (T-5b): a source exhausted at seek; one exhausted
mid-scan; every source exhausted; a cursor seeked past every key; and a start
key that exactly matches a record in some sources but not others.

`test_read_arrangements` (T-5a): the same assertions driven against a database
deliberately arranged as — one unordered file only; several unordered files; one
in-order file only; a mixture; after a repack that collapsed some but not all
files; **and with an empty in-order file among populated ones**. The answers
MUST be identical throughout. Then re-run with `ZSI_PROBE_ENDS` compiled out and
compare, including for keys below the first and above the last.

`test_model` (T-5): randomised operation sequences against an in-memory
reference model (a sorted array of key/value pairs), checked after every step by
**both** a point lookup and a full scan, cross-checked against each other —
the direct test for G-7 and D-14f. The generator MUST produce: the same key live
in several files at once; a key deleted in a newer file and present in older
ones; a key whose only version is in the oldest file; and keys adjacent in
comparator order but split across files. Repeat with the file set arranged so
sources hold overlapping key ranges — a merge that mishandles ties only fails
when ties occur. Seed the RNG from an env var (`ZS_TEST_SEED`) defaulting to a
fixed value, so failures reproduce.

- [ ] **Step 5: Verify and commit**

```bash
make check && make asan
git commit -am "feat: point lookup and the sorted-array merge cursor"
```

---

## Task 16: The write path

**Files:** `zeroskip.c` (WRITE PATH), `zstest.c`

**Requirements:** §5.3 — D-9, D-9a, D-9b, D-9c, D-11, D-13b; §6 — C-7, C-7a,
C-7b, C-7c, C-8, C-6; G-1, G-2; A-1, A-1a, A-1b

**Interfaces produced:**

```c
/* A transaction's own uncommitted records: a sorted array of owned copies. */
struct zsi_pending {
    char    *key;  size_t keylen;
    char    *val;  size_t vallen;    /* val == NULL for a deletion (A-1) */
};

struct zs_txn {
    struct zs_db        *db;
    struct zsi_snapshot *snap;       /* the txn's fixed snapshot */
    bool                 readonly;
    struct zsi_pending  *pend;  size_t npend, apend;   /* sorted by key */
    char                *retbuf;     /* backing for A-4 returned pointers */
    size_t               retlen;
};
```

- [ ] **Step 1: Transaction lifecycle**

`zs_db_begin_txn(db, shared, txnp)`: a read transaction takes a snapshot and no
lock (C-2, G-4). A write transaction takes the **write lock** (byte 0) plus the
in-process mutex, then a snapshot. `ZS_NONBLOCKING` → `ZS_LOCKED` rather than
waiting.

Writes accumulate in `pend`, a sorted array of owned copies, spliced on insert
and replacing an existing entry for the same key. A write inside a transaction
is visible to subsequent reads on that transaction and to nothing else until
commit (A-1a) — which is D-14's priority table row 1, and is what fills in Task
10's `ZSI_SRC_TXN` cursor arm. **Implement that arm now.**

`store` with `val == NULL` writes a deletion; with a non-NULL zero-length value
it stores an empty value. Distinct states (A-1, F-14). `ZS_IFNOTEXIST` →
`ZS_EXISTS` if present; `ZS_IFEXIST` → `ZS_NOTFOUND` if absent, evaluated
against the transaction's own view.

- [ ] **Step 2: Choosing the active file (D-9, D-9a)**

While holding the write lock, either append to a **clean** active file, or
create a new unordered file whose generation is exactly one higher (D-9b),
write a valid header, and append there.

Move to a new file when the active file is not clean (`!zsi_unordered_is_clean`),
or when it exceeds `rollover_size`. Rollover is cheap: a new header and nothing
else, since the writer never appends a pointer section to an unordered file
(D-11) — the previous file simply stays unordered until it is converted.

D-10: an active file with a corrupt header or zero length is a complete file
with zero spans, not an error; because it is not clean the writer moves on, so
no chain is ever built on an untrustworthy boundary (R-4).

After **creating** a data file, `fdatasync` the **directory** (C-6), otherwise
the name may be absent after a crash even though the contents are durable.

D-9c: allocating past `0xFFFFFFFF` fails with `ZS_FULL` rather than wrapping.

- [ ] **Step 3: Commit — the two gates (C-7)**

Under default durability a commit is:

1. append the span's data records, then **`fdatasync`**;
2. append the terminator, then **`fdatasync`** again.

`fdatasync` rather than `fsync` because appending changes only the metadata
needed to read the data back.

C-7a failure handling, and this is the part that is easy to get wrong:

| Failure | State | Required behaviour |
|---|---|---|
| gate 1 fails | no terminator was written | report the error; the transaction did not happen |
| gate 2 fails | terminator may or may not be durable | either outcome is correct, so report the error and do nothing else |

**MUST NOT retry a failed sync and treat success as evidence the data
survived** — a second call can succeed after the dirty pages were discarded.
Write that as a comment above the sync call, because retrying a failed syscall
is the reflex.

`ZS_NOSYNC` omits **both** gates (C-7c). Atomicity survives because a torn tail
is still detectable (F-22); durability does not, and neither does C-7a's
ordering guarantee.

On success, fold the committed records into the active file's index
incrementally (D-13b) via `zsi_index_insert` — the writer already knows every
record it appended and never rescans a file it is writing.

- [ ] **Step 4: Abort (C-8)**

An aborted transaction appends a `ROLLBACK` and syncs **neither** gate. If a
crash loses it, the active file is simply no longer clean, so the next writer
moves to a new file (D-9) and reaches the same state. Nothing is promised to a
caller, so there is nothing to make durable.

F-21: without a `ROLLBACK`, a later commit's span would enclose the aborted
records and make them live. That is why the record is written at all.

Discard `pend` and drop the delta entries on abort.

- [ ] **Step 5: Ancestors on write (F-17)**

For each record, decide `store_ancestor` by F-17's one rule: **omit exactly when
the ancestor equals the containing file's `start`**. Determine the ancestor by
looking the key up in the sources *below* the current active file: if the newest
existing record for the key lives in a file with `start == active->hdr.start`
(i.e. this same file), or the key is new, the ancestor is the active file's
start and is omitted. Otherwise store that file's `start` (F-16a — the `start`
of the range, not its `end`; since `start <= end`, D-19's containment test then
errs toward retaining a tombstone).

F-17a: "this is a later occurrence in the file" and "the ancestor equals this
file's start" coincide, so decoding never needs to establish whether a record is
the first occurrence of its key.

- [ ] **Step 6: Tests — T-4**

`test_write_basic`: store, fetch, delete, fetch-absent. Empty value versus
absent key, asserted distinct (A-1).

`test_write_txn_isolation`: read-your-own-writes inside a transaction, and
invisibility outside it until commit (A-1a). A second handle sees nothing until
commit.

`test_write_abort`: an aborted transaction leaves no visible records; the
`ROLLBACK` is present in the file; a subsequent commit's records are live and
the aborted ones are not (F-21).

`test_write_rollover`: writing past `rollover_size` creates generation 2; the
previous file stays unordered with no pointer section (D-11); both files' data
reads back.

`test_write_unclean_rollover`: append garbage to the active file behind the
library's back, reopen, write — the writer moves to a new generation rather than
appending (D-9, R-4).

`test_write_ancestors`: every row of F-17's table round-trips. In particular,
repeated writes to one key in a file store the ancestor **at most once**, and a
record with no stored ancestor decodes to its file's `start`.

`test_write_encoding_boundaries` (T-4): 255↔256-byte keys, 65535↔65536-byte
values, a 16MB span forcing a long terminator, a record landing exactly on
8-byte alignment, keys containing embedded NULs.

`test_write_full`: a database whose highest generation is `0xFFFFFFFF` fails a
rollover with `ZS_FULL` (D-9c). Seed by name.

- [ ] **Step 7: Verify and commit**

```bash
make check && make asan
git commit -am "feat: write path with two durability gates per commit"
```

---

## Task 17: The public API surface

**Files:** `zeroskip.c` (PUBLIC API), `zstest.c`

**Requirements:** §8 — A-0, A-1b, A-2, A-3, A-4, A-5, A-7; T-4

- [ ] **Step 1: Implement the three forms**

Every read and write entry point exists in three forms — on the database, on a
transaction, and via a cursor — and all three take `flags` (A-0). The `zs_db_*`
forms are convenience wrappers that open an implicit single-operation
transaction, so there is no operation reachable one way but not another.
Implement them literally as wrappers, so the wrapped path is the only path.

The `*_delete` forms are **macros** over `store` and `cursor_replace` (A-1b),
already in `zeroskip.h` from Task 1 — there is exactly one write path.

`zs_cursor_replace` writes at the cursor's current key. `zs_cursor_commit` /
`zs_cursor_abort` commit or abort the owning implicit transaction;
`zs_cursor_fini` releases a cursor inside a caller-owned transaction without
touching it.

A-2: there is no `yield` call and no yield flags — readers hold no lock, so
there is nothing to yield. A-3: no MVCC flag; snapshot isolation is the only
read mode. Do not add either even though twom has them; note why in a comment,
because porting from twom will tempt it.

A-4 pointer lifetimes: returned key and value pointers remain valid for the
lifetime of the transaction or cursor that produced them; for `zs_db_*` calls,
until the next call on that `struct zs_db`. Records read from a mapped file are
already stable for the snapshot's life, so return pointers directly into the
map. Records from a transaction's `pend` live in the transaction. Records
returned by a `zs_db_*` wrapper whose implicit transaction has ended must be
copied into `db`-owned scratch — that is the only case needing a buffer.

`zs_db_sync` syncs the active file. `zs_strerror` covers every code.

- [ ] **Step 2: Tests — T-4**

`test_api_three_forms`: every flag exercised through all three entry points and
asserted to behave **identically** (A-0) — `ZS_IFEXIST`, `ZS_IFNOTEXIST`,
`ZS_FETCHNEXT`, `ZS_SKIPROOT`, `ZS_CURSOR_PREFIX`. Drive this as a table of
(operation, flags, expected) run through a `db` runner, a `txn` runner and a
`cursor` runner, comparing all three.

`test_api_pointer_lifetime`: a pointer returned by `zs_txn_fetch` stays valid
until the transaction ends; a pointer from `zs_db_fetch` stays valid until the
next call on that handle (A-4). Run under ASan so a violation is caught rather
than merely observed to work.

`test_api_delete_macros`: `zs_db_delete` and `zs_txn_delete` behave as a store
of `NULL`; `ZS_IFEXIST` composes with them to mean "delete only if present"
(A-1b).

`test_api_readonly`: every mutating call on a `ZS_SHARED` handle returns
`ZS_READONLY` (A-5).

`test_api_custom_comparator`: a caller comparator implementing a different total
order, with a name, round-trips — the name is stored in every header and
reopening with the default comparator fails (F-11, A-7).

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: public API in all three entry-point forms"
```

---

## Task 18: Conversion

**Files:** `zeroskip.c` (CONVERSION), `zstest.c`

**Requirements:** D-12, D-12a–D-12d, D-20a, D-21, D-23, C-1b, C-1c, C-3, C-6,
C-6a, F-24a; T-10a

**Interfaces produced:**

```c
/* Convert one non-active unordered file to its single-generation in-order
 * form: <uuid>-N becomes <uuid>-N-N. */
static int zsi_convert_one(struct zs_db *db, struct zsi_file *f);

/* Convert every non-active unordered file, oldest first (D-12b). */
static int zsi_convert_pending(struct zs_db *db, struct zsi_snapshot *snap);

/* Create a staging file with O_CREAT|O_EXCL, advancing <n> until it
 * succeeds (D-20a). */
static int zsi_staging_open(struct zs_db *db, char *name_out);

/* Remove a data file: MUST hold the remove lock, and MUST verify a complete
 * set exists without it (D-6) under one unbroken hold (D-23). */
static int zsi_remove_file(struct zs_db *db, const char *name);
```

- [ ] **Step 1: Implement**

A writer that finds a **non-active unordered file** MUST convert it before it
finishes, oldest first, and MUST NOT go further — it does not merge in-order
files, which is the repacker's job (D-16).

Conversion: replay the source's committed spans, collect the newest record per
key, sort by key, write header + records in key order + pointer section +
trailer to a staging file, `fdatasync` the file, `rename` to
`zeroskip-<uuid>-N-N`, then `fdatasync` the **directory** (C-6). Only then
retire the input under the remove lock (D-23).

Ancestors are copied through verbatim — but note the containing file's `start`
is unchanged by a single-generation conversion (`N` → `N-N`), so a record whose
ancestor was omitted stays omitted and one that stored an ancestor keeps it.
F-17's rule is satisfied without recomputation.

The output has **no spans and no terminators**: every record in it is live by
construction and it is renamed only once finished (D-21, F-24a), so a commit
record would assert nothing not already guaranteed.

D-12c: conversion never takes the repack lock. It renames its output in without
any lock (C-1b — a repack's output `[a..b]` and a conversion's output `[c..c]`
with *c* > *b* are disjoint), and takes the remove lock only momentarily. **A
writer never waits on a repack.**

D-12b: oldest first, so the generation range stays a prefix of in-order files
followed by a suffix of unordered ones. Converting out of order would strand an
unordered file between in-order ones and block the repacker's cascade (D-16c).

D-12d: each conversion is bounded by `rollover_size`, so a writer's extra cost
is bounded and predictable rather than proportional to the database.

`zsi_remove_file` (D-23): take the remove lock, re-scan, verify the set still
tiles **without** the candidate, then unlink — all under one unbroken hold, so
the set cannot change in between. If verification fails, leave the file alone:
leaking a file costs disk space, removing a needed one costs the database. No
directory sync after `unlink` (C-6a).

- [ ] **Step 2: Tests — T-10a**

`test_convert_basic`: a rollover leaves generation 1 unordered; the next writer
converts it to `1-1`; the data reads back identically; the input is removed.

`test_convert_steady_state` (T-10a): drive many rollovers through a writer,
asserting after each that **exactly one** unordered file remains and the rest
are in-order (D-12a).

`test_convert_backlog` (T-10a): simulate several crashes each leaving an unclean
active file (append garbage, reopen, write, repeat), then assert successive
writers convert them **oldest first** and the count drains to one.

`test_convert_staging_exclusive`: a staging name already taken — assert `O_EXCL`
advances `<n>` rather than overwriting (D-20a).

`test_remove_refuses_when_incomplete` (D-23): attempt to remove a file the set
still needs, asserting refusal and that the file survives.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "feat: immediate conversion of non-active unordered files"
```

---

## Task 19: Repacking

**Files:** `zeroskip.c` (REPACK), `zstest.c`

**Requirements:** §5.6 — D-15 through D-24; F-16c, F-17; C-1a; T-7

**Interfaces produced:** the public `zs_db_repack` and `zs_db_should_repack`, plus:

```c
/* D-16 input selection over the in-order files of a snapshot.  Returns the
 * count selected, always a suffix of the in-order prefix (newest first). */
static size_t zsi_repack_select(struct zsi_snapshot *snap,
                                struct zsi_file ***sel);
```

- [ ] **Step 1: Implement selection (D-16)**

Works **only on in-order files**; converting unordered files is the writer's job
(D-12). Never touches the active file, and never touches an unordered file at
all (D-15).

1. Start from the newest in-order file.
2. If the result would be larger than the next lowest in-order file, include
   that file too and repeat.
3. Stop when every file is included or the next lowest in-order file is larger.

This yields geometrically sized in-order files and amortised O(log n) rewrites
per record. D-16b: one output for the whole selected set, not one per pair.

`zs_db_should_repack` reports whether D-16 currently has work (D-24).

C-1a and D-16a: the write and repack locks never contend because the two jobs
consume disjoint sets of files, and **the two jobs divide by whether a file has
an `end`** — a writer only converts files with `end == 0`, the repacker only
merges files with `end != 0`. That division is what makes them independent: a
writer's conversion is bounded by `rollover_size` and runs inline (D-12d), while
the repacker's cascade is unbounded and runs out of band. A file becomes visible
to the repacker precisely when the writer has finished with it.

- [ ] **Step 2: Implement the merge (D-17, D-17b)**

The output holds **exactly one record per key**, built from the live records of
all inputs, skipping rolled-back spans.

D-17b's **total order**, oldest to newest, and the merge must consider versions
in exactly this order:

1. across files, by increasing `start` generation — the tiling invariant (D-6)
   means ranges never overlap, so this is total;
2. within one unordered file, by increasing offset among committed spans;
3. an in-order file holds one record per key, so there is nothing to order.

V1 is the first version in that order, V3 the last. The emitted record takes
**V3's value** and **V1's ancestor** — from those records specifically, and by
no other route. Getting this wrong silently emits the wrong value or the wrong
ancestor, which is why T-7 tests the ordering directly.

D-17a / F-16c: ancestors are copied **verbatim**; nothing is renumbered and no
ancestor is recalculated. The generation named may since have been absorbed into
a file covering a range — harmless, because D-19 compares the absolute ancestor
against a generation range.

D-18's table, per key:

| V1's ancestor | V3's value | emit |
|---|---|---|
| `>= output start` | a deletion | **nothing** — drop the key |
| `>= output start` | a value | the **ancestor-omitting** form |
| `< output start` | either | the **ancestor-storing** form, ancestor = V1's |

D-19: a key is removed entirely **iff** its latest version is a deletion **and**
V1's ancestor lies inside the output range — its whole lifespan from create
through update to delete is contained. Otherwise the tombstone MUST be retained,
because an older file may still hold the key and dropping it would resurrect the
value.

D-19a: the emitted record MUST be written **even when a newer file already
shadows the key**. Being shadowed does not permit dropping a record; only D-19
does. The retained record carries the chain's reach, which no other file
records. Comment this at the emit site — it looks like a missed optimisation and
is not.

D-20: inputs are iterated in key order — from the pointer section where present,
otherwise from the same private index any reader builds. Nothing repack-specific.

- [ ] **Step 3: Implement output and publication**

Write to `zeroskip.tmp.<pid>.<n>` (D-20a's `O_CREAT|O_EXCL`) and `rename` to
`zeroskip-<uuid>-<start>-<end>` covering the entire range of **every** input,
only once complete (D-21, C-3). `fdatasync` the file, then the directory (C-6).
Then retire the inputs under the remove lock (D-23).

D-22: the output may legitimately contain **zero records**, in F-26g's form. It
MUST still be written, so the generation range stays tiled (D-6). Cheap and
short-lived — an empty file violates D-16's size relation maximally, so the next
repack absorbs it.

Hold the **repack lock** (byte 1) for the whole operation.

- [ ] **Step 4: Tests — T-7**

`test_repack_selection`: the cascade reaching D-16's geometric size relation
after many rollovers; several unordered files collapsing into only in-order
files as inputs; `zs_db_should_repack` agreeing with whether `zsi_repack_select`
returns work.

`test_repack_one_record_per_key`: for every arrangement of create, update and
delete spread across generations, exactly one record per key is emitted and
chains stay unbroken (F-16).

`test_repack_version_order` (D-17b, tested **directly**): several versions of
one key at increasing offsets within an unordered file; several across files
with different `start` generations; and both at once — asserting the emitted
value comes from **V3** and the emitted ancestor from **V1** under that total
order, not from whichever record the merge's internal iteration happened to
touch first or last.

`test_repack_d18_table`: each row of D-18 constructed and asserted, including
which encoding form is chosen.

`test_repack_d19_drop`: a key dropped only when its whole lifespan is inside the
output; a tombstone retained when V1's ancestor is below the output start.

`test_repack_resurrection` (D-19a): construct the resurrection **directly** —
assert both that the key stays absent, **and** that dropping the retained record
*does* produce the bug (via a variant build or an injected flag). The test must
fail if the rule is ever removed as an optimisation.

`test_repack_empty_output` (D-22): generation *X* creates one record and *X+1*
deletes it; repacking the two together writes an empty in-order file, the range
stays tiled, and the database reads as empty.

`test_repack_ancestors_verbatim`: an ancestor naming a generation since absorbed
into a wider range is copied through unchanged and D-19 still evaluates
correctly against the range (F-16c).

`test_repack_distant_ancestor` (F-16b): **there is no guarantee the ancestor is
numerically close.** Write a key in generation 1, leave it untouched through
twenty generations of unrelated writes, then update it — asserting the stored
ancestor is 1 (or whatever range absorbed it) and not something near 20. Then
repack and assert D-19 does **not** drop the key, since its chain reaches outside
the output. An implementation that assumed adjacency, or that used a narrow
relative encoding, passes every other test in this task and fails this one.

- [ ] **Step 5: Verify and commit**

```bash
make check && make asan
git commit -am "feat: size-tiered repacking with ancestor-aware tombstone retention"
```

---

## Task 20: Consistency checking and dump

**Files:** `zeroskip.c` (CONSISTENCY), `zstest.c`

**Requirements:** F-28, F-26f, F-26e, F-15 (non-canonical detection), T-6 negatives

- [ ] **Step 1: Implement `zs_db_check_consistency`**

Over every file in the snapshot:

- in-order: verify the pointer array is **strictly increasing by key** (F-28),
  which both confirms the sort and catches a repack that emitted a key twice
  (D-17); verify the records-region checksum (F-26e, F-26f).
- unordered: replay every span and verify each terminator's checksum.
- both: report a record storing an ancestor **equal to its own file's `start`**,
  which F-15 forbids as non-canonical (T-6's negative case).
- the file set: the resolved ranges tile (D-6).

- [ ] **Step 2: Implement `zs_db_dump`**

Print structure — files, generations, spans, record types, offsets — at the
detail level given. This is T-0a's `dump` subcommand's engine, so its output is
a **defined line format**, not free prose. Fix the format here:

```
FILE <name> kind=<unordered|inorder> start=<N> end=<N> csum=<id> size=<N>
SPAN <off> len=<N> term=<COMMIT|ROLLBACK> records=<N>
REC  <off> type=<0xNN> keylen=<N> vallen=<N|->  anc=<N> key=<hex>
PTRS <off> width=<32|64> count=<N>
```

- [ ] **Step 3: Tests — T-6 negatives**

`test_check_out_of_order_pointers`: a hand-built in-order file with two pointers
out of key order is reported (F-28); one with a duplicated key is reported.

`test_check_records_checksum`: a record body corrupted in place is reported
(F-26e).

`test_check_noncanonical_ancestor`: a hand-built file storing an ancestor equal
to its own `start` is reported (F-15, T-6).

`test_check_clean_database`: a normally produced database of every arrangement
passes.

- [ ] **Step 4: Verify and commit**

```bash
make check && make asan
git commit -am "feat: consistency checking and structural dump"
```

---

## Task 21: zstool — the driver contract

**Files:** Create `zstool.c`; modify `Makefile`

**Requirements:** T-0a in full

- [ ] **Step 1: Implement the subcommands**

Every one operates on a database directory given as the first argument. Output
is the defined line format, so the T-12/T-13 runner compares text, not
internals.

| Subcommand | Behaviour |
|---|---|
| `create --uuid U` | create a database with a given UUID, so output is reproducible |
| `store K V` / `delete K` | one transaction, one operation |
| `batch < script` | a sequence of operations in one transaction, so multi-record spans are testable |
| `get K` | print the value, or a defined not-found marker |
| `scan [--prefix P]` | print every visible pair in comparator order |
| `dump` | print structure — files, generations, spans, record types, offsets |
| `check` | run the consistency checks (F-28, F-26f) and report |
| `convert` / `repack` | force one conversion or one repack |
| `hold-write --for MS` | take the write lock and hold it, for lock-contention tests |

`--uuid` is the reason `zstool` exists rather than a test hook in the library
(T-1): corpus generation needs a fixed UUID, and the library's public API has no
business accepting one. Add one internal entry point, used only by this path:

```c
/* In zeroskip.c, declared in zeroskip.h under a clearly marked
 * "not part of the stable API" comment so zstool can call it.  Behaves exactly
 * as zs_db_open with ZS_CREATE, except that a database being created takes the
 * given UUID instead of a generated one.  Opening an existing database ignores
 * it. */
int zs_db_open_with_uuid(const char *dir, struct zs_open_data *setup,
                         const char *uuid_str, struct zs_db **dbp);
```

Prefer this over a second `zsi_`-prefixed static, because `zstool.c` is a
separate translation unit and cannot reach statics in `zeroskip.c`.

Define the formats precisely, since two implementations must match:
- `get` on a present key prints `<value-hex>\n`; absent prints `NOTFOUND\n`.
- `scan` prints `<key-hex> <value-hex>\n` per pair, in comparator order.
- Hex, not raw, so keys and values containing NULs and newlines survive the
  comparison — T-12 requires byte-for-byte `scan` agreement and raw output could
  not represent them.
- `batch` reads lines `store <key-hex> <value-hex>` or `delete <key-hex>`.

- [ ] **Step 2: Tests**

`test_zstool_roundtrip` in `zstest.c` is awkward (it would shell out); instead
add a `tests/tool.sh` driven by `make check` that exercises each subcommand and
compares against expected output, and note in `CLAUDE.md` that the tool's line
format is a compatibility surface.

- [ ] **Step 3: Verify and commit**

```bash
make && ./zstool create /tmp/zsx --uuid 4941da54-9406-4faa-a457-c4b65beae3eb
make check
git commit -am "feat: zstool implementing the T-0a driver contract"
```

---

## Task 22: The golden corpus

**Files:** Create `tests/corpus/`, `tests/corpus/README.md`; modify `zstest.c`, `Makefile`

**Requirements:** T-0, T-1, F-5d

- [ ] **Step 1: Define the portable description format**

**T-0: the corpus is language-neutral.** `tests/corpus/` holds data files
alongside a description of each in a portable text format, **not in any
implementation's source**. Every implementation validates against the same
bytes, and any implementation may generate the corpus — a corpus only one can
produce proves nothing.

Each case is a directory holding the database's files plus a `case.txt`:

```
uuid 4941da54-9406-4faa-a457-c4b65beae3eb
engine 1
comparator memcmp
op store 6b6579 76616c7565
op delete 6b657932
expect scan
6b6579 76616c7565
expect files
zeroskip-4941da54-9406-4faa-a457-c4b65beae3eb-00000001
```

Document the grammar in `tests/corpus/README.md` — it is part of the interop
surface, so it must be readable by someone implementing zeroskip in another
language with no access to this C code.

- [ ] **Step 2: Generate the corpus**

A `make corpus` target driving `zstool` with explicit UUIDs. Cases covering the
structural states T-12 lists: records still in the active file; a converted
single-generation in-order file; a merged multi-generation file; a rolled-back
span; a tombstone whose chain leaves the file; an empty in-order file; keys
spanning the short/big encoding boundary; keys containing NUL bytes. Each for
**engines 0 and 1** (F-5d excludes engine 2, since those files are readable only
by a caller supplying the same function).

Check the generated bytes into git. They are the contract.

- [ ] **Step 3: Tests — T-1**

`test_golden_decode`: for every corpus case, open the checked-in database and
assert `scan` matches `case.txt`'s expectation.

`test_golden_encode`: for every case, replay the `op` lines into a fresh
directory with the recorded UUID and assert the produced files are **byte
identical** to the checked-in ones. This is what makes F-15's canonical encoding
a tested property rather than an aspiration, and it is the single-implementation
half of T-12a.

Pin that a file's engine comes from its **own header** rather than the reader's
configuration: open an engine-0 corpus case from a handle configured with
`ZS_CSUM_XXHASH` and assert it reads correctly (F-5a, A-6).

- [ ] **Step 4: Verify and commit**

```bash
make corpus && make check
git add tests/corpus && git commit -m "test: language-neutral golden corpus for engines 0 and 1"
```

---

## Task 23: Malformed input

**Files:** Modify `zstest.c`; modify `Makefile`

**Requirements:** T-3, G-3, F-29, F-30, G-0b

- [ ] **Step 1: Implement the generative harness**

For **every** golden corpus file:

- truncate at **every byte offset**, not merely record boundaries;
- systematically bit-flip — every bit of every byte for small files, and a
  deterministic sample for large ones, seeded so failures reproduce.

Each case asserts: an error **or** the committed prefix, and never a crash, a
hang, or an out-of-bounds read. Note that "the committed prefix" is a legitimate
outcome for a truncated unordered file — F-24 makes it complete at its last
valid span — so the harness must accept both, and must assert the prefix is a
*prefix*, not merely non-empty.

Per-case wall-clock timeout via `alarm()` — **the timeout is the detector for
F-29**, so it must be present rather than implied.

- [ ] **Step 2: Wire the sanitisers**

T-3 requires ASan and UBSan. Ensure `make asan` builds the whole suite with
`-fsanitize=address,undefined` and that this test runs there. Add
`ZS_TEST_FUZZ_FULL=1` to switch from the sampled bit-flips to exhaustive, for CI
or an overnight run, and `log()` the sampling rate otherwise so a reduced run
never reads as full coverage.

- [ ] **Step 3: Verify and commit**

```bash
make asan && ZS_TEST_FUZZ_FULL=1 ./zstest malformed
git commit -am "test: truncation and bit-flip harness over the golden corpus"
```

---

## Task 24: Crash and sync-failure injection

**Files:** Create `zstest-crash.c` or extend `zstest.c`; modify `Makefile`

**Requirements:** T-8, T-8a, C-6, C-7, C-7a, G-2, G-3, R-4, D-9

- [ ] **Step 1: Implement the interposer**

A **test build** interposes `write`, `fdatasync`, `rename` and `unlink`, counts
calls, and aborts at call *N*. Implement with weak symbols and a
`-DZS_TEST_HOOKS` build of `zeroskip.c` that routes those four syscalls through
function pointers — cleaner and more portable than `LD_PRELOAD`, and it works on
macOS where `LD_PRELOAD` does not.

Drive a scripted workload for **every** *N* from 1 to the total call count.

- [ ] **Step 2: The assertions**

Each case asserts: reopen terminates within a timeout; **exactly a prefix of
committed transactions is visible**; nothing acknowledged is lost under default
durability; and a writer can then continue.

Both durability gates are crash points in their own right (C-7), **including the
window between them** — the state a single-gate design could not distinguish.

Targeted cases T-8 names explicitly: crash between the records and the first
gate; between the two gates; after the terminator but before the second gate;
mid-publish `rename`; mid-repack; after the pointer section but before the
trailer; leaving a non-8-aligned file length; and after an invalid terminator,
asserting the writer moves to a new generation rather than appending (R-4, D-9).

Both durability modes (default and `ZS_NOSYNC`).

Separately, with **directory syncs suppressed**, assert that a crash can lose a
*name* — the test that justifies C-6. This one asserts a failure occurs, so it
must be written carefully enough not to pass vacuously.

- [ ] **Step 3: T-8a sync failure**

The case C-7a exists for, which no crash test reaches: `fdatasync` made to
**fail**.

- gate 1 failing: assert **no terminator was written** and the error reached the
  caller, so the transaction plainly did not happen.
- gate 2 failing: assert the database is correct whichever way the terminator
  landed — either the commit is visible with durable data, or the span reads as
  absent.
- and in particular that the implementation does **not** retry the sync and
  treat success as proof the data survived. Assert by counting `fdatasync` calls
  after the injected failure and requiring zero.

- [ ] **Step 4: Verify and commit**

```bash
make crashtest && ./zstest-crash
git commit -am "test: crash injection at every syscall, and sync-failure handling"
```

---

## Task 25: Multi-process

**Files:** Modify `zstest.c`

**Requirements:** T-10, T-10b, C-4, C-4b, C-4c, C-4f, C-4g, D-23, G-5, G-4

- [ ] **Step 1: T-10 — real forked processes**

- a writer plus *N* readers, asserting snapshot stability across commits and
  that a fresh open sees them;
- two writers, exactly one proceeding; `ZS_LOCKED` under `ZS_NONBLOCKING`;
- **a writer `SIGKILL`ed holding the lock, asserting the next writer proceeds
  with no manual intervention** (G-5);
- a reader holding a snapshot across a repack, asserting its data stays readable
  while inputs are unlinked (C-4g — the kernel keeps each inode alive until the
  last descriptor *and mapping* is gone). Assert C-5's accepted cost in the same
  case: with the reader still holding the snapshot, the unlinked inodes' space is
  **still held**, and is released only once that reader exits. Measure with
  `statvfs` or the inode's link count and open count; the point is to record the
  cost as intended behaviour so a future reader does not file it as a leak;
- two processes racing to remove debris, asserting the surviving set still tiles
  and no needed file is removed;
- a directory seeded with the debris of two half-finished repacks over
  **overlapping** ranges, where naive independent cleanup would remove both and
  lose a generation;
- removal attempted **without** the remove lock, asserting refusal (D-23);
- concurrent repack and writer both proceeding, with publish serialised — and a
  writer whose conversion finds the repack lock held, asserting it **skips
  rather than waits** and that the next writer performs it (D-12c).

- [ ] **Step 2: T-10b — the snapshot protocol attacked directly**

These fail only under concurrency, so each step of C-4 gets its own case:

- a reader interrupted between scanning the directory and opening files, with a
  repack completing in the gap, asserting the retry converges on a tiling set
  (C-4a, C-4b). Use a test hook that pauses between C-4 steps 2 and 3;
- a file unlinked between steps 2 and 3, asserting `ENOENT` triggers a retry
  rather than a partial snapshot;
- a reader holding a snapshot while the writer commits repeatedly, asserting
  bytes below its boundary **never change** and growth above it is invisible
  (C-4c) — checksum the mapped prefix before and after;
- a writer killed mid-span while a reader scans, asserting the reader stops at
  the last valid terminator (C-4f). **This is the case that shows the terminator
  checksum, not a lock, is what makes reading a live file safe** — so it is the
  most valuable test in this task.

- [ ] **Step 2b: Note on flakiness**

Timing-dependent tests that pass by luck are worse than no tests. Where a case
needs an interleaving, force it with a test hook (a pause point compiled in
under `ZS_TEST_HOOKS`) rather than a `sleep`. Where a hook is impractical, loop
the case enough times to make a miss improbable and `log()` the iteration count.

- [ ] **Step 3: Verify and commit**

```bash
make check && make asan
git commit -am "test: multi-process concurrency and the snapshot protocol"
```

---

## Task 26: Benchmark tool

**Files:** Create `zsbench.c`; modify `Makefile`, `doc/`

**Requirements:** none normative — this exists because twom has one and the
comparison between the two libraries is the point.

- [ ] **Step 1: Port `../twom/twombench.c`**

Keep its shape: `--selftest`, `-n`, `--reps`, and the same workloads, so numbers
are comparable between the two libraries. Add zeroskip-specific measurements:
cost per rollover, cost of a conversion, cost of a repack cascade, and snapshot
open cost as a function of active-file size — the last being the number open
item 2 in the spec asks for before deciding whether a shared index is worth
reintroducing.

- [ ] **Step 2: Document**

`doc/benchmarking.md` describing the workloads and how to read the output.

- [ ] **Step 3: Verify and commit**

```bash
make bench
git commit -am "feat: zsbench, with the measurements open item 2 needs"
```

---

## Task 27: Conformance traceability

**Files:** Create `doc/conformance.md`

**Requirements:** T-11

- [ ] **Step 1: Build the map**

**T-11: `doc/conformance.md` maps every normative requirement in the spec to the
test enforcing it. A requirement with no test is a gap to close.** Each
implementation records which requirements it passes, so partial conformance is
visible.

Extract every labelled requirement from the spec (`grep -o '\*\*[FDCRATG]-[0-9a-z]*' `)
and build a table:

| Req | Statement (abbreviated) | Test | Status |
|---|---|---|---|
| F-1 | Integers are little-endian | `test_interop_constants` | pass |
| … | | | |

- [ ] **Step 2: Close or record the gaps**

Any requirement with no test is either given one now or recorded as an explicit
gap with a reason. Do not leave a blank cell — a blank reads as "covered" to the
next reader.

Also state which requirements this implementation does **not** claim: `C-1i`
(`F_OFD_SETLK`) is not implemented, and engine 2 is outside the corpus (F-5d).

- [ ] **Step 3: Verify and commit**

```bash
git add doc/conformance.md && git commit -m "docs: conformance map from every requirement to its test"
```

---

## Out of scope for this plan

**T-12, T-12a, T-13, T-14 (cross-implementation)** require a second
implementation to exist. This plan delivers the halves that make them possible —
the language-neutral corpus (T-0, Task 22), the driver contract (T-0a, Task 21),
and the byte-for-byte encode assertions that are T-12a's single-implementation
half (Task 22). T-14's threading case is covered for C in Task 13, since it must
be run per implementation rather than assumed.

The shared language-neutral runner for T-12/T-13 belongs in its own plan,
written when there is a second implementation to run it against.

**Spec open items (§11)** are deliberately not addressed: repack duration stays
unbounded (D-16b), and no shared index is built. Task 26 produces the
measurement open item 2 asks for before that decision is reconsidered.
