/* zeroskip.h - append-only ordered key-value store
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * A zeroskip database is a directory of immutable and append-only files, with
 * lock-free readers and a single writer.  Nothing is ever written except by
 * appending to a file or by creating a new one.
 *
 * The on-disk format, the database layout, the concurrency protocol and the
 * recovery rules are specified in
 * doc/specification.md, and are normative for
 * every implementation.  This header is one binding: its *semantics* are
 * normative, its spelling is not.
 */

#ifndef INCLUDED_ZEROSKIP_H
#define INCLUDED_ZEROSKIP_H

#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>

struct zs_db;
struct zs_txn;
struct zs_cursor;

enum zs_ret {
    ZS_OK          = 0,
    ZS_DONE        = 1,
    ZS_EXISTS      = -1,
    ZS_IOERROR     = -2,
    ZS_INTERNAL    = -3,
    ZS_LOCKED      = -4,
    ZS_NOTFOUND    = -5,
    ZS_READONLY    = -6,
    ZS_BADFORMAT   = -7,
    ZS_BADUSAGE    = -8,
    ZS_BADCHECKSUM = -9,
    ZS_FULL        = -10,
    ZS_AGAIN       = -11
};

/* Flags occupy one 32-bit space and are never reused for different meanings in
 * different calls, though not every flag is meaningful everywhere. */
enum zs_flagspec {
    ZS_CREATE        = 1<<0,   /* open:    create the database if absent */
    ZS_SHARED        = 1<<1,   /* open,txn: read-only */
    ZS_NOCSUM        = 1<<2,   /* open:    do not verify checksums on read */
    ZS_NOSYNC        = 1<<3,   /* open:    omit both durability gates on commit */
    ZS_NONBLOCKING   = 1<<4,   /* open,txn: ZS_LOCKED rather than wait for a lock */

    ZS_IFNOTEXIST    = 1<<11,  /* store:   only if absent, else ZS_EXISTS */
    ZS_IFEXIST       = 1<<12,  /* store:   only if present, else ZS_NOTFOUND */
    ZS_FETCHNEXT     = 1<<13,  /* fetch:   return the record AFTER the given key */
    ZS_SKIPROOT      = 1<<14,  /* foreach,cursor: skip an exact match on the start key */
    ZS_CURSOR_PREFIX = 1<<16,  /* foreach,cursor: treat the start key as a prefix
                                  and stop when a key leaves it */

    ZS_CSUM_NONE     = 1<<27,  /* open: write engine 0 into files this handle creates */
    ZS_CSUM_XXHASH   = 1<<28,  /* open: engine 1, the default */
    ZS_CSUM_EXTERNAL = 1<<29   /* open: engine 2; zs_open_data.csum MUST be supplied */
};

typedef int      zs_cb(void *rock, const char *key, size_t keylen,
                       const char *val, size_t vallen);
typedef int      zs_compar(const char *a, size_t alen,
                           const char *b, size_t blen);
typedef uint32_t zs_csum(const char *buf, size_t len);

struct zs_open_data {
    uint32_t     flags;
    zs_compar   *compar;         /* NULL = byte order */
    const char  *compar_name;    /* stored in every file header */
    zs_csum     *csum;           /* required for engine 2 */
    size_t       rollover_size;  /* 0 = default 2MB */
    void       (*error)(const char *msg, const char *fmt, ...);

    /* Pointer table cache (spec section 8).  NULL disables it, which is the
     * default: the library never picks a directory itself (P-2), because a
     * planted table yields wrong records and a world-writable default such as
     * /tmp would make planting one trivial.  MUST NOT name the database
     * directory -- that would let a read-only handle write into the database,
     * which is exactly what R-3 forbids.  Not created by the library; a missing
     * or unwritable directory disables the cache rather than failing the open. */
    const char  *index_dir;        /* A-8 */
    size_t       index_threshold;  /* A-9: 0 = a measured default, 32KB */
};

#define ZS_OPEN_DATA_INITIALIZER { 0, NULL, NULL, NULL, 0, NULL, NULL, 0 }

/* database operations
 *
 * Locks are ordered within one database, but the library cannot see across
 * two: a caller that holds locks on several databases while writing MUST
 * impose its own consistent order.
 */
int  zs_db_open(const char *dir, struct zs_open_data *setup, struct zs_db **dbp);
int  zs_db_close(struct zs_db **dbp);

/* non-transactional operations, each an implicit single-operation transaction */
int  zs_db_fetch(struct zs_db *db, const char *key, size_t keylen,
                 const char **keyp, size_t *keylenp,
                 const char **valp, size_t *vallenp, int flags);
int  zs_db_store(struct zs_db *db, const char *key, size_t keylen,
                 const char *val, size_t vallen, int flags);
/* start/startlen is where iteration BEGINS, not a filter.  Without
 * ZS_CURSOR_PREFIX this walks from that key to the end of the database; with it,
 * the key is also treated as a prefix and the scan stops when a key leaves it.
 * A NULL or zero-length start begins at the first key. */
int  zs_db_foreach(struct zs_db *db, const char *start, size_t startlen,
                   zs_cb *p, zs_cb *cb, void *rock, int flags);

/* transactions */
int  zs_db_begin_txn(struct zs_db *db, int shared, struct zs_txn **txnp);
int  zs_txn_commit(struct zs_txn **txnp);
int  zs_txn_abort(struct zs_txn **txnp);

int  zs_txn_fetch(struct zs_txn *txn, const char *key, size_t keylen,
                  const char **keyp, size_t *keylenp,
                  const char **valp, size_t *vallenp, int flags);
int  zs_txn_store(struct zs_txn *txn, const char *key, size_t keylen,
                  const char *val, size_t vallen, int flags);
int  zs_txn_foreach(struct zs_txn *txn, const char *start, size_t startlen,
                    zs_cb *p, zs_cb *cb, void *rock, int flags);

/* cursors, from a db (implicit transaction) or inside one.  key/keylen is the
 * seek position, and ZS_CURSOR_PREFIX additionally bounds the scan by it. */
int  zs_db_begin_cursor(struct zs_db *db, const char *key, size_t keylen,
                        struct zs_cursor **curp, int flags);
int  zs_txn_begin_cursor(struct zs_txn *txn, const char *key, size_t keylen,
                         struct zs_cursor **curp, int flags);
int  zs_cursor_next(struct zs_cursor *cur,
                    const char **keyp, size_t *keylenp,
                    const char **valp, size_t *vallenp);
int  zs_cursor_replace(struct zs_cursor *cur,
                       const char *val, size_t vallen, int flags);
int  zs_cursor_commit(struct zs_cursor **curp);
int  zs_cursor_abort(struct zs_cursor **curp);
void zs_cursor_fini(struct zs_cursor **curp);

/* Deletion is a store of a NULL value; these are macros, not functions, so
 * there is exactly one write path to implement and test.  A non-NULL
 * zero-length value stores an empty value, which is a distinct state from an
 * absent key. */
#define zs_db_delete(db, key, keylen, flags) \
        zs_db_store((db), (key), (keylen), NULL, 0, (flags))
#define zs_txn_delete(txn, key, keylen, flags) \
        zs_txn_store((txn), (key), (keylen), NULL, 0, (flags))
#define zs_cursor_delete(cur, flags) \
        zs_cursor_replace((cur), NULL, 0, (flags))

/* utility */
int  zs_db_repack(struct zs_db *db);
bool zs_db_should_repack(struct zs_db *db);

/* Convert the active generation, so every file in the database has a pointer
 * section and no reader has to replay a span chain (D-25).  Bounded by
 * rollover_size, so cheap enough to call routinely.  A no-op, not an error,
 * when there is nothing to seal. */
int  zs_db_seal(struct zs_db *db);
int  zs_db_check_consistency(struct zs_db *db);
int  zs_db_dump(struct zs_db *db, int detail);

/* Print the pointer table (spec section 8) covering each unordered file, as
 * text, for the interop runner to compare.  ZS_OK even when the cache is off or
 * no table exists: a table is never required, so its absence is a state to
 * report rather than an error. */
int  zs_db_index_dump(struct zs_db *db);
int  zs_db_sync(struct zs_db *db);
const char *zs_strerror(int r);

/* Not part of the stable API.
 *
 * Behaves as zs_db_open with ZS_CREATE, except that a database being created
 * takes the given UUID instead of a generated one.  Opening an existing
 * database ignores it.  This exists so zstool can generate a reproducible
 * golden corpus; no application should call it.
 */
int  zs_db_open_with_uuid(const char *dir, struct zs_open_data *setup,
                          const char *uuid_str, struct zs_db **dbp);

#endif /* INCLUDED_ZEROSKIP_H */
