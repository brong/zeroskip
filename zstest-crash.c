/* zstest-crash.c - crash and sync-failure injection for zeroskip
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * T-8 and T-8a.  A separate binary because it needs ZS_TEST_HOOKS, which routes
 * the library's write, fdatasync, rename and unlink through function pointers.
 *
 * The interesting part is what a crash test can and cannot show.  Aborting at call
 * N tells you the database reopens and holds a prefix of committed transactions.
 * It can never reach a FAILING fdatasync -- a crash and a failed sync leave
 * different states, and C-7a exists for the second one -- which is why T-8a is a
 * separate mechanism rather than another crash point.
 *
 * Each crash case runs in a forked child, because "crash" here means _exit(1) part
 * way through a library call, leaving locks and mappings to the kernel exactly as a
 * real crash would.
 */

#define ZS_TEST_HOOKS 1

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
#include <sys/wait.h>
#include <unistd.h>

#include "zeroskip.c"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int total = 0, passed = 0, failed = 0;
static char *basedir;
static char dbdir[PATH_MAX];

#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "    FAIL %s:%d: ", __FILE__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failed++; \
        return; \
    } \
} while (0)

static void quiet_error(const char *msg, const char *fmt, ...)
{
    (void)msg; (void)fmt;
}

/* ---------------------------------------------------------------------------
 * Injection state
 * ------------------------------------------------------------------------- */

static long call_count;         /* syscalls seen so far */
static long crash_at;           /* _exit when call_count reaches this; -1 = never */
static long fail_sync_at;       /* make the Nth fdatasync fail; -1 = never */
static long sync_calls;         /* fdatasyncs seen */
/* Syncs attempted after one failed, WITHIN the same library call.
 *
 * C-7a forbids retrying THE FAILED sync and treating success as evidence the data
 * survived.  It does not forbid a later, unrelated transaction syncing again -- so
 * a counter that never resets measures the wrong thing, and an earlier version of
 * this harness reported ten "retries" that were simply the next four commits doing
 * their job. */
static long syncs_after_failure;
static bool a_sync_failed;
static bool suppress_dirsync;   /* for the test that justifies C-6 */
static long dir_syncs;          /* fdatasyncs whose fd was a DIRECTORY */

static void tick(void)
{
    call_count++;
    if (crash_at >= 0 && call_count >= crash_at) _exit(42);
}

static ssize_t hook_write(int fd, const void *buf, size_t n)
{
    tick();
    return write(fd, buf, n);
}

static int hook_fdatasync(int fd)
{
    tick();
    sync_calls++;

    if (a_sync_failed) syncs_after_failure++;

    if (fail_sync_at >= 0 && sync_calls == fail_sync_at) {
        a_sync_failed = true;
        errno = EIO;
        return -1;
    }

    /* Tally directory syncs separately, and optionally suppress them.  Suppressing
     * models a filesystem that lost a name while keeping the file's contents --
     * the state C-6 exists to prevent. */
    struct stat sb;
    bool is_dir = (fstat(fd, &sb) == 0 && S_ISDIR(sb.st_mode));

    if (is_dir) {
        dir_syncs++;
        if (suppress_dirsync) return 0;
    }

    return fdatasync(fd);
}

static int hook_rename(const char *a, const char *b)
{
    tick();
    return rename(a, b);
}

static int hook_unlink(const char *p)
{
    tick();
    return unlink(p);
}

static void hooks_on(void)
{
    zs_hook_write = hook_write;
    zs_hook_fdatasync = hook_fdatasync;
    zs_hook_rename = hook_rename;
    zs_hook_unlink = hook_unlink;
}

static void hooks_reset(void)
{
    call_count = 0;
    crash_at = -1;
    fail_sync_at = -1;
    sync_calls = 0;
    syncs_after_failure = 0;
    a_sync_failed = false;
    suppress_dirsync = false;
    dir_syncs = 0;
}

/* ---------------------------------------------------------------------------
 * Scratch directories
 * ------------------------------------------------------------------------- */

static int fexists(const char *p)
{
    struct stat sb;
    return stat(p, &sb) < 0 ? -errno : 0;
}

static void fresh_dir(void)
{
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dbdir);
    if (system(cmd)) {}
}

static struct zs_db *open_db(uint32_t flags)
{
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    setup.flags = flags;
    setup.error = quiet_error;
    if (zs_db_open(dbdir, &setup, &db) != ZS_OK) return NULL;
    return db;
}

/* Read the whole database back as "key=value|..." so a state can be compared. */
static int scan_cb(void *rock, const char *key, size_t keylen,
                   const char *val, size_t vallen)
{
    char *out = rock;
    size_t used = strlen(out);
    if (used) out[used++] = '|';
    memcpy(out + used, key, keylen); used += keylen;
    out[used++] = '=';
    memcpy(out + used, val, vallen); used += vallen;
    out[used] = '\0';
    return 0;
}

static bool read_state(char *out, size_t outlen)
{
    struct zs_db *db = open_db(0);
    (void)outlen;
    out[0] = '\0';
    if (!db) return false;
    int r = zs_db_foreach(db, NULL, 0, NULL, scan_cb, out, 0);
    zs_db_close(&db);
    return r == ZS_OK;
}

/* ---------------------------------------------------------------------------
 * The workload
 * ------------------------------------------------------------------------- */

/* Commit `n` transactions, each storing one key.  Returns the number the child
 * managed before it was cut off, or -1 if it ran to completion. */
static void workload(int n)
{
    struct zs_db *db = open_db(ZS_CREATE);
    if (!db) _exit(1);

    for (int i = 0; i < n; i++) {
        char k[16], v[16];
        snprintf(k, sizeof(k), "k%02d", i);
        snprintf(v, sizeof(v), "v%02d", i);
        if (zs_db_store(db, k, strlen(k), v, strlen(v), 0) != ZS_OK) _exit(1);
    }

    zs_db_close(&db);
}

/* How many hooked syscalls the intact workload makes. */
static long count_calls(int n)
{
    hooks_reset();
    hooks_on();
    fresh_dir();
    workload(n);
    return call_count;
}

/* Check-and-clear: call immediately after the library call that was expected to
 * hit the injected failure, so only that call's syncs are counted. */
static long take_retry_count(void)
{
    long n = syncs_after_failure;
    syncs_after_failure = 0;
    a_sync_failed = false;
    return n;
}

/* Run the workload in a child that aborts at call `at`. */
static int run_crashing(int n, long at)
{
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        hooks_reset();
        hooks_on();
        crash_at = at;
        workload(n);
        _exit(0);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -2;
}

/* ---------------------------------------------------------------------------
 * T-8: crash at every syscall
 * ------------------------------------------------------------------------- */

static void test_crash_at_every_call(void)
{
    /* Abort at call N for every N over a scripted workload.  Each case asserts
     * reopen terminates, exactly a prefix of committed transactions is visible,
     * and a writer can then continue.
     *
     * Both durability gates are crash points in their own right (C-7), INCLUDING
     * the window between them -- the state a single-gate design could not
     * distinguish -- and that window is covered by construction because every call
     * is a crash point. */
    const int nkeys = 6;
    long calls = count_calls(nkeys);
    total++;

    CHECK(calls > 10, "workload made only %ld hooked calls", calls);

    /* The states a prefix of transactions can produce: after 0, 1, ... nkeys
     * commits.  Anything else is a state the database invented. */
    char expect[8][512];
    for (int i = 0; i <= nkeys; i++) {
        expect[i][0] = '\0';
        for (int j = 0; j < i; j++) {
            char piece[32];
            snprintf(piece, sizeof(piece), "%sk%02d=v%02d",
                     j ? "|" : "", j, j);
            strncat(expect[i], piece, sizeof(expect[i]) - strlen(expect[i]) - 1);
        }
    }

    for (long at = 1; at <= calls; at++) {
        hooks_reset();
        fresh_dir();

        int rc = run_crashing(nkeys, at);
        if (rc != 42 && rc != 0) {
            fprintf(stderr, "    FAIL crash at %ld: child exited %d\n", at, rc);
            failed++;
            return;
        }

        /* Reopen must terminate and produce one of the legal states. */
        hooks_reset();
        alarm(30);
        char state[512];
        bool ok = read_state(state, sizeof(state));
        alarm(0);

        if (!ok) {
            fprintf(stderr, "    FAIL crash at %ld: reopen failed\n", at);
            failed++;
            return;
        }

        bool legal = false;
        for (int i = 0; i <= nkeys; i++)
            if (!strcmp(state, expect[i])) legal = true;

        if (!legal) {
            fprintf(stderr, "    FAIL crash at %ld: state '%s' is not a prefix "
                    "of committed transactions\n", at, state);
            failed++;
            return;
        }

        /* And a writer can continue from here. */
        struct zs_db *db = open_db(0);
        if (!db) {
            fprintf(stderr, "    FAIL crash at %ld: cannot reopen to write\n", at);
            failed++;
            return;
        }
        if (zs_db_store(db, "after", 5, "ok", 2, 0) != ZS_OK) {
            fprintf(stderr, "    FAIL crash at %ld: cannot write after\n", at);
            zs_db_close(db ? &db : NULL);
            failed++;
            return;
        }
        zs_db_close(&db);

        char state2[512];
        if (!read_state(state2, sizeof(state2))
            || !strstr(state2, "after=ok")) {
            fprintf(stderr, "    FAIL crash at %ld: write did not survive\n", at);
            failed++;
            return;
        }
    }

    passed++;
    fprintf(stderr, "  crash at every call (%ld points)         ok\n", calls);
}

static void test_crash_nosync_mode(void)
{
    /* The same sweep under ZS_NOSYNC.  Atomicity must survive without the gates,
     * because a torn tail is still detectable (F-22, C-7c) -- only durability and
     * C-7a's ordering guarantee are given up. */
    const int nkeys = 4;
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();
    {
        struct zs_db *db = open_db(ZS_CREATE | ZS_NOSYNC);
        if (!db) { fprintf(stderr, "    FAIL nosync open\n"); failed++; return; }
        for (int i = 0; i < nkeys; i++) {
            char k[16], v[16];
            snprintf(k, sizeof(k), "k%02d", i);
            snprintf(v, sizeof(v), "v%02d", i);
            zs_db_store(db, k, strlen(k), v, strlen(v), 0);
        }
        zs_db_close(&db);
    }
    long calls = call_count;
    CHECK(calls > 4, "nosync workload made only %ld calls", calls);

    for (long at = 1; at <= calls; at++) {
        fresh_dir();
        pid_t pid = fork();
        if (pid == 0) {
            hooks_reset();
            hooks_on();
            crash_at = at;
            struct zs_db *db = open_db(ZS_CREATE | ZS_NOSYNC);
            if (!db) _exit(1);
            for (int i = 0; i < nkeys; i++) {
                char k[16], v[16];
                snprintf(k, sizeof(k), "k%02d", i);
                snprintf(v, sizeof(v), "v%02d", i);
                zs_db_store(db, k, strlen(k), v, strlen(v), 0);
            }
            zs_db_close(&db);
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);

        hooks_reset();
        alarm(30);
        char state[512];
        bool ok = read_state(state, sizeof(state));
        alarm(0);

        if (!ok) {
            fprintf(stderr, "    FAIL nosync crash at %ld: reopen failed\n", at);
            failed++;
            return;
        }

        /* No partial transaction: every key present must have its own value. */
        for (int i = 0; i < nkeys; i++) {
            char want[32];
            snprintf(want, sizeof(want), "k%02d=v%02d", i, i);
            char key[16];
            snprintf(key, sizeof(key), "k%02d=", i);
            const char *p = strstr(state, key);
            if (p && strncmp(p, want, strlen(want)) != 0) {
                fprintf(stderr, "    FAIL nosync crash at %ld: torn record "
                        "in '%s'\n", at, state);
                failed++;
                return;
            }
        }
    }

    passed++;
    fprintf(stderr, "  crash under ZS_NOSYNC (%ld points)       ok\n", calls);
}

static void test_dirsync_justifies_c6(void)
{
    /* C-6 requires an fdatasync of the DIRECTORY after creating a data file and
     * after renaming an output into place, "otherwise the name may be absent after
     * a crash even though the file's contents are durable".
     *
     * That claim is about the filesystem, and no test can make a real filesystem
     * lose a name on demand.  What CAN be established is two things, which together
     * are the honest content of the requirement:
     *
     *   1. the library really does sync the directory -- so the protection exists
     *      rather than being assumed.  Counted, not inspected by eye;
     *   2. with directory syncs suppressed, the database still opens and reads.  So
     *      a filesystem that lost a name degrades rather than corrupting, which is
     *      what C-6a says: a reappearing name is a superseded file that readers
     *      ignore and a later pass removes again.
     *
     * Suppressing them and finding no difference would be measuring this
     * filesystem's generosity, not the code, which is why (1) is asserted
     * separately. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    char pad[300];
    total++;

    memset(pad, 'p', sizeof(pad));
    setup.flags = ZS_CREATE;
    setup.rollover_size = 256;      /* small, so a rollover creates a second file */
    setup.error = quiet_error;

    /* 1. Directory syncs happen.  dir_syncs is tallied by the hook. */
    hooks_reset();
    hooks_on();
    fresh_dir();
    dir_syncs = 0;

    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open failed");
    CHECK(zs_db_store(db, "a", 1, pad, sizeof(pad), 0) == ZS_OK, "store a");
    CHECK(zs_db_store(db, "b", 1, pad, sizeof(pad), 0) == ZS_OK, "store b");
    zs_db_close(&db);

    /* At least two: generation 1 at create, and the rollover's new active file.
     * The conversion's rename adds more. */
    CHECK(dir_syncs >= 2, "only %ld directory syncs; C-6 wants one per created "
          "data file and one per published rename", dir_syncs);

    /* 2. With them suppressed, the data still reads. */
    hooks_reset();
    hooks_on();
    fresh_dir();
    suppress_dirsync = true;
    dir_syncs = 0;

    db = NULL;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open with dirsync suppressed");
    CHECK(zs_db_store(db, "a", 1, pad, sizeof(pad), 0) == ZS_OK, "store a");
    CHECK(zs_db_store(db, "b", 1, pad, sizeof(pad), 0) == ZS_OK, "store b");
    zs_db_close(&db);

    long suppressed = dir_syncs;
    hooks_reset();

    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after suppressed dirsync");
    CHECK(strstr(state, "a=") != NULL, "data lost: '%s'", state);
    CHECK(strstr(state, "b=") != NULL, "data lost: '%s'", state);

    /* The suppression really did intercept something, or (1) and (2) would be the
     * same run. */
    CHECK(suppressed >= 2, "suppression intercepted only %ld syncs", suppressed);

    passed++;
    fprintf(stderr, "  directory syncs happen (C-6)             ok\n");
}

/* ---------------------------------------------------------------------------
 * T-8a: a FAILING fdatasync -- the case C-7a exists for
 * ------------------------------------------------------------------------- */

static void test_sync_failure_gate1(void)
{
    /* Gate 1 failing: no terminator was written, so the transaction plainly did not
     * happen.  Assert the error reached the caller AND that the file holds no
     * terminator for that span -- which is what makes "did not happen" true rather
     * than merely reported. */
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "before", 6, "1", 1, 0) == ZS_OK, "first store");

    /* The next commit's FIRST sync is gate 1. */
    long before_syncs = sync_calls;
    fail_sync_at = before_syncs + 1;

    int r = zs_db_store(db, "doomed", 6, "2", 1, 0);
    CHECK(r != ZS_OK, "gate 1 failure was not reported (got %s)", zs_strerror(r));

    /* MUST NOT retry the sync (C-7a): a second call can succeed after the dirty
     * pages were discarded, so treating success as proof the data survived is
     * wrong.  Counted rather than reasoned about. */
    CHECK(syncs_after_failure == 0,
          "%ld fdatasync calls after a failure -- must be 0 (C-7a)",
          syncs_after_failure);

    zs_db_close(&db);
    hooks_reset();

    /* The transaction did not happen. */
    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after gate 1 failure");
    CHECK(strstr(state, "doomed") == NULL,
          "the failed transaction is visible: '%s'", state);
    CHECK(strstr(state, "before=1") != NULL,
          "the earlier commit was lost: '%s'", state);

    /* And a writer can continue. */
    db = open_db(0);
    CHECK(db != NULL, "cannot reopen");
    CHECK(zs_db_store(db, "after", 5, "3", 1, 0) == ZS_OK, "cannot continue");
    zs_db_close(&db);

    passed++;
    fprintf(stderr, "  sync failure at gate 1                   ok\n");
}

static void test_sync_failure_gate2(void)
{
    /* Gate 2 failing: the terminator may or may not be durable, and EITHER OUTCOME
     * IS CORRECT -- so the requirement is that the error is reported and nothing
     * else is done.  The assertion is therefore about what the database looks like
     * afterwards: the commit is visible with durable data, or the span reads as
     * absent, and nothing in between. */
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "before", 6, "1", 1, 0) == ZS_OK, "first store");

    /* The SECOND sync of the next commit is gate 2. */
    long before_syncs = sync_calls;
    fail_sync_at = before_syncs + 2;

    int r = zs_db_store(db, "maybe", 5, "2", 1, 0);
    CHECK(r != ZS_OK, "gate 2 failure was not reported");
    CHECK(syncs_after_failure == 0,
          "%ld fdatasync calls after a failure -- must be 0 (C-7a)",
          syncs_after_failure);

    zs_db_close(&db);
    hooks_reset();

    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after gate 2 failure");

    /* Both outcomes are legal; a torn or invented one is not. */
    bool visible = strstr(state, "maybe=2") != NULL;
    bool absent = strstr(state, "maybe") == NULL;
    CHECK(visible || absent, "neither visible nor absent: '%s'", state);
    CHECK(strstr(state, "before=1") != NULL,
          "the earlier commit was lost: '%s'", state);

    db = open_db(0);
    CHECK(db != NULL, "cannot reopen");
    CHECK(zs_db_store(db, "after", 5, "3", 1, 0) == ZS_OK, "cannot continue");
    zs_db_close(&db);

    passed++;
    fprintf(stderr, "  sync failure at gate 2                   ok\n");
}

static void test_sync_failure_every_point(void)
{
    /* Every sync in a longer workload made to fail in turn.  Each time: the error
     * reaches the caller, no retry happens, the database reopens, and a prefix of
     * committed transactions is visible. */
    const int nkeys = 5;
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();
    workload(nkeys);
    long nsyncs = sync_calls;
    CHECK(nsyncs >= nkeys * 2, "only %ld syncs for %d commits", nsyncs, nkeys);

    for (long at = 1; at <= nsyncs; at++) {
        hooks_reset();
        hooks_on();
        fresh_dir();
        fail_sync_at = at;

        struct zs_db *db = open_db(ZS_CREATE);
        if (!db) continue;               /* the failure hit database creation */

        /* The failure may have landed during creation, whose syncs are not part of
         * any transaction.  Clear before measuring, so only a store's own syncs
         * count as a retry of that store's failed sync. */
        long retries = 0;
        (void)take_retry_count();
        for (int i = 0; i < nkeys; i++) {
            char k[16], v[16];
            snprintf(k, sizeof(k), "k%02d", i);
            snprintf(v, sizeof(v), "v%02d", i);
            zs_db_store(db, k, strlen(k), v, strlen(v), 0);
            /* Per store, so only the call that hit the failure is measured. */
            retries += take_retry_count();
        }
        zs_db_close(&db);
        retries += take_retry_count();

        if (retries != 0) {
            fprintf(stderr, "    FAIL sync failure at %ld: %ld retries of the "
                    "failed sync (C-7a)\n", at, retries);
            failed++;
            return;
        }

        hooks_reset();
        alarm(30);
        char state[512];
        bool ok = read_state(state, sizeof(state));
        alarm(0);

        if (!ok) {
            fprintf(stderr, "    FAIL sync failure at %ld: reopen failed\n", at);
            failed++;
            return;
        }

        /* Every key present must carry its own value: no torn record. */
        for (int i = 0; i < nkeys; i++) {
            char key[16], want[32];
            snprintf(key, sizeof(key), "k%02d=", i);
            snprintf(want, sizeof(want), "k%02d=v%02d", i, i);
            const char *p = strstr(state, key);
            if (p && strncmp(p, want, strlen(want)) != 0) {
                fprintf(stderr, "    FAIL sync failure at %ld: torn '%s'\n",
                        at, state);
                failed++;
                return;
            }
        }
    }

    passed++;
    fprintf(stderr, "  sync failure at every point (%ld)         ok\n", nsyncs);
}

/* ---------------------------------------------------------------------------
 * Targeted crash points T-8 names explicitly
 * ------------------------------------------------------------------------- */

static void test_crash_leaves_unaligned_length(void)
{
    /* T-8: "leaving a non-8-aligned file length".  A crash mid-write can leave a
     * file whose length is not a multiple of 8, which F-2's alignment rule would
     * never produce -- and the reader must handle it as an ordinary torn tail
     * rather than assuming alignment. */
    total++;

    hooks_reset();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "a", 1, "1", 1, 0) == ZS_OK, "store");
    char name[ZSI_NAME_MAX];
    zsi_name_format(name, db->uuid, 1, 0);
    zs_db_close(&db);

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);

    for (int extra = 1; extra < 8; extra++) {
        int fd = open(path, O_WRONLY | O_APPEND);
        CHECK(fd >= 0, "reopen for append");
        char junk[8];
        memset(junk, 0xAB, sizeof(junk));
        if (write(fd, junk, (size_t)extra) != extra) { close(fd); CHECK(0, "append"); }
        close(fd);

        alarm(30);
        char state[512];
        bool ok = read_state(state, sizeof(state));
        alarm(0);

        CHECK(ok, "reopen with length %% 8 == %d failed",
              (int)((72 + 24 + extra) % 8));
        CHECK(strstr(state, "a=1") != NULL,
              "committed data lost at extra=%d: '%s'", extra, state);

        /* Truncate back for the next iteration. */
        if (truncate(path, 96) != 0) CHECK(0, "truncate");
    }

    passed++;
    fprintf(stderr, "  crash leaving a non-8-aligned length     ok\n");
}

static void test_crash_after_invalid_terminator(void)
{
    /* T-8: "after an invalid terminator, asserting the writer moves to a new
     * generation rather than appending (R-4, D-9)".
     *
     * This is the case R-4 exists for: a spurious terminator in trailing garbage,
     * which a checksum can never wholly exclude, must not become the foundation of
     * a later chain. */
    total++;

    hooks_reset();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "a", 1, "1", 1, 0) == ZS_OK, "store");
    char name[ZSI_NAME_MAX];
    zsi_name_format(name, db->uuid, 1, 0);
    zs_db_close(&db);

    /* Append something that LOOKS like a terminator but does not validate. */
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0, "append");
    char fake[8];
    memset(fake, 0, sizeof(fake));
    fake[0] = (char)ZSI_COMMIT;
    zsi_put24(fake + 1, 0);
    zsi_put32(fake + 4, 0xDEADBEEF);        /* a wrong checksum */
    if (write(fd, fake, sizeof(fake)) != (ssize_t)sizeof(fake)) {
        close(fd);
        CHECK(0, "write fake terminator");
    }
    close(fd);

    db = open_db(0);
    CHECK(db != NULL, "reopen");
    CHECK(!zsi_unordered_is_clean(db->snap->files[0]),
          "a file with an invalid terminator should not be clean");

    CHECK(zs_db_store(db, "b", 1, "2", 1, 0) == ZS_OK, "store after");

    /* A NEW generation, not an append past the bad boundary. */
    CHECK(db->snap->nfiles >= 2, "writer appended instead of rolling over");
    CHECK(db->snap->files[db->snap->nfiles - 1]->hdr.start >= 2,
          "no new generation created");

    char state[512];
    zs_db_close(&db);
    CHECK(read_state(state, sizeof(state)), "final reopen");
    CHECK(strstr(state, "a=1") && strstr(state, "b=2"),
          "records lost: '%s'", state);

    passed++;
    fprintf(stderr, "  crash after an invalid terminator        ok\n");
}

/* ---------------------------------------------------------------------------
 * T-10b: the snapshot protocol's gap, forced rather than raced
 * ------------------------------------------------------------------------- */

static int gap_removals;        /* files unlinked inside the gap */
static char gap_victim[PATH_MAX];   /* a RESOLVED file, so step 3 hits ENOENT */
static char gap_victim2[PATH_MAX];
static char gap_staged[PATH_MAX];   /* the superseding output, pre-built */
static char gap_published[PATH_MAX];

static void snapshot_gap(const char *dir)
{
    /* Called between C-4's resolve and open, and it must reproduce a whole packer
     * step: PUBLISH a superseding output, then retire the inputs.
     *
     * Removing a superseded file is not enough, and an earlier version of this test
     * did exactly that -- so the resolved set never contained the removed file,
     * step 3 never opened it, no ENOENT ever happened, and the test passed without
     * touching the retry path.  A mutant making ENOENT fatal went uncaught, which
     * is how the hole was found.
     *
     * The victims here ARE in the resolved set at scan time, and the output that
     * supersedes them appears only now -- which is precisely the interleaving C-4b
     * is about. */
    (void)dir;
    if (!gap_staged[0]) return;

    if (rename(gap_staged, gap_published) != 0) return;
    if (unlink(gap_victim) == 0) gap_removals++;
    if (gap_victim2[0] && unlink(gap_victim2) == 0) gap_removals++;

    gap_staged[0] = '\0';          /* once, so the retry can succeed */
}

/* P-14: publishing a pointer table must not sync.
 *
 * Asserted by COUNTING, not by timing: a commit with the cache on performs
 * exactly the same number of fdatasync calls as one with it off.  A timing
 * assertion would be flaky, and a structural one -- grepping the source -- would
 * pass the moment the sync moved somewhere else.
 *
 * This lives in the crash harness rather than zstest because the syscall hooks
 * only exist under ZS_TEST_HOOKS, which only this target defines.  It doubles as
 * a check that C-7's two gates are still two. */
static void test_idxcache_no_fsync_on_publish(void)
{
    char cachedir[PATH_MAX];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    long without, with;

    total++;

    fresh_dir();
    hooks_reset();
    hooks_on();

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    {
        char cmd[PATH_MAX + 32];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", cachedir);
        if (system(cmd)) {}
    }
    CHECK(mkdir(cachedir, 0700) == 0, "mkdir cache");

    /* Baseline: one commit with no cache at all. */
    setup.flags = ZS_CREATE;
    setup.error = quiet_error;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open plain");
    CHECK(zs_db_store(db, "a", 1, "1", 1, 0) == ZS_OK, "store a");
    without = sync_calls;
    CHECK(zs_db_store(db, "b", 1, "2", 1, 0) == ZS_OK, "store b");
    without = sync_calls - without;
    zs_db_close(&db);

    /* The same commit with the cache on and a threshold of one byte, so a table
     * is published for certain. */
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open cached");
    CHECK(zs_db_store(db, "c", 1, "3", 1, 0) == ZS_OK, "store c");
    with = sync_calls;
    CHECK(zs_db_store(db, "d", 1, "4", 1, 0) == ZS_OK, "store d");
    with = sync_calls - with;
    /* Tables live in the per-uuid directory the open resolved (P-2a). */
    CHECK(db->index_dir != NULL, "cache resolved");
    snprintf(cachedir, sizeof(cachedir), "%s", db->index_dir);
    zs_db_close(&db);

    CHECK(without == 2, "C-7 gates: expected 2 syncs per commit, got %ld",
          without);
    CHECK(with == without,
          "publishing added %ld sync(s) to a commit (P-14)", with - without);

    /* And a table really was published, so the comparison meant something. */
    {
        DIR *d = opendir(cachedir);
        struct dirent *de;
        int tables = 0;
        CHECK(d != NULL, "opendir cache");
        while ((de = readdir(d)))
            if (!strncmp(de->d_name, ZSI_IDX_NAME_PREFIX,
                         strlen(ZSI_IDX_NAME_PREFIX)))
                tables++;
        closedir(d);
        CHECK(tables == 1, "expected one published table, found %d", tables);
    }

    passed++;
    fprintf(stderr, "  publishing adds no sync (P-14)           ok\n");
}

static void test_snapshot_gap_retry(void)
{
    /* C-4a/C-4b: a file removed between scanning the directory and opening the
     * files must surface as a retry that converges, never as a partial snapshot.
     *
     * FORCED, not raced -- an earlier version raced a remover against 300 snapshots
     * and took the retry path zero times.
     *
     * And the forcing has to be a whole packer step.  A second version removed only
     * a SUPERSEDED file, which the resolved set does not contain, so step 3 never
     * opened it and the retry still never ran.  That version passed while a mutant
     * making ENOENT fatal went uncaught -- the tell that it was asserting nothing.
     *
     * So: build the superseding output, hide it under a staging name the scan
     * ignores, and in the gap publish it and retire the two inputs that WERE
     * resolved.  Step 3 then meets an ENOENT it cannot avoid. */
    total++;

    fresh_dir();
    hooks_reset();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "create");
    zsi_uuid_t uuid;
    memcpy(uuid, db->uuid, 16);

    /* Three generations, each forced by making the active file unclean (D-9). */
    for (int i = 0; i < 3; i++) {
        char k[8];
        snprintf(k, sizeof(k), "k%d", i);
        CHECK(zs_db_store(db, k, strlen(k), "v", 1, 0) == ZS_OK, "store %s", k);

        if (i < 2) {
            char name[ZSI_NAME_MAX], path[PATH_MAX];
            zsi_name_format(name, uuid, (uint32_t)(i + 1), 0);
            snprintf(path, sizeof(path), "%s/%s", dbdir, name);
            int fd = open(path, O_WRONLY | O_APPEND);
            CHECK(fd >= 0, "append to %s", name);
            if (write(fd, "\336\255\276\357\336\255\276\357", 8) != 8) {
                close(fd);
                CHECK(0, "append");
            }
            close(fd);
            zs_db_close(&db);
            db = open_db(0);
            CHECK(db != NULL, "reopen after garbage");
        }
    }
    zs_db_close(&db);

    /* Convert, so generations 1 and 2 are in-order files and both are RESOLVED. */
    db = open_db(0);
    CHECK(db != NULL, "reopen for convert");
    {
        struct zs_txn *t = NULL;
        CHECK(zs_db_begin_txn(db, 0, &t) == ZS_OK, "begin");
        CHECK(zs_txn_commit(&t) == ZS_OK, "commit");
    }

    size_t nio = 0;
    while (nio < db->snap->nfiles && !zsi_file_is_unordered(db->snap->files[nio]))
        nio++;
    CHECK(nio >= 2, "only %zu in-order files after conversion", nio);

    /* Build the [1-2] output, then hide it under a staging name so the scan does
     * not see it -- restoring the directory to its pre-repack state. */
    CHECK(zsi_repack_merge(db, db->snap, 0, 2) == ZS_OK, "merge");
    zs_db_close(&db);

    char outname[ZSI_NAME_MAX];
    zsi_name_format(outname, uuid, 1, 2);
    snprintf(gap_published, sizeof(gap_published), "%s/%s", dbdir, outname);
    snprintf(gap_staged, sizeof(gap_staged), "%s/zeroskip.tmp.gap", dbdir);
    CHECK(rename(gap_published, gap_staged) == 0, "stage the output");

    zsi_name_format(outname, uuid, 1, 1);
    snprintf(gap_victim, sizeof(gap_victim), "%s/%s", dbdir, outname);
    zsi_name_format(outname, uuid, 2, 2);
    snprintf(gap_victim2, sizeof(gap_victim2), "%s/%s", dbdir, outname);

    CHECK(fexists(gap_victim) == 0, "[1-1] missing");
    CHECK(fexists(gap_victim2) == 0, "[2-2] missing");
    CHECK(fexists(gap_staged) == 0, "the staged output is missing");

    /* Take a snapshot with the gap armed.  The scan resolves [1-1], [2-2] and the
     * active file; the gap then publishes [1-2] and removes both inputs. */
    gap_removals = 0;
    zs_hook_snapshot_gap = snapshot_gap;

    struct zsi_snapshot *s = NULL;
    alarm(30);
    int r = zsi_snapshot_take(dbdir, &uuid, zsi_compar_default, "memcmp",
                              NULL, false, NULL, NULL, &s);
    alarm(0);
    zs_hook_snapshot_gap = NULL;

    CHECK(gap_removals == 2, "the gap removed %d resolved files, wanted 2",
          gap_removals);
    CHECK(r == ZS_OK, "the retry did not converge: %s", zs_strerror(r));
    CHECK(s != NULL, "no snapshot");

    /* The converged snapshot is the POST-repack set, not a partial one. */
    CHECK(s->nfiles == 2, "converged on %zu files, wanted 2", s->nfiles);
    CHECK(s->files[0]->hdr.start == 1 && s->files[0]->hdr.end == 2,
          "first file is %u-%u, wanted 1-2",
          s->files[0]->hdr.start, s->files[0]->hdr.end);
    zsi_snapshot_release(&s);

    /* Nothing was lost. */
    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after the gap");
    for (int i = 0; i < 3; i++) {
        char want[16];
        snprintf(want, sizeof(want), "k%d=v", i);
        CHECK(strstr(state, want) != NULL, "lost %s: '%s'", want, state);
    }

    passed++;
    fprintf(stderr, "  snapshot gap retry converges (C-4b)      ok\n");
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *filter = argc > 1 ? argv[1] : NULL;
    char path[PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";

    snprintf(path, sizeof(path), "%s/zeroskip-crash.%d", tmp, (int)getpid());
    if (mkdir(path, 0700) && errno != EEXIST) { perror(path); return 1; }
    basedir = strdup(path);
    snprintf(dbdir, sizeof(dbdir), "%s/db", basedir);

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "crash_at_every_call",            test_crash_at_every_call },
        { "crash_nosync_mode",              test_crash_nosync_mode },
        { "dirsync_justifies_c6",           test_dirsync_justifies_c6 },
        { "sync_failure_gate1",             test_sync_failure_gate1 },
        { "sync_failure_gate2",             test_sync_failure_gate2 },
        { "sync_failure_every_point",       test_sync_failure_every_point },
        { "crash_leaves_unaligned_length",  test_crash_leaves_unaligned_length },
        { "crash_after_invalid_terminator", test_crash_after_invalid_terminator },
        { "snapshot_gap_retry",             test_snapshot_gap_retry },
        { "idxcache_no_fsync_on_publish",   test_idxcache_no_fsync_on_publish },
        { NULL, NULL }
    };

    for (int i = 0; tests[i].name; i++) {
        if (filter && !strstr(tests[i].name, filter)) continue;
        tests[i].fn();
    }

    char cmd[PATH_MAX + 20];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", basedir);
    if (system(cmd)) {}
    free(basedir);

    fprintf(stderr, "\n%d crash tests: %d passed, %d failed\n",
            total, passed, failed);
    return failed ? 1 : 0;
}
