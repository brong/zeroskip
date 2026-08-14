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

.PHONY: all clean check check-noprobe crashtest conformance stdcheck test asan leaks mutate bench corpus install uninstall zeroskip.pc

all: libzeroskip.a $(LINKNAME) zstool zstest zsbench

# Object files
#
# No -Wno-unused-function.  It was scaffolding while the internal helpers landed
# ahead of their callers, and it earned its keep twice: removing it exposed that
# the writer was rescanning the active file instead of maintaining its index
# incrementally (D-13b), and its checklist tracked what remained.  It is gone now
# that every helper has a caller, because from here on an unused static means dead
# code and should say so.
zeroskip.o: zeroskip.c zeroskip.h xxhash.h
	$(CC) $(CFLAGS) -c -o $@ $<

zeroskip.pic.o: zeroskip.c zeroskip.h xxhash.h
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

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
# The C suite, then zstool's driver contract.  Both, because the tool's line
# format is what a cross-implementation runner compares (T-0a) and a change to it
# breaks other implementations rather than this one.
check: zstest zstool zstest-crash
	@$(MAKE) --no-print-directory stdcheck
	./zstest
	./zstest-crash
	./tests/tool.sh
	./tests/conformance.sh

# The library targets C99 and should keep targeting it: zeroskip.c is VENDORED
# into other projects, so whatever standard it requires, every host build has to
# meet -- and a vendored file demanding a newer -std than its host is the same
# papercut as one demanding a feature macro the host does not set.
#
# What this guards is the other direction: a consumer may build at a newer
# standard, and nothing here may FAIL at one.  Syntax-only, because conformance
# is a front-end question, which keeps the whole tree at about a third of a
# second -- cheap enough to run inside check rather than be a target nobody
# remembers.  -Werror because Cyrus builds that way.
STDCHECK_STDS = c11 c17
STDCHECK_SRCS = zeroskip.c zstool.c zsbench.c zstest.c zstest-crash.c

stdcheck:
	@for std in $(STDCHECK_STDS); do \
	    for f in $(STDCHECK_SRCS); do \
	        $(CC) $(CFLAGS) -std=$$std -Werror -fsyntax-only $$f \
	            || { echo "stdcheck: $$f does not compile under -std=$$std"; exit 1; }; \
	    done; \
	    echo "  -std=$$std: $(words $(STDCHECK_SRCS)) files clean"; \
	done

# T-11: check doc/conformance.md against the spec in both directions, and that
# every test it cites still exists.  A citation is only evidence while the test it
# names is still there.
conformance:
	./tests/conformance.sh

test: check

# T-8/T-8a: crash and sync-failure injection.  Needs ZS_TEST_HOOKS, which routes
# the library's write, fdatasync, rename and unlink through function pointers, so it
# is a separate binary rather than part of zstest -- the hooks must not be present in
# a shipped build.
zstest-crash: zstest-crash.c zeroskip.c zeroskip.h xxhash.h
	$(CC) $(CFLAGS) -o $@ zstest-crash.c $(LDLIBS)

crashtest: zstest-crash
	./zstest-crash

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
	ZS_TEST_NO_FORK=1 MallocStackLogging=1 leaks --atExit -- ./zstest
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
	./zsbench -n 4000 --reps 1

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
	rm -f zstool zstest zstest-crash zsbench zeroskip.pc
	rm -rf zstool.dSYM zstest.dSYM zstest-crash.dSYM zsbench.dSYM
