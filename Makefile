# zeroskip - append-only ordered key-value store
#
# Copyright (c) 2026 Fastmail Pty Ltd
#
# Available under any of: CC0-1.0, 0BSD, or MIT-0
# See LICENSE-CC0, LICENSE-0BSD, or LICENSE-MIT-0 for details.
#
# Versioning: MAJOR.MINOR.PATCH (semver)
#   Bump MAJOR for ABI-breaking changes
#   Bump MINOR for new features (backward compatible)
#   Bump PATCH for bug fixes
VERSION_MAJOR = 0
VERSION_MINOR = 1
VERSION_PATCH = 0
VERSION = $(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)

CC ?= cc
AR ?= ar

# -fno-strict-aliasing is precautionary rather than known to be required: the
# little-endian accessors in zeroskip.c go through memcpy, so they do not
# actually punt on aliasing the way twom.c's integer casts do.  It stays
# because the cost is nil and a future optimisation that does cast the mmap'd
# char * would otherwise be miscompiled silently.
CFLAGS ?= -Wall -Wextra -g -O2 -fno-strict-aliasing
CFLAGS += -std=c99
UNAME_S := $(shell uname -s)

# _GNU_SOURCE / _DARWIN_C_SOURCE for the POSIX bits C99 alone does not expose
# (mmap, fcntl locking, PATH_MAX).  BSDs need neither.
ifeq ($(UNAME_S),Linux)
CFLAGS += -D_GNU_SOURCE
endif
ifeq ($(UNAME_S),Darwin)
CFLAGS += -D_DARWIN_C_SOURCE
endif

# Appended last, so a caller (or the asan target) can add flags without having
# to restate the platform defines above -- restating them is how a sanitiser
# build quietly ends up compiling different code from the one being shipped.
EXTRA_CFLAGS ?=
CFLAGS += $(EXTRA_CFLAGS)

# No external libraries: UUID generation is self-contained, xxHash is vendored,
# and everything else is POSIX.
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib
INCLUDEDIR ?= $(PREFIX)/include
BINDIR ?= $(PREFIX)/bin
PKGCONFIGDIR ?= $(LIBDIR)/pkgconfig

# Shared library names and link flags (platform-specific).
#   SOFILE   - the real versioned library file
#   SONAME   - the compatibility name embedded in the library / linked against
#   LINKNAME - the unversioned developer symlink used at link time
ifeq ($(UNAME_S),Darwin)
SOFILE   = libzeroskip.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH).dylib
SONAME   = libzeroskip.$(VERSION_MAJOR).dylib
LINKNAME = libzeroskip.dylib
SHLIB_LDFLAGS = -dynamiclib \
	-Wl,-install_name,@rpath/$(SONAME) \
	-Wl,-compatibility_version,$(VERSION_MAJOR) \
	-Wl,-current_version,$(VERSION)
else
SOFILE   = libzeroskip.so.$(VERSION_MAJOR).$(VERSION_MINOR).$(VERSION_PATCH)
SONAME   = libzeroskip.so.$(VERSION_MAJOR)
LINKNAME = libzeroskip.so
SHLIB_LDFLAGS = -shared -Wl,-soname,$(SONAME)
endif

.PHONY: all clean check check-noprobe test asan leaks mutate bench corpus install uninstall zeroskip.pc

all: libzeroskip.a $(LINKNAME) zstool zstest zsbench

# Object files
#
# -Wno-unused-function is scaffolding for bottom-up development: the internal
# helpers land section by section, and until the public API is implemented some
# of them genuinely have no caller in the library build.  zstest reaches them
# all (it includes zeroskip.c), so they are not untested, merely not yet wired.
# REMOVE THIS once the PUBLIC API section is complete -- a warning-free build
# without it is the acceptance criterion for that task, and leaving it in would
# hide genuinely dead code from then on.
LIBCFLAGS = $(CFLAGS) -Wno-unused-function

zeroskip.o: zeroskip.c zeroskip.h xxhash.h
	$(CC) $(LIBCFLAGS) -c -o $@ $<

zeroskip.pic.o: zeroskip.c zeroskip.h xxhash.h
	$(CC) $(LIBCFLAGS) -fPIC -c -o $@ $<

# Static library
libzeroskip.a: zeroskip.o
	$(AR) rcs $@ $<

# Shared library
$(LINKNAME): zeroskip.pic.o
	$(CC) $(SHLIB_LDFLAGS) -o $(SOFILE) $< $(LDLIBS)
	ln -sf $(SOFILE) $(SONAME)
	ln -sf $(SONAME) $@

# Tools
zstool: zstool.c zeroskip.h libzeroskip.a
	$(CC) $(CFLAGS) -o $@ zstool.c libzeroskip.a $(LDLIBS)

# zstest #includes zeroskip.c rather than linking the library, so it can assert
# against internal statics -- the interoperability constants of T-2c have to be
# checked against literals at the level where they are computed, not through the
# public API where a compensating pair of bugs would cancel out.  It therefore
# must NOT also link libzeroskip.a.
zstest: zstest.c zeroskip.c zeroskip.h xxhash.h
	$(CC) $(CFLAGS) -o $@ zstest.c $(LDLIBS)

zsbench: zsbench.c zeroskip.h libzeroskip.a
	$(CC) $(CFLAGS) -o $@ zsbench.c libzeroskip.a $(LDLIBS)

# Tests
check: zstest
	./zstest

test: check

# D-14d's first-and-last probe is a search strategy that cannot change any
# answer, so the whole suite must pass with it compiled out (T-5a).  Cheap
# insurance against it quietly becoming load-bearing.
check-noprobe:
	$(MAKE) clean
	$(MAKE) zstest EXTRA_CFLAGS="-DZSI_PROBE_ENDS=0"
	./zstest

# T-3 requires the suite to run under ASan and UBSan.  -O1 keeps frame pointers
# and line numbers useful without making the fuzz cases unbearably slow.
asan:
	$(MAKE) clean
	$(MAKE) zstest EXTRA_CFLAGS="-O1 -fsanitize=address,undefined \
		-fno-omit-frame-pointer -fno-sanitize-recover=all"
	./zstest

# Leak check.  AddressSanitizer includes LeakSanitizer on Linux but NOT on macOS,
# so `make asan` alone proves nothing about leaks there -- hence the split.
#
# Both variants rebuild from clean, for the same reason `asan` does: every target
# here produces a binary called `zstest`, so make cannot tell an instrumented one
# from a plain one and will happily reuse whichever was built last.  Skipping the
# clean made `make leaks` inspect an ASan build and fail with "malloc replacement
# library without the required support", which reads like a tooling problem and is
# really a stale binary.
leaks:
	$(MAKE) clean
ifeq ($(UNAME_S),Darwin)
	$(MAKE) zstest
	MallocStackLogging=1 leaks --atExit -- ./zstest
else
	$(MAKE) zstest EXTRA_CFLAGS="-O1 -fsanitize=address -fno-omit-frame-pointer"
	ASAN_OPTIONS=detect_leaks=1 ./zstest
endif

# Verify the suite can actually fail, by introducing the bugs it guards against.
# Slow (one full rebuild per mutant), so it is not part of `make check`.
mutate:
	./tests/mutate.sh

# Benchmarks
bench: zsbench
	./zsbench --selftest
	./zsbench -n 2000 --reps 2

# Regenerate the language-neutral golden corpus (T-0).  The checked-in bytes
# are the contract, so this target exists to add cases, not to paper over a
# diff: if it changes an existing case, that is a format change.
corpus: zstool
	./tests/gencorpus.sh

# Install
install: libzeroskip.a $(LINKNAME) zstool zeroskip.pc
	install -d $(DESTDIR)$(LIBDIR)
	install -m 644 libzeroskip.a $(DESTDIR)$(LIBDIR)/
	install -m 755 $(SOFILE) $(DESTDIR)$(LIBDIR)/
	ln -sf $(SOFILE) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/$(LINKNAME)
	install -d $(DESTDIR)$(INCLUDEDIR)
	install -m 644 zeroskip.h $(DESTDIR)$(INCLUDEDIR)/
	install -d $(DESTDIR)$(BINDIR)
	install -m 755 zstool $(DESTDIR)$(BINDIR)/
	install -d $(DESTDIR)$(PKGCONFIGDIR)
	install -m 644 zeroskip.pc $(DESTDIR)$(PKGCONFIGDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libzeroskip.a
	rm -f $(DESTDIR)$(LIBDIR)/$(SOFILE)
	rm -f $(DESTDIR)$(LIBDIR)/$(SONAME)
	rm -f $(DESTDIR)$(LIBDIR)/$(LINKNAME)
	rm -f $(DESTDIR)$(INCLUDEDIR)/zeroskip.h
	rm -f $(DESTDIR)$(BINDIR)/zstool
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/zeroskip.pc

# pkg-config file (generated)
zeroskip.pc:
	@echo 'prefix=$(PREFIX)' > $@
	@echo 'libdir=$(LIBDIR)' >> $@
	@echo 'includedir=$(INCLUDEDIR)' >> $@
	@echo '' >> $@
	@echo 'Name: zeroskip' >> $@
	@echo 'Description: Append-only ordered key-value store' >> $@
	@echo 'Version: $(VERSION)' >> $@
	@echo 'Libs: -L$${libdir} -lzeroskip $(LDLIBS)' >> $@
	@echo 'Cflags: -I$${includedir}' >> $@

clean:
	rm -f *.o libzeroskip.a libzeroskip.so* libzeroskip.*.dylib libzeroskip.dylib
	rm -f zstool zstest zsbench zeroskip.pc
	rm -rf zstool.dSYM zstest.dSYM zsbench.dSYM
