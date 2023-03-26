/*
 * zeroskip is free software; you can redistribute it and/or modify
 * it under the terms of the MIT license. See LICENSE for details.
 *
 * Copyright (c) 2018 Partha Susarla <mail@spartha.org>
 *
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "cmds.h"
#include <libzeroskip/util.h>
#include <libzeroskip/zeroskip.h>

static void usage_and_die(const char *progname)
{
         fprintf(stderr, "Usage: %s %s\n", progname, cmd_get_usage);


        exit(EXIT_FAILURE);
}

int cmd_get(int argc, char **argv, const char *progname)
{
        static struct option long_options[] = {
                {"config", required_argument, NULL, 'c'},
                {"help", no_argument, NULL, 'h'},
                {NULL, 0, NULL, 0}
        };
        int option;
        int option_index;
        const char *config_file = NULL;
        struct zsdb *db = NULL;
        char *dbname = NULL;
        char *key = NULL;
        const unsigned char *value = NULL;
        size_t vallen = 0, i;
        int ret;

        while((option = getopt_long(argc, argv, "c:h?", long_options,
                                    &option_index)) != -1) {
                switch (option) {
                case 'c':
                        config_file = optarg;
                        break;
                case 'h':
                case '?':
                default:
                        usage_and_die(progname);
                };
        }

        if (argc - optind != 2) {
                usage_and_die(progname);
        }

        dbname = argv[optind++];
        key = argv[optind++];

        cmd_parse_config(config_file);

        if (zsdb_init(&db, NULL, NULL) != ZS_OK) {
                fprintf(stderr, "ERROR: Failed initialising DB.\n");
                ret = EXIT_FAILURE;
                goto done;
        }

        if (zsdb_open(db, dbname, MODE_RDWR) != ZS_OK) {
                fprintf(stderr, "ERROR: Could not open DB %s.\n", dbname);
                ret = EXIT_FAILURE;
                goto done;
        }

        if (zsdb_fetch(db, (const unsigned char *)key, strlen(key),
                       &value, &vallen)) {
                fprintf(stderr, "ERROR: Cannot find record with key %s in %s\n",
                      key, dbname);
                ret = EXIT_FAILURE;
                goto done;
        }

        fprintf(stderr, "Found record with key %s, has value of length %zu: ",
                key, vallen);
        for (i = 0; i < vallen; i++)
                fprintf(stderr, "%c", value[i]);

        fprintf(stderr, "\n");

        ret = EXIT_SUCCESS;
done:
        if (zsdb_close(db) != ZS_OK) {
                fprintf(stderr, "ERROR: Could not close DB.\n");
                ret = EXIT_FAILURE;
        }

        zsdb_final(&db);

        exit(ret);
}
