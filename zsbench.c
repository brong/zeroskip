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

static int nrecs = 20000;
static int reps = 3;
static size_t valsize = 100;
static int selftest = 0;
static char workdir[1024];

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

static struct zs_db *open_at(const char *dir, uint32_t flags, size_t rollover)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;

    setup.flags = flags;
    setup.rollover_size = rollover;
    setup.error = quiet_error;
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

static void bench_sequential_store(void)
{
    char dir[1200];
    char *val = malloc(valsize);
    memset(val, 'v', valsize);

    printf("  %-34s", "store, one txn each");
    fflush(stdout);

    double best = 1e18;
    for (int r = 0; r < reps; r++) {
        snprintf(dir, sizeof(dir), "%s/seq", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 0);

        double t0 = now();
        for (int i = 0; i < nrecs; i++) {
            char k[32];
            snprintf(k, sizeof(k), "key%08d", i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        double dt = now() - t0;
        if (dt < best) best = dt;

        if (r == reps - 1)
            printf("%8.0f/s  %5.2fs  %d files  %.1fx amp\n",
                   nrecs / best, best, count_files(dir),
                   (double)dir_bytes(dir) / (double)(nrecs * (valsize + 12)));
        zs_db_close(&db);
        cleanup(dir);
    }
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
        char label[64];
        snprintf(label, sizeof(label), "store, %d per txn", per);
        printf("  %-34s", label);
        fflush(stdout);

        snprintf(dir, sizeof(dir), "%s/batch", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, 0);

        double t0 = now();
        int done = 0;
        while (done < nrecs) {
            struct zs_txn *txn = NULL;
            if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) break;
            for (int i = 0; i < per && done < nrecs; i++, done++) {
                char k[32];
                snprintf(k, sizeof(k), "key%08d", done);
                zs_txn_store(txn, k, strlen(k), val, valsize, 0);
            }
            zs_txn_commit(&txn);
        }
        double dt = now() - t0;

        printf("%8.0f/s  %5.2fs\n", nrecs / dt, dt);
        zs_db_close(&db);
        cleanup(dir);
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
        char k[32];
        snprintf(k, sizeof(k), "key%08d", i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);

    /* Before and after a repack, because the cost of a point lookup is
     * proportional to the NUMBER OF FILES (D-14d) -- which is the whole reason the
     * repack policy exists. */
    for (int pass = 0; pass < 2; pass++) {
        db = open_at(dir, 0, 0);
        if (pass == 1) {
            while (zs_db_should_repack(db)) zs_db_repack(db);
        }

        int files = count_files(dir);
        char label[64];
        snprintf(label, sizeof(label), "fetch (%d files)", files);
        printf("  %-34s", label);
        fflush(stdout);

        double t0 = now();
        int hits = 0;
        for (int i = 0; i < nrecs; i++) {
            char k[32];
            const char *v;
            size_t vl;
            snprintf(k, sizeof(k), "key%08d", (i * 7919) % nrecs);
            if (zs_db_fetch(db, k, strlen(k), NULL, NULL, &v, &vl, 0) == ZS_OK)
                hits++;
        }
        double dt = now() - t0;
        printf("%8.0f/s  %5.2fs  %d hits\n", nrecs / dt, dt, hits);
        zs_db_close(&db);
    }

    cleanup(dir);
    free(val);
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
        char k[32];
        snprintf(k, sizeof(k), "key%08d", i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }
    zs_db_close(&db);

    db = open_at(dir, 0, 0);
    printf("  %-34s", "full scan");
    fflush(stdout);

    double t0 = now();
    struct zs_cursor *c = NULL;
    long seen = 0;
    if (zs_db_begin_cursor(db, NULL, 0, &c, ZS_SHARED) == ZS_OK) {
        const char *k, *v;
        size_t kl, vl;
        while (zs_cursor_next(c, &k, &kl, &v, &vl) == ZS_OK) seen++;
        zs_cursor_abort(&c);
    }
    double dt = now() - t0;
    printf("%8.0f/s  %5.2fs  %ld records\n", seen / dt, dt, seen);

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
        char label[64];
        snprintf(label, sizeof(label), "store, rollover %zuk", sizes[s] / 1024);
        printf("  %-34s", label);
        fflush(stdout);

        snprintf(dir, sizeof(dir), "%s/roll", workdir);
        cleanup(dir);
        struct zs_db *db = open_at(dir, ZS_CREATE, sizes[s]);

        double t0 = now();
        for (int i = 0; i < nrecs; i++) {
            char k[32];
            snprintf(k, sizeof(k), "key%08d", i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        double dt = now() - t0;

        printf("%8.0f/s  %5.2fs  %d files\n", nrecs / dt, dt, count_files(dir));
        zs_db_close(&db);
        cleanup(dir);
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

    snprintf(dir, sizeof(dir), "%s/cascade", workdir);
    cleanup(dir);
    struct zs_db *db = open_at(dir, ZS_CREATE, 32 * 1024);

    for (int i = 0; i < nrecs; i++) {
        char k[32];
        snprintf(k, sizeof(k), "key%08d", i);
        zs_db_store(db, k, strlen(k), val, valsize, 0);
    }

    int before = count_files(dir);
    printf("  %-34s", "repack cascade");
    fflush(stdout);

    double t0 = now();
    int rounds = 0;
    while (zs_db_should_repack(db)) {
        if (zs_db_repack(db) != ZS_OK) break;
        rounds++;
        if (rounds > 100) break;
    }
    double dt = now() - t0;

    printf("%5.2fs  %d -> %d files  %d rounds\n",
           dt, before, count_files(dir), rounds);

    zs_db_close(&db);
    cleanup(dir);
    free(val);
}

static void bench_snapshot_open(void)
{
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
            char k[32];
            snprintf(k, sizeof(k), "key%08d", i);
            zs_db_store(db, k, strlen(k), val, valsize, 0);
        }
        zs_db_close(&db);

        size_t bytes = dir_bytes(dir);

        /* Time the open alone, several times, keeping the best. */
        double best = 1e18;
        for (int r = 0; r < 5; r++) {
            double t0 = now();
            db = open_at(dir, 0, 512 * 1024 * 1024);
            double dt = now() - t0;
            zs_db_close(&db);
            if (dt < best) best = dt;
        }

        printf("    %-12d %-12.1f %-12.3f %.3f\n",
               n, (double)bytes / 1024.0, best * 1000.0,
               best * 1e6 / (double)n);

        cleanup(dir);
    }

    printf("    (linear us/record means the replay dominates; a shared index\n"
           "     would trade this scan for a sort -- see open item 2)\n");
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

static int usage(void)
{
    fprintf(stderr,
        "usage: zsbench [options]\n"
        "\n"
        "  --selftest       verify the harness, then exit\n"
        "  -n N             records per run (default %d)\n"
        "  --reps N         repetitions, best kept (default %d)\n"
        "  --value N        value size in bytes (default %zu)\n"
        "  --dir PATH       working directory (default $TMPDIR)\n",
        nrecs, reps, valsize);
    return 2;
}

int main(int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";
    snprintf(workdir, sizeof(workdir), "%s/zsbench.%d", tmp, (int)getpid());

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--selftest")) selftest = 1;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) nrecs = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--reps") && i + 1 < argc) reps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--value") && i + 1 < argc)
            valsize = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc)
            snprintf(workdir, sizeof(workdir), "%s", argv[++i]);
        else return usage();
    }

    if (mkdir(workdir, 0700) && errno != EEXIST) {
        perror(workdir);
        return 1;
    }

    int rc = 0;
    if (selftest) {
        rc = run_selftest();
    } else {
        printf("zeroskip benchmark: %d records, %zu-byte values, %d reps\n\n",
               nrecs, valsize, reps);
        bench_sequential_store();
        bench_batched_store();
        bench_fetch();
        bench_scan();
        bench_rollover_and_convert();
        bench_repack_cascade();
        bench_snapshot_open();
    }

    cleanup(workdir);
    return rc;
}
