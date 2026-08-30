#
# bc -- GNU bc, the arbitrary-precision calculator language. A
# genuinely missing base tool confirmed empirically (ADR-0056): a real
# kernel hostbuild against the "dev" toolchain image failed generating
# include/generated/timeconst.h (kernel/time/timeconst.bc, computing
# HZ-related time constants) with "bc: command not found". Not part of
# coreutils (a separate GNU project), and nothing else in this recipe
# catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="bc"
pkg_version="1.08.1-3"
pkg_source="https://ftp.gnu.org/gnu/bc/bc-1.08.1.tar.gz"
pkg_sha256="b71457ffeb210d7ea61825ff72b3e49dc8f2c1a04102bbe23591d783d1bfe996"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/bc-1.08.1-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
# Nothing at runtime: bc links against libc alone.
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
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="1.08.1-3: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"


pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info"
}
