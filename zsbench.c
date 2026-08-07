/* zsbench.c - benchmark tool for zeroskip
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * Workloads are kept comparable with the sibling twom library's twombench, so
 * numbers can be read side by side.  Documented in doc/benchmarking.md.
 */

#include <stdio.h>
#include <string.h>

#include "zeroskip.h"

static int usage(void)
{
    fprintf(stderr,
        "usage: zsbench [options]\n"
        "\n"
        "  --selftest         verify the harness, then exit\n"
        "  -n N               records per run\n"
        "  --reps N           repetitions\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--help")) return usage();

    fprintf(stderr, "zsbench: %s\n", zs_strerror(ZS_INTERNAL));
    fprintf(stderr, "zsbench: benchmarks are not yet implemented\n");
    return 2;
}
