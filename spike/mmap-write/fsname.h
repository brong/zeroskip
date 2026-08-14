/* Name the filesystem a benchmark actually ran on, so a pasted result carries
 * its own provenance.
 *
 * These numbers are meaningless without it -- the default path is under /tmp,
 * which is tmpfs on many hosts (making every figure a memory benchmark), and
 * the whole question this spike is asking is filesystem-specific: ZFS, ext4 and
 * APFS handle a truncate over dirty mapped pages three different ways.  One
 * round of "which filesystem was that?" after the fact is one too many.
 *
 * Darwin hands out the name directly; Linux has only a magic number, so the
 * table below covers what we actually run on and prints the raw magic for
 * anything else rather than guessing. */
#ifndef FSNAME_H
#define FSNAME_H

#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <sys/param.h>
#include <sys/mount.h>
#elif defined(__linux__)
#include <sys/vfs.h>
#endif

static void fsname(const char *path, char *out, size_t outlen)
{
#if defined(__APPLE__)
    struct statfs sb;
    if (statfs(path, &sb) == 0)
        snprintf(out, outlen, "%s", sb.f_fstypename);
    else
        snprintf(out, outlen, "?");
#elif defined(__linux__)
    struct statfs sb;
    if (statfs(path, &sb) != 0) { snprintf(out, outlen, "?"); return; }
    switch ((unsigned long)sb.f_type) {
    case 0x2fc12fc1UL: snprintf(out, outlen, "zfs");    break;
    case 0xEF53UL:     snprintf(out, outlen, "ext2/3/4"); break;
    case 0x01021994UL: snprintf(out, outlen, "tmpfs");  break;
    case 0x58465342UL: snprintf(out, outlen, "xfs");    break;
    case 0x9123683EUL: snprintf(out, outlen, "btrfs");  break;
    case 0x6969UL:     snprintf(out, outlen, "nfs");    break;
    default:
        snprintf(out, outlen, "magic 0x%lx", (unsigned long)sb.f_type);
        break;
    }
#else
    (void)path;
    snprintf(out, outlen, "unknown");
#endif
}

/* Call with the benchmark's target path BEFORE creating the file: the name is
 * looked up on the containing directory, which exists either way. */
static void banner(const char *what, const char *path, int nops, int recsize)
{
    char dir[1024], fs[64];
    const char *slash = strrchr(path, '/');
    size_t dlen = slash ? (size_t)(slash - path) : 0;

    if (!dlen || dlen >= sizeof(dir)) snprintf(dir, sizeof(dir), ".");
    else { memcpy(dir, path, dlen); dir[dlen] = '\0'; }

    fsname(dir, fs, sizeof(fs));
    printf("%s: %s on %s, %d ops of %d bytes\n", what, path, fs, nops, recsize);
}

#endif
