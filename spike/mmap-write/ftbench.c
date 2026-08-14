/* Throwaway: price the primitives the mmap-write spike composed, on this
 * machine's filesystem.  Each scenario appends ~150 bytes 20000 times.
 *
 *   A  write(2) append                          (the shipped writer's shape)
 *   B  chunked ftruncate-up + memcpy + exact ftruncate-down, 1GB window
 *   C  ftruncate pair only, no mapping, no store
 *   D  ftruncate pair only, 1GB window present
 *   E  memcpy into a pre-sized file through the window (faults only)
 *   F  chunked ftruncate-up + memcpy, NO down-truncate, 1GB window
 */
/* These files are compiled by hand (see README), so they carry the platform
 * defines the Makefile supplies for the library: -std=c99 alone hides
 * ftruncate/pwrite/fdatasync on glibc, and an implicit declaration returns
 * int, which truncates nothing and mismeasures everything. */
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

#define N     20000
#define REC   152
#define CHUNK (1u << 20)
#define GIANT ((size_t)1 << 30)

/* ZSI_TXN_CHUNK, kept in step with zeroskip.c: case A' has to be the
 * incumbent as it really is, or the comparison is against nothing. */
#define CHUNK64 (64 * 1024)

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
    /* 3 decimals: the memcpy-only cases are tens of nanoseconds, and at one
     * decimal every one of them printed "0.0 us/op" -- which reads as "free"
     * when the whole question is which of them is least free. */
    printf("  %-58s %10.0f/s  %8.3f us/op\n", label, N / dt, dt / N * 1e6);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/ftbench.dat";

    banner("ftbench", path, N, REC);
    char rec[REC];
    memset(rec, 'r', sizeof(rec));

    /* A: plain write() append -- one syscall per record.
     *
     * NOT what the library does, and the distinction decides the spike: A is
     * the strawman, A' is the incumbent. */
    {
        int fd = fresh(path);
        double t0 = now();
        for (int i = 0; i < N; i++)
            if (write(fd, rec, REC) != REC) { perror("write"); exit(1); }
        report("A: write() append (one syscall per record)", now() - t0);
        close(fd);
    }

    /* A': what zsi_txn_stream ACTUALLY does -- memcpy into a reused chunk
     * buffer, one write() when it fills.  ZSI_TXN_CHUNK is 64KB, so at this
     * record size ~430 records share a syscall and the buffer stays hot in
     * cache instead of faulting in fresh pages of a giant mapping.
     *
     * Every mmap case below has to beat THIS to be worth its complexity, not
     * A.  Comparing against A flatters the spike by the entire batching
     * factor, which is where its apparent per-store win came from. */
    {
        int fd = fresh(path);
        char *chunk = malloc(CHUNK64);
        size_t used = 0;
        if (!chunk) { perror("malloc"); exit(1); }
        double t0 = now();
        for (int i = 0; i < N; i++) {
            if (used + REC > CHUNK64) {
                if (write(fd, chunk, used) != (ssize_t)used) { perror("write"); exit(1); }
                used = 0;
            }
            memcpy(chunk + used, rec, REC);
            used += REC;
        }
        if (used && write(fd, chunk, used) != (ssize_t)used) { perror("write"); exit(1); }
        report("A': chunked write() (64KB buffer, the incumbent)", now() - t0);
        free(chunk);
        close(fd);
    }

    /* B: full spike sequence, 1GB window */
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
                if (ftruncate(fd, (off_t)want) < 0) { perror("ft up"); exit(1); }
                wphys = want;
            }
            memcpy(map + wsize, rec, REC);
            wsize = need;
            if (ftruncate(fd, (off_t)wsize) < 0) { perror("ft down"); exit(1); }
            wphys = wsize;
        }
        report("B: ft-up + memcpy + ft-down, 1GB window", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* C: ftruncate pair only, no mapping */
    {
        int fd = fresh(path);
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            wsize += REC;
            if (ftruncate(fd, (off_t)(wsize + CHUNK)) < 0) { perror("ft"); exit(1); }
            if (ftruncate(fd, (off_t)wsize) < 0) { perror("ft"); exit(1); }
        }
        report("C: ftruncate pair only, no mapping", now() - t0);
        close(fd);
    }

    /* D: ftruncate pair only, 1GB window mapped */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            wsize += REC;
            if (ftruncate(fd, (off_t)(wsize + CHUNK)) < 0) { perror("ft"); exit(1); }
            if (ftruncate(fd, (off_t)wsize) < 0) { perror("ft"); exit(1); }
        }
        report("D: ftruncate pair only, 1GB window", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* E: memcpy through the window into a pre-sized file (fault cost only) */
    {
        int fd = fresh(path);
        if (ftruncate(fd, (off_t)((size_t)N * REC)) < 0) { perror("ft"); exit(1); }
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            memcpy(map + wsize, rec, REC);
            wsize += REC;
        }
        report("E: memcpy only, pre-sized file, 1GB window", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* F: chunked ft-up + memcpy, no down-truncate */
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
                if (ftruncate(fd, (off_t)want) < 0) { perror("ft up"); exit(1); }
                wphys = want;
            }
            memcpy(map + wsize, rec, REC);
            wsize = need;
        }
        report("F: ft-up (chunked) + memcpy, no down-truncate", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    unlink(path);
    return 0;
}
