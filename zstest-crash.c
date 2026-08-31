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

/* A path to a file INSIDE the database: dbdir, a '/', a file name and the NUL.
 *
 * Sized from those parts rather than PATH_MAX alone, because gcc checks snprintf
 * against the DECLARED bounds of its arguments -- dbdir is PATH_MAX and a name is
 * up to ZSI_NAME_MAX, so "%s/%s" into a PATH_MAX buffer is a -Wformat-truncation
 * warning even though no real dbdir comes close.  Stating the bound where it is
 * relied on is the same choice the fileset scan makes for the same reason, and
 * Cyrus builds -Werror. */
#define DBPATH_MAX (PATH_MAX + ZSI_NAME_MAX + 2)

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
/* Tear the Nth write: write half its bytes and report half, then fail the NEXT
 * write outright.  That is a partial write followed by a failure, which is the
 * one state in which `flushed` stops describing what the file holds -- and so
 * the state C-8b's discard must refuse to reason from. */
static long tear_write_at;
static long write_calls;
static bool torn_yet;
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
static long renames_seen;       /* renames so far */
static long syncs_at_first_rename;   /* sync_calls when the first rename ran,
                                      * or -1 if none has: C-6b's ordering --
                                      * the output sync BEFORE the publish */

static void tick(void)
{
    call_count++;
    if (crash_at >= 0 && call_count >= crash_at) _exit(42);
}

static ssize_t hook_write(int fd, const void *buf, size_t n)
{
    tick();
    write_calls++;
    if (tear_write_at >= 0 && write_calls == tear_write_at && n > 1) {
        torn_yet = true;
        return write(fd, buf, n / 2);          /* short: write_all loops */
    }
    if (torn_yet) { errno = EIO; return -1; }  /* ... and the loop's next call fails */
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
    if (renames_seen++ == 0) syncs_at_first_rename = sync_calls;
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
    tear_write_at = -1;
    write_calls = 0;
    torn_yet = false;
    sync_calls = 0;
    syncs_after_failure = 0;
    a_sync_failed = false;
    suppress_dirsync = false;
    dir_syncs = 0;
    renames_seen = 0;
    syncs_at_first_rename = -1;
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

static void test_nosync_structural_syncs(void)
{
    /* C-6b/C-7c: ZS_NOSYNC omits the two commit gates and NOTHING else.  The
     * structural syncs are integrity, not durability: an implementation that
     * skipped them could publish a name pointing at a partial file and then
     * retire the inputs that were the records' only complete copy -- a crash
     * would cost converted generations, not the active tail the caller
     * agreed to risk.  Each structural operation has an exact signature --
     * the output fdatasync BEFORE its publishing rename, the directory
     * fdatasync after -- and the counts are asserted exactly: a commit
     * contributing even one sync, or a structural site contributing one
     * fewer, is a named bug either way. */
    struct zs_db *db;
    const char *v; size_t vl;
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();

    db = open_db(ZS_CREATE | ZS_NOSYNC);
    CHECK(db != NULL, "nosync open failed");

    /* Commits sync nothing under ZS_NOSYNC -- the flag working (C-7c). */
    hooks_reset();
    CHECK(zs_db_store(db, "a", 1, "1", 1, 0) == ZS_OK, "store a failed");
    CHECK(zs_db_store(db, "b", 1, "2", 1, 0) == ZS_OK, "store b failed");
    CHECK(sync_calls == 0, "a NOSYNC commit synced (%ld calls)", sync_calls);

    /* A conversion: the output before the publishing rename (C-6b), the
     * directory after it (C-6).  Exactly one of each. */
    hooks_reset();
    CHECK(zs_db_seal(db) == ZS_OK, "seal failed");
    CHECK(sync_calls == 2 && dir_syncs == 1,
          "conversion made %ld syncs, %ld of them directory; C-6b wants "
          "output + directory", sync_calls, dir_syncs);
    CHECK(syncs_at_first_rename == 1,
          "%ld syncs before the publishing rename; C-6b wants the output "
          "durable first", syncs_at_first_rename);

    /* Creating the next active file: header, then directory (C-6b, C-6). */
    hooks_reset();
    CHECK(zs_db_store(db, "c", 1, "3", 1, 0) == ZS_OK, "store c failed");
    CHECK(sync_calls == 2 && dir_syncs == 1,
          "creation made %ld syncs, %ld of them directory; C-6b wants "
          "header + directory", sync_calls, dir_syncs);

    /* A repack output carries the same signature as a conversion's. */
    CHECK(zs_db_seal(db) == ZS_OK, "second seal failed");
    hooks_reset();
    CHECK(zs_db_compact(db) == ZS_OK, "compact failed");
    CHECK(sync_calls == 2 && dir_syncs == 1,
          "repack made %ld syncs, %ld of them directory; C-6b wants "
          "output + directory", sync_calls, dir_syncs);
    CHECK(syncs_at_first_rename == 1,
          "%ld syncs before the repack's publishing rename",
          syncs_at_first_rename);

    /* And nothing was lost along the way. */
    CHECK(zs_db_fetch(db, "a", 1, NULL, NULL, &v, &vl, 0) == ZS_OK, "fetch a");
    CHECK(zs_db_fetch(db, "c", 1, NULL, NULL, &v, &vl, 0) == ZS_OK, "fetch c");
    zs_db_close(&db);

    passed++;
    fprintf(stderr, "  NOSYNC keeps the structural syncs (C-6b) ok\n");
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

static void test_txn_buffer_grows_to_one_write(void)
{
    /* C-8's buffer grows rather than flushing, so a span well over the initial
     * ZSI_TXN_CHUNK still leaves in ONE write with its terminator (C-7).
     *
     * Counted rather than reasoned about, and counted on the SECOND transaction:
     * the first one creates the active file, whose header write would otherwise
     * be scored against the span.  Before the buffer grew, a 108KB span was three
     * writes -- two chunk flushes and the terminator. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    struct zs_txn *txn = NULL;
    char val[100];
    long writes;
    /* Comfortably over ZSI_TXN_CHUNK and comfortably under ZSI_TXN_CHUNK_MAX. */
    const size_t nrecs = (ZSI_TXN_CHUNK / sizeof(val)) * 4;

    total++;

    fresh_dir();
    hooks_reset();
    hooks_on();

    memset(val, 'v', sizeof(val));
    setup.flags = ZS_CREATE;
    setup.error = quiet_error;
    setup.rollover_size = (size_t)512 * 1024 * 1024;   /* no conversion mid-test */
    setup.rollover_txns = (size_t)1 << 40;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open");

    for (int pass = 0; pass < 2; pass++) {
        writes = call_count;
        CHECK(zs_db_begin_txn(db, 0, &txn) == ZS_OK, "begin");
        for (size_t i = 0; i < nrecs; i++) {
            char k[32];
            snprintf(k, sizeof(k), "p%dk%08zu", pass, i);
            CHECK(zs_txn_store(txn, k, strlen(k), val, sizeof(val), 0) == ZS_OK,
                  "store");
        }
        CHECK(zs_txn_commit(&txn) == ZS_OK, "commit");
    }

    /* call_count ticks on write, fdatasync, rename and unlink; this transaction
     * does one of the middle two and none of the last two, so the excess over 2
     * is writes. */
    CHECK(call_count - writes == 2,
          "a %zu-record span took %ld syscalls, expected 1 write + 1 sync",
          nrecs, call_count - writes);

    zs_db_close(&db);
    hooks_reset();

    passed++;
    fprintf(stderr, "  a grown buffer commits in one write       ok\n");
}

static void test_torn_flush_then_abort(void)
{
    /* The one state C-8b's discard must refuse to reason from: a flush that wrote
     * SOME bytes and then failed.  `flushed` did not advance, so the transaction's
     * own accounting says the span never reached the file -- while it partly did.
     *
     * What this asserts is that an acknowledged commit either side of it survives.
     * The mechanism that delivers that is NOT C-8b's poison check: the next write
     * transaction's C-4i probe sees the file has grown, rebuilds, finds the last
     * valid span below the orphan bytes, judges the file UNCLEAN and rolls over to
     * a new generation (D-9a, R-4).  The poison check keeps the discard's stated
     * precondition honest -- it stops the writer concluding "nothing was written"
     * from an accounting value it cannot trust -- but the rollover is the backstop,
     * which is why the two mutants aiming at the check are `equivalent`.
     *
     * Nothing tested this path before, in either direction. */
    struct zs_db *db;
    struct zs_txn *txn = NULL;
    const char *v = NULL;
    size_t vl = 0;
    char state[512];

    total++;

    fresh_dir();
    hooks_reset();
    hooks_on();

    db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "before", 6, "1", 1, 0) == ZS_OK, "first store");

    /* Store, then read it back WITHOUT ZS_EPHEMERAL, which forces the flush --
     * and tear that flush's write. */
    CHECK(zs_db_begin_txn(db, 0, &txn) == ZS_OK, "begin");
    CHECK(zs_txn_store(txn, "torn", 4, "xyz", 3, 0) == ZS_OK, "store");
    tear_write_at = write_calls + 1;
    (void)zs_txn_fetch(txn, "torn", 4, NULL, NULL, &v, &vl, 0);
    tear_write_at = -1;
    torn_yet = false;

    CHECK(zs_txn_abort(&txn) == ZS_OK, "abort");

    /* A later commit must land somewhere safe and must not be lost. */
    CHECK(zs_db_store(db, "after", 5, "3", 1, 0) == ZS_OK, "later store");
    zs_db_close(&db);
    hooks_reset();

    CHECK(read_state(state, sizeof(state)), "reopen after a torn flush");
    CHECK(strstr(state, "before=1") != NULL,
          "the earlier commit was lost: '%s'", state);
    CHECK(strstr(state, "after=3") != NULL,
          "the later commit was lost: '%s'", state);
    CHECK(strstr(state, "torn") == NULL,
          "the aborted record is visible: '%s'", state);

    /* And a writer can still continue. */
    db = open_db(0);
    CHECK(db != NULL, "cannot reopen");
    CHECK(zs_db_store(db, "more", 4, "4", 1, 0) == ZS_OK, "cannot continue");
    zs_db_close(&db);

    passed++;
    fprintf(stderr, "  a torn flush then an abort (C-8b)        ok\n");
}

static void test_sync_failure_gate(void)
{
    /* The commit's only sync (C-7) made to fail.  The terminator is already
     * written by then, so the outcome is UNKNOWN: the commit may be visible with
     * durable data, or the span may read as absent, and either is correct.  What
     * is NOT allowed is a third state -- a torn or invented one -- or losing an
     * earlier commit, or wedging the database.
     *
     * This was two tests until 2026-08-18, one per gate.  The first asserted the
     * stronger thing C-7a used to promise: a gate-1 failure meant no terminator
     * had been written, so the transaction plainly did not happen.  One gate
     * cannot offer that -- writing the terminator only after a successful sync IS
     * the second gate -- and the promise was never reachable through this API,
     * since both gates returned the same ZS_IOERROR.  Losing the assertion is the
     * real cost of the change, recorded here rather than quietly dropped. */
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "before", 6, "1", 1, 0) == ZS_OK, "first store");

    /* The next commit's only sync is the gate. */
    long before_syncs = sync_calls;
    fail_sync_at = before_syncs + 1;

    int r = zs_db_store(db, "maybe", 5, "2", 1, 0);
    CHECK(r != ZS_OK, "the gate failure was not reported (got %s)", zs_strerror(r));

    /* MUST NOT retry the sync (C-7a): a second call can succeed after the dirty
     * pages were discarded, so treating success as proof the data survived is
     * wrong.  Counted rather than reasoned about. */
    CHECK(syncs_after_failure == 0,
          "%ld fdatasync calls after a failure -- must be 0 (C-7a)",
          syncs_after_failure);

    zs_db_close(&db);
    hooks_reset();

    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after a gate failure");

    /* Both outcomes are legal; a torn or invented one is not. */
    bool visible = strstr(state, "maybe=2") != NULL;
    bool absent = strstr(state, "maybe") == NULL;
    CHECK(visible || absent, "neither visible nor absent: '%s'", state);
    CHECK(strstr(state, "before=1") != NULL,
          "the earlier commit was lost: '%s'", state);

    /* And a writer can continue: an unknown outcome must not be a wedged
     * database (T-8a). */
    db = open_db(0);
    CHECK(db != NULL, "cannot reopen");
    CHECK(zs_db_store(db, "after", 5, "3", 1, 0) == ZS_OK, "cannot continue");
    zs_db_close(&db);

    passed++;
    fprintf(stderr, "  sync failure at the gate                 ok\n");
}

static void test_sync_failure_seals(void)
{
    /* C-7d: after a gate failure the SAME HANDLE must seal the generation before
     * it appends anything, and the seal must be a published file -- synced,
     * renamed, directory synced.
     *
     * test_sync_failure_gate closes the handle and reopens, which is why it never
     * saw this: the bug is entirely in continuing.  Nothing on disk distinguishes
     * readable from durable, so the failed commit's span is still servable from
     * the page cache; the next begin's C-4i probe sees the size change, rebuilds,
     * replays, finds every span VALID and the file CLEAN, and happily appends.
     * A recovery that then rejects that span completes the file below it (F-22,
     * F-24) and takes the later commit with it -- and the later commit returned
     * ZS_OK.  Only the writer knows a gate failed, so only the writer can stop it.
     *
     * The discriminating assertion is the RENAME, not the data: without the seal
     * every store below still succeeds and still reads back, on a machine where
     * the pages were never really lost.  What changes is whether generation N was
     * published as an in-order file before generation N+1 began. */
    total++;

    hooks_reset();
    hooks_on();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "before", 6, "1", 1, 0) == ZS_OK, "first store");

    fail_sync_at = sync_calls + 1;
    int r = zs_db_store(db, "maybe", 5, "2", 1, 0);
    CHECK(r != ZS_OK, "the gate failure was not reported (got %s)", zs_strerror(r));

    /* Everything from here is the NEXT write on the SAME handle. */
    long renames_before = renames_seen;
    long dirsyncs_before = dir_syncs;

    CHECK(zs_db_store(db, "after", 5, "3", 1, 0) == ZS_OK,
          "a write after a gate failure must still succeed once the seal is done");

    CHECK(renames_seen > renames_before,
          "no file was published after the gate failure -- the generation was "
          "appended to rather than sealed (C-7d)");
    CHECK(dir_syncs > dirsyncs_before,
          "the seal's output was published without a directory sync (C-6)");

    /* C-6b's ordering, applied to this rename: the output is synced BEFORE it is
     * published, or the name points at a file that may be torn. */
    CHECK(syncs_at_first_rename > 0,
          "the seal renamed before it synced its output (C-6b)");

    zs_db_close(&db);
    hooks_reset();

    /* And the earlier commit survives.  "maybe" is C-7a's unknown outcome and may
     * legally be either -- the seal resolves it, and which way depends on whether
     * the bytes survived to be copied (D-20b), which on a machine that did not
     * really lose the pages means it did. */
    char state[512];
    CHECK(read_state(state, sizeof(state)), "reopen after a gate failure");
    CHECK(strstr(state, "before=1") != NULL,
          "the earlier commit was lost: '%s'", state);
    CHECK(strstr(state, "after=3") != NULL,
          "the commit after the seal was lost: '%s'", state);

    /* Phase 2: the same rule reached through zs_db_sync under ZS_NOSYNC.  That
     * mode has no commit gate at all (C-7c), so the caller's own durability point
     * is the only gate there is -- and a failure there leaves exactly the same
     * readable-but-not-durable tail.  The seal still publishes, because its syncs
     * are structural and no flag relaxes them (C-6b). */
    hooks_reset();
    hooks_on();
    fresh_dir();

    db = open_db(ZS_CREATE | ZS_NOSYNC);
    CHECK(db != NULL, "nosync open failed");
    CHECK(zs_db_store(db, "n1", 2, "1", 1, 0) == ZS_OK, "nosync store");

    fail_sync_at = sync_calls + 1;
    CHECK(zs_db_sync(db) != ZS_OK, "the injected zs_db_sync failure was not reported");

    renames_before = renames_seen;
    CHECK(zs_db_store(db, "n2", 2, "2", 1, 0) == ZS_OK, "nosync store after a failed sync");
    CHECK(renames_seen > renames_before,
          "a failed zs_db_sync did not force a seal before the next write (C-7d)");

    zs_db_close(&db);
    hooks_reset();

    CHECK(read_state(state, sizeof(state)), "reopen after a failed zs_db_sync");
    CHECK(strstr(state, "n2=2") != NULL, "the post-seal commit was lost: '%s'", state);

    passed++;
    fprintf(stderr, "  a failed gate seals before writing       ok\n");
}

static void test_commit_has_one_gate(void)
{
    /* C-7: one sync per commit, on BOTH commit paths.
     *
     * The paths differ in where the span's bytes are when the terminator is
     * built.  A span still in the chunk buffer is merged with its terminator and
     * leaves in one write; a span already flushed -- anything larger than
     * ZSI_TXN_CHUNK -- writes the terminator separately.  The second path is
     * where the old first gate lived, so a sync re-added there is invisible to
     * any small-transaction assertion: the mutant "commit: syncs before the
     * terminator too" went NOT CAUGHT until this test existed, because every
     * other sync-counting test commits a single record. */
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db;
    struct zs_txn *txn = NULL;
    char *big;
    long syncs;
    /* Sized against ZSI_TXN_CHUNK_MAX, not ZSI_TXN_CHUNK.  The chunk GROWS on
     * demand from 64KB to 4MB, so a span sized against the initial value stays
     * buffered and takes the MERGED path -- leaving the flushed path, which is
     * where the old first gate lived, never executed.  This test was sized
     * against the 64KB figure and so had lost the power it exists for: the
     * mutant it was written to kill went NOT CAUGHT in the 2026-08-27 run, and
     * format 4's smaller records had pushed the span further inside the buffer
     * still.  x2 so it exceeds the ceiling even at the smallest record size. */
    size_t nrecs = (ZSI_TXN_CHUNK_MAX / 100) * 2;

    total++;

    fresh_dir();
    hooks_reset();
    hooks_on();

    /* rollover_size and rollover_txns are PINNED out of the way, and that is
     * what makes the flushed-path assertion mean anything.  A span big enough to
     * exceed the 4MB chunk ceiling is also big enough to trip D-25d's seal at the
     * 2MB default -- and a conversion carries structural syncs of its own (C-6b),
     * which the assertion below has to tolerate.  So without this pin the escape
     * hatch swallows the very sync the test is looking for, and the mutant lives
     * whatever the span size. */
    setup.flags = ZS_CREATE;
    setup.error = quiet_error;
    setup.rollover_size = (size_t)512 * 1024 * 1024;
    setup.rollover_txns = (size_t)1 << 40;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open failed");

    /* Merged path: one small record, so the span never leaves the chunk. */
    syncs = sync_calls;
    CHECK(zs_db_store(db, "small", 5, "1", 1, 0) == ZS_OK, "small store");
    CHECK(sync_calls - syncs == 1,
          "merged path: expected 1 sync, got %ld", sync_calls - syncs);

    /* Flushed path: a span far larger than the chunk, so the terminator is
     * written on its own.  Still one gate. */
    big = malloc(100);
    CHECK(big != NULL, "malloc");
    memset(big, 'v', 100);
    syncs = sync_calls;
    CHECK(zs_db_begin_txn(db, 0, &txn) == ZS_OK, "begin");
    for (size_t i = 0; i < nrecs; i++) {
        char k[32];
        snprintf(k, sizeof(k), "big%08zu", i);
        CHECK(zs_txn_store(txn, k, strlen(k), big, 100, 0) == ZS_OK, "big store");
    }
    CHECK(zs_txn_commit(&txn) == ZS_OK, "big commit");
    free(big);

    /* EXACTLY one, with no escape hatch: the pins above mean no conversion can
     * fire, so any second sync is the commit's and the assertion is unconditional.
     * That is the whole point of this case -- the flushed path is where the old
     * first gate lived, so a sync re-added there is invisible to every
     * small-transaction assertion in the suite. */
    CHECK(db->stats.conversions == 0,
          "flushed path: a conversion fired, so the pins did not hold");
    CHECK(sync_calls - syncs == 1,
          "flushed path: expected exactly 1 sync, got %ld", sync_calls - syncs);

    zs_db_close(&db);
    hooks_reset();

    passed++;
    fprintf(stderr, "  one gate per commit, both paths (C-7)    ok\n");
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
    CHECK(nsyncs >= nkeys, "only %ld syncs for %d commits", nsyncs, nkeys);

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
    /* T-8: trailing junk bytes.  A crash mid-write can leave a file with a
     * partial record appended after the last valid span, and the reader must
     * handle it as an ordinary torn tail (F-24) without losing what was
     * committed below it.
     *
     * This case was "a non-8-aligned file length" until version 4, when records
     * stopped being padded to a multiple of 8 (F-2) -- so an odd length is now
     * ordinary rather than impossible, and what is under test is the trailing
     * garbage rather than its alignment.  The clean length is measured for the
     * same reason. */
    total++;

    hooks_reset();
    fresh_dir();

    struct zs_db *db = open_db(ZS_CREATE);
    CHECK(db != NULL, "open failed");
    CHECK(zs_db_store(db, "a", 1, "1", 1, 0) == ZS_OK, "store");
    char name[ZSI_NAME_MAX];
    zsi_name_current(name, db->uuid);
    zs_db_close(&db);

    char path[DBPATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);

    /* The clean size, measured rather than computed: records are packed since
     * version 4 (F-2), so a file's length is whatever its records add up to and
     * hardcoding it here is how this test broke when the encoding changed. */
    struct stat st;
    CHECK(stat(path, &st) == 0, "stat");
    off_t clean = st.st_size;

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

        CHECK(ok, "reopen with %d junk bytes appended failed", extra);
        CHECK(strstr(state, "a=1") != NULL,
              "committed data lost at extra=%d: '%s'", extra, state);

        /* Truncate back for the next iteration. */
        if (truncate(path, clean) != 0) CHECK(0, "truncate");
    }

    passed++;
    fprintf(stderr, "  crash leaving trailing junk bytes        ok\n");
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
    zsi_name_current(name, db->uuid);
    zs_db_close(&db);

    /* Append something that LOOKS like a terminator but does not validate. */
    char path[DBPATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dbdir, name);
    int fd = open(path, O_WRONLY | O_APPEND);
    CHECK(fd >= 0, "append");
    char fake[ZSI_TERMLEN];
    memset(fake, 0, sizeof(fake));
    /* A well-formed COMMIT: keylen 0, vallen 16, a zero span length -- and a
     * deliberately wrong checksum, so it decodes but does not validate. */
    zsi_put16(fake, ZSI_MUSTBEONE);
    zsi_put16(fake + 2, ZSI_TERM_VALLEN);
    zsi_put64(fake + ZSI_TERM_OFF_SPANLEN, 0);
    zsi_put64(fake + ZSI_TERM_OFF_CSUM, 0xDEADBEEFDEADBEEFull);
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
static char gap_victim[DBPATH_MAX];   /* a RESOLVED file, so step 3 hits ENOENT */
static char gap_victim2[DBPATH_MAX];
static char gap_staged[DBPATH_MAX];   /* the superseding output, pre-built */
static char gap_published[DBPATH_MAX];

static void snapshot_gap(const char *dir)
{
    /* Called between C-4's resolve and open, and it must reproduce a whole packer
     * step: PUBLISH a superseding output, then retire the inputs.
     *
     * Removing a superseded file is not enough: the resolved set never contains
     * one, so step 3 never opens it, no ENOENT ever happens, and the retry path
     * is never touched.
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
 * Asserted by COUNTING, not by timing: an operation that publishes a table
 * performs exactly the same number of fdatasync calls as the same operation with
 * the cache off.  A timing assertion would be flaky, and a structural one --
 * grepping the source -- would pass the moment the sync moved somewhere else.
 *
 * MEASURED ACROSS AN OPEN, not across a commit, and that is what this test got
 * wrong.  Since P-13 a commit publishes NOTHING -- D-13b's fold maintains the
 * index incrementally, so there is no replay to amortise -- so comparing two
 * commits compares two operations that both publish nothing, and the mutant
 * "idx: syncs before publishing" went NOT CAUGHT in the 2026-08-27 full run.
 * Publication happens where an index is built by REPLAY, which is an open.
 *
 * The old publication guard counted tables in the cache ROOT, where P-2a never
 * puts any -- the same mis-aimed count CLAUDE.md records for
 * test_seal_at_commit_skips_table_publish.  It counts the resolved per-uuid
 * directory now.
 *
 * This lives in the crash harness rather than zstest because the syscall hooks
 * only exist under ZS_TEST_HOOKS, which only this target defines. */
static void test_idxcache_no_fsync_on_publish(void)
{
    /* resolved holds cachedir plus a uuid and a separator.  Sized to say so:
     * gcc checks the DECLARED bounds, and cachedir is itself PATH_MAX, so
     * PATH_MAX for the result is a -Wformat-truncation error under -Werror --
     * which Cyrus builds with.  Same reason DBPATH_MAX exists above. */
    char cachedir[PATH_MAX], resolved[PATH_MAX + ZSI_UUID_STR_LEN + 2];
    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    struct zs_db *db = NULL;
    long without, with;
    char uu[ZSI_UUID_STR_LEN];

    total++;

    fresh_dir();
    hooks_reset();
    hooks_on();

    snprintf(cachedir, sizeof(cachedir), "%s/cache", basedir);
    CHECK(mkdir(cachedir, 0700) == 0 || errno == EEXIST, "mkdir cache");

    /* Records to replay at the next open, written with no cache configured. */
    setup.flags = ZS_CREATE;
    setup.error = quiet_error;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open");
    for (int i = 0; i < 20; i++) {
        char k[16];
        snprintf(k, sizeof(k), "k%02d", i);
        CHECK(zs_db_store(db, k, strlen(k), "v", 1, 0) == ZS_OK, "store");
    }
    zsi_uuid_unparse(db->uuid, uu);
    CHECK(zs_db_close(&db) == ZS_OK, "close");
    snprintf(resolved, sizeof(resolved), "%s/%s", cachedir, uu);

    /* An open with NO cache: it replays and publishes nothing. */
    setup.flags = 0;
    setup.index_dir = NULL;
    setup.index_threshold = 0;
    without = sync_calls;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open uncached");
    without = sync_calls - without;
    CHECK(zs_db_close(&db) == ZS_OK, "close uncached");

    /* The same open WITH the cache and a threshold of one byte, so the replay
     * publishes.  Same syscall count, or P-14 is broken. */
    setup.index_dir = cachedir;
    setup.index_threshold = 1;
    with = sync_calls;
    CHECK(zs_db_open(dbdir, &setup, &db) == ZS_OK, "open cached");
    with = sync_calls - with;
    CHECK(db->index_dir != NULL, "cache resolved");
    CHECK(zs_db_close(&db) == ZS_OK, "close cached");

    CHECK(with == without,
          "publishing added %ld sync(s) to an open (P-14)", with - without);

    /* And a table really was published, counted in the RESOLVED per-database
     * directory (P-2a) -- so the comparison meant something. */
    {
        DIR *d = opendir(resolved);
        struct dirent *de;
        int tables = 0;
        CHECK(d != NULL, "opendir %s", resolved);
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
     * And the forcing has to be a whole packer step: removing only a SUPERSEDED
     * file leaves it out of the resolved set, so step 3 never opens it and the
     * retry still never runs.
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
            char name[ZSI_NAME_MAX], path[DBPATH_MAX];
            zsi_name_current(name, uuid);
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

    /* Convert, so generations 1 and 2 are in-order files and both are RESOLVED.
     *
     * ZS_NOAUTOREPACK because this test's subject is the layout BEFORE a
     * repack -- it builds the [1-2] output by hand below.  D-16e would merge
     * the two the moment this begin ran, leaving nothing to merge (A-14). */
    db = open_db(ZS_NOAUTOREPACK);
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
                              NULL, NULL, NULL, NULL, &s);
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
    char path[DBPATH_MAX];
    const char *tmp = getenv("TMPDIR");
    if (!tmp) tmp = "/tmp";

    snprintf(path, sizeof(path), "%s/zeroskip-crash.%d", tmp, (int)getpid());
    if (mkdir(path, 0700) && errno != EEXIST) { perror(path); return 1; }
    basedir = strdup(path);
    snprintf(dbdir, sizeof(dbdir), "%s/db", basedir);

    struct { const char *name; void (*fn)(void); } tests[] = {
        { "crash_at_every_call",            test_crash_at_every_call },
        { "crash_nosync_mode",              test_crash_nosync_mode },
        { "nosync_structural_syncs",        test_nosync_structural_syncs },
        { "dirsync_justifies_c6",           test_dirsync_justifies_c6 },
        { "commit_has_one_gate",            test_commit_has_one_gate },
        { "txn_buffer_grows",               test_txn_buffer_grows_to_one_write },
        { "torn_flush_then_abort",          test_torn_flush_then_abort },
        { "sync_failure_gate",              test_sync_failure_gate },
        { "sync_failure_seals",             test_sync_failure_seals },
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
