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
pkg_version="23.7-6"
pkg_source="https://deb.debian.org/debian/pool/main/p/psmisc/psmisc_23.7.orig.tar.xz"
pkg_sha256="58c55d9c1402474065adae669511c191de374b0871eec781239ab400b907c327"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/psmisc-23.7-5.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="6fe24c27c01af0fbc1a009e80a2bfff30c555e439fa75c7ee28a71f79820bb11"
# ncurses: `watch` and `pstree` use terminal handling, and configure
# refuses outright without it -- "Cannot find tinfo, ncurses or termcap
# libraries". Never declared before, because pkg_seed_image_baseline()
# stages libtinfo.so.6 into every image as part of the runtime floor,
# so the library was always present by accident while the headers and
# link library came from whatever the shared sandbox happened to hold.
pkg_depends="ncurses"
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
#   ncurses   configure links a real test program against it and stops
#             if that fails, so it is needed to build and not only to run
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils ncurses"


# __STDC_NO_VLA__: glibc's <regex.h> declares regexec()'s match array
# with a real C99 VLA-in-prototype whose size references the NEXT
# parameter, and TCC's parser rejects it outright ('__nmatch'
# undeclared). The header already has an #ifndef __STDC_NO_VLA__ branch
# for a compiler without VLA support, and TCC genuinely is one, so
# saying so accurately steers it onto glibc's own supported path -- no
# hand-redeclaration of regex_t/regexec needed. Same fix flex 2.6.4-4
# and daemon/src/logstore.c already carry.
# --disable-harden-flags is psmisc's own documented option, not a
# workaround invented here: by default it adds GCC-family hardening
# (-Wl,-z,relro among them) that TCC rejects as unsupported linker
# options. Those flags harden the produced binary; they are not
# required for it to be correct, and TCC has no equivalent to offer.
# Worth knowing rather than glossing: binaries built by this toolchain
# do not get RELRO or the rest of that set, here or anywhere else --
# that is a property of the compiler, and this option only stops psmisc
# asking for something it cannot have.
pkg_build() {
	CC=tcc CPPFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr --disable-harden-flags
	make -j"$(nproc)"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
