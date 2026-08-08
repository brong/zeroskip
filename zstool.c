/* zstool.c - standalone CLI tool for zeroskip databases
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * Implements the driver contract from the spec's section 9.1 (T-0a): a fixed set
 * of subcommands over a database directory, with a defined line format, so one
 * language-neutral runner can drive every implementation.
 *
 * Keys and values are HEX on the way in and out.  Not for elegance: keys may
 * contain NUL bytes and newlines (F-13), and T-12 requires two implementations'
 * `scan` output match byte for byte -- which raw output cannot express.  Hex is
 * the only encoding where "the same bytes" and "the same text" coincide.
 *
 * Without this contract each language reimplements the conformance suite and they
 * drift apart, which is the failure mode the whole exercise exists to prevent.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zeroskip.h"

#define NOTFOUND_MARKER "NOTFOUND"

static void oops(const char *what, int r)
{
    fprintf(stderr, "zstool: %s: %s\n", what, zs_strerror(r));
    exit(1);
}

static void tool_error(const char *msg, const char *fmt, ...)
{
    (void)fmt;
    fprintf(stderr, "zstool: %s\n", msg);
}

/* Decode hex in place into a malloc'd buffer.  Returns NULL on malformed input;
 * an odd length or a non-hex digit is an error rather than something to salvage,
 * because a runner feeding us junk should hear about it. */
static char *unhex(const char *in, size_t *lenp)
{
    size_t n = strlen(in);

    if (n % 2) return NULL;
    *lenp = n / 2;

    char *out = malloc(*lenp ? *lenp : 1);
    if (!out) return NULL;

    for (size_t i = 0; i < *lenp; i++) {
        int hi = -1, lo = -1;
        char c = in[i * 2], d = in[i * 2 + 1];
        if (c >= '0' && c <= '9') hi = c - '0';
        else if (c >= 'a' && c <= 'f') hi = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') hi = c - 'A' + 10;
        if (d >= '0' && d <= '9') lo = d - '0';
        else if (d >= 'a' && d <= 'f') lo = d - 'a' + 10;
        else if (d >= 'A' && d <= 'F') lo = d - 'A' + 10;
        if (hi < 0 || lo < 0) { free(out); return NULL; }
        out[i] = (char)((hi << 4) | lo);
    }

    return out;
}

static void puthex(const char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) printf("%02x", (unsigned char)p[i]);
}

static const char *salvage_kind_name(int kind)
{
    switch (kind) {
    case ZS_SALVAGE_FILE_UNREADABLE: return "file-unreadable";
    case ZS_SALVAGE_HEADER_INVALID:  return "header-invalid";
    case ZS_SALVAGE_ENGINE_GUESSED:  return "engine-guessed";
    case ZS_SALVAGE_GAP:             return "gap";
    case ZS_SALVAGE_PTRS_IGNORED:    return "ptrs-ignored";
    case ZS_SALVAGE_SPAN_LOST:       return "span-lost";
    case ZS_SALVAGE_SPAN_ROLLBACK:   return "span-rollback";
    case ZS_SALVAGE_RESYNC:          return "resync";
    case ZS_SALVAGE_KEY_UNVERIFIED:  return "key-unverified";
    case ZS_SALVAGE_KEY_MAYBE_STALE: return "key-maybe-stale";
    }
    return "unknown";
}

struct salvage_tally { unsigned long stale, unverified, lost; };

/* One line per event, in the driver contract's shape (T-0a).  Fields that do
 * not apply to a kind are OMITTED rather than printed as zero, so a diff
 * between two implementations shows a real difference rather than padding. */
static int salvage_report(void *rock, const struct zs_salvage_event *ev)
{
    struct salvage_tally *t = rock;

    printf("SALVAGE kind=%s", salvage_kind_name(ev->kind));
    if (ev->file) printf(" file=%s", ev->file);
    if (ev->generation) printf(" generation=%08X", ev->generation);
    if (ev->length) printf(" length=%zu", ev->length);
    if (ev->offset) printf(" offset=%zu", ev->offset);
    if (ev->key) { printf(" key="); puthex(ev->key, ev->keylen); }
    printf("\n");

    if (ev->kind == ZS_SALVAGE_KEY_MAYBE_STALE) t->stale++;
    if (ev->kind == ZS_SALVAGE_KEY_UNVERIFIED) t->unverified++;
    if (ev->kind == ZS_SALVAGE_SPAN_LOST) t->lost++;

    return 0;
}

static int scan_cb(void *rock, const char *key, size_t keylen,
                   const char *val, size_t vallen)
{
    (void)rock;
    puthex(key, keylen);
    putchar(' ');
    puthex(val, vallen);
    putchar('\n');
    return 0;
}

static int usage(void)
{
    fprintf(stderr,
        "usage: zstool <dir> <command> [args]\n"
        "\n"
        "  create [--uuid U] [--engine 0|1]\n"
        "                         create a database\n"
        "  store KEYHEX VALHEX    one transaction, one store\n"
        "  delete KEYHEX          one transaction, one delete\n"
        "  batch                  a script on stdin, all in ONE transaction\n"
        "  get KEYHEX             print the value in hex, or " NOTFOUND_MARKER "\n"
        "  scan [--prefix PHEX]   every visible pair, in comparator order\n"
        "  dump [--detail N]      print structure\n"
        "  check                  run the consistency checks\n"
        "  convert                force conversion of non-active unordered files\n"
        "  repack                 force one repack\n"
        "  seal                   convert the active generation (D-25)\n"
        "  compact                merge the whole database into one file (D-26)\n"
        "  salvage TO [--unverified]\n"
        "                         rebuild what is readable into a NEW database\n"
        "                         at TO; never writes the source (section 9)\n"
        "  hold-write --for MS    take the write lock and hold it\n"
        "  index-dump             print the pointer table state (spec section 8)\n"
        "\n"
        "  --index-dir PATH       enable the pointer table cache for this run;\n"
        "                         MUST NOT be the database directory (P-2)\n"
        "\n"
        "Keys and values are hex, so embedded NULs and newlines survive the\n"
        "comparison an interop runner makes.\n"
        "\n"
        "batch script lines:\n"
        "  store KEYHEX VALHEX\n"
        "  delete KEYHEX\n");
    return 2;
}

int main(int argc, char **argv)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    const char *dir, *cmd;
    const char *uuid = NULL, *prefix = NULL, *index_dir = NULL;
    int detail = 0, engine = -1;
    long hold_ms = 0;
    int r;

    if (argc < 3) return usage();
    dir = argv[1];
    cmd = argv[2];

    /* Salvage does not open the source as a database -- it cannot, since the
     * databases worth salvaging are the ones that will not open (S-2).  So it
     * runs before the open below rather than in the dispatch chain. */
    if (!strcmp(cmd, "salvage")) {
        struct zs_salvage_data ss = ZS_SALVAGE_DATA_INITIALIZER;
        struct salvage_tally tally = { 0, 0, 0 };

        if (argc < 4) return usage();
        for (int i = 4; i < argc; i++)
            if (!strcmp(argv[i], "--unverified"))
                ss.flags |= ZS_SALVAGE_UNVERIFIED;

        ss.report = salvage_report;
        ss.rock = &tally;
        ss.error = tool_error;

        r = zs_db_salvage(dir, argv[3], &ss);
        if (r != ZS_OK) oops("salvage", r);

        printf("SALVAGED stale=%lu unverified=%lu lost=%lu\n",
               tally.stale, tally.unverified, tally.lost);
        return 0;
    }

    /* Options, wherever they appear after the command. */
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "--uuid") && i + 1 < argc)        uuid = argv[++i];
        else if (!strcmp(argv[i], "--prefix") && i + 1 < argc) prefix = argv[++i];
        else if (!strcmp(argv[i], "--detail") && i + 1 < argc) detail = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--for") && i + 1 < argc)    hold_ms = atol(argv[++i]);
        else if (!strcmp(argv[i], "--engine") && i + 1 < argc)  engine = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--index-dir") && i + 1 < argc) index_dir = argv[++i];
    }

    setup.error = tool_error;

    /* The pointer table cache (spec section 8).  The interop runner needs it so
     * one implementation can publish a table another reads -- which is the only
     * way to test that the format is really shared rather than merely written.
     * A threshold of one byte, because a corpus case is far smaller than any
     * sensible default and would otherwise never publish anything. */
    setup.index_dir = index_dir;
    if (index_dir) setup.index_threshold = 1;

    /* Everything but `create` opens an existing database.  `check` and the read
     * commands could open read-only, but `convert` and `repack` must not, and a
     * uniform open keeps the tool's behaviour predictable for a runner. */
    if (!strcmp(cmd, "create")) {
        setup.flags = ZS_CREATE;
        /* Which engine files this database's creator writes (A-6).  Engine 2 is
         * deliberately unreachable from here: a file written under it is readable
         * only by a caller supplying the same function, so it cannot appear in a
         * shared corpus (F-5d). */
        if (engine == 0) setup.flags |= ZS_CSUM_NONE;
        else if (engine == 1) setup.flags |= ZS_CSUM_XXHASH;
        r = uuid ? zs_db_open_with_uuid(dir, &setup, uuid, &db)
                 : zs_db_open(dir, &setup, &db);
        if (r != ZS_OK) oops("create", r);
        zs_db_close(&db);
        return 0;
    }

    r = zs_db_open(dir, &setup, &db);
    if (r != ZS_OK) oops("open", r);

    if (!strcmp(cmd, "store")) {
        if (argc < 5) return usage();
        size_t kl, vl;
        char *k = unhex(argv[3], &kl);
        char *v = unhex(argv[4], &vl);
        if (!k || !v) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
        r = zs_db_store(db, k, kl, v, vl, 0);
        if (r != ZS_OK) oops("store", r);
        free(k); free(v);

    } else if (!strcmp(cmd, "delete")) {
        if (argc < 4) return usage();
        size_t kl;
        char *k = unhex(argv[3], &kl);
        if (!k) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
        r = zs_db_delete(db, k, kl, 0);
        if (r != ZS_OK && r != ZS_NOTFOUND) oops("delete", r);
        free(k);

    } else if (!strcmp(cmd, "batch")) {
        /* All operations in ONE transaction, so multi-record spans are testable
         * (T-0a).  A single-operation-per-invocation tool could never produce a
         * span with more than one record in it. */
        struct zs_txn *txn = NULL;
        char line[65536];

        r = zs_db_begin_txn(db, 0, &txn);
        if (r != ZS_OK) oops("begin", r);

        while (fgets(line, sizeof(line), stdin)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = '\0';
            if (!line[0] || line[0] == '#') continue;

            char *sp = strchr(line, ' ');
            if (!sp) { fprintf(stderr, "zstool: bad batch line\n"); return 2; }
            *sp++ = '\0';

            if (!strcmp(line, "store")) {
                char *sp2 = strchr(sp, ' ');
                if (!sp2) { fprintf(stderr, "zstool: bad store line\n"); return 2; }
                *sp2++ = '\0';
                size_t kl, vl;
                char *k = unhex(sp, &kl);
                char *v = unhex(sp2, &vl);
                if (!k || !v) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
                r = zs_txn_store(txn, k, kl, v, vl, 0);
                if (r != ZS_OK) oops("store", r);
                free(k); free(v);
            } else if (!strcmp(line, "delete")) {
                size_t kl;
                char *k = unhex(sp, &kl);
                if (!k) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
                r = zs_txn_store(txn, k, kl, NULL, 0, 0);
                if (r != ZS_OK) oops("delete", r);
                free(k);
            } else {
                fprintf(stderr, "zstool: unknown batch op '%s'\n", line);
                return 2;
            }
        }

        r = zs_txn_commit(&txn);
        if (r != ZS_OK) oops("commit", r);

    } else if (!strcmp(cmd, "get")) {
        if (argc < 4) return usage();
        size_t kl;
        char *k = unhex(argv[3], &kl);
        if (!k) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
        const char *v;
        size_t vl;
        r = zs_db_fetch(db, k, kl, NULL, NULL, &v, &vl, 0);
        if (r == ZS_OK) { puthex(v, vl); putchar('\n'); }
        else if (r == ZS_NOTFOUND) printf("%s\n", NOTFOUND_MARKER);
        else oops("get", r);
        free(k);

    } else if (!strcmp(cmd, "scan")) {
        size_t pl = 0;
        char *p = NULL;
        if (prefix) {
            p = unhex(prefix, &pl);
            if (!p) { fprintf(stderr, "zstool: bad hex\n"); return 2; }
        }
        r = zs_db_foreach(db, p, pl, NULL, scan_cb, NULL,
                          p ? ZS_CURSOR_PREFIX : 0);
        if (r != ZS_OK) oops("scan", r);
        free(p);

    } else if (!strcmp(cmd, "dump")) {
        r = zs_db_dump(db, detail);
        if (r != ZS_OK) oops("dump", r);

    } else if (!strcmp(cmd, "index-dump")) {
        r = zs_db_index_dump(db);
        if (r != ZS_OK) oops("index-dump", r);

    } else if (!strcmp(cmd, "check")) {
        r = zs_db_check_consistency(db);
        if (r == ZS_OK) printf("OK\n");
        else { printf("FAILED %s\n", zs_strerror(r)); zs_db_close(&db); return 1; }

    } else if (!strcmp(cmd, "convert")) {
        /* A store of nothing is the smallest thing that makes a writer run its
         * conversion pass (D-12), and it leaves no record behind. */
        struct zs_txn *txn = NULL;
        r = zs_db_begin_txn(db, 0, &txn);
        if (r != ZS_OK) oops("begin", r);
        r = zs_txn_commit(&txn);
        if (r != ZS_OK) oops("commit", r);

    } else if (!strcmp(cmd, "seal")) {
        r = zs_db_seal(db);
        if (r != ZS_OK) oops("seal", r);

    } else if (!strcmp(cmd, "compact")) {
        /* ZS_BADFORMAT means it merged what it could and something is left
         * (D-28), which a runner needs to tell apart from an outright failure. */
        r = zs_db_compact(db);
        if (r != ZS_OK) oops("compact", r);

    } else if (!strcmp(cmd, "repack")) {
        r = zs_db_repack(db);
        if (r != ZS_OK) oops("repack", r);

    } else if (!strcmp(cmd, "hold-write")) {
        /* For lock-contention tests (T-13): take the write lock, announce that it
         * is held, and hold it.  The announcement matters -- a runner needs to know
         * the lock is taken before it starts the process it expects to block, or the
         * test races. */
        struct zs_txn *txn = NULL;
        r = zs_db_begin_txn(db, 0, &txn);
        if (r != ZS_OK) oops("begin", r);
        printf("HELD\n");
        fflush(stdout);
        usleep((useconds_t)(hold_ms > 0 ? hold_ms : 100) * 1000);
        r = zs_txn_abort(&txn);
        if (r != ZS_OK) oops("abort", r);

    } else {
        zs_db_close(&db);
        return usage();
    }

    zs_db_close(&db);
    return 0;
}
