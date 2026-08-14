/* zsbench.c - benchmark tool for zeroskip
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * Workloads are kept comparable with the sibling twom library's twombench, so
 * numbers can be read side by side, plus the measurements this design specifically
 * needs:
 *
 *   - cost per rollover, and per conversion, since a writer pays both inline
 *     (D-12d) and the claim is that they are bounded by rollover_size;
 *   - cost of a repack cascade, which is unbounded by design (open item 1);
 *   - SNAPSHOT OPEN COST as a function of active-file size.  That last one is the
 *     number open item 2 asks for before deciding whether a shared index is worth
 *     reintroducing: every snapshot replays the active file, bounded by
 *     rollover_size but paid per open.  Nobody should reintroduce shared mutable
 *     state on a hunch.
 *
 * Documented in doc/benchmarking.md.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "zeroskip.h"

#define MAX_REPS   99
#define MAX_RESULTS 128

static int nrecs = 20000;
static int reps = 3;
static size_t valsize = 100;
/* zsbench's historical key shape is "key%08d", 11 bytes.  It stays the default
 * rather than adopting twombench's 16, because every figure recorded in
 * doc/benchmarking.md and CLAUDE.md was measured at this width; a side-by-side
 * run passes a matching --keysize to both tools. */
static size_t keysize = 11;
static const char *csum_name = "xxh64";
static int selftest = 0;
static const char *filter = NULL;
static const char *csv_path = NULL;
static char workdir[1024];

/* ------------------------------------------------------------------------- */

/* Repetition timing.
 *
 * Every workload runs `reps` times and reports the median, which is what makes
 * a number worth comparing against another machine or another library.  Until
 * 2026-08-14 only `store, one txn each` did: the other nine timed a single run
 * while the banner printed "%d reps" over the whole report.
 *
 * Each workload owns its own repetition loop rather than being driven by a
 * registry, because what must be rebuilt between repetitions differs and only
 * the workload knows.  A read-only one may time the same database again;
 * one that MUTATES must rebuild first, and `repack cascade` most of all --
 * it consumes the file layout it measures, so a second repetition over the
 * same database would find nothing to do and post an excellent time. */
struct reptimes {
    double t[MAX_REPS];
    int    n;
};

static void rep_add(struct reptimes *rt, double secs)
{
    if (rt->n < MAX_REPS) rt->t[rt->n++] = secs;
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

/* Median rather than best.  The minimum is the luckiest run rather than the
 * representative one, and for the workloads dominated by fdatasync it is the
 * one where the filesystem happened to be idle. */
static double rep_median(const struct reptimes *rt)
{
    double s[MAX_REPS];
    if (rt->n < 1) return 0.0;
    memcpy(s, rt->t, (size_t)rt->n * sizeof(s[0]));
    qsort(s, (size_t)rt->n, sizeof(s[0]), cmp_double);
    return rt->n % 2 ? s[rt->n / 2]
                     : (s[rt->n / 2 - 1] + s[rt->n / 2]) / 2.0;
}

static double rep_min(const struct reptimes *rt)
{
    double m = rt->n ? rt->t[0] : 0.0;
    for (int i = 1; i < rt->n; i++) if (rt->t[i] < m) m = rt->t[i];
    return m;
}

static double rep_max(const struct reptimes *rt)
{
    double m = rt->n ? rt->t[0] : 0.0;
    for (int i = 1; i < rt->n; i++) if (rt->t[i] > m) m = rt->t[i];
    return m;
}

struct bench_result {
    char   name[64];
    size_t ops;
    int    reps;
    double median_ms, min_ms, max_ms;
};

static struct bench_result results[MAX_RESULTS];
static size_t nresults = 0;

/* Whether this workload runs at all.  Matches twombench's filter: a substring
 * of the name, or everything when none was given. */
static int selected(const char *name)
{
    return !filter || strstr(name, filter) != NULL;
}

/* Keep a row for --csv without printing anything.
 *
 * The study workloads -- the open, threshold and compaction sweeps -- print
 * multi-column tables of their own, so they store their rows this way and do
 * their own formatting.  Everything else goes through record() below.
 *
 * `slug` is NOT the display label, for two reasons.  The labels contain commas
 * ("store, one txn each"), which a CSV field cannot; and several of them embed
 * a measured quantity -- "fetch (2 files)" -- so the join key would change with
 * -n and a mapping table could not name it.  Slugs are twombench-shaped:
 * lowercase, underscore-separated, stable across every configuration. */
static void record_quiet(const char *slug, size_t ops, const struct reptimes *rt)
{
    if (nresults >= MAX_RESULTS) return;
    struct bench_result *r = &results[nresults++];
    snprintf(r->name, sizeof(r->name), "%s", slug);
    r->ops = ops;
    r->reps = rt->n;
    r->median_ms = rep_median(rt) * 1000.0;
    r->min_ms = rep_min(rt) * 1000.0;
    r->max_ms = rep_max(rt) * 1000.0;
}

/* Print the human-readable line and keep the row for --csv.
 *
 * `note` carries the per-workload extras -- file counts, hit counts, write
 * amplification -- which stay in the readable output only: they differ per
 * workload and have nowhere to go in a schema shared with another tool. */
static void record(const char *slug, const char *label, size_t ops,
                   const struct reptimes *rt, const char *note)
{
    double med = rep_median(rt);

    printf("  %-34s%8.0f/s  %6.2fs", label, med > 0 ? (double)ops / med : 0.0,
           med);
    if (note && *note) printf("  %s", note);
    printf("\n");
    fflush(stdout);

    record_quiet(slug, ops, rt);
}

/* Deliberately byte-identical to twombench's CSV_HEADER, so the two tools'
 * output can be joined without a translation step.  tests/benchcmp.sh does
 * that join and carries the workload mapping. */
#define CSV_HEADER \
    "benchmark,ops,reps,median_ms,min_ms,max_ms,ops_per_sec,n,keysize,valsize,csum"

static int csv_write(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "zsbench: cannot write %s\n", path);
        return -1;
    }
    fprintf(f, "%s\n", CSV_HEADER);
    for (size_t i = 0; i < nresults; i++) {
        const struct bench_result *r = &results[i];
        fprintf(f, "%s,%zu,%d,%.3f,%.3f,%.3f,%.0f,%d,%zu,%zu,%s\n",
                r->name, r->ops, r->reps,
                r->median_ms, r->min_ms, r->max_ms,
                r->median_ms > 0 ? (double)r->ops / (r->median_ms / 1000.0) : 0.0,
                nrecs, keysize, valsize, csum_name);
    }
    if (fclose(f)) {
        fprintf(stderr, "zsbench: cannot flush %s\n", path);
        return -1;
    }
    return 0;
}

/* Keys at the configured width.  The historical "key%08d" is what --keysize 11
 * produces, so the default run is unchanged; a wider key is padded and a
 * narrower one uses as many digits as fit, which keeps them distinct as long as
 * the width admits nrecs of them. */
static void makekey(char *buf, size_t bufsz, long i)
{
    size_t want = keysize < bufsz ? keysize : bufsz - 1;
    int digits = (int)(want > 3 ? want - 3 : 1);
    snprintf(buf, bufsz, "key%0*ld", digits, i);
    buf[want] = '\0';
}

static void quiet_error(const char *msg, const char *fmt, ...)
{
    (void)msg;
    (void)fmt;
}

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static void cleanup(const char *dir)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd)) {}
}

/* twombench spells its engines "xxh64" and "null"; the same two words select
 * F-5's engine 1 and engine 0 here, so a side-by-side run configures both
 * tools identically. */
static uint32_t csum_flag(void)
{
    return strcmp(csum_name, "null") == 0 ? (uint32_t)ZS_CSUM_NONE
                                          : (uint32_t)ZS_CSUM_XXHASH;
}

static struct zs_db *open_at(const char *dir, uint32_t flags, size_t rollover)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    flags |= csum_flag();
    setup.flags = flags;
    setup.rollover_size = rollover;
    setup.error = quiet_error;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) {
        fprintf(stderr, "zsbench: cannot open %s\n", dir);
        exit(1);
    }
    return db;
}

/* The same, with the pointer table cache enabled (spec section 8). */
static struct zs_db *open_cached(const char *dir, uint32_t flags, size_t rollover,
                                 const char *idxdir, size_t threshold)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    flags |= csum_flag();
    setup.flags = flags;
    setup.rollover_size = rollover;
    setup.error = quiet_error;
    setup.index_dir = idxdir;
    setup.index_threshold = threshold;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) {
        fprintf(stderr, "zsbench: cannot open %s\n", dir);
        exit(1);
    }
    return db;
}

static size_t dir_bytes(const char *dir)
{
    /* Total size of the data files, for reporting write amplification. */
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "cat '%s'/zeroskip-* 2>/dev/null | wc -c", dir);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    long long n = 0;
    if (fscanf(fp, "%lld", &n) != 1) n = 0;
    pclose(fp);
    return (size_t)n;
}

static int count_files(const char *dir)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
             "ls '%s' 2>/dev/null | grep -c '^zeroskip-'", dir);
    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;
    int n = 0;
    if (fscanf(fp, "%d", &n) != 1) n = 0;
    pclose(fp);
    return n;
}

/* ------------------------------------------------------------------------- */

/* Mutating: a fresh database per repetition, or the second one measures
 * overwrites of the first's records rather than inserts. */
static void bench_sequential_store(void)
{
    char dir[1200], note[96] = "";
    char *val = malloc(valsize);
    struct reptimes rt = { {0}, 0 };
    memset(val, 'v', valsize);

    if (!selected("store, one txn each")) { free(val); return; }

    for (int r = 0; r < reps; r++) {
        snprintf(dir, sizeof(dir), "%s/seq", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 0);

        double t0 = now();
        for (int i = 0; i < nrecs; i++) {
            char k[64];
            makekey(k, sizeof(k), i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        rep_add(&rt, now() - t0);

        snprintf(note, sizeof(note), "%d files  %.1fx amp", count_files(dir),
                 (double)dir_bytes(dir) / (double)(nrecs * (valsize + 12)));
        zs_db_close(&db);
        cleanup(dir);
    }

    record("store_1_per_txn", "store, one txn each", (size_t)nrecs, &rt, note);
    free(val);
}

static void bench_batched_store(void)
{
    /* C-7b: the two durability gates are paid per TRANSACTION, not per record, so a
     * caller that batches amortises them.  This is the measurement that shows by
     * how much, and it is the reason zs_txn_* exists alongside zs_db_*. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    for (int per = 10; per <= 1000; per *= 10) {
        char label[64], slug[64];
        struct reptimes rt = { {0}, 0 };
        snprintf(label, sizeof(label), "store, %d per txn", per);
        if (!selected(label)) continue;

        for (int r = 0; r < reps; r++) {
            snprintf(dir, sizeof(dir), "%s/batch", workdir);
            cleanup(dir);
            struct zs_db *db = open_at(dir, ZS_CREATE, 0);

            double t0 = now();
            int done = 0;
            while (done < nrecs) {
                struct zs_txn *txn = NULL;
                if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) break;
                for (int i = 0; i < per && done < nrecs; i++, done++) {
                    char k[64];
                    makekey(k, sizeof(k), done);
                    zs_txn_store(txn, k, strlen(k), val, valsize, 0);
                }
                zs_txn_commit(&txn);
            }
            rep_add(&rt, now() - t0);
            zs_db_close(&db);
            cleanup(dir);
        }

        snprintf(slug, sizeof(slug), "store_%d_per_txn", per);
        record(slug, label, (size_t)nrecs, &rt, "");
    }
    free(val);
}

static void bench_fetch(void)
{
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    snprintf(dir, sizeof(dir), "%s/fetch", workdir);
    cleanup(dir);
    struct zs_db *db = open_at(dir, ZS_CREATE, 0);

    for (int i = 0; i < nrecs; i++) {
        char k[64];
        makekey(k, sizeof(k), i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);

    /* Before and after a repack, because the cost of a point lookup is
     * proportional to the NUMBER OF FILES (D-14d) -- which is the whole reason the
     * repack policy exists.
     *
     * Read-only, so the repetitions reuse the database the pass built.  The
     * repack in pass 1 mutates and therefore happens once, outside the loop --
     * it is setup for what is being timed, not part of it. */
    for (int pass = 0; pass < 2; pass++) {
        char label[64], slug[64];
        int files, hits = 0;
        struct reptimes rt = { {0}, 0 };

        db = open_at(dir, 0, 0);
        if (pass == 1) {
            while (zs_db_should_repack(db)) zs_db_repack(db);
        }

        files = count_files(dir);
        /* The repacked pass is named as such rather than by its file count
         * alone: at a small -n the repack changes nothing and both passes would
         * otherwise carry the same name, which is a collision in --csv. */
        snprintf(label, sizeof(label), "fetch (%d files%s)", files,
                 pass ? ", repacked" : "");
        snprintf(slug, sizeof(slug), "fetch%s", pass ? "_repacked" : "");
        if (!selected(label)) { zs_db_close(&db); continue; }

        for (int r = 0; r < reps; r++) {
            double t0 = now();
            hits = 0;
            for (int i = 0; i < nrecs; i++) {
                char k[64];
                const char *v;
                size_t vl;
                makekey(k, sizeof(k), (i * 7919) % nrecs);
                if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) == ZS_OK)
                    hits++;
            }
            rep_add(&rt, now() - t0);
        }

        {
            char note[96];
            snprintf(note, sizeof(note), "%d hits", hits);
            record(slug, label, (size_t)nrecs, &rt, note);
        }
        zs_db_close(&db);
    }

    cleanup(dir);
    free(val);
}

/* Read-only, so every repetition scans the same database. */
static void bench_scan_once(struct zs_db *db, const char *slug,
                            const char *label)
{
    struct reptimes rt = { {0}, 0 };
    long seen = 0;
    char note[96];

    if (!selected(label)) return;

    for (int r = 0; r < reps; r++) {
        double t0 = now();
        struct zs_cursor *c = NULL;
        seen = 0;
        if (zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED) == ZS_OK) {
            const char *k, *v;
            size_t kl, vl;
            while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) seen++;
            zs_cursor_abort(&c);
        }
        rep_add(&rt, now() - t0);
    }

    snprintf(note, sizeof(note), "%ld records", seen);
    record(slug, label, (size_t)seen, &rt, note);
}

static void bench_scan(void)
{
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    snprintf(dir, sizeof(dir), "%s/scan", workdir);
    cleanup(dir);
    struct zs_db *db = open_at(dir, ZS_CREATE, 0);
    for (int i = 0; i < nrecs; i++) {
        char k[64];
        makekey(k, sizeof(k), i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);

    /* Two lines, because per-record stores build the most fragmented file
     * the format can produce -- one span per record -- and a scan of that is
     * the write-heavy worst case, not the steady state.  The compacted line
     * is what long-lived data reads like. */
    db = open_at(dir, 0, 0);
    bench_scan_once(db, "scan", "full scan");
    zs_db_close(&db);

    db = open_at(dir, 0, 0);
    zs_db_compact(db);
    bench_scan_once(db, "scan_compacted", "full scan, compacted");
    zs_db_close(&db);

    cleanup(dir);
    free(val);
}

static void bench_rollover_and_convert(void)
{
    /* D-12d: a writer's extra cost is bounded by rollover_size rather than
     * proportional to the database.  Measured as the per-store cost at several
     * rollover sizes: if the claim holds, a smaller rollover means more frequent
     * but individually cheaper conversions, and the throughput should not collapse
     * as the database grows. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    static const size_t sizes[] = { 16 * 1024, 64 * 1024, 256 * 1024,
                                    2 * 1024 * 1024 };

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        char label[64], slug[64], note[96] = "";
        struct reptimes rt = { {0}, 0 };
        snprintf(label, sizeof(label), "store, rollover %zuk", sizes[s] / 1024);
        snprintf(slug, sizeof(slug), "store_rollover_%zuk", sizes[s] / 1024);
        if (!selected(label)) continue;

        /* Mutating: a fresh database per repetition. */
        for (int r = 0; r < reps; r++) {
            snprintf(dir, sizeof(dir), "%s/roll", workdir);
            cleanup(dir);
            struct zs_db *db = open_at(dir, ZS_CREATE, sizes[s]);

            double t0 = now();
            for (int i = 0; i < nrecs; i++) {
                char k[64];
                makekey(k, sizeof(k), i);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            rep_add(&rt, now() - t0);

            snprintf(note, sizeof(note), "%d files", count_files(dir));
            zs_db_close(&db);
            cleanup(dir);
        }

        record(slug, label, (size_t)nrecs, &rt, note);
    }
    free(val);
}

static void bench_repack_cascade(void)
{
    /* Open item 1: a repack cascade is unbounded in duration by design (D-16b).
     * Measured so the trade is a number rather than a worry. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    char note[96] = "";
    struct reptimes rt = { {0}, 0 };
    size_t merged = 0;

    if (!selected("repack cascade")) { free(val); return; }

    /* The most strongly mutating workload there is: a cascade CONSUMES the file
     * layout it measures, so the database has to be rebuilt for every
     * repetition.  Timing the same one twice would find nothing to repack on
     * the second pass and post an excellent number for doing nothing. */
    for (int r = 0; r < reps; r++) {
        snprintf(dir, sizeof(dir), "%s/cascade", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 32 * 1024);

        for (int i = 0; i < nrecs; i++) {
            char k[64];
            makekey(k, sizeof(k), i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }

        int before = count_files(dir);
        double t0 = now();
        int rounds = 0;
        while (zs_db_should_repack(db)) {
            if (zs_db_repack(db) != ZS_OK) break;
            rounds++;
            if (rounds > 100) break;
        }
        rep_add(&rt, now() - t0);

        int after = count_files(dir);
        merged = (size_t)(before > after ? before - after : 0);
        snprintf(note, sizeof(note), "%d -> %d files  %d rounds",
                 before, after, rounds);
        zs_db_close(&db);
        cleanup(dir);
    }

    /* ops is FILES MERGED AWAY, not records: the cascade's unit of work is a
     * file, and a per-record rate here would invite comparison with the store
     * workloads, which measure something else entirely. */
    record("repack", "repack cascade", merged, &rt, note);
    free(val);
}

static void bench_snapshot_open(void)
{
    if (!selected("snapshot open")) return;

    /* THE NUMBER OPEN ITEM 2 ASKS FOR.
     *
     * Every snapshot replays the active file to build its private index (D-13d).
     * That is bounded by rollover_size but paid per open, and the spec says the way
     * to share it -- an append-only (key, offset) log per file -- should not be
     * built "before there is a number".  This is the number.
     *
     * Measured as open cost against the number of records in the active file, at a
     * rollover large enough that nothing converts during the run. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    printf("\n  snapshot open cost vs active-file size (open item 2)\n");
    printf("    %-12s %-12s %-12s %s\n",
           "records", "active KB", "open ms", "us/record");

    for (int n = 250; n <= 16000; n *= 2) {
        snprintf(dir, sizeof(dir), "%s/snap", workdir);
        cleanup(dir);

        /* A rollover big enough that everything stays in one active file. */
        struct zs_db *db = open_at(dir, ZS_CREATE, 512 * 1024 * 1024);
        for (int i = 0; i < n; i++) {
            char k[64];
            makekey(k, sizeof(k), i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        zs_db_close(&db);

        size_t bytes = dir_bytes(dir);

        /* Read-only: the same database is opened `reps` times. */
        struct reptimes rt = { {0}, 0 };
        for (int r = 0; r < reps; r++) {
            double t0 = now();
            db = open_at(dir, 0, 512 * 1024 * 1024);
            rep_add(&rt, now() - t0);
            zs_db_close(&db);
        }
        double med = rep_median(&rt);

        printf("    %-12d %-12.1f %-12.3f %.3f\n",
               n, (double)bytes / 1024.0, med * 1000.0,
               med * 1e6 / (double)n);

        /* One open is one op, so ops_per_sec reads as opens per second. */
        {
            char row[64];
            snprintf(row, sizeof(row), "snapshot_open_n%d", n);
            record_quiet(row, 1, &rt);
        }

        cleanup(dir);
    }

    printf("    (linear us/record means the replay dominates; the pointer table\n"
           "     cache removes both the scan and the sort -- see spec section 8)\n");
    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_cached_open(void)
{
    if (!selected("open cached") && !selected("open plain")) return;

    /* What spec section 8 buys: the same open, with a published pointer table
     * to seed the index from.
     *
     * The comparison is deliberately open-against-open on the SAME database,
     * because the cache changes nothing else -- same files, same records, same
     * answers.  The cached column is measured after one warming open, since the
     * point of a cache is what the SECOND process pays.
     *
     * A cold row is reported too.  The warm number flatters the cache: with the
     * data file already in page cache the replay it avoids is cheap, while the
     * whole reason the cache is worth having is a process that would otherwise
     * fault the entire active file in. */
    char dir[1200], idx[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    printf("\n  open cost with and without a pointer table (spec section 8)\n");
    printf("    %-12s %-12s %-12s %-12s %s\n",
           "records", "active KB", "plain ms", "cached ms", "speedup");

    for (int n = 250; n <= 16000; n *= 2) {
        snprintf(dir, sizeof(dir), "%s/cachedopen", workdir);
        snprintf(idx, sizeof(idx), "%s/cachedopen-idx", workdir);
        cleanup(dir);
        cleanup(idx);
        if (mkdir(idx, 0700) && errno != EEXIST) { perror(idx); exit(1); }

        /* Threshold 1: publish at every opportunity, so the table is current.
         * The threshold's own cost is measured separately below. */
        struct zs_db *db = open_cached(dir, ZS_CREATE, 512 * 1024 * 1024, idx, 1);
        for (int i = 0; i < n; i++) {
            char k[64];
            makekey(k, sizeof(k), i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        zs_db_close(&db);

        size_t bytes = dir_bytes(dir);

        struct reptimes pt = { {0}, 0 }, ct = { {0}, 0 };
        for (int r = 0; r < reps; r++) {
            double t0 = now();
            db = open_at(dir, 0, 512 * 1024 * 1024);
            rep_add(&pt, now() - t0);
            zs_db_close(&db);
        }
        for (int r = 0; r < reps; r++) {
            double t0 = now();
            db = open_cached(dir, 0, 512 * 1024 * 1024, idx, 1);
            rep_add(&ct, now() - t0);
            zs_db_close(&db);
        }
        double plain = rep_median(&pt), cached = rep_median(&ct);

        printf("    %-12d %-12.1f %-12.3f %-12.3f %.1fx\n",
               n, (double)bytes / 1024.0, plain * 1000.0, cached * 1000.0,
               cached > 0 ? plain / cached : 0.0);

        {
            char row[64];
            snprintf(row, sizeof(row), "open_plain_n%d", n);
            record_quiet(row, 1, &pt);
            snprintf(row, sizeof(row), "open_cached_n%d", n);
            record_quiet(row, 1, &ct);
        }

        cleanup(dir);
        cleanup(idx);
    }

    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_index_threshold(void)
{
    if (!selected("publish")) return;

    /* What P-13's threshold trades.
     *
     * Low: a table is republished often, so every commit carries an O(records)
     * merge and an O(records) write, and a bulk load goes quadratic.
     * High: publishing is rare, so the next reader replays further.
     *
     * Both costs are reported against the same workload, which is what makes the
     * default a measurement rather than a guess. */
    char dir[1200], idx[1200];
    char *val = malloc(valsize);
    static const size_t thresholds[] = { 1, 512, 4096, 32768, 262144, 1048576 };
    memset(val, 'v', valsize);

    printf("\n  publish threshold: write cost vs open cost (P-13)\n");
    printf("    %-14s %-14s %-14s %s\n",
           "threshold", "store ms", "open ms", "table KB");

    /* The baseline, with no cache at all.  It belongs in this table rather than
     * beside it: a write transaction refreshes its snapshot at BEGIN (C-4), and
     * that refresh replays the active file from the last published point.  With
     * no table there is no published point, so every commit replays the whole
     * file and a one-store-per-transaction load is quadratic.  The threshold is
     * what bounds that replay, which is a bigger effect than the republication
     * cost it also controls. */
    {
        struct reptimes st = { {0}, 0 }, ot = { {0}, 0 };
        struct zs_db *db = NULL;

        /* The store phase MUTATES, so each repetition rebuilds from nothing;
         * the open phase then measures the database the last one left. */
        for (int r = 0; r < reps; r++) {
            snprintf(dir, sizeof(dir), "%s/thresh", workdir);
            cleanup(dir);
            double t0 = now();
            db = open_at(dir, ZS_CREATE, 512 * 1024 * 1024);
            for (int i = 0; i < nrecs; i++) {
                char k[64];
                makekey(k, sizeof(k), i);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            zs_db_close(&db);
            rep_add(&st, now() - t0);
        }

        for (int r = 0; r < reps; r++) {
            double s0 = now();
            db = open_at(dir, 0, 512 * 1024 * 1024);
            rep_add(&ot, now() - s0);
            zs_db_close(&db);
        }

        printf("    %-14s %-14.1f %-14.3f %s\n",
               "no cache", rep_median(&st) * 1000.0,
               rep_median(&ot) * 1000.0, "-");
        record_quiet("publish_store_nocache", (size_t)nrecs, &st);
        record_quiet("publish_open_nocache", 1, &ot);
        cleanup(dir);
    }

    for (size_t t = 0; t < sizeof(thresholds) / sizeof(thresholds[0]); t++) {
        snprintf(dir, sizeof(dir), "%s/thresh", workdir);
        snprintf(idx, sizeof(idx), "%s/thresh-idx", workdir);
        cleanup(dir);
        cleanup(idx);
        if (mkdir(idx, 0700) && errno != EEXIST) { perror(idx); exit(1); }

        struct reptimes st = { {0}, 0 }, ot = { {0}, 0 };
        struct zs_db *db = NULL;

        for (int r = 0; r < reps; r++) {
            cleanup(dir);
            cleanup(idx);
            if (mkdir(idx, 0700) && errno != EEXIST) { perror(idx); exit(1); }
            double t0 = now();
            db = open_cached(dir, ZS_CREATE, 512 * 1024 * 1024,
                             idx, thresholds[t]);
            for (int i = 0; i < nrecs; i++) {
                char k[64];
                makekey(k, sizeof(k), i);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            zs_db_close(&db);
            rep_add(&st, now() - t0);
        }

        for (int r = 0; r < reps; r++) {
            double s0 = now();
            db = open_cached(dir, 0, 512 * 1024 * 1024, idx, thresholds[t]);
            rep_add(&ot, now() - s0);
            zs_db_close(&db);
        }
        double store = rep_median(&st), open_best = rep_median(&ot);

        /* How large the surviving table is.  Total bytes written cannot be
         * observed after the fact -- each publish replaces the last -- so this
         * reports the steady-state size, and the store column carries the
         * republication cost.  One level down: tables live in a per-uuid
         * directory under the configured root (P-2a). */
        char cmd[2600];
        snprintf(cmd, sizeof(cmd),
                 "cat '%s'/*/zeroskip.index-* 2>/dev/null | wc -c", idx);
        FILE *fp = popen(cmd, "r");
        long long tb = 0;
        if (fp) { if (fscanf(fp, "%lld", &tb) != 1) tb = 0; pclose(fp); }

        printf("    %-14zu %-14.1f %-14.3f %.1f\n",
               thresholds[t], store * 1000.0, open_best * 1000.0,
               (double)tb / 1024.0);

        {
            char row[64];
            snprintf(row, sizeof(row), "publish_store_t%zu", thresholds[t]);
            record_quiet(row, (size_t)nrecs, &st);
            snprintf(row, sizeof(row), "publish_open_t%zu", thresholds[t]);
            record_quiet(row, 1, &ot);
        }

        cleanup(dir);
        cleanup(idx);
    }

    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_compact(void)
{
    if (!selected("compact")) return;

    /* D-26/D-27: what compaction costs and what it reclaims.
     *
     * The reclamation is the number worth having.  A repack collapses duplicate
     * versions within its inputs, but only a compaction spanning the whole
     * generation interval can drop TOMBSTONES (D-19), so the interesting figure
     * is how much a heavily-deleted database shrinks.
     *
     * Reported against a repacked baseline rather than a raw one, so the column
     * shows what compaction adds over the repack a caller would already be
     * running -- not the space repacking would have reclaimed anyway. */
    char dir[1200];
    char *val = malloc(valsize);
    static const int deleted_pct[] = { 0, 25, 50, 75 };
    memset(val, 'v', valsize);

    printf("\n  compaction: cost and reclamation (D-26, D-27)\n");
    printf("    %-12s %-12s %-12s %-12s %s\n",
           "deleted %", "repacked KB", "compact KB", "reclaimed", "compact ms");

    for (size_t t = 0; t < sizeof(deleted_pct) / sizeof(deleted_pct[0]); t++) {
        struct reptimes rt = { {0}, 0 };
        size_t repacked = 0, compacted = 0;

        /* Compaction collapses the database into one file, so it can only be
         * measured once per build: every repetition reconstructs the deleted
         * state from scratch. */
        for (int rep = 0; rep < reps; rep++) {
            snprintf(dir, sizeof(dir), "%s/compact", workdir);
            cleanup(dir);

            struct zs_db *db = open_at(dir, ZS_CREATE, 64 * 1024);
            for (int i = 0; i < nrecs; i++) {
                char k[64];
                makekey(k, sizeof(k), i);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            for (int i = 0; i < nrecs * deleted_pct[t] / 100; i++) {
                char k[64];
                makekey(k, sizeof(k), i);
                zs_db_delete(db, k, strlen(k), 0);
            }

            /* The baseline a caller already has: sealed and repacked as far as
             * D-16's rule goes. */
            zs_db_seal(db);
            while (zs_db_should_repack(db)) zs_db_repack(db);
            repacked = dir_bytes(dir);

            double t0 = now();
            int r = zs_db_compact(db);
            rep_add(&rt, now() - t0);
            if (r != ZS_OK) {
                fprintf(stderr, "zsbench: compact failed: %s\n", zs_strerror(r));
                exit(1);
            }
            compacted = dir_bytes(dir);
            zs_db_close(&db);
            cleanup(dir);
        }

        printf("    %-12d %-12.1f %-12.1f %-11.1f%% %.1f\n",
               deleted_pct[t], (double)repacked / 1024.0,
               (double)compacted / 1024.0,
               repacked ? 100.0 * (double)(repacked - compacted) / (double)repacked
                        : 0.0,
               rep_median(&rt) * 1000.0);

        {
            char row[64];
            snprintf(row, sizeof(row), "compact_del%d", deleted_pct[t]);
            record_quiet(row, 1, &rt);
        }
    }

    printf("    (compaction is UNBOUNDED: it rewrites everything in one call\n"
           "     while writers continue -- see spec open item 1 and D-29)\n");
    free(val);
}

/* ------------------------------------------------------------------------- */

static int run_selftest(void)
{
    /* Enough to prove the harness measures a working database rather than an empty
     * one -- a benchmark that silently ran against a failed open would report
     * excellent numbers. */
    char dir[1200];
    snprintf(dir, sizeof(dir), "%s/selftest", workdir);
    cleanup(dir);

    struct zs_db *db = open_at(dir, ZS_CREATE, 0);
    int fail = 0;

    for (int i = 0; i < 500; i++) {
        char k[32], v[32];
        snprintf(k, sizeof(k), "key%04d", i);
        snprintf(v, sizeof(v), "val%04d", i);
        if (zs_db_store(db, k, strlen(k), v, strlen(v), 0) != ZS_OK) fail++;
    }

    for (int i = 0; i < 500; i++) {
        char k[32], want[32];
        const char *v;
        size_t vl;
        snprintf(k, sizeof(k), "key%04d", i);
        snprintf(want, sizeof(want), "val%04d", i);
        if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) != ZS_OK) fail++;
        else if (vl != strlen(want) || memcmp(v, want, vl) != 0) fail++;
    }

    if (zs_db_check_consistency(db) != ZS_OK) fail++;
    zs_db_close(&db);
    cleanup(dir);

    printf("selftest: %s\n", fail ? "FAILED" : "ok");
    return fail ? 1 : 0;
}

/* The long option names match twombench's, so a side-by-side run configures
 * both tools with the same words.  --value and --dir are zsbench's original
 * spellings and still work, undocumented, so existing invocations and scripts
 * do not break. */
static int usage(void)
{
    fprintf(stderr,
        "usage: zsbench [options] [filter]\n"
        "\n"
        "Runs each benchmark whose name contains <filter>,\n"
        "or all of them if no filter is given.\n"
        "\n"
        "  --selftest       verify the harness, then exit\n"
        "  -n, --records N  records per run (default %d)\n"
        "      --reps N     repetitions per benchmark, median kept (default %d)\n"
        "      --keysize N  key length in bytes (default %zu, minimum 4)\n"
        "      --valsize N  value length in bytes (default %zu)\n"
        "      --csum ENG   xxh64 (default) or null\n"
        "      --path DIR   working directory (default $TMPDIR)\n"
        "      --csv FILE   write results as CSV\n",
        nrecs, reps, keysize, valsize);
    return 2;
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(workdir, sizeof(workdir), "%s/zsbench.%d", tmp, (int)getpid());

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if ((!strcmp(argv[i], "-n") || !strcmp(argv[i], "--records"))
                 && i + 1 < argc) nrecs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keysize") && i + 1 < argc)
            keysize = (size_t)atol(argv[++i]);
        else if ((!strcmp(argv[i], "--valsize") || !strcmp(argv[i], "--value"))
                 && i + 1 < argc) valsize = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--csum") && i + 1 < argc)
            csum_name = argv[++i];
        else if ((!strcmp(argv[i], "--path") || !strcmp(argv[i], "--dir"))
                 && i + 1 < argc)
            snprintf(workdir, sizeof(workdir), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--csv") && i + 1 < argc)
            csv_path = argv[++i];
        else if (argv[i][0] != '-' && !filter) filter = argv[i];
        else return usage();
    }

    if (nrecs < 1) {
        fprintf(stderr, "zsbench: -n/--records must be at least 1\n");
        return 2;
    }
    if (reps < 1 || reps > MAX_REPS) {
        fprintf(stderr, "zsbench: --reps must be between 1 and %d\n", MAX_REPS);
        return 2;
    }
    /* "key" plus at least one digit, or the keys stop being distinct. */
    if (keysize < 4) {
        fprintf(stderr, "zsbench: --keysize must be at least 4\n");
        return 2;
    }
    if (strcmp(csum_name, "xxh64") && strcmp(csum_name, "null")) {
        fprintf(stderr, "zsbench: --csum must be xxh64 or null\n");
        return 2;
    }

    if (mkdir(workdir, 0700) && errno != EEXIST) {
        perror(workdir);
        return 1;
    }

    int rc = 0;
    if (selftest) {
        rc = run_selftest();
    } else {
        printf("zeroskip benchmark: %d records, %zu-byte keys, "
               "%zu-byte values, %d reps (median)\n\n",
               nrecs, keysize, valsize, reps);
        bench_sequential_store();
        bench_batched_store();
        bench_fetch();
        bench_scan();
        bench_rollover_and_convert();
        bench_repack_cascade();
        bench_snapshot_open();
        bench_cached_open();
        bench_index_threshold();
        bench_compact();

        if (csv_path && csv_write(csv_path) != 0) rc = 1;
    }

    cleanup(workdir);
    return rc;
}
