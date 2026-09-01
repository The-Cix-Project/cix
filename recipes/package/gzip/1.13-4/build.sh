#
# gzip -- GNU gzip, the DEFLATE compressor CLI. A genuinely missing
# base tool confirmed empirically (ADR-0056): a real kernel hostbuild
# against the "dev" toolchain image failed at its very last step --
# compressing the linked vmlinux into arch/x86/boot/compressed/
# vmlinux.bin.gz on the way to a real bzImage -- with
# "gzip: command not found". Not part of coreutils (a separate GNU
# project), and nothing else in this recipe catalog happened to need
# it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="gzip"
pkg_version="1.13-4"
pkg_source="https://ftp.gnu.org/gnu/gzip/gzip-1.13.tar.xz"
pkg_sha256="7454eb6935db17c6655576c2e1b0fabefd38b4d0936e0f87f48cd062ce91a057"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/gzip-1.13-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="c0b645a6134fef2dbbaf487d77f4a736b44d552d383479c218dc97bc5aa414d6"
# Nothing at runtime: gzip links against libc alone.
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
pkg_changelog="1.13-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"


pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info"
}
