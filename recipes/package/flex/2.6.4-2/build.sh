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
pkg_version="2.6.4-2"
pkg_source="https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz"
pkg_sha256="e87aae032bf07c26f85ac0ed3250998c37621d95f8bd748b31f15b33c45ee995"
pkg_depends=""

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
pkg_build() {
	CC=tcc CPPFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr
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
