#
# libcap -- POSIX.1e capability manipulation: libcap.so.2, the header
# every consumer compiles against, and the setcap/getcap/getpcaps/capsh
# tools. Also libpsx, libcap's own helper for applying a syscall across
# every thread of a process.
#
# This package exists because of issue #110. `coreutils` links against
# libcap (that is how `ls` shows file capabilities) and had been
# staging the BUILD HOST's Debian copy -- `cp -a
# /lib/x86_64-linux-gnu/libcap.so.2 ...` -- a library no recipe built,
# no package owned, and nothing declared. That works only for as long
# as a build container happens to be seeded from a Debian filesystem,
# which under ADR-0199 it no longer is. The alternative was building
# coreutils --disable-libcap and losing a real feature. A distribution
# builds its own libraries.
#
# Source is kernel.org's own canonical libcap2 release directory.
#
pkg_name="libcap"
pkg_version="2.78"
pkg_source="https://mirrors.edge.kernel.org/pub/linux/libs/security/linux-privs/libcap2/libcap-2.78.tar.xz"
pkg_sha256="0d621e562fd932ccf67b9660fb018e468a683d7b827541df27813228c996bb11"
# Nothing at runtime: confirmed with readelf against the built
# libcap.so.2.78 -- libc.so.6 is its only NEEDED entry.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler, pinned via CC/BUILD_CC below
#   libc-dev  headers and libc to compile and link against
#   make      libcap has no configure at all -- it is a plain Makefile
#   bash      pkg_build() runs under it, and the Makefiles shell out
#   coreutils ln/rm/cat/mkdir/install/nproc throughout the build and
#             its install step
#   sed       the Makefiles rewrite generated headers with it
#   grep      the same
#   binutils  objcopy specifically: libcap reads the ELF interpreter
#             path out of a test binary (`objcopy --dump-section
#             .interp`) to embed in cap_magic.o
#   diffutils the build verifies its own generated capshdoc.c against a
#             regenerated copy with `diff -u` and fails if they differ
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils diffutils"

# Three overrides, each for a real reason:
#
#   lib=lib     Make.Rules computes the library subdirectory by running
#               `ldd /usr/bin/ld | grep ld-linux | cut -d/ -f2`, which
#               needs ldd (a glibc shell script this project does not
#               package) and only exists to tell lib from lib64 on
#               multilib distributions. This project installs to
#               /usr/lib, so the answer is known and asking is a
#               dependency for nothing.
#
#   LD=...      Make.Rules defines LD as `$(CC) -Wl,-x -shared`. TCC
#               rejects `-x` outright: `unsupported linker option`.
#               -Wl,-x discards local symbols from the output -- a size
#               and tidiness measure with no effect on the library's
#               API, ABI, or behaviour -- so LD is set to the same
#               command without it.
#
#   MAGIC=      libcap links libcap.so with `-Wl,-e,__so_start`, which
#               makes the shared object directly executable so that
#               running it prints its own version. TCC rejects `-e` the
#               same way. Nothing links against or depends on that; it
#               is a diagnostic convenience. The library, its SONAME,
#               and all 75 exported functions are unaffected.
#
# Both rejected flags are genuine gaps in TCC 0.9.27's linker option
# handling rather than anything wrong with libcap, and are tracked as
# such rather than quietly worked around forever.
#
# GOLANG=no because libcap's optional Go bindings are not something this
# project has any consumer for, and detecting them shells out to `go`.
pkg_build() {
	make CC=tcc BUILD_CC=tcc LD="tcc -shared" MAGIC= GOLANG=no \
	     lib=lib prefix=/usr -j"$(nproc)"

	# libcap's own build produces no shared library at all if SHARED=no
	# is ever inferred, and a static-only libcap would satisfy nothing
	# that links against it. Assert the artifact rather than trust the
	# exit status (the lesson zlib 1.3.2-6 records).
	if [ ! -f libcap/libcap.so.2 ]; then
		echo "libcap: no shared library was built" >&2
		exit 1
	fi
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" CC=tcc BUILD_CC=tcc LD="tcc -shared" \
	     MAGIC= GOLANG=no lib=lib prefix=/usr
	# Static archives and manual pages are dropped the same way every
	# other recipe in this catalog drops them -- nothing here links
	# statically, and no image ships man pages.
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/lib"/*.a
}
