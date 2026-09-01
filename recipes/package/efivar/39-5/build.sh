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
pkg_version="39-5"
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
pkg_build_depends="bash coreutils make gcc binutils linux-headers sed grep gawk findutils diffutils"
pkg_changelog="39-5: -Wno-error=discarded-qualifiers via ERRORS_GCC -- glibc 2.44's C23 const-generic bsearch makes guid.c's two lookups return const void *, which -Werror rejects (#187 made these headers reachable by retiring libc-dev)"

# ENABLE_DOCS=0. -1 said docs were off by default and that was simply
# wrong -- src/include/defaults.mk line 127 reads `ENABLE_DOCS ?= 1`.
# The library built perfectly and then the whole run died in docs/ on
#   mandoc: command not found
# after every real artifact was already linked. mandoc is not worth
# packaging to produce man pages pkg_install() would discard anyway.
#
# -march=native needs no override, contrary to what -1 also assumed and
# tried to pass. It is HOST_MARCH in defaults.mk and reaches only
# HOST_CFLAGS -- the build-time helpers (makeguids) that run on the
# build machine and are never installed. Confirmed against -1's own
# build log: makeguids carries -march=native and every library object
# (crc32.o, dp.o, ...) does not. So the shipped library is already
# portable, and the flag -1 passed was a fix for a problem that did not
# exist.
# -Wno-error=discarded-qualifiers, and why it is the right knob.
#
# -4 built against libc-dev's glibc 2.36 headers. Retiring libc-dev
# (#187) did not just swap a package -- it moved this build onto the
# glibc 2.44 headers, which carry a C23 const-generic bsearch:
#
#   #if __GLIBC_USE (ISOC23) && defined __glibc_const_generic && !defined _LIBC
#   # define bsearch(KEY, BASE, NMEMB, SIZE, COMPAR)                 \
#     __glibc_const_generic (BASE, const void *,                     \
#                            bsearch (KEY, BASE, NMEMB, SIZE, COMPAR))
#   #endif
#
# With a const BASE it now yields const void * rather than void *.
# efivar searches two const tables (efi_well_known_guids[],
# efi_well_known_names[]) and assigns the result to a plain
# struct efivar_guidname *, so guid.c:88 and guid.c:205 became
# "assignment discards const qualifier" -- errors, not warnings,
# because ERRORS defaults to -Werror. efivar passes -D_GNU_SOURCE,
# which is what turns ISOC23 on despite -std=gnu11.
#
# Neither site writes through the pointer: :205 memcpy()s out of it and
# :88 hands it to a caller that only reads. So the code is correct and
# the diagnostic is glibc becoming more precise, not efivar becoming
# wrong. Const-correcting :88 properly would change
# _get_common_guidname()'s out-parameter to const struct
# efivar_guidname ** and ripple into its callers -- an upstream API
# change, not a packaging decision, so it is not made here.
#
# ERRORS_GCC is upstream's own extension point: defaults.mk line 32 is
#   ERRORS ?= -Werror $(call family,ERRORS)
# so setting it appends after -Werror and touches nothing else.
# Overriding CFLAGS instead would replace OPTIMIZE/DEBUGINFO/WARNINGS
# wholesale, and CPPFLAGS would work only by ordering accident.
pkg_build() {
	make CC=/usr/bin/gcc ENABLE_DOCS=0 LIBDIR=/lib/x86_64-linux-gnu \
	     ERRORS_GCC=-Wno-error=discarded-qualifiers -j"$(nproc)"
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
	make CC=/usr/bin/gcc ENABLE_DOCS=0 install DESTDIR="$PKG_DESTDIR" \
	     LIBDIR=/lib/x86_64-linux-gnu \
	     ERRORS_GCC=-Wno-error=discarded-qualifiers
	rm -rf "$PKG_DESTDIR/usr/bin"
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libefivar.so.1"

	# The .pc files move to where pkgconf actually looks. mokutil finds
	# efivar ONLY through PKG_CHECK_MODULES, so a .pc sitting outside
	# /usr/lib/pkgconfig:/usr/share/pkgconfig is the same as no efivar
	# at all -- exactly the failure openssl 3.0.20-2 was written to fix
	# (#174), one package later.
	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"/*.pc \
	   "$PKG_DESTDIR/usr/lib/pkgconfig/"
	rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"

	# And it must name the directory the libraries are really in. -2
	# passed LIBDIR only to `install`, so the .pc was GENERATED with the
	# default and shipped saying libdir=/usr/lib64 while every library
	# sat in /lib/x86_64-linux-gnu -- a file that verifies, resolves,
	# and then hands a consumer a -L pointing at nothing. -3 passes
	# LIBDIR to the build too, because @@LIBDIR@@ is substituted when
	# the .pc is generated, not when it is copied. Same lesson openssl
	# recorded: install where the files belong rather than relocating
	# them and leaving a second statement of where they live.
	case "$(cat "$PKG_DESTDIR/usr/lib/pkgconfig/efivar.pc")" in
	*"libdir=/lib/x86_64-linux-gnu"*) ;;
	*)
		echo "efivar.pc does not name /lib/x86_64-linux-gnu" >&2
		exit 1
		;;
	esac
}
