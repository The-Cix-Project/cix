#
# sed -- GNU sed, the stream editor. A genuinely missing base tool
# confirmed empirically (ADR-0056): a real kernel hostbuild against the
# "dev" toolchain image failed outright the moment its own build
# scripts (GNU Make's own Makefile machinery, scripts/kconfig/
# merge_config.sh) shelled out to `sed`, which this project had never
# packaged at all -- not part of coreutils (a separate GNU project),
# and nothing else in this recipe catalog happened to need it before
# now.
#
# Source is GNU's own canonical ftp.gnu.org release, unchanged from 4.9.
#
pkg_name="sed"
pkg_version="4.9-3"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/sed-4.9-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="0fd1ea98220dc735775bbeb03f86c07a4f10914551d9decd91ccf9567589df78"
# Nothing at runtime: GNU sed links against libc alone.
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
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# 4.9's own bare `CC=tcc ./configure` failed at final link, confirmed
# live against the real "dev" image build: dozens of gnulib helper
# symbols (xnrealloc, c_isalnum, set_binary_mode, ...) reported "defined
# twice" across sed/sed-*.o and lib/libsed.a, plus a parallel batch of
# SELinux shim symbols (getcon, matchpathcon, security_check_context,
# ...) -- gnulib's own no-op fallback definitions for an unavailable
# libselinux, emitted the exact same way. This is the identical, already
# root-caused TCC/gnulib static-inline conformance gap m4.recipe's own
# build.sh documents in full: gnulib's `_GL_INLINE`/`_GL_EXTERN_INLINE`
# machinery trusts a bare `__STDC_VERSION__` >= 199901L to mean real,
# correctly-scoped C99 inline semantics when `__GNUC__` is undefined;
# TCC claims that version but does not implement the "declared inline,
# no non-inline instantiation anywhere" rule correctly, so every
# translation unit that includes the shared gnulib header emits a full,
# strong, globally-visible definition instead of a file-local one --
# the SELinux shims are just more code going through the exact same
# `_GL_INLINE`-tagged path, not a second bug. Fixed identically: define
# `_GL_EXTERN_INLINE_STDHEADER_BUG` to force gnulib's own designed-in
# `static _GL_UNUSED` fallback. Confirmed against the real build
# sandbox: this one define alone cleared every "defined twice" error,
# both the plain-gnulib batch and the SELinux-shim batch together.
#
# The remaining `tcc: error: undefined symbol '__dso_handle'` is the
# same real, environment-specific TCC/glibc CRT gap sysklogd.recipe and
# m4.recipe both already document (this project's real build sandbox on
# 192.168.15.95 has no ambient `__dso_handle` the way a dev workstation
# glibc might) -- fixed the same proven way: a `weak` stub object passed
# as a bare object-file path in LIBS (never `-lxxx`, which m4.recipe's
# own trail already found gets conditionally-extracted away for a
# weak-only archive member on this toolchain).
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
