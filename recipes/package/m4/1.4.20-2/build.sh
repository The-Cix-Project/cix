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
pkg_version="1.4.20-2"
pkg_source="https://mirrors.kernel.org/gnu/m4/m4-1.4.20.tar.xz"
pkg_sha256="e236ea3a1ccf5f6c270b1c4bb60726f371fa49459a8eaaebc90b216b328daf2b"

# No pkg_artifact_sha256 here: nothing has been published for this
# version yet, so the artifact tier (ADR-0122) is skipped and every
# install builds from source. A checksum goes in only once a real Cix
# host has produced and published those exact bytes -- never carried
# forward from an earlier version.
#
# Nothing at runtime: m4 links against libc alone (confirmed via ldd
# against a real build). The tools it needs to BUILD are declared in
# pkg_build_depends below, not here.
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
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="1.4.20-2: tell configure the POSIX-2024 spawn addchdir name is present, so gnulib stops redeclaring it over glibc 2.44's header alias (#198)"

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
#
# 1.4.20-1 (#198) was an upstream bump, on the theory that 1.4.19's
# vendored gnulib simply predated glibc 2.44. That theory was wrong.
# 1.4.20 failed identically (same message, different line number:
# ./spawn.h:1507 rather than :1445), which is what forced the real
# diagnosis below. The bump is kept -- 1.4.20 is the current release and
# there is no reason to go back -- but it is not the fix.
#
#   ./spawn.h:1507: error: incompatible types for redefinition of
#       'posix_spawn_file_actions_addchdir'
#
# The real cause, confirmed directly against our own built glibc 2.44
# rather than inferred:
#
#   readelf -sDW lib/x86_64-linux-gnu/libc.so.6 \
#     | grep posix_spawn_file_actions_add
#     -> posix_spawn_file_actions_addchdir_np      (and only _np)
#
# glibc 2.44 exports the POSIX-2024 name `posix_spawn_file_actions_addchdir`
# as a HEADER-LEVEL ALIAS ONLY -- spawn.h declares it with __REDIRECT_NTH
# pointing at the real, exported `..._np` symbol. There is no symbol of
# that name in libc.so.6 at all.
#
# autoconf's AC_CHECK_FUNC link probe deliberately declares the function
# itself (`char posix_spawn_file_actions_addchdir();`) rather than
# including the header, precisely so a macro definition can't fake a
# result. Here that design decision backfires: bypassing the header also
# bypasses the asm alias, so the probe links against a bare name that
# genuinely does not exist, and concludes the function is ABSENT --
# ac_cv_func_posix_spawn_file_actions_addchdir=no.
#
# configure then takes this branch (configure:53558):
#
#   if test $ac_cv_func_posix_spawn_file_actions_addchdir = yes; then
#     REPLACE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR=1
#   else
#     HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDCHDIR=0
#   fi
#
# and HAVE=0 makes gnulib's generated spawn.h emit its OWN prototype
# (_GL_FUNCDECL_SYS, lib/spawn.in.h:967) after #include_next-ing the
# system one -- a plain declaration landing on top of glibc's aliased
# declaration of the same name. That is the collision. It is not about
# gnulib being old: 1.4.19 and 1.4.20 hit it identically, and any
# gnulib-based package using this module will hit it against glibc 2.44.
#
# The fix is to correct the probe's answer, not to work around its
# consequence. The function IS available to any translation unit that
# includes <spawn.h>, which is the only way C code may legitimately call
# it -- so `yes` is the truthful answer, and autoconf's own cache
# variable is the designed-in mechanism for supplying one a probe cannot
# determine for itself.
#
# Saying yes also lands on the strictly safer of the two branches:
# REPLACE=1 makes gnulib #define the name to rpl_posix_spawn_file_actions_addchdir
# and compile its own implementation (lib/spawn_faction_addchdir.c, via
# GL_COND_OBJ_SPAWN_FACTION_ADDCHDIR). So m4 ends up calling gnulib's own
# code, never glibc's alias -- the header conflict cannot recur, and the
# result does not depend on the alias existing at all.
#
# Only addchdir needs this. Its sibling addfchdir is aliased the same way
# in glibc 2.44's spawn.h, but m4 does not enable that gnulib module
# (HAVE_POSIX_SPAWN_FILE_ACTIONS_ADDFCHDIR is never zeroed in configure),
# so its declaration block is never emitted.
#
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	CC=tcc AR=ar RANLIB=ranlib CFLAGS="-D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ac_cv_func_posix_spawn_file_actions_addchdir=yes \
	    ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"

	# Gate the fix rather than trusting it: if configure ever stops
	# taking the REPLACE branch, gnulib is redeclaring the function
	# again and the build should fail here with a clear reason, not
	# 1500 lines later inside a generated header.
	if ! grep -q 'rpl_posix_spawn_file_actions_addchdir' lib/spawn.h; then
		echo "m4: configure did not take the REPLACE branch for" \
		     "posix_spawn_file_actions_addchdir -- see this recipe's" \
		     "own comment for why that matters (#198)" >&2
		exit 1
	fi

	make
}

# Confirmed via ldd against a real build: m4 links against nothing but
# libc -- no extra runtime libraries to stage, unlike most of this
# project's other recipes.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
