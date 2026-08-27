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
/* Where a long key VARIES, which decides what a key comparison costs and is
 * therefore a property of the workload, not a detail.  "head" puts the digits
 * first and pads after them -- a message-id or a G<40 random chars> key, which
 * two comparisons settle.  "shared" pads first, so every key agrees for its
 * first --keysize-11 bytes, the shape of a structured name like
 * "Ndomain!user.foo" repeated over a long run.  At the default --keysize 11
 * there is no padding and the two are identical. */
static const char *keyshape = "head";
static int selftest = 0;
static const char *filter = NULL;
static const char *csv_path = NULL;
static char workdir[1024];

/* Setup and run as separate invocations: --setup / --run.
 *
 * A read-only workload's fixture costs far more to build than the thing it
 * measures costs to run -- 435 seconds against 0.25 for a 500k-record scan on a
 * network-backed mount -- so `perf record ./zsbench scan` profiles the fixture
 * build and reports almost nothing about the merge loop.  FIXTURE_FLAGS took the
 * fdatasync storm out of that build; this takes the build out of the PROCESS.
 * --setup builds the fixtures under --path and exits; --run times them and
 * builds nothing, so the profiled process opens a database and reads it.
 *
 * Only the read-only workloads can split.  A store workload's setup IS its
 * measurement, and `scan in a write txn` needs a live transaction, which cannot
 * cross a process boundary; UNSPLITTABLE() names them, and the run banner says
 * they were skipped rather than leaving a short report to be read as a full one. */
enum phase { PHASE_ALL, PHASE_SETUP, PHASE_RUN };
static enum phase phase = PHASE_ALL;

#define UNSPLITTABLE() do { if (phase != PHASE_ALL) return; } while (0)

/* ------------------------------------------------------------------------- */

/* Repetition timing.
 *
 * Every workload runs `reps` times and reports the median, which is what makes
 * a number worth comparing against another machine or another library.
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

    if (keyshape[0] == 's') {           /* shared: pad first, vary last */
        int digits = (int)(want > 3 ? want - 3 : 1);
        snprintf(buf, bufsz, "key%0*ld", digits, i);
    } else {                            /* head: vary first, pad after */
        int digits = (int)(want > 11 ? 8 : (want > 3 ? want - 3 : 1));
        size_t n = (size_t)snprintf(buf, bufsz, "key%0*ld", digits, i);
        while (n < want) buf[n++] = 'x';
    }

    buf[want] = '\0';
}

/* Every workload builds its keys in a buffer of this size.  makekey clamps to
 * the buffer it is given, so a buffer smaller than --keysize would silently
 * measure shorter keys than the run claims to be measuring. */
#define ZSB_KEYMAX 1024

/* Key i of a strided PERMUTATION of [0, nrecs), for the workloads whose subject
 * is the order keys arrive in rather than how many there are.
 *
 * i -> (i * P) mod n is a permutation when gcd(P, n) = 1.  P is prime and larger
 * than any n anyone benchmarks, so the only way to lose that is n being a
 * multiple of P; the second prime covers it, and being a multiple of both needs
 * n above 10^12.
 *
 * Two workloads want it, for the same reason from opposite ends: ascending keys
 * are the best case for the sorted structures on both sides of the library.  On
 * the read side they build a file set whose ranges never overlap, so the merge
 * never merges; on the write side every insert into a transaction's pending set
 * lands at the end. */
static long long strided(long long i)
{
    long long p = (nrecs % 1000003 == 0) ? 999983 : 1000003;
    return (i * p) % nrecs;
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

/* A second fixture in a different state, without a second build.
 *
 * `fetch, repacked` and `full scan, compacted` must not repack or compact the
 * fixture they have just timed IN PLACE: that works only while one process runs
 * both lines in order, and a reused fixture would then be found already
 * transformed, timing the same database twice.  Each timed line gets its own
 * directory, copied from the first rather than stored again, since the copy is
 * a fraction of the cost of nrecs commits. */
static void copydir(const char *src, const char *dst)
{
    char cmd[4096];
    cleanup(dst);
    snprintf(cmd, sizeof(cmd), "cp -R '%s' '%s'", src, dst);
    if (system(cmd)) {
        fprintf(stderr, "zsbench: cannot copy %s to %s\n", src, dst);
        exit(1);
    }
}

/* Removing a fixture the workload has finished with -- unless the point of this
 * invocation was to leave it behind. */
static void fixture_done(const char *dir)
{
    if (phase == PHASE_ALL) cleanup(dir);
}

/* A fixture --setup should have left here, and did not.  --setup ignores the
 * filter precisely so this cannot happen by pairing two invocations differently,
 * which leaves a stale or half-removed --path directory.  Named here rather than
 * left to the open, which would report a database that cannot be opened and send
 * the reader looking for corruption. */
static void fixture_require(const char *dir)
{
    struct stat st;
    if (stat(dir, &st) == 0 && S_ISDIR(st.st_mode)) return;
    fprintf(stderr, "zsbench: fixture %s is missing -- "
                    "run --setup on this --path again\n", dir);
    exit(2);
}

/* The parameters the fixtures were built with.
 *
 * A --run at a different -n would fetch keys that were never stored and scan a
 * database of the wrong size, then report an excellent number for missing every
 * time.  That is the failure --selftest exists to prevent, arriving by a
 * different door, so the pairing is checked rather than trusted.
 *
 * A --run therefore ADOPTS whatever it was not told, rather than defaulting:
 * requiring the parameters to be repeated would make the common invocation --
 * `perf record ./zsbench --path=DIR --run scan` -- refuse at any -n but the
 * default, and under a profiler a refusal looks like a benchmark that ran
 * rather than one that never started.  A parameter that IS given still has to
 * match: adopting it silently would answer a question the caller asked about a
 * different database. */
#define STAMP_NAME "zsbench.setup"

static int nrecs_given = 0, keysize_given = 0, valsize_given = 0,
           csum_given = 0;

static void stamp_write(void)
{
    char path[1200];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", workdir, STAMP_NAME);
    f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fprintf(f, "%d %zu %zu %s\n", nrecs, keysize, valsize, csum_name);
    if (fclose(f)) { perror(path); exit(1); }
}

static void stamp_check(void)
{
    char path[1200], cs[64] = "";
    int n = 0;
    size_t ks = 0, vs = 0;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", workdir, STAMP_NAME);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "zsbench: no fixtures in %s -- "
                        "run --setup there first\n", workdir);
        exit(2);
    }
    if (fscanf(f, "%d %zu %zu %63s", &n, &ks, &vs, cs) != 4) {
        fprintf(stderr, "zsbench: cannot read %s\n", path);
        fclose(f);
        exit(2);
    }
    fclose(f);

    /* Adopting means these are no longer the command line's, so they are
     * validated here as well as there. */
    if (n < 1 || ks < 4 || (strcmp(cs, "xxh64") && strcmp(cs, "null"))) {
        fprintf(stderr, "zsbench: %s is not a fixture stamp\n", path);
        exit(2);
    }

    if ((nrecs_given   && n  != nrecs)   ||
        (keysize_given && ks != keysize) ||
        (valsize_given && vs != valsize) ||
        (csum_given    && strcmp(cs, csum_name))) {
        /* Only what was actually GIVEN is echoed back: printing the effective
         * values would show defaults the caller never typed, next to fixture
         * values they do not match, and read as four disagreements. */
        fprintf(stderr,
                "zsbench: --run does not match the fixtures in %s\n"
                "  fixtures: -n %d --keysize %zu --valsize %zu --csum %s\n"
                "  this run:", workdir, n, ks, vs, cs);
        if (nrecs_given)   fprintf(stderr, " -n %d", nrecs);
        if (keysize_given) fprintf(stderr, " --keysize %zu", keysize);
        if (valsize_given) fprintf(stderr, " --valsize %zu", valsize);
        if (csum_given)    fprintf(stderr, " --csum %s", csum_name);
        fprintf(stderr, "\n  (omit them and --run takes the fixtures' own)\n");
        exit(2);
    }

    nrecs = n;
    keysize = ks;
    valsize = vs;
    /* csum_name may point into argv; the stamp's copy is a local, so keep it in
     * static storage of its own. */
    {
        static char adopted[64];
        snprintf(adopted, sizeof(adopted), "%s", cs);
        csum_name = adopted;
    }
}

/* twombench spells its engines "xxh64" and "null"; the same two words select
 * F-5's engine 1 and engine 0 here, so a side-by-side run configures both
 * tools identically. */
static uint32_t csum_flag(void)
{
    return strcmp(csum_name, "null") == 0 ? (uint32_t)ZS_CSUM_NONE
                                          : (uint32_t)ZS_CSUM_XXHASH;
}

/* D-9d's span bound seals the active file every `rollover_txns` commits, and for
 * a one-store-per-transaction load that -- not `rollover_size` -- is what caps
 * what an opener replays.  The open-cost fixtures below plot open cost AGAINST
 * the number of records in the active file, so they have to pin the span bound
 * out of the way or the x-axis stops being the thing they vary.  It cost this
 * document two wrong tables: with the bound at its default the fixtures sealed
 * mid-load, and every "plain open" figure was of a 670-record tail rather than
 * of the file named in the row.  Zero leaves the library default. */
static size_t rollover_txns_override;

static struct zs_db *open_at(const char *dir, uint32_t flags, size_t rollover)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    flags |= csum_flag();
    setup.flags = flags;
    setup.rollover_size = rollover;
    setup.rollover_txns = rollover_txns_override;
    setup.error = quiet_error;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) {
        fprintf(stderr, "zsbench: cannot open %s\n", dir);
        exit(1);
    }
    return db;
}

/* Building a FIXTURE for a read-only workload.
 *
 * ZS_NOSYNC, deliberately, and it does not change what is measured: it skips
 * C-7's two commit gates and nothing else (C-6b), so the spans, terminators
 * and records are byte-identical -- still one span per record, which is the
 * fragmented shape these workloads exist to scan.  Only the durability of
 * writing the fixture differs, and nobody is measuring that.
 *
 * Worth a great deal on real storage.  A 500k-record scan fixture on a
 * network-backed mount took 435 seconds to build and 0.25 seconds to scan, so
 * `perf stat` over the whole process reported the fsync storm and not the merge
 * loop at all.  A benchmark you cannot profile is most of the way to a
 * benchmark you cannot trust. */
#define FIXTURE_FLAGS (ZS_CREATE | ZS_NOSYNC)


/* The same, with the pointer table cache enabled (spec section 8). */
static struct zs_db *open_cached(const char *dir, uint32_t flags, size_t rollover,
                                 const char *idxdir, size_t threshold)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    flags |= csum_flag();
    setup.flags = flags;
    setup.rollover_size = rollover;
    setup.rollover_txns = rollover_txns_override;
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

/* One line per fixture built, so --setup reports what a later --run can time. */
static void setup_note(const char *dir)
{
    int files = count_files(dir);
    printf("setup: %-40s %d records, %d file%s\n", dir, nrecs, files,
           files == 1 ? "" : "s");
    fflush(stdout);
}

/* ------------------------------------------------------------------------- */

/* Mutating: a fresh database per repetition, or the second one measures
 * overwrites of the first's records rather than inserts. */
static void bench_sequential_store(void)
{
    UNSPLITTABLE();
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
            char k[ZSB_KEYMAX];
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
    UNSPLITTABLE();
    /* C-7b: the two durability gates are paid per TRANSACTION, not per record, so a
     * caller that batches amortises them.  This is the measurement that shows by
     * how much, and it is the reason zs_txn_* exists alongside zs_db_*. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    /* nrecs means "all of them in one transaction", which is the shape twom's
     * fillseq uses.  Without it the closest zsbench figure commits every 1000,
     * so a side-by-side bulk-load row compared batching policy rather than the
     * two libraries -- 20 commits and 40 fdatasyncs against one and two. */
    for (long long per = 10; per <= (long long)nrecs * 10; per *= 10) {
        char label[64], slug[64];
        struct reptimes rt = { {0}, 0 };
        if (per > nrecs) per = nrecs;
        snprintf(label, sizeof(label), per == nrecs ? "store, all in one txn"
                                                    : "store, %lld per txn", per);
        if (!selected(label)) { if (per == nrecs) break; else continue; }

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
                    char k[ZSB_KEYMAX];
                    makekey(k, sizeof(k), done);
                    zs_txn_store(txn, k, strlen(k), val, valsize, 0);
                }
                zs_txn_commit(&txn);
            }
            rep_add(&rt, now() - t0);
            zs_db_close(&db);
            cleanup(dir);
        }

        if (per == nrecs) snprintf(slug, sizeof(slug), "store_all_per_txn");
        else              snprintf(slug, sizeof(slug), "store_%lld_per_txn", per);
        record(slug, label, (size_t)nrecs, &rt, "");
        if (per == nrecs) break;    /* the sweep ends at "all" */
    }
    free(val);
}

/* The same bulk load, with the keys arriving in a different ORDER.
 *
 * `store, all in one txn` above stores ascending keys, and so does every other
 * store workload here -- which is the best case for a transaction's pending
 * array and hides its cost completely.  zsi_pend_set keeps the pending records
 * in one sorted array and splices each new key in with a memmove: an ascending
 * key always lands at the end and moves nothing, a random one moves half the
 * array.  So the cost is quadratic in the transaction's size, and no figure this
 * tool produced could show it.  Measured through zstool before this line
 * existed, one transaction, 100-byte values:
 *
 *      n        ascending   random
 *      25 000   0.01s       0.08s
 *      50 000   0.02s       0.31s
 *      100 000  0.06s       1.22s
 *      200 000  0.19s       4.99s      <- four times the time for twice the keys
 *
 * Read against `store, all in one txn`, which is this same loop over the same
 * keys in ascending order.  The gap between them IS the pending array's
 * insertion cost, and it should shrink to nothing if that array ever gets the
 * base+delta treatment zsi_index has (see ZSI_DELTA_MAX, whose comment describes
 * this exact bug for the read side).
 *
 * A caller cannot dodge it by sorting first: the point of a transaction is that
 * the caller writes what it has when it has it, and cyrusdb's callers do. */
static void bench_random_store(void)
{
    UNSPLITTABLE();
    char dir[1200], note[96];
    char *val = malloc(valsize);
    struct reptimes rt = { {0}, 0 };
    memset(val, 'v', valsize);

    if (!selected("store, all in one txn, random")) { free(val); return; }

    for (int r = 0; r < reps; r++) {
        snprintf(dir, sizeof(dir), "%s/randstore", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 0);

        double t0 = now();
        struct zs_txn *txn = NULL;
        if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);
        for (int i = 0; i < nrecs; i++) {
            char k[ZSB_KEYMAX];
            makekey(k, sizeof(k), (long)strided(i));
            zs_txn_store(txn, k, strlen(k), val, valsize, 0);
        }
        /* Committed inside the timed region, as the ascending line is, or the
         * two would differ in more than the key order. */
        zs_txn_commit(&txn);
        rep_add(&rt, now() - t0);

        zs_db_close(&db);
        cleanup(dir);
    }

    snprintf(note, sizeof(note), "vs `store, all in one txn`");
    record("store_all_random", "store, all in one txn, random", (size_t)nrecs,
           &rt, note);
    free(val);
}

/* Read-after-write inside one transaction: store a record, then read it back,
 * for every record.  This is what a layered consumer does -- SQLite's btree
 * reads before nearly every insert, for rowid allocation, uniqueness and the
 * overwrite probe -- and it is NOT what a store-only loop measures.
 *
 * The difference is the whole of A-4b.  A read of a record this transaction
 * just stored has to see bytes that are still in the writer's chunk buffer, so
 * without ZS_EPHEMERAL it forces a write(2) and the buffering is defeated: one
 * write per record instead of one per 64KB.  With the flag it is answered out
 * of the buffer and costs nothing.
 *
 * It exists because its absence produced a wrong answer.  Asked in August 2026
 * whether the library wrote through per record on a bulk load, this tool could
 * only say "no" -- every store workload here writes without ever reading, so
 * none of them can see the flush.  The consumer's profile showed 300k write()
 * against 10 fdatasync over 100k rows in ONE transaction, which is this effect
 * exactly, and no zsbench figure would have predicted it. */
static void bench_read_after_write(void)
{
    UNSPLITTABLE();
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    static const struct {
        const char *slug, *label;
        int flags, probe_first;
    } passes[] = {
        /* Read back what was just stored: the read HITS a pending record, so
         * the bytes really are wanted and only A-4b's shorter lifetime can
         * avoid writing them out first. */
        { "read_after_write",           "store+read back, durable",   0, 0 },
        { "read_after_write_ephemeral", "store+read back, ephemeral",
          ZS_EPHEMERAL, 0 },
        /* Probe THEN store, which is what a btree layer does before an insert.
         * The read misses every time, and a miss is settled against the
         * pending array's key without materialising anything -- so this needs
         * no flag and should track the store-only rate. */
        { "probe_then_store",           "probe (miss)+store",         0, 1 },
    };

    for (unsigned pass = 0; pass < sizeof(passes)/sizeof(passes[0]); pass++) {
        struct reptimes rt = { {0}, 0 };

        if (!selected(passes[pass].label)) continue;

        for (int r = 0; r < reps; r++) {
            snprintf(dir, sizeof(dir), "%s/raw", workdir);
            cleanup(dir);
            struct zs_db *db = open_at(dir, ZS_CREATE, 0);
            struct zs_txn *txn = NULL;
            if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);

            double t0 = now();
            for (int i = 0; i < nrecs; i++) {
                char k[ZSB_KEYMAX];
                const char *v;
                size_t vl;
                makekey(k, sizeof(k), i);
                if (passes[pass].probe_first) {
                    zs_txn_fetch(txn, k, strlen(k), NULL, NULL, &v, &vl,
                                 passes[pass].flags);
                    zs_txn_store(txn, k, strlen(k), val, valsize, 0);
                } else {
                    zs_txn_store(txn, k, strlen(k), val, valsize, 0);
                    zs_txn_fetch(txn, k, strlen(k), NULL, NULL, &v, &vl,
                                 passes[pass].flags);
                }
            }
            /* Committed inside the timed region, because the store rows above
             * are: aborting instead excluded C-7's two gates and made this
             * workload look FASTER than the plain store it contains. */
            zs_txn_commit(&txn);
            rep_add(&rt, now() - t0);

            zs_db_close(&db);
            cleanup(dir);
        }

        record(passes[pass].slug, passes[pass].label, (size_t)nrecs, &rt, "");
    }
    free(val);
}

/* Store into a database that ALREADY HOLDS FILES.
 *
 * Every other store workload here begins in an empty directory, where a store
 * touches nothing but the active file.  With files present it costs more, and
 * not for the reason a reader would guess: before appending anything, a store
 * must decide the record's ancestor (F-16, F-17), and zsi_ancestor_for resolves
 * that by searching the file set newest to oldest for the key.  So every store
 * carries a point lookup, whether or not the caller ever reads.
 *
 * The two lines bound it.  An UPDATE stops at the first file holding the key,
 * so it pays a partial search but materialises a record when it lands.  A
 * CREATE is absent everywhere, so it searches every file to the bottom with no
 * early exit and materialises nothing -- and that is the shape that matters,
 * because loading new keys into a database that already holds data is what a
 * bulk load IS.
 *
 * Read against `store, all in one txn`, which is this same loop over an empty
 * directory: same -n, same single transaction, no files to search.  The gap
 * between them is what the ancestor costs.
 *
 * Rebuilt per repetition rather than reused, because storing mutates what it
 * measures -- the second repetition would find the keys already present and
 * turn every create into an update.  ZS_NOAUTOREPACK for the usual reason: the
 * file count IS the independent variable here, and the cascade would merge it
 * away at the begin (D-16e). */
static void bench_store_into_files(void)
{
    UNSPLITTABLE();
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    for (int pass = 0; pass < 2; pass++) {
        char label[64], slug[64], note[96];
        struct reptimes rt = { {0}, 0 };
        int files = 0;

        for (int r = 0; r < reps; r++) {
            snprintf(dir, sizeof(dir), "%s/storefiles", workdir);
            cleanup(dir);

            /* EVEN keys only, and per-record commits -- which is what produces
             * several files.  Even/odd so that the create pass can insert keys
             * that are absent but INTERLEAVED with these.  Appending above the
             * existing range instead would measure the wrong thing entirely:
             * D-14d's end probe rejects an out-of-range key in two comparisons,
             * so a miss above the top is the cheapest lookup the format has,
             * while the miss a bulk load actually pays is a full binary search
             * of every file.  Measured that way round, `create` came out
             * FASTER than `update`, which is how the mistake surfaced.
             *
             * FIXTURE_FLAGS because this loop is SETUP -- it builds the
             * database that is then stored into.  The timed region below opens
             * separately and keeps its durability gates, which is what the
             * workload measures. */
            struct zs_db *db = open_at(dir, FIXTURE_FLAGS, 0);
            for (int i = 0; i < nrecs; i++) {
                char k[ZSB_KEYMAX];
                makekey(k, sizeof(k), (long)i * 2);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            zs_db_close(&db);

            db = open_at(dir, ZS_NOAUTOREPACK, 0);
            files = count_files(dir);

            /* The label carries the file count, so the filter cannot be applied
             * until a database exists to count -- the same order bench_fetch
             * uses, and the same accepted cost: one build before a filtered-out
             * workload discovers it was filtered out. */
            if (r == 0) {
                snprintf(label, sizeof(label), "store into %d files (%s)",
                         files, pass ? "create" : "update");
                snprintf(slug, sizeof(slug), "store_into_files_%s",
                         pass ? "create" : "update");
                if (!selected(label)) {
                    zs_db_close(&db);
                    cleanup(dir);
                    break;
                }
            }

            struct zs_txn *txn = NULL;
            if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);

            double t0 = now();
            for (int i = 0; i < nrecs; i++) {
                char k[ZSB_KEYMAX];
                /* update: the even keys, all present.  create: the odd ones,
                 * all absent and interleaved among them. */
                makekey(k, sizeof(k), (long)i * 2 + (pass ? 1 : 0));
                zs_txn_store(txn, k, strlen(k), val, valsize, 0);
            }
            /* Committed inside the timed region for the same reason
             * bench_read_after_write gives: aborting would drop C-7's two gates
             * and make this look faster than the plain store it contains. */
            zs_txn_commit(&txn);
            rep_add(&rt, now() - t0);

            zs_db_close(&db);
            cleanup(dir);
        }

        if (!rt.n) continue;                    /* filtered out above */
        snprintf(note, sizeof(note), "%d files searched", files);
        record(slug, label, (size_t)nrecs, &rt, note);
    }

    free(val);
}

/* Before and after a repack, because the cost of a point lookup is proportional
 * to the NUMBER OF FILES (D-14d) -- which is the whole reason the repack policy
 * exists.
 *
 * Two directories rather than one repacked in place.  The repack is setup either
 * way -- it mutates, so it was always outside the timed loop -- but done in place
 * it CONSUMES the fragmented fixture, which only works while one process runs
 * both passes in order.  Copying costs a fraction of nrecs commits. */
static void fetch_dirs(char *plain, char *repacked, size_t sz)
{
    snprintf(plain, sz, "%s/fetch", workdir);
    snprintf(repacked, sz, "%s/fetch-repacked", workdir);
}

static void fetch_setup(void)
{
    char plain[1200], repacked[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);
    fetch_dirs(plain, repacked, sizeof(plain));

    cleanup(plain);
    struct zs_db *db = open_at(plain, FIXTURE_FLAGS, 0);
    for (int i = 0; i < nrecs; i++) {
        char k[ZSB_KEYMAX];
        makekey(k, sizeof(k), i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);
    free(val);

    copydir(plain, repacked);
    db = open_at(repacked, 0, 0);
    while (zs_db_should_repack(db)) zs_db_repack(db);
    zs_db_close(&db);

    if (phase == PHASE_SETUP) { setup_note(plain); setup_note(repacked); }
}

static void bench_fetch(void)
{
    char dirs[2][1200];
    fetch_dirs(dirs[0], dirs[1], sizeof(dirs[0]));

    if (phase != PHASE_RUN) fetch_setup();
    if (phase == PHASE_SETUP) return;

    /* Read-only, so the repetitions reuse the fixture. */
    for (int pass = 0; pass < 2; pass++) {
        const char *dir = dirs[pass];
        char label[64], slug[64];
        int files, hits = 0;
        struct reptimes rt = { {0}, 0 };
        struct zs_db *db;

        fixture_require(dir);
        db = open_at(dir, 0, 0);

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
                char k[ZSB_KEYMAX];
                const char *v;
                size_t vl;
                makekey(k, sizeof(k), (i * 7919LL) % nrecs);
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

    fixture_done(dirs[0]);
    fixture_done(dirs[1]);
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

/* Two lines, because per-record stores build the most fragmented file the format
 * can produce -- one span per record -- and a scan of that is the write-heavy
 * worst case, not the steady state.  The compacted line is what long-lived data
 * reads like.
 *
 * The compaction has its own directory for the reason fetch_setup gives, and
 * more sharply: compacting in place turned the fragmented fixture into the
 * compacted one, so a second invocation timing `full scan` would have measured
 * the compacted database under the fragmented line's name -- the merge overhead
 * this pair exists to isolate, reported as zero. */
static void scan_dirs(char *plain, char *compacted, size_t sz)
{
    snprintf(plain, sz, "%s/scan", workdir);
    snprintf(compacted, sz, "%s/scan-compacted", workdir);
}

static void scan_setup(void)
{
    char plain[1200], compacted[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);
    scan_dirs(plain, compacted, sizeof(plain));

    cleanup(plain);
    struct zs_db *db = open_at(plain, FIXTURE_FLAGS, 0);
    for (int i = 0; i < nrecs; i++) {
        char k[ZSB_KEYMAX];
        makekey(k, sizeof(k), i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);
    free(val);

    copydir(plain, compacted);
    db = open_at(compacted, 0, 0);
    zs_db_compact(db);
    zs_db_close(&db);

    if (phase == PHASE_SETUP) { setup_note(plain); setup_note(compacted); }
}

static void bench_scan(void)
{
    char plain[1200], compacted[1200];
    scan_dirs(plain, compacted, sizeof(plain));

    if (phase != PHASE_RUN) scan_setup();
    if (phase == PHASE_SETUP) return;

    /* No filter check here: the two labels differ, and bench_scan_once applies
     * the filter per line.  A filter of "compacted" selects the second line
     * only, and testing either label out here would drop it. */
    fixture_require(plain);
    struct zs_db *db = open_at(plain, 0, 0);
    bench_scan_once(db, "scan", "full scan");
    zs_db_close(&db);

    fixture_require(compacted);
    db = open_at(compacted, 0, 0);
    bench_scan_once(db, "scan_compacted", "full scan, compacted");
    zs_db_close(&db);

    /* There is no "full scan, no verify" row any more.  It priced ZS_NOCSUM
     * against a verifying scan, and since version 4 no record carries a checksum
     * (F-32), so a scan verifies nothing and the flag is rejected outright
     * (A-18).  The row would have measured two identical configurations, and its
     * open now fails.
     *
     * What it measured is worth remembering rather than the row: verification was
     * the largest single item in a scan profile, and larger than the profile
     * suggested, because a key-only traversal never dereferences the values a
     * cursor hands back -- so verifying pulled every value byte into cache that
     * the scan would otherwise never read.  The cost was memory traffic, not
     * hashing, which is why removing it is worth more than a faster hash would
     * have been. */

    fixture_done(plain);
    fixture_done(compacted);
}

/* Scans where the merge actually MERGES.
 *
 * `full scan` above does not, and it took a profile to notice.  Its keys are
 * stored in ascending order, so every generation -- and every repack output
 * built from them -- holds a contiguous key range no other file touches:
 *
 *     gen 00000001-00000080  131072 recs  key00000000 .. key00131071
 *     gen 00000081-000000C0   65536 recs  key00131072 .. key00196607
 *     gen 000000C1-000000C2    2048 recs  key00196608 .. key00198655
 *     gen 000000C3-000000C3    1024 recs  key00198656 .. key00199679
 *
 * So one arm is live and the rest sit on first keys above everything being
 * yielded.  D-14e's re-sort declines to move on every step but the three file
 * boundaries, and its duplicate scan breaks on the first comparison every time.
 * The line measures decode and checksums with three idle arms; the ~30% it
 * gives up against the compacted line is arm-array stride and one extra
 * comparison per record, not merge work.  Anyone tuning zsi_cur_resort_head
 * against it would be tuning against a workload that never calls the expensive
 * half.
 *
 * These two lines are that workload, and they differ from `full scan` in the
 * ORDER OF THE STORES and nothing else:
 *
 *   interleaved  keys stored in a strided permutation, so each generation is
 *                scattered across the whole key range and every file overlaps
 *                every other.  The winning arm changes at nearly every step, so
 *                the re-sort lifts a 160-byte arm and shifts the array.
 *   shadowed     every key stored TWICE, in two files that no cascade merges.
 *                Each step finds the key duplicated, so D-14e step 3 advances
 *                the stale arm as well and the step ends in a full
 *                zsi_cur_sort rather than the incremental resort.
 */
static void scanmerge_dirs(char *inter, char *shadow, size_t sz)
{
    snprintf(inter, sz, "%s/scan-interleaved", workdir);
    snprintf(shadow, sz, "%s/scan-shadowed", workdir);
}

static void scanmerge_setup(void)
{
    char inter[1200], shadow[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);
    scanmerge_dirs(inter, shadow, sizeof(inter));

    /* Per-record commits and the cascade left on, exactly as scan_setup does,
     * so this line and `full scan` differ in the key order and nothing else. */
    cleanup(inter);
    struct zs_db *db = open_at(inter, FIXTURE_FLAGS, 0);
    for (int i = 0; i < nrecs; i++) {
        char k[ZSB_KEYMAX];
        makekey(k, sizeof(k), (long)strided(i));
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);

    /* One transaction per pass, because a commit that crosses rollover_size
     * seals the generation itself (D-25d) -- so each pass lands as ONE in-order
     * file holding every key.  Per-record commits could not: rollover_txns
     * bounds a generation at 1024 spans as well as at rollover_size, which is
     * what cuts `full scan`'s fixture into 1024-record generations.
     * ZS_NOAUTOREPACK so the cascade does not merge the two passes back
     * together, which would collapse exactly the duplicates being measured. */
    cleanup(shadow);
    db = open_at(shadow, FIXTURE_FLAGS | ZS_NOAUTOREPACK, 0);
    for (int pass = 0; pass < 2; pass++) {
        struct zs_txn *txn = NULL;
        if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);
        for (int i = 0; i < nrecs; i++) {
            char k[ZSB_KEYMAX];
            makekey(k, sizeof(k), (long)strided(i));
            zs_txn_store(txn, k, strlen(k), val, valsize, 0);
        }
        if (zs_txn_commit(&txn) != ZS_OK) exit(1);
        zs_db_seal(db);         /* whatever D-25d left behind */
    }
    zs_db_close(&db);

    free(val);
    if (phase == PHASE_SETUP) { setup_note(inter); setup_note(shadow); }
}

static void bench_scan_merge(void)
{
    char inter[1200], shadow[1200];
    scanmerge_dirs(inter, shadow, sizeof(inter));

    if (phase != PHASE_RUN) scanmerge_setup();
    if (phase == PHASE_SETUP) return;

    fixture_require(inter);
    struct zs_db *db = open_at(inter, ZS_NOAUTOREPACK, 0);
    bench_scan_once(db, "scan_interleaved", "full scan, interleaved");
    zs_db_close(&db);

    fixture_require(shadow);
    db = open_at(shadow, ZS_NOAUTOREPACK, 0);
    bench_scan_once(db, "scan_shadowed", "full scan, shadowed");
    zs_db_close(&db);

    fixture_done(inter);
    fixture_done(shadow);
}

/* Traverse from INSIDE a write transaction, which every other read workload
 * here misses.
 *
 * bench_scan reads through a handle, so the merge is files only.  A traversal
 * on an open write transaction adds the transaction's own arm, and that arm is
 * a different machine from a file cursor: its position is a KEY rather than an
 * index (D-14j-a), so each step re-resolves it by binary search over the
 * pending array, and each record is decoded back out of the active file through
 * the transaction's mappings rather than read straight from a pointer section.
 *
 * It exists for the same reason bench_read_after_write does, one level up.  That
 * one covers point reads inside a write transaction; nothing covered a WALK, so
 * a malloc + memcpy + free per step in the transaction arm sat unmeasured until
 * a consumer's profile happened to show it.  Both lines are wanted: the first
 * isolates the arm, the second is the cyrusdb shape -- a mostly-committed
 * database walked by a transaction that is also writing -- where the arm's cost
 * is paid on top of a normal merge rather than instead of it. */
static void bench_txn_scan_once(struct zs_txn *txn, const char *slug,
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
        if (zs_txn_begin_cursor(txn, NULL, 0, &c, 0) == ZS_OK) {
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

static void bench_txn_scan(void)
{
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    /* All pending: nothing is committed, so the transaction arm is the whole
     * source and the line is the arm's per-step cost with nothing else in it.
     *
     * This half cannot be split.  Its setup is nrecs records pending in a LIVE
     * transaction, which is process state rather than anything on disk, so there
     * is nothing for --setup to leave behind. */
    if (phase == PHASE_ALL && selected("scan in a write txn")) {
        snprintf(dir, sizeof(dir), "%s/txnscan", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 0);
        struct zs_txn *txn = NULL;
        if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);

        for (int i = 0; i < nrecs; i++) {
            char k[ZSB_KEYMAX];
            makekey(k, sizeof(k), i);
            zs_txn_store(txn, k, strlen(k), val, valsize, 0);
        }

        bench_txn_scan_once(txn, "txn_scan", "scan in a write txn");

        /* Aborted, not committed: the commit is not what is being measured, and
         * it is outside the timed region either way. */
        zs_txn_abort(&txn);
        zs_db_close(&db);
        cleanup(dir);
    }

    /* Mostly committed, a few pending: the arm joins a real merge.  The
     * overwrites are spread across the key range so the arm is consulted
     * throughout the walk rather than being exhausted early.
     *
     * This half splits PARTLY: the committed database is a fixture, and the
     * handful of pending overwrites (nrecs/16) are re-done in the run phase,
     * because a live transaction cannot cross a process boundary.  Built with
     * FIXTURE_FLAGS, since nothing here measures the writing of it.
     *
     * PHASE_SETUP ignores the filter, for the reason main() gives. */
    if (phase == PHASE_SETUP || selected("scan in a write txn, few pending")) {
        snprintf(dir, sizeof(dir), "%s/txnscanmix", workdir);

        if (phase != PHASE_RUN) {
            cleanup(dir);
            struct zs_db *fx = open_at(dir, FIXTURE_FLAGS, 0);
            for (int i = 0; i < nrecs; i++) {
                char k[ZSB_KEYMAX];
                makekey(k, sizeof(k), i);
                zs_db_store(fx, k, strlen(k), val, valsize, 0);
            }
            zs_db_close(&fx);
            if (phase == PHASE_SETUP) setup_note(dir);
        }
        if (phase == PHASE_SETUP) { free(val); return; }

        fixture_require(dir);
        struct zs_db *db = open_at(dir, 0, 0);

        struct zs_txn *txn = NULL;
        if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);
        int step = nrecs / 16 > 0 ? nrecs / 16 : 1;
        for (int i = 0; i < nrecs; i += step) {
            char k[ZSB_KEYMAX];
            makekey(k, sizeof(k), i);
            zs_txn_store(txn, k, strlen(k), val, valsize, 0);
        }

        bench_txn_scan_once(txn, "txn_scan_mixed",
                            "scan in a write txn, few pending");

        zs_txn_abort(&txn);
        zs_db_close(&db);
        fixture_done(dir);
    }

    free(val);
}

static void bench_rollover_and_convert(void)
{
    UNSPLITTABLE();
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
                char k[ZSB_KEYMAX];
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
    UNSPLITTABLE();
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
        /* ZS_NOAUTOREPACK, or this benchmark measures NOTHING.  Since D-16e a
         * writer runs the cascade itself at a begin that starts a generation,
         * so by the time the loop below asks, the stores have already merged
         * the files away: it reported "4 -> 4 files, 0 rounds" and a rate of
         * zero, which reads as a fast cascade rather than an absent one.  Same
         * rule as any test whose subject is a file layout (A-14). */
        struct zs_db *db = open_at(dir, ZS_CREATE | ZS_NOAUTOREPACK, 32 * 1024);

        for (int i = 0; i < nrecs; i++) {
            char k[ZSB_KEYMAX];
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

    /* ops is RECORDS, matching what twombench's repack reports (*ops = n), so
     * the two are joinable: both operations rewrite the whole dataset, and
     * records-rewritten-per-second is the unit they share.  Counting files
     * merged away instead -- which this did first -- put records/sec beside
     * files/sec in the comparison and produced a ratio 900x out.  The file
     * counts stay in the note, where they describe rather than divide. */
    (void)merged;
    record("repack", "repack cascade", (size_t)nrecs, &rt, note);
    free(val);
}

static void bench_snapshot_open(void)
{
    UNSPLITTABLE();
    if (!selected("snapshot open")) return;

    /* THE NUMBER OPEN ITEM 2 ASKS FOR.
     *
     * Every snapshot replays the active file to build its private index (D-13d).
     * That is bounded by rollover_size but paid per open, and the spec says the way
     * to share it -- an append-only (key, offset) log per file -- should not be
     * built "before there is a number".  This is the number.
     *
     * Measured as open cost against the number of records in the active file, at a
     * rollover large enough that nothing converts during the run -- which since
     * D-9d means pinning the SPAN bound too, not only the byte one. */
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    rollover_txns_override = (size_t)1 << 40;

    printf("\n  snapshot open cost vs active-file size (open item 2)\n");
    printf("    %-12s %-12s %-12s %s\n",
           "records", "active KB", "open ms", "us/record");

    for (int n = 250; n <= 16000; n *= 2) {
        snprintf(dir, sizeof(dir), "%s/snap", workdir);
        cleanup(dir);

        /* A rollover big enough that everything stays in one active file. */
        struct zs_db *db = open_at(dir, ZS_CREATE, 512 * 1024 * 1024);
        for (int i = 0; i < n; i++) {
            char k[ZSB_KEYMAX];
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
    rollover_txns_override = 0;
    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_cached_open(void)
{
    UNSPLITTABLE();
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
     * fault the entire active file in.
     *
     * The span bound is pinned for the same reason as in `snapshot open`, and
     * here it is what the whole table is FOR: at the default it seals mid-load,
     * so both columns measure a small tail and the speedup reads as 1.0x for
     * reasons that have nothing to do with the cache.  The second table below
     * measures exactly that effect, on purpose, at the default. */
    char dir[1200], idx[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    rollover_txns_override = (size_t)1 << 40;

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
            char k[ZSB_KEYMAX];
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

    rollover_txns_override = 0;

    /* WHAT THE CACHE IS WORTH AT THE DEFAULTS, which is a different question and
     * has a cliff in it.
     *
     * The table above pins D-9d's span bound so that its x-axis means something.
     * Unpinned -- which is what a caller gets -- the active file is capped by
     * min(rollover_size, rollover_txns spans), so how much an opener replays is
     * set by the TRANSACTION SIZE and not by the record count: the same 16 000
     * records are one 2 MB file in few large transactions and a ~670-record tail
     * in one-store transactions, because 16 000 spans trips the 1024-span bound
     * fifteen times and each seal converts the file away.
     *
     * So the cache's open-side value is whatever is left inside that bound, and
     * for the smallest transactions D-9d has already collected it.  This is the
     * row that says which regime a caller is in; it is also the fixture whose
     * absence let this document quote a 17.7x speedup that its own default
     * configuration could not reach. */
    printf("\n  open cost vs transaction size, at the default span bound (D-9d)\n");
    printf("    %-14s %-12s %-12s %-12s %s\n",
           "records/txn", "spans", "plain ms", "cached ms", "speedup");

    for (int batch = 1; batch <= 16000; batch *= 40) {
        snprintf(dir, sizeof(dir), "%s/cachedopen", workdir);
        snprintf(idx, sizeof(idx), "%s/cachedopen-idx", workdir);
        cleanup(dir);
        cleanup(idx);
        if (mkdir(idx, 0700) && errno != EEXIST) { perror(idx); exit(1); }

        struct zs_db *db = open_cached(dir, ZS_CREATE, 512 * 1024 * 1024, idx, 1);
        struct zs_txn *txn = NULL;
        for (int i = 0; i < 16000; i++) {
            char k[ZSB_KEYMAX];
            if (i % batch == 0 && zs_db_begin_txn(db, 0, &txn) != ZS_OK) exit(1);
            makekey(k, sizeof(k), i);
            zs_txn_store(txn, k, strlen(k), val, valsize, 0);
            if ((i + 1) % batch == 0 && zs_txn_commit(&txn) != ZS_OK) exit(1);
        }
        if (txn && zs_txn_commit(&txn) != ZS_OK) exit(1);
        zs_db_close(&db);

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

        printf("    %-14d %-12d %-12.3f %-12.3f %.1fx\n",
               batch, 16000 / batch, plain * 1000.0, cached * 1000.0,
               cached > 0 ? plain / cached : 0.0);

        cleanup(dir);
        cleanup(idx);
    }

    printf("    (the cache buys an open only where the span bound has not\n"
           "     already sealed the file -- see doc/benchmarking.md)\n");

    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_index_threshold(void)
{
    UNSPLITTABLE();
    if (!selected("publish")) return;

    /* What P-13's threshold trades.
     *
     * Low: a table is republished often, so whoever publishes carries an
     * O(records) merge and an O(records) write.
     * High: publishing is rare, so the next reader replays further.
     *
     * Both costs are reported against the same workload, which is what makes the
     * default a measurement rather than a guess.
     *
     * EXPECT THE STORE COLUMN TO BE FLAT, and do not read that as the threshold
     * not working: since P-13 a commit publishes nothing, so this sole writer
     * publishes only at its own open and the threshold cannot reach its store
     * cost.  The end that still moves is the open, and the shape that pays the
     * low end is a REBUILDING one -- writers alternating across processes -- which
     * this single-process harness cannot produce.  The span bound is pinned so
     * that the threshold is exercised over a full generation rather than over the
     * ~1024-span tail D-9d would otherwise leave. */
    char dir[1200], idx[1200];
    char *val = malloc(valsize);
    static const size_t thresholds[] = { 1, 512, 4096, 32768, 262144, 1048576 };
    memset(val, 'v', valsize);

    rollover_txns_override = (size_t)1 << 40;

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
                char k[ZSB_KEYMAX];
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
                char k[ZSB_KEYMAX];
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

    rollover_txns_override = 0;
    free(val);
}

/* ------------------------------------------------------------------------- */

static void bench_compact(void)
{
    UNSPLITTABLE();
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
                char k[ZSB_KEYMAX];
                makekey(k, sizeof(k), i);
                zs_db_store(db, k, strlen(k), val, valsize, 0);
            }
            for (int i = 0; i < (long long)nrecs * deleted_pct[t] / 100; i++) {
                char k[ZSB_KEYMAX];
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
        "      --setup      build the read-only workloads' fixtures and exit\n"
        "      --run        time fixtures a --setup left behind, building none\n"
        "  -n, --records N  records per run (default %d)\n"
        "      --reps N     repetitions per benchmark, median kept (default %d)\n"
        "      --keysize N  key length in bytes (default %zu, minimum 4)\n"
        "      --valsize N  value length in bytes (default %zu)\n"
        "      --keyshape S head (default) or shared -- where a long key\n"
        "                   varies; identical at the default --keysize\n"
        "      --csum ENG   xxh64 (default) or null\n"
        "      --path DIR   working directory (default $TMPDIR)\n"
        "      --csv FILE   write results as CSV\n"
        "\n"
        "--setup and --run need the same --path, and the same -n, --keysize,\n"
        "--valsize and --csum.  They exist so a profiler sees the measurement\n"
        "and not the fixture build:\n"
        "\n"
        "  ./zsbench --path=/tmp/fix --setup scan\n"
        "  perf record -g ./zsbench --path=/tmp/fix --run scan\n",
        nrecs, reps, keysize, valsize);
    return 2;
}

int main(int argc, char **argv)
{
    int path_given = 0;
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(workdir, sizeof(workdir), "%s/zsbench.%d", tmp, (int)getpid());

    for (int i = 1; i < argc; i++) {
        /* --opt=value and --opt value are both accepted.  An unrecognised
         * spelling must be a usage error rather than a silent default: under a
         * profiler, a usage dump looks like a benchmark that completed
         * instantly rather than one that never ran. */
        char opt[64];
        const char *inl = NULL, *val = NULL;
        const char *eq = strchr(argv[i], '=');

        if (argv[i][0] == '-' && eq && (size_t)(eq - argv[i]) < sizeof(opt)) {
            size_t n = (size_t)(eq - argv[i]);
            memcpy(opt, argv[i], n);
            opt[n] = '\0';
            inl = eq + 1;
        } else {
            snprintf(opt, sizeof(opt), "%s", argv[i]);
        }

        /* Every option below that takes a value calls this exactly once, so an
         * option given without one is a usage error rather than a silent
         * default. */
        #define ARGVAL() (val = inl ? inl : (i + 1 < argc ? argv[++i] : NULL), \
                          val ? val : (const char *)NULL)

        if (!strcmp(opt, "--selftest")) selftest = 1;
        else if (!strcmp(opt, "--setup") || !strcmp(opt, "--run")) {
            enum phase want = !strcmp(opt, "--setup") ? PHASE_SETUP : PHASE_RUN;
            if (phase != PHASE_ALL && phase != want) {
                fprintf(stderr, "zsbench: --setup and --run are exclusive\n");
                return 2;
            }
            phase = want;
        }
        else if (!strcmp(opt, "-n") || !strcmp(opt, "--records")) {
            if (!ARGVAL()) return usage();
            nrecs = atoi(val);
            nrecs_given = 1;
        }
        else if (!strcmp(opt, "--reps")) {
            if (!ARGVAL()) return usage();
            reps = atoi(val);
        }
        else if (!strcmp(opt, "--keysize")) {
            if (!ARGVAL()) return usage();
            keysize = (size_t)atol(val);
            keysize_given = 1;
        }
        else if (!strcmp(opt, "--valsize") || !strcmp(opt, "--value")) {
            if (!ARGVAL()) return usage();
            valsize = (size_t)atol(val);
            valsize_given = 1;
        }
        else if (!strcmp(opt, "--keyshape")) {
            if (!ARGVAL()) return usage();
            keyshape = val;
        }
        else if (!strcmp(opt, "--csum")) {
            if (!ARGVAL()) return usage();
            csum_name = val;
            csum_given = 1;
        }
        else if (!strcmp(opt, "--path") || !strcmp(opt, "--dir")) {
            if (!ARGVAL()) return usage();
            snprintf(workdir, sizeof(workdir), "%s", val);
            path_given = 1;
        }
        else if (!strcmp(opt, "--csv")) {
            if (!ARGVAL()) return usage();
            csv_path = val;
        }
        else if (argv[i][0] != '-' && !filter) filter = argv[i];
        else return usage();

        #undef ARGVAL
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
    if (keysize > ZSB_KEYMAX - 1) {
        fprintf(stderr, "zsbench: --keysize must be at most %d\n",
                ZSB_KEYMAX - 1);
        return 2;
    }
    if (keysize < 4) {
        fprintf(stderr, "zsbench: --keysize must be at least 4\n");
        return 2;
    }
    if (strcmp(keyshape, "head") && strcmp(keyshape, "shared")) {
        fprintf(stderr, "zsbench: --keyshape must be head or shared\n");
        return 2;
    }
    if (strcmp(csum_name, "xxh64") && strcmp(csum_name, "null")) {
        fprintf(stderr, "zsbench: --csum must be xxh64 or null\n");
        return 2;
    }
    /* The default workdir carries the pid, so two invocations would never name
     * the same directory -- a phase without --path could not work. */
    if (phase != PHASE_ALL && !path_given) {
        fprintf(stderr, "zsbench: --setup and --run require --path\n");
        return 2;
    }
    if (phase != PHASE_ALL && selftest) {
        fprintf(stderr, "zsbench: --selftest takes no phase\n");
        return 2;
    }

    if (mkdir(workdir, 0700) && errno != EEXIST) {
        perror(workdir);
        return 1;
    }

    if (phase == PHASE_RUN) stamp_check();

    int rc = 0;
    if (selftest) {
        rc = run_selftest();
    } else if (phase == PHASE_SETUP) {
        printf("zsbench setup: %d records, %zu-byte keys, %zu-byte values, "
               "into %s\n", nrecs, keysize, valsize, workdir);
        /* The filter is deliberately NOT applied here, and a given one says so.
         * It belongs to the run phase, where it costs nothing to be exact; a
         * setup phase that honoured it would let the two invocations disagree
         * about which fixtures exist, and `fetch (N files)` cannot be tested
         * against a filter before the files it names have been built anyway.
         * So --setup always builds all five, and --run can rely on that. */
        if (filter)
            printf("  (--setup builds every fixture; the filter `%s' applies "
                   "to --run)\n", filter);
        bench_fetch();
        bench_scan();
        bench_scan_merge();
        bench_txn_scan();
        stamp_write();
        /* The suggestion has to be a command that WORKS.  It omitted -n at
         * first, which the stamp check then refused at every fixture size but
         * the default -- and under `perf record` a refusal costs the whole
         * session, reporting as a dozen samples of the dynamic linker rather
         * than as a benchmark that never started.  --run adopts the fixtures'
         * parameters now, so nothing here has to be repeated; --reps is
         * suggested because it does not come from the stamp and a run phase is
         * short by design: at 200k records the whole thing is 40ms, which is
         * too few samples to profile. */
        printf("\nnow: perf record -g %s --path=%s --run --reps 20%s%s\n",
               argv[0], workdir, filter ? " " : "", filter ? filter : "");
    } else {
        printf("zeroskip benchmark: %d records, %zu-byte keys, "
               "%zu-byte values, %d reps (median)\n",
               nrecs, keysize, valsize, reps);
        /* Named rather than left out, or a short report reads as a full one. */
        if (phase == PHASE_RUN)
            printf("run phase over %s: the store workloads, `scan in a write "
                   "txn`,\n  rollover, repack cascade, the open and threshold "
                   "sweeps and compaction\n  are skipped -- their setup is what "
                   "they measure.\n", workdir);
        printf("\n");
        bench_sequential_store();
        bench_batched_store();
        bench_random_store();
        bench_read_after_write();
        bench_store_into_files();
        bench_fetch();
        bench_scan();
        bench_scan_merge();
        bench_txn_scan();
        bench_rollover_and_convert();
        bench_repack_cascade();
        bench_snapshot_open();
        bench_cached_open();
        bench_index_threshold();
        bench_compact();

        if (csv_path && csv_write(csv_path) != 0) rc = 1;
    }

    /* A phase leaves its work behind: the fixtures are the point of --setup, and
     * --run wants them still there for the next profiling pass. */
    if (phase == PHASE_ALL) cleanup(workdir);
    return rc;
}
