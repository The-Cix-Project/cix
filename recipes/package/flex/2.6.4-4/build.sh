#
# flex -- the fast lexical analyzer generator. Same dev-toolchain
# family as bison.recipe -- lex/yacc are the classic pairing, and this
# project's own iproute2.recipe already depends on a real bison at
# build time (staged via test/test_image_fixture.c's own toolchain
# extras), though not flex.
#
# Source is the real GitHub Releases distribution tarball (upstream's
# own canonical release point, confirmed to already carry a
# pre-generated ./configure -- unlike a bare git-archive snapshot,
# checked directly: a codeload.github.com tag snapshot is a different,
# much smaller, unbuildable-without-flex-itself artifact). Checksum
# verified against Debian's own published .orig.tar.gz for the same
# upstream version -- byte-identical, same sha256. Unchanged from
# 2.6.4.
#
pkg_name="flex"
pkg_version="2.6.4-4"
pkg_source="https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz"
pkg_sha256="e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995"
# flex EXECUTES m4 at runtime -- its scanner skeleton is an m4
# template, so a flex with no m4 on PATH fails on every invocation,
# not just unusual ones. Never declared before; true since the day
# this recipe was written.
pkg_depends="m4"
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not
# documentation -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK, and config.status's own substitution fallback
#   binutils  flex builds libfl.a, which needs ar and ranlib
#   m4        flex's own build runs the flex it just built over its
#             skeleton, which shells straight out to m4
#
# Sufficiency is enforced by the build itself. Minimality is
# review, not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils m4"


# 2.6.4's own bare `CC=tcc ./configure` failed its own regex.h
# AC_CHECK_HEADER probe ("regex.h: present but cannot be compiled" ->
# "checking for regex.h... no" -> "required header not found") --
# the already-documented TCC gap (see daemon/src/logstore.c's own
# include-block comment, and CLAUDE.md): TCC cannot parse glibc's real
# `regexec()` prototype as written (a genuine C99 VLA-in-prototype size
# expression referencing the *next* parameter,
# `regmatch_t __pmatch[_Restrict_arr_ _REGEX_NELTS(__nmatch)]`), but
# the header's own `_REGEX_NELTS` macro already has a real
# `#ifndef __STDC_NO_VLA__` fallback branch for exactly this compiler
# class -- `-D__STDC_NO_VLA__=1` steers it there, which TCC parses
# fine, no hand-redeclaration of any regex.h type/function needed.
# CC_FOR_BUILD is not redundant with CC. flex's configure runs a SECOND
# compiler probe (AX_PROG_CC_FOR_BUILD, for tools it needs to run during
# its own build rather than ship), and that macro does not read CC at
# all -- it looks for gcc, then cc, then cl.exe, and gives up. So with
# CC=tcc accepted and cached a few lines earlier, the same configure
# still stopped with "no acceptable C compiler found in $PATH". It only
# ever passed because a shared build sandbox had gcc sitting in it,
# which means every flex this project installed had its build-time
# helpers compiled by a compiler no recipe named (#109).
pkg_build() {
	CC=tcc CC_FOR_BUILD=tcc CPPFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# flex itself links only against libm/libc (confirmed via ldd) -- no
# extra runtime libraries to stage for the flex binary itself. libfl
# (the flex runtime scanner library, real programs built from a
# flex-generated .c file without -noyywrap link against it via -lfl) is
# kept, including its versioned SONAME symlink chain
# (libfl.so -> libfl.so.2 -> libfl.so.2.0.0, confirmed via `ls -la`),
# the same two-symlinks-plus-real-target pattern this project's other
# recipes already use for their own runtime libs. FlexLexer.h (the
# public C++ scanner interface) is kept too, unlike most other recipes'
# usr/include stripping -- this recipe's whole purpose is letting other
# things be built against it. Static/libtool archives (.a/.la) and docs
# are dropped.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib/libfl.a" "$PKG_DESTDIR/usr/lib/libfl.la"
}
