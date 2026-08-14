/* Round 2: can exact extension avoid the dirty-page truncate penalty? */
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

#define N     20000
#define REC   152
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
    printf("  %-58s %8.0f/s  %6.1f us/op\n", label, N / dt, dt / N * 1e6);
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/tmp/ftbench2.dat";

    banner("ftbench2", path, N, REC);
    char rec[REC];
    memset(rec, 'r', sizeof(rec));

    /* G: EXACT ftruncate-up per store + memcpy (no slop ever exists) */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            if (ftruncate(fd, (off_t)(wsize + REC)) < 0) { perror("ft"); exit(1); }
            memcpy(map + wsize, rec, REC);
            wsize += REC;
        }
        report("G: exact ft-up per store + memcpy, 1GB window", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* H: extend by pwrite of the LAST byte, then memcpy the rest */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N; i++) {
            if (pwrite(fd, rec + REC - 1, 1, (off_t)(wsize + REC - 1)) != 1) {
                perror("pwrite"); exit(1);
            }
            memcpy(map + wsize, rec, REC - 1);
            wsize += REC;
        }
        report("H: pwrite last byte to extend + memcpy, 1GB window", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    /* I: exact ft-up ONCE PER TXN (batch of 10 stores memcpy'd, then one
     *    exact extension covering them all -- WRONG ORDER for SIGBUS, so
     *    extend first to the batch end): models per-terminator extension */
    {
        int fd = fresh(path);
        char *map = mmap(NULL, GIANT, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { perror("mmap"); exit(1); }
        size_t wsize = 0;
        double t0 = now();
        for (int i = 0; i < N / 10; i++) {
            if (ftruncate(fd, (off_t)(wsize + 10 * REC)) < 0) { perror("ft"); exit(1); }
            for (int j = 0; j < 10; j++) {
                memcpy(map + wsize, rec, REC);
                wsize += REC;
            }
        }
        report("I: exact ft-up per 10-store batch + memcpy", now() - t0);
        munmap(map, GIANT);
        close(fd);
    }

    unlink(path);
    return 0;
}
