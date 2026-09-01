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
pkg_version="1.35-6"
pkg_source="https://ftp.gnu.org/gnu/tar/tar-1.35.tar.gz"
pkg_sha256="14d55e32063ea9526e057fbf35fcabd53378e769787eff7919c3755b02d2b57e"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/tar-1.35-5.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="b5993d0e319fa612e4ed5c9450c32945076f76b4675912a489582332012e0978"
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
# tcc carries the do-while/continue codegen fix (issue #122) that made
# THIS package list and extract exactly one member per archive while
# reporting success -- the recipe never changed, the compiler did. The
# build-tool resolver takes the NEWEST installed tcc (a bare name, like
# every other recipe: pkg_build_depends does not parse an @version pin
# and treats "tcc@0.9.27-7" as a literal package name that matches
# nothing -- see #127), so this resolves to 0.9.27-7 as long as no
# older tcc is the highest installed. The self-test below is the real
# guarantee regardless of which tcc built it: a broken tar cannot pass
# a content round-trip.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils"
pkg_changelog="1.35-6: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"


# Plain autotools, confirmed directly. FORCE_UNSAFE_CONFIGURE=1/
# MAKEINFO=true: the same reasoning coreutils.recipe's own header
# comment already established (this build container runs everything
# as root with no texinfo present).
pkg_build() {
	FORCE_UNSAFE_CONFIGURE=1 CC=tcc ./configure --prefix=/usr MAKEINFO=true
	make -j"$(nproc)" MAKEINFO=true

	# Build-time self-test (issue #122): create a two-file-plus-
	# subdirectory archive with the tar just built, list it, extract
	# it, and compare CONTENT -- exit status alone is demonstrably
	# worthless here: the 1.35-2 binary listed one member per archive
	# and exited 0 on every operation. The list is checked member by
	# member, the extraction by diff -r.
	mkdir -p selftest/probe/sub
	printf one > selftest/probe/a.txt
	printf twotwo > selftest/probe/b.txt
	printf three > selftest/probe/sub/c.txt
	( cd selftest && \
	  ../src/tar -cf probe.tar probe && \
	  ../src/tar -tf probe.tar > listing && \
	  grep -q '^probe/a.txt$' listing && \
	  grep -q '^probe/b.txt$' listing && \
	  grep -q '^probe/sub/c.txt$' listing && \
	  mkdir extract && cd extract && ../../src/tar -xf ../probe.tar && \
	  for f in probe/a.txt probe/b.txt probe/sub/c.txt; do
		[ "$(sha256sum < "../$f")" = "$(sha256sum < "$f")" ] || exit 1
	  done ) || {
		echo "tar: self-test FAILED -- this tar cannot round-trip its own archive" >&2
		exit 1
	}
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
