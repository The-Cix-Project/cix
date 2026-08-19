#
# grep -- GNU grep, the pattern-matching filter. A genuinely missing
# base tool confirmed empirically (ADR-0056): a real kernel hostbuild
# against the "dev" toolchain image failed outright the moment its
# own build scripts (scripts/kconfig/merge_config.sh) shelled out to
# `grep`, which this project had never packaged at all -- like sed, a
# separate GNU project, not part of coreutils, and nothing else in
# this recipe catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release, unchanged from
# 3.11.
#
pkg_name="grep"
pkg_version="3.11-2"
pkg_source="https://ftp.gnu.org/gnu/grep/grep-3.11.tar.gz"
pkg_sha256="1f31014953e71c3cddcedb97692ad7620cb9d6d04fbdc19e0d8dd836f87622bb"
pkg_depends=""

# 3.11's own bare `CC=tcc ./configure` failed the same, already
# root-caused way m4.recipe/sed 4.9-2/coreutils 9.11-3 all document:
# TCC's C99 static-inline conformance gap makes gnulib emit full,
# globally-visible definitions (to_uchar, xnrealloc, imbrlen, mb_clen,
# the c_is*/c_to* ctype shims, mb_width_aux/mb_copy/mbuiter_multi_*)
# instead of file-local ones, producing dozens of "defined twice"
# errors across grep.o/kwsearch.o/kwset.o/searchutils.o and
# lib/libgreputils.a. Same fix: `_GL_EXTERN_INLINE_STDHEADER_BUG=1`
# forces gnulib's own designed-in `static _GL_UNUSED` fallback.
#
# The accompanying `tcc: error: undefined symbol '__dso_handle'` is the
# same real, environment-specific bare-tcc-link CRT gap sysklogd/m4/
# sed/coreutils all already document -- fixed the same proven way: a
# `weak` stub object passed as a bare object-file path in LIBS (never
# `-lxxx`, which m4.recipe's own trail already found gets
# conditionally extracted away for a weak-only archive member on this
# toolchain).
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	CC=tcc CFLAGS="-D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale" "$PKG_DESTDIR/usr/lib"/*.a
}
