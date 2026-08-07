# zeroskip

An append-only ordered key-value store: a directory of immutable and
append-only files, with lock-free readers and a single writer.

Nothing is ever written except by appending to a file or by creating a new one.
No file is ever modified in place or truncated, and there is no mutable object
of any kind — no manifest, no shared cache. Readers take no lock and see a fixed
snapshot; a writer never blocks them, and they never block it.

## Build and install

```
    $ make
    $ make check
    $ make install
```

Requires a C99 compiler and POSIX (`mmap`, `fcntl` locking, `/dev/urandom`).
Builds on Linux, macOS and the BSDs with **no external libraries** — xxHash is
vendored and UUID generation is self-contained. There is nothing to configure.

`make install` honours `PREFIX` (default `/usr/local`) and `DESTDIR`, and
installs a `pkg-config` file. `make uninstall` reverses it.

## Using it

```c
#include <zeroskip.h>

struct zs_open_data setup = ZS_OPEN_DATA_INITIALIZER;
struct zs_db *db;

setup.flags = ZS_CREATE;
if (zs_db_open("/path/to/db", &setup, &db) != ZS_OK)
    return -1;

zs_db_store(db, "key", 3, "value", 5, 0);

const char *key, *val;
size_t keylen, vallen;
if (zs_db_fetch(db, "key", 3, &key, &keylen, &val, &vallen, 0) == ZS_OK)
    printf("%.*s\n", (int)vallen, val);

zs_db_close(&db);
```

Every read and write exists in three forms — on the database, on a transaction,
and via a cursor — and all three take flags, so no operation is reachable one
way but not another. The `zs_db_*` forms are wrappers that open an implicit
single-operation transaction; batching many operations into one `zs_txn_*`
transaction amortises the two `fdatasync` calls a commit costs.

Deletion is a store of a `NULL` value. A non-`NULL` zero-length value stores an
empty value, which is a distinct state from an absent key.

## Tools

```
    $ zstool <dir> scan            # every visible pair, in comparator order
    $ zstool <dir> dump            # files, generations, spans, record types
    $ zstool <dir> check           # consistency checks
    $ zstool <dir> repack          # force one repack
```

## Tests

```
    $ make check                   # the whole suite
    $ ./zstest cursor              # tests matching a substring
    $ make asan                    # under AddressSanitizer and UBSan
```

`tests/corpus/` holds a language-neutral golden corpus — data files plus a
portable text description — so any implementation in any language can validate
against the same bytes.

## Specification

[`doc/specification.md`](doc/specification.md)
specifies the on-disk format, the database layout, the concurrency and
durability protocol, and recovery, so that **independent implementations in
different languages interoperate on the same database concurrently**. It is
normative; this C library is one binding of it.

[`doc/overview.md`](doc/overview.md) is a shorter introduction.
[`doc/conformance.md`](doc/conformance.md) maps every requirement in the spec to
the test enforcing it.

The sibling library [`twom`](https://github.com/brong/twom) is a mutable
single-file skiplist. zeroskip suits workloads that are append-heavy, want
readers that never take a lock, and tolerate compaction happening out of band.

## Non-goals

Multi-writer concurrency, cross-database transactions, secondary indexes,
compression, network access, in-place value mutation.

## License

This software is available under any of the following licenses, at your choice:

- [CC0 1.0 Universal](LICENSE-CC0) — public domain dedication
- [Zero-Clause BSD (0BSD)](LICENSE-0BSD)
- [MIT No Attribution (MIT-0)](LICENSE-MIT-0)

The vendored `xxhash.h` is BSD-2-Clause, © Yann Collet.
