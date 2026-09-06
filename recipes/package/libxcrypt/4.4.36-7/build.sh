#
# libxcrypt -- libcrypt.so.1, the password-hashing library.
#
# glibc removed crypt() from libc; on modern systems libcrypt is this
# separate project. mokutil needs it -- src/mokutil.c includes
# <crypt.h> directly, and its DT_NEEDED confirms it:
#
#   mokutil  libcrypto.so.3 libefivar.so.1 libkeyutils.so.1
#            libcrypt.so.1 libc.so.6
#
# mokutil is what enrolls this project's Secure Boot signing
# certificate during an install (image/src/cix-install.c, ADR-0015),
# and image/src/mkinstalleriso.c stages it and its whole closure into
# the installer image -- previously by reading them off whatever
# machine ran the build. This is the last library in that closure with
# no Cix package behind it.
#
pkg_name="libxcrypt"
pkg_version="4.4.36-7"
pkg_source="https://github.com/besser82/libxcrypt/releases/download/v4.4.36/libxcrypt-4.4.36.tar.xz"
pkg_sha256="e5e1f4caee0a01de2aee26e3138807d6d3ca2b8e67287966d1fefd65e1fd8943"
pkg_artifact_sha256="0084d0f2c1132ace47604da5c0cd860d620b64b45c16982f0d9e7ca73ca779c8"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#   bash coreutils  the recipe functions and what configure/libtool run
#   make            the build
#   gcc libc-dev    Tier 3. -1 tried TCC and libxcrypt refused outright:
#                     lib/crypt-port.h:87:
#                       error: "Don't know how to prevent function inlining"
#                   The guard is `#if defined __GNUC__ && __GNUC__ >= 3`
#                   for NO_INLINE, with #error as the only alternative,
#                   and TCC does not define __GNUC__.
#
#                   -D__GNUC__=4 would satisfy it and was rejected. That
#                   is claiming a compiler identity we do not implement,
#                   and this project has already paid for exactly that:
#                   CLAUDE.md records gnulib trusting TCC's real
#                   __STDC_VERSION__ while TCC does not implement C99
#                   inline semantics, which produced "defined twice"
#                   link failures across twenty object files. Every
#                   other __GNUC__ branch in libxcrypt -- and it has
#                   many -- would start taking GCC paths on the same
#                   false premise.
#
#                   The build also runs -Wall -Wextra -Wconversion
#                   -Wformat=2 -Wlogical-op ... -Werror, a warning set
#                   written against GCC's diagnostics, so even past the
#                   #error TCC's differing warnings would fail it.
#   binutils        ar/ranlib for libtool's convenience archive, and ld
#   sed grep gawk findutils diffutils
#                   what an autoconf configure reaches for on nearly
#                   every substitution and feature test
#   perl            libxcrypt generates its hash-function dispatch table
#                   and symbol version map with perl scripts at build
#                   time (build-aux/*.pl), so it is a real build input
#                   rather than a documentation nicety
pkg_toolchain="gcc"
pkg_toolchain_reason="compiler identity check: the build requires __GNUC__, which TCC does not define"
pkg_build_depends="bash coreutils make gcc linux-headers binutils sed grep gawk findutils diffutils perl"
pkg_changelog="4.4.36-7: install to /usr/lib rather than the multiarch directory (#184 stage 2). The multiarch triplet is a Debian convention for letting several architectures share one filesystem and a Cix image has one architecture, so this platform is collapsing onto a single library directory. Nothing about consumers changes: glibc's compiled-in search path is exactly slibdir plus libdir, and libdir has been /usr/lib since 2.44-7, so a linker and a loader both find the library there today. The artifact approval is dropped because those bytes came from the previous revision. 4.4.36-6: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

# --disable-static: nothing here links libcrypt statically, and the
# static archive would be the only reason to run libtool's archive path.
# --disable-obsolete-api: drops the ancient DES/NIS entry points kept
# only for binary compatibility with programs built decades ago. mokutil
# calls crypt() itself, which is not obsolete; shipping the compat
# surface would widen the library for no consumer here.
# --disable-failure-tokens keeps crypt() returning NULL on failure --
# glibc's own behaviour, and what code that checks for NULL expects.
pkg_build() {
	# --disable-werror: upstream's own switch, for a compiler newer than
	# the release. -2 built with gcc and died on
	#   lib/util-base64.c:24:3: error: initializer-string for array of
	#     'unsigned char' truncates NUL terminator but destination lacks
	#     'nonstring' attribute (66 chars into 65 available)
	#     [-Werror=unterminated-string-initialization]
	# That diagnostic is new in GCC 15/16; libxcrypt 4.4.36 is from 2024
	# and predates it. The code is deliberately right -- a 64-character
	# base64 alphabet in a 64-byte table, with the NUL intentionally not
	# stored -- so the warning is a false positive against correct code,
	# not a defect to fix.
	#
	# Turning off -Werror rather than patching the source keeps the
	# third-party code unmodified and uses the knob upstream provides
	# for this exact situation. Warnings are still emitted and still in
	# the build log; they simply stop being fatal.
	CC=/usr/bin/gcc ./configure --prefix=/usr --libdir=/usr/lib \
	    --disable-static --disable-obsolete-api --disable-werror
	make -j"$(nproc)"
}

# --libdir above puts libcrypt.so.1 straight into this project's own
# runtime-library directory, and generates libcrypt.pc naming it
# correctly, rather than installing elsewhere and relocating -- the
# lesson openssl 3.0.20-2 recorded, and the one efivar 39-3 had to
# learn twice.
#
# The .pc no longer has to move. Autotools installs it under
# $libdir/pkgconfig, and with libdir at /usr/lib that IS pkgconf's own
# search path (/usr/lib/pkgconfig:/usr/share/pkgconfig, confirmed by
# running `pkg-config --variable pc_path pkg-config` on a real host).
# The relocation this recipe used to do existed only because libdir
# pointed somewhere pkgconf does not look -- #174's whole family of
# fixes was the multiarch split showing up one package at a time
# (#184).
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
		# No pkgconfig relocation any more: with libdir at /usr/lib the
	# install writes .pc files straight where pkgconf looks. The
	# workaround and the problem it worked around were the same thing
	# (#184).

	# libtool's .la files describe a static-link world this project does
	# not use, and carry absolute build-time paths that are wrong the
	# moment the package moves.
	rm -f "$PKG_DESTDIR/usr/lib"/*.la

	# -L on the symlinks: `test -e` follows them, and a dev symlink
	# pointing at an absolute /lib path does not resolve inside
	# PKG_DESTDIR (keyutils 1.6.3-5 failed exactly there).
	# libcrypt.so.2, not .so.1, and that is correct rather than a
	# surprise. --disable-obsolete-api removes the ancient DES/NIS entry
	# points, which is a different ABI, so libxcrypt gives it its own
	# soname. Debian ships .so.1 because it ENABLES that compat surface
	# for the sake of binaries built decades ago.
	#
	# -3 asserted .so.1 -- Debian's number, copied from the DT_NEEDED of
	# Debian's mokutil rather than from what this recipe actually builds
	# -- and failed on a library that had installed perfectly. Nothing
	# in this project links libcrypt for compatibility with old
	# binaries; our own mokutil will link whatever we build, and the
	# closure staged into an installer ISO is measured from our
	# binaries, not assumed from Debian's.
	test -e "$PKG_DESTDIR/usr/lib/libcrypt.so.2"
	test -L "$PKG_DESTDIR/usr/lib/pkgconfig/libcrypt.pc"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libxcrypt.pc"
}
