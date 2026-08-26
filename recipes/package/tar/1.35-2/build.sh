#
# tar -- GNU tar. PKG_TAR_BIN's own real use (daemon/src/pkg.c's
# extract_tarball()) is plain archive extraction, no ACL/SELinux
# extended-attribute preservation, no exotic backend -- a real local
# build confirmed this project's own default `./configure` here
# produces a `tar` binary linking against nothing but libc.so.6 (this
# dev sandbox's own Debian-packaged system tar additionally links
# libacl/libselinux/libpcre2 for optional features this project never
# uses; the default from-source build simply doesn't build those in).
#
# tar itself has no compression code at all (confirmed via a real
# `ldd`/ABI trace on this build) -- gzip/bzip2/xz (this recipe set's
# own gzip.recipe/bzip2.recipe/xz.recipe) are separately shelled out
# to via $PATH for any compressed archive, matching mkbootroot.c's own
# documented reasoning for staging all three onto the control-plane
# image alongside tar.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against a second independent mirror (mirrors.kernel.org) --
# byte-identical, same sha256.
#
pkg_name="tar"
pkg_version="1.35-2"
pkg_source="https://ftp.gnu.org/gnu/tar/tar-1.35.tar.gz"
pkg_sha256="14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/tar-1.35-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="69d1beea1642feb533bd8f47721e1e6cfc9d3368bcd5850374d41be5ccf4f6a2"
# Nothing at runtime: tar links against libc alone.
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


# Plain autotools, confirmed directly. FORCE_UNSAFE_CONFIGURE=1/
# MAKEINFO=true: the same reasoning coreutils.recipe's own header
# comment already established (this build container runs everything
# as root with no texinfo present).
pkg_build() {
	FORCE_UNSAFE_CONFIGURE=1 CC=tcc ./configure --prefix=/usr MAKEINFO=true
	make -j"$(nproc)" MAKEINFO=true
}

# Real files from this recipe's own build. usr/libexec/rmt (remote-tape
# support, needs a real rsh/ssh-reachable remote host -- nothing in
# this project ever uses `tar --rmt-command`), info/man pages, and
# locale data are dropped, matching every other recipe's own
# doc-stripping convention.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/libexec" "$PKG_DESTDIR/usr/share"
}
