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

# Two real, distinct gaps found the hard way against the actual build
# sandbox (not reproducible from source inspection alone):
#
# 1. `make -j"$(nproc)"` corrupted `lib/libm4.a`: the real captured
#    build output showed dozens of gnulib helper symbols (xnmalloc,
#    c_toupper, mb_copy, ...) reported "defined twice" by tcc's own
#    linker when it finally tried to link src/m4 against that archive
#    -- the same small block of symbol names repeated verbatim several
#    times over, the signature of concurrent `ar` invocations racing
#    non-atomically on the same archive file under parallel make
#    (Automake's own recursive-make archive rules assume a locking
#    `ar`; nothing here guarantees that). Serial `make` (no `-j`)
#    removes the race entirely -- m4 itself is small enough that the
#    lost parallelism is not a real cost.
# 2. The final `src/m4` link failed separately with `tcc: error:
#    undefined symbol '__dso_handle'` -- the same environment-specific
#    TCC/glibc CRT gap `sysklogd.recipe` already found and fixed (see
#    its own comment); m4's build goes through ordinary Automake-driven
#    linking rather than a hand-rolled tcc invocation, so the fix here
#    is a tiny static archive containing the same weak stub, added to
#    `LIBS` so Automake's own generated link command picks it up for
#    every binary it produces -- no Makefile surgery needed.
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o
	ar rcs libdso_stub.a dso_stub.o

	CC=tcc ./configure --prefix=/usr LIBS="-L$(pwd) -ldso_stub"
	make
}

# Confirmed via ldd against a real build: m4 links against nothing but
# libc -- no extra runtime libraries to stage, unlike most of this
# project's other recipes.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
