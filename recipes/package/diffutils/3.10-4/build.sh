#
# diffutils -- GNU diff/cmp/diff3/sdiff. A genuinely missing base tool
# confirmed empirically (ADR-0056): a real kernel hostbuild against
# the "dev" toolchain image printed repeated non-fatal
# "diff: command not found" warnings from scripts/sync-check.sh (its
# tools/ vs kernel-source ABI-header sync check). Not part of
# coreutils (a separate GNU project), and nothing else in this recipe
# catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="diffutils"
pkg_version="3.10-4"
pkg_source="https://ftp.gnu.org/gnu/diffutils/diffutils-3.10.tar.xz"
pkg_sha256="90e5e93cc724e4ebe12ede80df1634063c7a855692685919bfe60b556c9bd09e"
# Nothing at runtime: diffutils links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln throughout configure and make
#   sed       an autoconf configure rewrites its own output with sed on
#             essentially every substitution it makes
#   grep      the same, for every feature test that greps a compiler or
#             header for a pattern
#   binutils  gnulib is archived into lib/libdiffutils.a before linking
#   gawk      config.status generates every Makefile through awk
#   perl      diffutils regenerates its own man pages during `make`,
#             through the help2man Perl script it bundles. Without perl
#             the shell tries to run it and dies on `use Text::Tabs`.
#             The man pages are then deleted at install like every
#             other recipe's here, so this dependency buys artifacts
#             this project discards -- but the rule is driven by
#             $(SRC_VERSION_C), which is generated during the build and
#             is therefore always newer than the tarball's shipped
#             copies, so defeating it would mean racing make on
#             timestamps. Declaring the tool the build genuinely runs
#             is the honest answer; skipping man page generation
#             properly is a separate change to make deliberately.
#
# That last entry is measured, not assumed. Earlier autotools recipes
# here declared gawk on the reasoning that AC_PROG_AWK exists -- true,
# but not the same as the package needing it, and "no extras" is the
# half of this property that nothing enforces. So 3.10-2 left it out
# deliberately, as an experiment the build environment is able to
# settle: it failed with `./config.status: line 2898: awk: command not
# found` / `config.status: error: could not create Makefile`.
#
# So awk is genuinely required by any autoconf-generated configure, and
# genuinely NOT required by a package with no configure at all (libcap
# 2.78-2 declares no gawk and builds). Both halves now rest on a build
# that was actually run rather than on reasoning about autoconf.
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils perl"

# src/diff.c initializes a char array from a PARENTHESIZED string
# literal:
#
#     static char const C_ifdef_group_formats[]
#       = (/* UNCHANGED */ "%=" "\0" ... );
#
# TCC rejects that -- "character array initializer must be a literal,
# optionally enclosed in braces" -- and TCC is right. C99 6.7.9p14 says
# an array of character type may be initialized by a character string
# literal, optionally in braces; a parenthesized expression is neither.
# GCC and Clang accept it as an extension, which is why this has never
# been noticed anywhere else, and why diffutils has never once built in
# this project: the recipe has said CC=tcc since it was written and the
# package is installed in no image at all.
#
# The parentheses are pure grouping around a concatenation of commented
# string literals -- removing them leaves a byte-identical initializer.
# Verified by exercising the code they belong to rather than by
# inspection: `diff -D FOO` (the only consumer of this array) emits the
# correct #ifndef/#else/#endif output from the patched build.
#
# The real fix is a compiler that accepts the GCC extension, which a
# newer TCC does; that is the toolchain bootstrap's problem, not this
# recipe's, and it is tracked rather than assumed away.
pkg_build() {
	grep -q '= (/\* UNCHANGED \*/' src/diff.c || {
		echo "diffutils: the parenthesized initializer is gone -- re-check this patch" >&2
		exit 1
	}
	sed -i -e 's|= (/\* UNCHANGED \*/|= /* UNCHANGED */|' \
	       -e 's|"#endif /\* @ \*/\\n");|"#endif /* @ */\\n";|' src/diff.c

	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale"
}
