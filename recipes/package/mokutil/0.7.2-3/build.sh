#
# mokutil -- the tool that enrolls a Machine Owner Key.
#
# This is what stages this project's own Secure Boot signing
# certificate for enrollment during an install: cix-install.c's
# enroll_signing_key() mounts efivarfs and runs `mokutil --import`, so
# that shim will trust the Cix-signed systemd-boot on the next boot
# (ADR-0015). image/src/mkinstalleriso.c stages it, and its whole
# library closure, into the installer image.
#
# Until now it was read straight off whatever machine ran the ISO
# build, at /usr/bin/mokutil, along with libefivar, libkeyutils and
# libcrypt. That is why `POST /v1/system/iso` could not run on a real
# Cix host: none of it is in the control-plane root, and all of it
# happens to be on the development machine. This recipe is the last of
# that closure to get a package -- shim 16.1-1, efivar 39-3,
# keyutils 1.6.3-6 and libxcrypt 4.4.36-4 are the others.
#
# Upstream ships no release tarball, only git tags, so this fetches a
# codeload archive of the tag -- the same thing sbsigntools.recipe does
# for ccan and efivar.recipe for its own source.
#
pkg_name="mokutil"
pkg_version="0.7.2-3"
pkg_source="https://codeload.github.com/lcp/mokutil/tar.gz/refs/tags/0.7.2"
pkg_sha256="839d677c4fc9805f1565703ca32863e4652692c53da66a88ae9b9e30676f9e17"
pkg_artifact_sha256="2ddb5dd2a01700c333d821280edfbc45eeebe7e7b2382df1801ed386c98334bc"
pkg_depends="efivar keyutils libxcrypt openssl"

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#
# The four libraries appear here as well as in pkg_depends because a
# composed build environment is built from pkg_build_depends ALONE:
# their headers and .pc files have to be present to compile and link
# against, not merely installed alongside the result. Same reason
# sbsigntools names gnu-efi/libuuid/binutils-dev twice.
#
#   bash coreutils make tcc libc-dev binutils
#                   the build proper
#   autoconf automake
#                   there is no configure script in the tag archive --
#                   only configure.ac and autogen.sh -- so the autotools
#                   are real build inputs here, not just for maintainers.
#                   NOT libtool: -1 declared it and failed with
#                     declared build tool "libtool" is not installed
#                     anywhere ... install it first
#                   which was the composed-environment mechanism working
#                   exactly as intended -- naming the missing tool rather
#                   than building against something fuller than declared.
#                   Checked instead of installed: configure.ac contains
#                   no LT_INIT, AC_PROG_LIBTOOL or AM_PROG_AR, so
#                   autoreconf never runs libtoolize and mokutil builds
#                   plain programs with no shared library of its own.
#                   The declaration was wrong, not the environment.
#   m4              autoconf drives it
#   pkgconf         mokutil finds ALL FOUR of its libraries through
#                   PKG_CHECK_MODULES (openssl, efivar, libkeyutils),
#                   so without this nothing is found and configure stops
#   sed grep gawk findutils diffutils
#                   what an autoconf configure reaches for
#   efivar keyutils libxcrypt openssl
#                   the libraries themselves
pkg_build_depends="bash coreutils make tcc linux-headers binutils autoconf automake m4 pkgconf sed grep gawk findutils diffutils efivar keyutils libxcrypt openssl"
pkg_changelog="0.7.2-3: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# autogen.sh is not run: it ends by invoking ./configure with whatever
# arguments it was given, and it also expects a git checkout. The real
# steps it performs -- autoreconf over configure.ac -- are run directly
# instead, the same posture sbsigntools.recipe takes toward its own
# autogen.sh for the same reason.
#
# --disable-bash-completion: configure looks for the bash-completion
# package through pkg-config and this project has no such package.
# Completions are for interactive shells; nothing invokes mokutil that
# way here -- cix-install.c execs it directly with fixed arguments.
pkg_build() {
	autoreconf -fi
	CC=tcc ./configure --prefix=/usr --disable-bash-completion
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
	test -x "$PKG_DESTDIR/usr/bin/mokutil"
}
