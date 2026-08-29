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
pkg_version="1.6.3-2"
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
pkg_build_depends="bash coreutils make tcc libc-dev binutils sed grep"

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
pkg_build() {
	make CC=tcc LIBDIR=/lib/x86_64-linux-gnu USRLIBDIR=/usr/lib \
	     BUILDFOR=64-bit NO_ARLIB=1 -j"$(nproc)"
}

pkg_install() {
	make CC=tcc LIBDIR=/lib/x86_64-linux-gnu USRLIBDIR=/usr/lib \
	     BUILDFOR=64-bit NO_ARLIB=1 install DESTDIR="$PKG_DESTDIR"

	# keyctl/request-key and the man pages: real, and unused here.
	rm -rf "$PKG_DESTDIR/bin" "$PKG_DESTDIR/sbin" "$PKG_DESTDIR/usr/share"
	rm -rf "$PKG_DESTDIR/etc"

	# The .pc moves to where pkgconf actually looks. Upstream installs
	# it under $(LIBDIR)/pkgconfig, which is /lib/x86_64-linux-gnu here
	# and so outside /usr/lib/pkgconfig:/usr/share/pkgconfig -- and a
	# .pc pkg-config cannot find is the same as no library at all, which
	# is exactly the failure openssl 3.0.20-2 and efivar 39-3 were both
	# written to fix (#174).
	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig/libkeyutils.pc" \
	   "$PKG_DESTDIR/usr/lib/pkgconfig/"
	rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"

	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libkeyutils.so.1"
	test -e "$PKG_DESTDIR/usr/lib/libkeyutils.so"
}
