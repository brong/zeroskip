/* zeroskip.c - append-only ordered key-value store
 *
 * A directory of immutable and append-only files, with lock-free readers and a
 * single writer.  The design rests on one invariant, without exception:
 * nothing is ever written except by appending to a file or by creating a new
 * file.  No file is ever modified in place or truncated, and there is no
 * mutable object of any kind - no manifest, no shared cache.
 *
 * The format and protocol are specified in
 * doc/specification.md.  Requirement labels in
 * the comments below (F-n format, D-n database, C-n concurrency, R-n recovery,
 * A-n API, G-n guarantee) cite that document, and doc/conformance.md maps each
 * to the test that enforces it.
 *
 * Sections appear in dependency order: each may only call downwards into those
 * above it.
 *
 * See LICENSE.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "zeroskip.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

/********** TUNING *************/

/********** LIBRARY SUPPORT *************/

/********** COMPARATORS *************/

/********** CHECKSUMS *************/

/********** FILENAMES *************/

/********** FILE HEADER *************/

/********** RECORDS *************/

/********** POINTER SECTION *************/

/********** FILE OBJECT *************/

/********** UNORDERED FILE *************/

/********** PRIVATE INDEX *************/

/********** PER-FILE CURSOR *************/

/********** FILE SET *************/

/********** SNAPSHOT *************/

/********** FILE LOCKING *************/

/********** READ PATH *************/

/********** WRITE PATH *************/

/********** CONVERSION *************/

/********** REPACK *************/

/********** CONSISTENCY *************/

/********** OPEN AND CLOSE *************/

/********** PUBLIC API *************/

const char *zs_strerror(int r)
{
    switch (r) {
    case ZS_OK:           return "success";
    case ZS_DONE:         return "iteration complete";
    case ZS_EXISTS:       return "record already exists";
    case ZS_IOERROR:      return "I/O error";
    case ZS_INTERNAL:     return "internal error";
    case ZS_LOCKED:       return "database is locked";
    case ZS_NOTFOUND:     return "record not found";
    case ZS_READONLY:     return "database is read-only";
    case ZS_BADFORMAT:    return "bad file format";
    case ZS_BADUSAGE:     return "bad usage";
    case ZS_BADCHECKSUM:  return "checksum mismatch";
    case ZS_FULL:         return "generation space exhausted";
    case ZS_AGAIN:        return "file set changed, retry";
    }

    return "unknown error";
}
