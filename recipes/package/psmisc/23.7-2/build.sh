#
# psmisc -- fuser/killall/pstree/peekfd, the standard small-utilities
# set for the jump box's own process-management diagnostics.
#
# Source is Debian's own ".orig.tar.xz" (the exact, unmodified
# upstream release, same convention iputils.recipe's own comment
# already established) rather than GitLab's raw tag archive -- the
# first build attempt against that tag archive failed running
# ./autogen.sh: automake's own installed shebang couldn't execute
# cleanly in this project's build sandbox (real toolchain gap, not a
# psmisc bug), so this uses a real release tarball that already ships
# a pre-generated ./configure instead of needing autoreconf at all.
#
pkg_name="psmisc"
pkg_version="23.7-2"
pkg_source="https://deb.debian.org/debian/pool/main/p/psmisc/psmisc_23.7.orig.tar.xz"
pkg_sha256="58c55d9c1402474065adae669511c191de374b0871eec781239ab400b907c327"
# Nothing at runtime: psmisc links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from exactly
# these and nothing else, so this list is not documentation -- it is
# the environment.
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
#   gawk      config.status generates every Makefile through awk --
#             measured, not assumed: a build environment without it
#             fails at `config.status: line NNNN: awk: command not
#             found` (see diffutils 3.10-3)
#   binutils  gnulib is archived into a static convenience library
#             before the final link, which needs ar and ranlib
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
