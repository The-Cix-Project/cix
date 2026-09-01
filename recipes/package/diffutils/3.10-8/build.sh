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
pkg_version="3.10-8"
pkg_source="https://ftp.gnu.org/gnu/diffutils/diffutils-3.10.tar.xz"
pkg_sha256="90e5e93cc724e4ebe12ede80df1634063c7a855692685919bfe60b556c9bd09e"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/diffutils-3.10-5.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="5a76069537f2fa4e7143f123fb4875b945ddcebc4e64c0f6a498e95c57198be7"
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
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="3.10-8: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 3.10-7: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

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
# dist_man1_MANS= turns off man page generation, which this recipe was
# deleting at install anyway. Not a workaround for a broken tool: it is
# saying plainly that this project does not ship man pages, in the one
# place that decides whether they get built.
#
# 3.10-4 instead declared perl, because diffutils regenerates its man
# pages during `make` through the help2man Perl script it bundles. That
# was accurate and still failed -- the kernel had no CONFIG_BINFMT_SCRIPT
# (issue #114), so `#!/usr/bin/perl` was handed to bash, which reported
# a syntax error on `use Text::Tabs`. Fixing the kernel is the right
# fix for that and is tracked separately; building files we then delete
# was never right regardless, and dropping them removes a real
# dependency rather than hiding one.
pkg_build() {
	grep -q '= (/\* UNCHANGED \*/' src/diff.c || {
		echo "diffutils: the parenthesized initializer is gone -- re-check this patch" >&2
		exit 1
	}
	sed -i -e 's|= (/\* UNCHANGED \*/|= /* UNCHANGED */|' \
	       -e 's|"#endif /\* @ \*/\\n");|"#endif /* @ */\\n";|' src/diff.c

	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true dist_man1_MANS=
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true dist_man1_MANS=
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale"
}
