/* Throwaway spike bench: single-store and batched commits under ZS_NOSYNC,
 * to isolate the mmap-write variant's cost from the fdatasync gates. */
#include "zeroskip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static void quiet_error(const char *msg, const char *fmt, ...)
{
    (void)msg; (void)fmt;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/tmp/nsbench-db";
    int n = argc > 2 ? atoi(argv[2]) : 20000;
    int per = argc > 3 ? atoi(argv[3]) : 1;
    char cmd[1024];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd)) {}

    struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
    setup.flags = ZS_CREATE | ZS_NOSYNC;
    setup.error = quiet_error;

    struct zs_db *db = NULL;
    if (zs_db_open(dir, &setup, &db) != ZS_OK) {
        fprintf(stderr, "open failed\n");
        return 1;
    }

    char val[100];
    memset(val, 'v', sizeof(val));

    double t0 = now();
    int done = 0;
    while (done < n) {
        struct zs_txn *txn = NULL;
        if (zs_db_begin_txn(db, 0, &txn) != ZS_OK) { fprintf(stderr, "begin\n"); return 1; }
        for (int i = 0; i < per && done < n; i++, done++) {
            char k[32];
            snprintf(k, sizeof(k), "key%08d", done);
            if (zs_txn_store(txn, k, strlen(k), val, sizeof(val), 0) != ZS_OK) {
                fprintf(stderr, "store\n");
                return 1;
            }
        }
        if (zs_txn_commit(&txn) != ZS_OK) { fprintf(stderr, "commit\n"); return 1; }
    }
    double dt = now() - t0;

    printf("nosync %d records, %d per txn: %8.0f/s  %.2fs\n",
           n, per, n / dt, dt);
    zs_db_close(&db);
    return 0;
}
