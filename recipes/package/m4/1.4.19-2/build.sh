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
pkg_version="1.4.19-2"
pkg_source="https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.xz"
pkg_sha256="63aede5c6d33b6d9b13511cd0be2cac046f2e70fd0a07aa9573a04a82783af96"
# binutils (ADR/recipe: recipes/package/binutils/2.42-2), for a real,
# known-good ar/ranlib -- see pkg_build()'s own comment for why this
# build genuinely needs one, unlike most recipes in this set.
# Nothing at runtime: m4 links against libc alone.
#
# Through 1.4.19 this said `pkg_depends="binutils"`, which was never
# true -- m4 does not shell out to ar or ld, it needs them to BUILD.
# The field was carrying build dependencies because there was nowhere
# else to put them, and that had a real cost the moment anything else
# was declared honestly: binutils' `ar` genuinely needs libfl at
# runtime, flex genuinely needs m4 at runtime, and with m4 claiming
# binutils as a runtime dependency the three formed a cycle the
# installer refused outright. There is no cycle in the real
# relationships -- only in the mislabelled one.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln throughout configure and make
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK, and config.status's own substitution fallback
#   binutils  the AR=ar RANLIB=ranlib this recipe passes explicitly --
#             gnulib is archived into lib/libm4.a before m4 links
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"

# Root cause and real fix, found by reading gnulib's own generated
# lib/config.h (via config.hin in the release tarball) directly rather
# than guessing further: `tcc`'s final link of `src/m4` against
# `../lib/libm4.a` used to report dozens of gnulib helper symbols
# (xnmalloc, c_toupper, mb_copy, xsum, ...) as "defined twice" --
# confirmed via `nm -A lib/*.o | grep ' T xnmalloc$'` that ~20
# different, correctly-and-uniquely-named .o files (basename.o,
# canonicalize.o, dirname.o, quotearg.o, xalloc-die.o, ...) each
# emitted a real, strong, globally-visible definition of the same
# helper, instead of the file-local one C99 `static inline` should
# produce (`ar t lib/libm4.a | sort | uniq -d` showed zero duplicate
# *member* names, ruling out archive corruption first).
#
# gnulib's own `_GL_INLINE`/`_GL_EXTERN_INLINE` machinery (lib/
# config.hin, generated into every build's config.h) picks its
# linkage strategy from `__STDC_VERSION__`/`__GNUC__` alone when
# `__GNUC__` is undefined: `199901L <= __STDC_VERSION__` is enough to
# select real `inline`/`extern inline` linkage, no further compiler
# identification. TCC defines `__STDC_VERSION__` as a real C99 value
# but does not implement C99 inline/extern-inline semantics correctly
# (a function relying on the "declared inline nowhere given a
# non-inline instantiation" rule should stay purely translation-unit-
# local; TCC instead emits a full external definition regardless) --
# gnulib's own probe has no way to detect this, since it only checks
# for a *claimed* C99 version, never actual inline behavior.
#
# gnulib already ships the exact escape hatch this needs:
# `_GL_EXTERN_INLINE_STDHEADER_BUG`, originally written for a
# different real compiler/libc combination with broken extern-inline
# semantics (see config.hin's own comment, Apple/DragonFly/FreeBSD).
# Defining it (any value; only `defined` is checked) forces gnulib's
# own designed-in safe fallback: `_GL_INLINE`/`_GL_EXTERN_INLINE`
# become `static _GL_UNUSED` -- genuinely `static`, so every
# translation unit keeps its own real, correctly-local copy, and the
# archive-level symbol collision this bug caused simply can't happen
# anymore. Verified locally end to end before touching the real build
# sandbox: a full `configure`+`make` against the real m4-1.4.19
# tarball, real tcc, this one extra define, produces a working `m4`
# binary (`define(GREET, hello world)GREET` expands correctly); `nm -A`
# on the resulting lib/*.o confirms `xnmalloc` is now `t` (local), not
# `T`, in all ~20 files -- the exact, direct fix for the exact,
# confirmed cause, not a workaround for a symptom.
#
# The `tcc: error: undefined symbol '__dso_handle'` seen alongside the
# above was NOT a cascading symptom of the static-inline bug the way
# it first looked (verified live against the real box after the fix
# above landed: the "defined twice" errors were completely gone, only
# this one remained) -- it's a separate, real, environment-specific
# gap, the same class `sysklogd.recipe` already documents (this dev
# sandbox's own host glibc happens to provide `__dso_handle` ambiently,
# masking the question locally; the real build sandbox on 192.168.15.95
# does not). The first attempt at the fix here used `libdso_stub.a`
# (an archive, `-ldso_stub`) -- confirmed via a real `make V=1` capture
# that it genuinely reached the final link command in the right
# position, yet `__dso_handle` still came back undefined. A minimal
# standalone reproduction (tcc main.c -L. -lweak -o test, weak symbol
# in an archive) linked clean, ruling out "TCC never extracts a
# weak-only archive member" as the cause -- the real archive-extraction
# mechanics for this specific case remain unexplained. Switched to
# passing `dso_stub.o` as a *bare object file path* in `LIBS` instead
# of an archive -- unlike `-lxxx`, a plain object file on the link
# line is never conditionally extracted, it's always included, the
# exact same shape `sysklogd.recipe`'s own hand-rolled `tcc` invocation
# already uses successfully. Sidesteps the archive question entirely
# rather than resolving it.
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	CC=tcc AR=ar RANLIB=ranlib CFLAGS="-D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"
	make
}

# Confirmed via ldd against a real build: m4 links against nothing but
# libc -- no extra runtime libraries to stage, unlike most of this
# project's other recipes.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
