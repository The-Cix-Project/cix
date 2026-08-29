#
# keyutils -- libkeyutils.so.1, the kernel key-management library.
#
# Needed because mokutil links it, and finds it only through
# pkg-config: mokutil's configure.ac carries
#   PKG_CHECK_MODULES(LIBKEYUTILS, [libkeyutils >= 1.5])
# alongside its openssl and efivar checks. mokutil is what enrolls this
# project's Secure Boot signing certificate during an install
# (image/src/cix-install.c, ADR-0015), and image/src/mkinstalleriso.c
# stages it into the installer image -- previously by reading it, and
# its whole library closure, off whatever machine happened to run the
# build. Packaging that closure is what lets an ISO be built on a Cix
# host at all.
#
# Only the library is installed here. keyctl and request-key are real
# tools but nothing in this project invokes them, and a package should
# ship what something needs rather than everything upstream builds.
#
pkg_name="keyutils"
pkg_version="1.6.3-6"
# Fetched from Debian's source archive, not kernel.org's cgit snapshot
# endpoint. -1 used the latter and failed on the box with a bare
#   curl: (22) The requested URL returned error: 404
# while the same URL had fetched successfully from the development
# machine minutes earlier -- and then began returning 404 there too.
# cgit generates snapshot tarballs on demand and does not keep them, so
# it is not a source a recipe can depend on: a build that works today
# fails tomorrow for no reason visible in the recipe.
#
# Debian's orig.tar.gz is BYTE-IDENTICAL to that snapshot (same
# sha256, confirmed by fetching both and comparing), so this changes
# where the bytes come from and nothing about the bytes -- the same
# reasoning libc-dev.recipe records for moving off ftp.gnu.org: the
# checksum approves a byte sequence, not a hostname, so a stable mirror
# needs no new trust.
pkg_source="http://deb.debian.org/debian/pool/main/k/keyutils/keyutils_1.6.3.orig.tar.gz"
pkg_sha256="a61d5706136ae4c05bd48f86186bcfdbd88dd8bd5107e3e195c924cfc1b39bb4"
pkg_artifact_sha256="d3f2645454a8083a08706202656643447a5840897def4189f53306803c1aa988"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#   bash coreutils  the recipe functions, plus the install/mkdir/ln/rm
#                   this Makefile's own install target drives
#   make            the build
#   tcc libc-dev    plain C with no GCC-specific construct in it --
#                   TCC by default, per the 3-tier policy, and it needs
#                   no exception here
#   binutils        ld, and ar for the static archive the build makes
#                   on its way even with NO_ARLIB=1
#   sed grep        the Makefile substitutes its .pc template with sed
#                   and greps in its own detection logic
#
#   gcc         the C compiler, and the C++ one for a header check. -3
#               used tcc and every object compiled cleanly -- the gap is
#               at the LINK: keyutils builds its shared library with
#                 -Wl,--version-script,version.lds
#               and TCC rejects that outright,
#                 tcc: error: unsupported linker option '--version-script'
#               confirmed with a three-line probe (a trivial .so with a
#               version script fails under TCC and links under gcc), so
#               it is TCC's gap and not this package's.
#
#               The Makefile does expose LIBVERS, so the version script
#               could have been dropped to keep TCC. That was rejected:
#               it would ship a libkeyutils exporting every symbol
#               instead of the versioned set upstream publishes -- a
#               real change to a shipped library's ABI surface, made to
#               satisfy a compiler preference rather than a requirement.
#               Tier 3 with this project's own gcc 16.2.0-11 preserves
#               upstream's intent exactly, which is the point of the
#               exception list (ADR-0170, ADR-0211).
#
#               The C++ header check keyutils' `all` target
#               depends on `cxx`, whose rule is
#                 $(CXX) -x c++-header -fsyntax-only keyutils.h
#               with no knob to disable it. -2 failed there with
#                 make: g++: No such file or directory
#               after tcc had already built every .o successfully. The
#               check is a real one worth running (it verifies the
#               installed header is usable from C++), so it is given a
#               real compiler -- this project's own gcc 16.2.0-11, which
#               ships usr/bin/g++ and cc1plus -- rather than stubbed out
#               with something that trivially succeeds and verifies
#               nothing.
pkg_build_depends="bash coreutils make tcc libc-dev binutils sed grep gcc"

# LIBDIR, USRLIBDIR and BUILDFOR are all passed explicitly, and each
# one is load-bearing rather than tidiness:
#
#   LIBDIR      guarded by `ifeq ($(origin LIBDIR),undefined)`, and its
#               fallback is `ldd /usr/bin/make | grep /libc[.]`. No image
#               in this project has ldd -- only a real distribution ships
#               that wrapper script (CLAUDE.md records this) -- so
#               leaving it undefined would run a command that does not
#               exist and derive the library directory from empty output.
#   USRLIBDIR   same guard, derived from LIBDIR when absent.
#   BUILDFOR    NOT guarded: `BUILDFOR := $(shell file /usr/bin/make ...)`
#               runs unconditionally, and `file` is not present either.
#               Naming it on the command line means make ignores that
#               assignment and never evaluates it.
#
# Setting LIBDIR to /lib/x86_64-linux-gnu puts the shared library where
# every other package in this set stages one. USRLIBDIR stays /usr/lib,
# which is where the Makefile puts the -ldl-style development symlink
# and what it writes into the .pc's own libdir -- so `-L/usr/lib
# -lkeyutils` resolves through that symlink to the real object.
# Builds the library and the C++ header check, not `all`.
#
# -4 built `all` and got everything -- libkeyutils, keyctl, request-key
# -- before dying on the last binary:
#   ld: cannot find -lresolv
#   make: *** [Makefile:163: key.dns_resolver] Error 1
# key.dns_resolver is a kernel request-key helper for DNS lookups.
# Nothing in this project invokes it, and libresolv is in no Cix package
# at all: glibc has it, but libc-dev cannot stage it because libc-dev's
# own build environment is the PREVIOUS libc-dev, which does not have it
# either -- the same circularity that made 2.36-7 name its predecessor
# as a build input.
#
# So this builds what it ships. The considered alternative was a stub
# libresolv.a to satisfy the link (the shape CLAUDE.md records for
# __dso_handle, and what glibc itself does for libdl -- an 8-byte
# placeholder archive). Rejected: that fabricates a library purely to
# link a binary this package then discards, which is effort spent
# producing something nobody wants and a symbol resolution nobody has
# checked.
pkg_build() {
	make CC=/usr/bin/gcc CXX=/usr/bin/g++ LIBDIR=/lib/x86_64-linux-gnu USRLIBDIR=/usr/lib \
	     BUILDFOR=64-bit NO_ARLIB=1 libkeyutils.so.1.10 cxx -j"$(nproc)"

	# Upstream's own generator, not a hand-written .pc -- the version
	# and paths come from the same template `make install` would use.
	make CC=/usr/bin/gcc LIBDIR=/lib/x86_64-linux-gnu USRLIBDIR=/usr/lib \
	     BUILDFOR=64-bit NO_ARLIB=1 pkgconfig
}

pkg_install() {
	# The library install steps upstream's own `install` target performs
	# for the shared library, and only those -- its `install` depends on
	# `all`, which is what pulls in the key.dns_resolver link that cannot
	# be satisfied here. Same layout, same symlink shape:
	#   the real object and its SONAME link in LIBDIR, the development
	#   symlink in USRLIBDIR pointing at LIBDIR's SONAME.
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu" \
	         "$PKG_DESTDIR/usr/lib" \
	         "$PKG_DESTDIR/usr/include" \
	         "$PKG_DESTDIR/usr/lib/pkgconfig"

	cp -a libkeyutils.so.1.10 "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	ln -s libkeyutils.so.1.10 "$PKG_DESTDIR/lib/x86_64-linux-gnu/libkeyutils.so.1"
	ln -s /lib/x86_64-linux-gnu/libkeyutils.so.1 "$PKG_DESTDIR/usr/lib/libkeyutils.so"
	cp -a keyutils.h "$PKG_DESTDIR/usr/include/"

	# Straight to /usr/lib/pkgconfig, where pkgconf actually looks --
	# upstream installs under $(LIBDIR)/pkgconfig, which is outside its
	# search path here, and a .pc pkg-config cannot find is the same as
	# no library at all (#174, as with openssl 3.0.20-2 and efivar 39-3).
	cp -a libkeyutils.pc "$PKG_DESTDIR/usr/lib/pkgconfig/"

	# -L, not -e, for the development symlink. `test -e` FOLLOWS a
	# symlink, and this one points at the absolute /lib/x86_64-linux-gnu/
	# libkeyutils.so.1 -- a path that is correct on an installed image and
	# does not resolve inside PKG_DESTDIR, where nothing is mounted at /.
	# -5 built the library perfectly and then failed here, silently,
	# because `test` says nothing when it fails: an assertion added to
	# catch a missing file rejected a correctly-created one.
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libkeyutils.so.1.10"
	test -L "$PKG_DESTDIR/lib/x86_64-linux-gnu/libkeyutils.so.1"
	test -L "$PKG_DESTDIR/usr/lib/libkeyutils.so"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libkeyutils.pc"
}
