/* zstool.c - standalone CLI tool for zeroskip databases
 *
 * Copyright (c) 2026 Fastmail Pty Ltd
 *
 * Available under any of: CC0-1.0, 0BSD, or MIT-0
 * See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
 *
 * Implements the driver contract from the spec's section 9.1 (T-0a): a fixed
 * set of subcommands over a database directory, with a defined line format, so
 * one language-neutral runner can drive every implementation.
 */

#include <stdio.h>
#include <string.h>

#include "zeroskip.h"

static int usage(void)
{
    fprintf(stderr,
        "usage: zstool <dir> <command> [args]\n"
        "\n"
        "  create --uuid U        create a database with a given UUID\n"
        "  store K V              one transaction, one store\n"
        "  delete K               one transaction, one delete\n"
        "  batch < script         a sequence of operations in one transaction\n"
        "  get K                  print the value, or NOTFOUND\n"
        "  scan [--prefix P]      print every visible pair in comparator order\n"
        "  dump                   print structure\n"
        "  check                  run the consistency checks\n"
        "  convert                force one conversion\n"
        "  repack                 force one repack\n"
        "  hold-write --for MS    take the write lock and hold it\n"
        "\n"
        "Keys and values are hex encoded, so embedded NULs and newlines\n"
        "survive comparison by the interop runner.\n");
    return 2;
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc < 3) return usage();

    fprintf(stderr, "zstool: %s\n", zs_strerror(ZS_INTERNAL));
    fprintf(stderr, "zstool: subcommands are not yet implemented\n");
    return 2;
}
