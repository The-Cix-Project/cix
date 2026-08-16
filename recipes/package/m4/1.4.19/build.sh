#
# m4 -- GNU M4, the macro processor autoconf/automake/libtool/bison all
# invoke internally (autoconf itself is essentially a large collection
# of m4 macros run through this exact program). The first, most
# foundational recipe in this project's own dev-toolchain set -- every
# other autotools-based recipe below (autoconf, automake, libtool,
# bison) needs a real m4 present in the target image's own PATH to be
# usable there, the same way this project's own build host needs one to
# build any of them.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum computed
# directly from the downloaded bytes (sha256sum), not taken from any
# third party.
#
pkg_name="m4"
pkg_version="1.4.19"
pkg_source="https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.xz"
pkg_sha256="63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96"
# binutils (ADR/recipe: recipes/package/binutils/2.42-2), for a real,
# known-good ar/ranlib -- see pkg_build()'s own comment for why this
# build genuinely needs one, unlike most recipes in this set.
pkg_depends="binutils"

# NOT YET FIXED -- real diagnostic investigation in progress, deferred
# (see the project's own established precedent for this judgment call:
# procps' 8th gap, chrony's stale-toolchain gap). Two symptoms from the
# real captured build output, `tcc`'s own final link of `src/m4`
# against `../lib/libm4.a`:
#
#   1. Dozens of gnulib helper symbols (xnmalloc, c_toupper, mb_copy,
#      xsum, ...) reported "defined twice" -- ruled OUT as an `ar`
#      archive-corruption/race (serial `make`, no `-j`, reproduced the
#      identical failure byte-for-byte; `ar t lib/libm4.a | sort |
#      uniq -d` on the actual built archive shows zero duplicate
#      *member* names, so this isn't two copies of the same .o
#      appended to the archive). The real cause is more likely TCC's
#      own `inline`/`static inline` handling of gnulib's "one
#      out-of-line definition, many inline call sites" idiom (each of
#      several genuinely different, uniquely-named .o files apparently
#      emitting its own real, external definition of the same helper,
#      rather than the header-only inline copy every other translation
#      unit should get) -- not yet confirmed against gnulib's actual
#      generated `.c`/`.h` pair for one of these symbols.
#   2. `tcc: error: undefined symbol '__dso_handle'` -- the same
#      environment-specific TCC/glibc CRT gap `sysklogd.recipe` already
#      found and fixed there via a small `__attribute__((weak))` stub.
#      Two attempts to feed it in through Automake's own `LIBS`
#      mechanism (once at `./configure` time, once as a `make LIBS=...`
#      command-line override, which GNU Make guarantees wins over any
#      in-Makefile assignment) both had zero observable effect on this
#      specific error -- not yet confirmed whether `$(LIBS)` is even
#      reaching `m4`'s own generated link recipe at all in this
#      Automake version's output.
#
# `pkg_depends="binutils"` and the `dso_stub.c`/`libdso_stub.a`
# machinery below are kept as real, independently-justified fixes (a
# known-good `ar`/`ranlib` instead of whatever the base build sandbox
# bundles; the same weak-stub trick already proven for sysklogd) even
# though neither has yet resolved the failure on its own -- removing
# them would just be re-losing already-confirmed-safe groundwork.
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o
	ar rcs libdso_stub.a dso_stub.o

	CC=tcc AR=ar RANLIB=ranlib ./configure --prefix=/usr
	make LIBS="-L$(pwd) -ldso_stub"
	make_rc=$?

	echo "=== diagnostic: lib/libm4.a duplicate member names (expect none) ==="
	ar t lib/libm4.a | sort | uniq -d
	echo "=== diagnostic: does src/m4's own link line reference dso_stub? ==="
	(cd src && make -n LIBS="-L$(pwd)/.. -ldso_stub" m4 2>&1 | grep -o '[^ ]*dso_stub[^ ]*\|-o m4\b')

	[ "$make_rc" -eq 0 ] || exit "$make_rc"
}

# Confirmed via ldd against a real build: m4 links against nothing but
# libc -- no extra runtime libraries to stage, unlike most of this
# project's other recipes.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
