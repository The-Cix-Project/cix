#
# make -- GNU Make. The most foundational dev-toolchain recipe in this
# set after m4/binutils: every other autotools recipe in this project
# (and anything a user builds inside a real "dev" image) shells out to
# make itself. Built here using the isolated build container's own
# already-present host make (the standard, unremarkable "build make
# with an existing make" case -- no bootstrapping trick needed, the
# same as gcc.recipe's own single-stage "build gcc with an existing
# gcc").
#
# Source is GNU's own canonical ftp.gnu.org release, checksum computed
# directly from the downloaded bytes (sha256sum), not taken from any
# third party.
#
pkg_name="make"
pkg_version="4.4.1-3"
pkg_source="https://ftp.gnu.org/gnu/make/make-4.4.1.tar.gz"
pkg_sha256="dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/make-4.4.1-3.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="dfd2136d27b1ea7b0f804a16f4d6f0c8a19172bbb6828007c2b073e8ab22fef1"
# Nothing at runtime: GNU make links against libc alone.
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
#   binutils  for `ld`, not for `ar`: -2 declared everything but this and
#             configure stopped with "no acceptable ld found in $PATH"
#             before writing a Makefile at all
#
# Sufficiency is enforced by the build itself -- an environment
# holding exactly these either produces the package or does not.
# Minimality is review, not enforcement (ADR-0199 is explicit that
# this is the honest limit).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# Plain autotools, confirmed directly. MAKEINFO=true skips texinfo doc
# generation, same reasoning as every other recipe in this batch.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# Confirmed via ldd: links against nothing but libc, no extra runtime
# libraries to stage.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share"
}
