/* zsbench - A tool to benchmark Zeroskip. This is based on the db_bench tool
 *           in LevelDB.
 */

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <libzeroskip/cstring.h>
#include <libzeroskip/macros.h>
#include <libzeroskip/strarray.h>
#include <libzeroskip/util.h>
#include <libzeroskip/version.h>
#include <libzeroskip/zeroskip.h>

/* Globals */
static char *DBNAME;
static char *BENCHMARKS;
static int NUMRECS = 1000;
static int new_db = 0;          /* set to 1 if we created a new db */

static struct option long_options[] = {
        {"benchmarks", required_argument, NULL, 'b'},
        {"db", required_argument, NULL, 'd'},
        {"numrecs", optional_argument, NULL, 'n'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
};

static void usage(const char *progname)
{
        printf("Usage: %s [OPTION]... [DB]...\n", progname);

        printf("  -b, --benchmarks     comma separated list of benchmarks to run\n");
        printf("                       Available benchmarks:\n");
        printf("                       * writeseq    - write values in sequential key order\n");
        printf("                       * writerandom - write values in random key order\n");
        printf("\n");
        printf("  -d, --db             the db to run the benchmarks on\n");
        printf("  -n, --numrecs        number of records to write[default: 1000]\n");
        printf("  -h, --help           display this help and exit\n");
}

static char *create_tmp_dir_name(void)
{
       static const char charset[] =
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789";
        static const int num_chars = 62;
        uint64_t value;
        struct timeval tv;
        const char *tmpdir;
        char path[PATH_MAX];
        char *dirname_template;
        int i, ret = 1;
        struct stat sb;

        gettimeofday(&tv, NULL);
        value = ((size_t)(tv.tv_usec << 16)) ^ tv.tv_sec ^ getpid();

        tmpdir = getenv("TMPDIR");
        if (!tmpdir)
                tmpdir = "/tmp";

        snprintf(path, sizeof(path), "%s/zsbench-XXXXXX", tmpdir);

        dirname_template = &path[strlen(path) - 6];

        /* TMP_MAX: The minimum number of unique filenames generated by
         * tmpnam() */
        for (i = 0; i < TMP_MAX; ++i) {
                uint64_t v = value;
                int j;

                ret = 1;

                /* Fill in the random bits. */
                for (j = 0; j < 6; j++) {
                        dirname_template[j] = charset[v % num_chars];
                        v /= num_chars;
                }

                if (stat(path, &sb) == -1) {
                        if (errno == ENOENT) {
                                /* FOUND an unused path! */
                                ret = 0;
                                break;
                        }
                }

                value += 9999;
        }

        if (!ret)
                return xstrdup(path);
        else
                return NULL;
}

static char *generate_random_string(char *str, size_t length)
{
        static const char charset[] =
                "abcdefghijklmnopqrstuvwxyz"
                "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                "0123456789"
                "!@#$%^&*()-=_+|{}[];<>,./?:";

        if (length) {
                --length;

                for (size_t n = 0; n < length; n++) {
                        int pos = rand() % (int) (sizeof charset - 1);
                        str[n] = charset[pos];
                }

                str[length] = '\0';
        }

        return str;
}

static char *random_string(size_t length)
{
     char *s = xmalloc(length + 1);

     generate_random_string(s, length);

     return s;
}

static void cleanup_db_dir(void)
{
        recursive_rm(DBNAME);
        free(DBNAME);
        DBNAME = NULL;
}

static uint64_t get_time_now(void)
{
        struct timeval tv;
        gettimeofday(&tv, NULL);

        return tv.tv_sec * 1000000 + tv.tv_usec;
}

static void print_warnings(void)
{
}

static void print_environment(void)
{
        fprintf(stderr, "Zeroskip:       version %s\n", ZS_VERSION);

#if defined(LINUX)
        time_t now = time(NULL);
        fprintf(stderr, "Date:           %s", ctime(&now));;

        FILE *cpuinfo = fopen("/proc/cpuinfo", "r");
        if (cpuinfo != NULL) {
                char line[1000];
                int num_cpus = 0;
                cstring cpu_type;
                cstring cache_size;

                cstring_init(&cpu_type, 0);
                cstring_init(&cache_size, 0);

                while (fgets(line, sizeof(line), cpuinfo) != NULL) {
                        const char *sep = strchr(line, ':');
                        cstring key, val;

                        if (sep == NULL)
                                continue;

                        cstring_init(&key, 0);
                        cstring_init(&val, 0);

                        cstring_add(&key, line, (sep - 1 - line));
                        cstring_trim(&key);

                        cstring_addstr(&val, (sep + 1));
                        cstring_trim(&val);

                        if (strcmp("model name", key.buf) == 0) {
                                ++num_cpus;
                                cstring_release(&cpu_type);
                                cstring_addstr(&cpu_type, val.buf);
                        } else if (strcmp("cache size", key.buf) == 0) {
                                cstring_release(&cache_size);
                                cstring_addstr(&cache_size, val.buf);
                        }

                        cstring_release(&key);
                        cstring_release(&val);
                }

                fclose(cpuinfo);

                fprintf(stderr, "CPU:            %d * [%s]\n",
                        num_cpus, cpu_type.buf);
                fprintf(stderr, "CPUCache:       %s\n", cache_size.buf);

                cstring_release(&cpu_type);
                cstring_release(&cache_size);
        }
#endif
}

static void print_header(void)
{
        print_environment();
        print_warnings();
        fprintf(stdout, "------------------------------------------------\n");
}


static size_t do_write_seq(void)
{
        int i;
        int ret;
        struct zsdb *db = NULL;
        size_t bytes = 0;

        /* Open Zeroskip DB */
        ret = zsdb_init(&db, NULL, NULL);
        assert(ret == ZS_OK);
        ret = zsdb_open(db, DBNAME, new_db ? MODE_CREATE : MODE_RDWR);
        assert(ret == ZS_OK);

        zsdb_write_lock_acquire(db, 0);

        for (i = 0; i < NUMRECS; i++) {
                char key[100];
                size_t keylen, vallen;
                char *val;

                snprintf(key, sizeof(key), "%016d", i);
                keylen = strlen(key);
                vallen = keylen * 2;
                val = random_string(vallen);

                ret = zsdb_add(db, (unsigned char *)key, keylen,
                               (unsigned char *)val, vallen, NULL);

                assert(ret == ZS_OK);
                bytes += (keylen + vallen);
        }

        zsdb_write_lock_release(db);
        zsdb_commit(db, NULL);

        /* Close Zeroskip DB */
        ret = zsdb_close(db);
        assert(ret == ZS_OK);
        zsdb_final(&db);


        return bytes;
}

static void do_write_random(void)
{
        printf("do_write_random\n");
}

static int parse_options(int argc, char **argv, const struct option *options)
{
        int option;
        int option_index;

        while ((option = getopt_long(argc, argv, "d:b:h?",
                                     long_options, &option_index)) != -1) {
                switch (option) {
                case 'b':
                        BENCHMARKS = optarg;
                        break;
                case 'd':
                        DBNAME = optarg;
                        break;
                case 'n':
                        NUMRECS = atoi(optarg);
                        break;
                case 'h':
                        _fallthrough_;
                case '?':
                        usage(basename(argv[0]));
                        exit(option == 'h');
                }
        }

        return 0;
}

static int run_benchmarks(void)
{
        struct str_array benchmarks;
        int i;
        uint64_t start, finish;
        size_t bytes;

        print_header();

        str_array_from_strsplit(&benchmarks, BENCHMARKS, ',');

        for (i = 0; i < benchmarks.count; i++) {
                if (strcmp(benchmarks.datav[i], "writeseq") == 0) {
                        start = get_time_now();
                        bytes = do_write_seq();
                        finish = get_time_now();

                        fprintf(stderr, "writeseq: %zu bytes written in %" PRIu64 " μs.\n",
                                bytes, (finish - start));
                } else if (strcmp(benchmarks.datav[i], "writerandom") == 0) {
                        do_write_random();
                } else {
                        fprintf(stderr, "Unknown benchmark '%s'\n",
                                benchmarks.datav[i]);
                }
        }

        str_array_clear(&benchmarks);
        return 0;
}

int main(int argc, char **argv)
{
        int ret = EXIT_SUCCESS;
        int seed = 1103515245;

        if (argc == 0) {
                usage(basename(argv[0]));
                ret = EXIT_FAILURE;
                goto done;
        }

        /* Random Seed */
        srand(time(NULL) * seed);

        ret = parse_options(argc, argv, long_options);

        if (BENCHMARKS == NULL) {
                fprintf(stderr, "No benchmarks specified.\n");
                ret = EXIT_FAILURE;
                usage(basename(argv[0]));
                goto done;
        }

        if (DBNAME == NULL) {
                new_db = 1;
                DBNAME = create_tmp_dir_name();
                assert(DBNAME != NULL);
                printf("Creating a new DB: %s\n", DBNAME);
        } else {
                printf("Using existing DB: %s\n", DBNAME);
        }

        ret = run_benchmarks();

        if (new_db) {
                cleanup_db_dir();
        }

done:
        exit(ret);
}
