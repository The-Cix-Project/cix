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
pkg_version="3.11-6"
pkg_source="https://ftp.gnu.org/gnu/grep/grep-3.11.tar.gz"
pkg_sha256="1f31014953e71c3cddcedb97692ad7620cb9d6d04fbdc19e0d8dd836f87622bb"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/grep-3.11-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="c8f94c6e1d6c742d5cd800255f98283ad6cf4b2363304db7d3a0e3f1e0765dd4"
# Nothing at runtime: GNU grep links against libc alone (PCRE is not
# enabled by this build).
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not documentation
# -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       an autoconf configure rewrites its own output with sed on
#             essentially every substitution it makes
#   grep      the same, for every feature test that greps a compiler or
#             header for a pattern
#   gawk      AC_PROG_AWK, and config.status falls back to awk whenever a
#             substitution list outgrows what sed can take in one pass
#   binutils  gnulib is built as a static convenience archive first
#             (lib/lib*.a), which needs ar and ranlib, even though
#             nothing static is installed
#
# Sufficiency is enforced by the build itself -- an environment
# holding exactly these either produces the package or does not.
# Minimality is review, not enforcement (ADR-0199 is explicit that
# this is the honest limit).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="3.11-6: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 3.11-5: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"


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
