/* Round 3: price the GATED commit sequences end to end.
 *
 *   K: plain writer's txn:  write(rec); fdatasync; write(term); fdatasync
 *   J: v4 mmap txn:         pwrite z @1MB lead; memcpy rec; fdatasync;
 *                           ftruncate down exact; pwrite z; memcpy term; fdatasync
 *   L: exact-only mmap txn: pwrite z; memcpy rec; fdatasync;
 *                           pwrite z; memcpy term; fdatasync
 *   M: J without the down-truncate (slop persists; measures the truncate)
 */
/* Platform defines: see the note in ftbench.c. */
#if defined(__linux__)
#define _GNU_SOURCE
#elif defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "fsname.h"

#define N     2000
#define REC   152
#define TERM  40
#define CHUNK (1u << 20)
#define GIANT ((size_t)1 << 30)

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int fresh(const char *path)
{
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { perror("open"); exit(1); }
    return fd;
}

static void report(const char *label, double dt)
{
    printf("  %-58s %8.0f/s  %6.1f us/txn\n", label, N / dt, dt / N * 1e6);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/ftbench3.dat";

    banner("ftbench3", path, N, REC);
    char rec[REC], term[TERM];
    static const char z = 0;
    memset(rec, 'r', sizeof(rec));
    memset(term, 't', sizeof(term));

    /* K: plain writer shape */
    {
        int fd = fresh(path);
        double t0 = now();
        for (int i = 0; i < N; i++) {
            if (write(fd, rec, REC) != REC) { perror("w"); exit(1); }
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
            if (write(fd, term, TERM) != TERM) { perror("w"); exit(1); }
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
        }
        report("K: write;sync;write;sync (plain shape)", now() - t0);
        close(fd);
    }

    /* J: v4 mmap shape with chunk lead + post-sync down-truncate */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0, wphys = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            size_t need = wsize + REC;
            if (need > wphys) {
                size_t want = (need + need / 4 + CHUNK - 1) & ~((size_t)CHUNK - 1);
                if (pwrite(fd, &z, 1, (off_t)(want - 1)) != 1) { perror("p"); exit(1); }
                wphys = want;
            }
            memcpy(map + wsize, rec, REC);
            wsize = need;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
            if (wphys > wsize) {
                if (ftruncate(fd, (off_t)wsize) < 0) { perror("t"); exit(1); }
                wphys = wsize;
            }
            if (pwrite(fd, &z, 1, (off_t)(wsize + TERM - 1)) != 1) { perror("p"); exit(1); }
            memcpy(map + wsize, term, TERM);
            wsize += TERM;
            wphys = wsize;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
        }
        report("J: v4 shape (chunk lead, post-sync down-trunc)", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* L: exact-only mmap shape */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            if (pwrite(fd, &z, 1, (off_t)(wsize + REC - 1)) != 1) { perror("p"); exit(1); }
            memcpy(map + wsize, rec, REC);
            wsize += REC;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
            if (pwrite(fd, &z, 1, (off_t)(wsize + TERM - 1)) != 1) { perror("p"); exit(1); }
            memcpy(map + wsize, term, TERM);
            wsize += TERM;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
        }
        report("L: exact pwrite-extend only", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* M: J minus the down-truncate (slop persists across iterations) */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0, wphys = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            size_t need = wsize + REC + TERM;
            if (need > wphys) {
                size_t want = (need + need / 4 + CHUNK - 1) & ~((size_t)CHUNK - 1);
                if (pwrite(fd, &z, 1, (off_t)(want - 1)) != 1) { perror("p"); exit(1); }
                wphys = want;
            }
            memcpy(map + wsize, rec, REC);
            wsize += REC;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
            memcpy(map + wsize, term, TERM);
            wsize += TERM;
            if (fdatasync(fd) < 0) { perror("s"); exit(1); }
        }
        report("M: J minus down-truncate (keeps slop)", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    unlink(path);
    return 0;
}
