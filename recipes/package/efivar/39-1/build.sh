#
# efivar -- libefivar.so.1, the library for reading and writing UEFI
# variables through efivarfs.
#
# Needed because mokutil links it, and mokutil is what enrolls this
# project's own Secure Boot signing certificate as a MOK during an
# install (image/src/cix-install.c's enroll_signing_key(), ADR-0015).
# image/src/mkinstalleriso.c stages both into the installer image, and
# until now read them off whatever machine happened to run it -- which
# is a large part of why `POST /v1/system/iso` cannot run on a real Cix
# host. Measured, not assumed:
#
#   mokutil          libcrypto.so.3  libefivar.so.1  libkeyutils.so.1
#                    libcrypt.so.1  libc.so.6
#   libefivar.so.1   libdl.so.2  libc.so.6
#
# Tier 3: built with gcc, not TCC, and this is a real capability gap
# rather than a flag problem. efivar's src/util.h includes <tgmath.h>
# unconditionally, glibc's <tgmath.h> includes <complex.h>, and TCC
# cannot parse it -- it stops inside bits/cmathcalls.h on the first
# _Complex declaration:
#
#   /usr/include/x86_64-linux-gnu/bits/cmathcalls.h:55:
#       error: ';' expected (got "cacos")
#
# Reduced to a two-line probe to be sure this is TCC and not efivar:
# `#include <tgmath.h>` alone exits 1 under TCC and 0 under gcc. TCC
# has no _Complex support, so no amount of recipe work reaches a
# different outcome. Same shape as the gaps ADR-0211 put gnu-efi and
# python on this list for, and the gcc used is this project's own
# 16.2.0-11 from the artifact cache -- "not TCC" no longer means "not
# ours" (ADR-0170 Tier 3, ADR-0211).
#
# Source is the upstream release tag. rhboot publishes no release
# asset for efivar, only the auto-generated tag tarball, so this
# fetches codeload directly -- the same thing sbsigntools.recipe
# already does for ccan, and for the same reason.
#
pkg_name="efivar"
pkg_version="39-1"
pkg_source="https://codeload.github.com/rhboot/efivar/tar.gz/refs/tags/39"
pkg_sha256="c9edd15f2eeeea63232f3e669a48e992c7be9aff57ee22672ac31f5eca1609a6"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#   bash coreutils  the recipe functions, nproc/mkdir/cp/mv/rm
#   make            the build
#   gcc binutils    the Tier-3 compiler above, and the as/ld it drives
#   libc-dev        headers and CRT
#   sed grep gawk findutils diffutils
#                   efivar's own makefiles and rules.mk drive these
#
# Not declared, having checked: efivar 39 needs no popt (its src
# Makefile names it nowhere -- that dependency belongs to the efibootmgr
# tools, not this one), and no git, because the release tarball ships
# src/include/version.mk carrying a literal VERSION=39 rather than
# deriving it from a repository.
pkg_build_depends="bash coreutils make gcc binutils libc-dev sed grep gawk findutils diffutils"

# ENABLE_DOCS is off by default (the top-level Makefile only adds the
# docs subdir when it is 1), so `make` builds src/ alone and needs no
# mandoc. -march=native is efivar's own default and is deliberately
# overridden: this library is staged into an installer ISO that must
# boot on machines other than the one that built it, and native
# scheduling would tie it to this host's CPU.
pkg_build() {
	make CC=/usr/bin/gcc ARCH_CFLAGS= -j"$(nproc)"
}

# LIBDIR defaults to $(PREFIX)/lib64; this project stages runtime
# libraries at /lib/x86_64-linux-gnu (the same convention openssl
# 3.0.20-2 and pkg_seed_image_baseline() already use), so it is set
# explicitly rather than moved afterwards -- the lesson openssl 3.0.20-2
# recorded about installing where the files belong instead of relocating
# them and leaving a second statement of where they live.
#
# The efivar/efisecdb CLI tools are dropped. Nothing in this project
# invokes them: what is needed here is the shared library mokutil links
# against, and shipping two more binaries would be two more things to
# reason about for no behaviour.
pkg_install() {
	make CC=/usr/bin/gcc install DESTDIR="$PKG_DESTDIR" \
	     LIBDIR=/lib/x86_64-linux-gnu
	rm -rf "$PKG_DESTDIR/usr/bin"
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libefivar.so.1"
}
