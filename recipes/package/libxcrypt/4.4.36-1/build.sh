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
pkg_version="4.4.36-1"
pkg_source="https://github.com/besser82/libxcrypt/releases/download/v4.4.36/libxcrypt-4.4.36.tar.xz"
pkg_sha256="e5e1f4caee0a01de2aee26e3138807d6d3ca2b8e67287966d1fefd65e1fd8943"
pkg_depends=""

# ADR-0199/0209: composed from exactly these, no fallback (#168).
#   bash coreutils  the recipe functions and what configure/libtool run
#   make            the build
#   tcc libc-dev    TCC by default -- this is ordinary C with no GCC
#                   extension in its build, so it needs no exception
#   binutils        ar/ranlib for libtool's convenience archive, and ld
#   sed grep gawk findutils diffutils
#                   what an autoconf configure reaches for on nearly
#                   every substitution and feature test
#   perl            libxcrypt generates its hash-function dispatch table
#                   and symbol version map with perl scripts at build
#                   time (build-aux/*.pl), so it is a real build input
#                   rather than a documentation nicety
pkg_build_depends="bash coreutils make tcc libc-dev binutils sed grep gawk findutils diffutils perl"

# --disable-static: nothing here links libcrypt statically, and the
# static archive would be the only reason to run libtool's archive path.
# --disable-obsolete-api: drops the ancient DES/NIS entry points kept
# only for binary compatibility with programs built decades ago. mokutil
# calls crypt() itself, which is not obsolete; shipping the compat
# surface would widen the library for no consumer here.
# --disable-failure-tokens keeps crypt() returning NULL on failure --
# glibc's own behaviour, and what code that checks for NULL expects.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --libdir=/lib/x86_64-linux-gnu \
	    --disable-static --disable-obsolete-api
	make -j"$(nproc)"
}

# --libdir above puts libcrypt.so.1 straight into this project's own
# runtime-library directory, and generates libcrypt.pc naming it
# correctly, rather than installing elsewhere and relocating -- the
# lesson openssl 3.0.20-2 recorded, and the one efivar 39-3 had to
# learn twice.
#
# The .pc still has to move: autotools installs it under
# $libdir/pkgconfig, which here is /lib/x86_64-linux-gnu/pkgconfig and
# so outside pkgconf's search path (/usr/lib/pkgconfig:/usr/share/
# pkgconfig, confirmed by running `pkg-config --variable pc_path
# pkg-config` on a real host). A .pc pkg-config cannot find is the same
# as no library at all -- #174, for the fourth package in this chain.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"/*.pc \
	   "$PKG_DESTDIR/usr/lib/pkgconfig/"
	rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"

	# libtool's .la files describe a static-link world this project does
	# not use, and carry absolute build-time paths that are wrong the
	# moment the package moves.
	rm -f "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.la

	# -L on the symlinks: `test -e` follows them, and a dev symlink
	# pointing at an absolute /lib path does not resolve inside
	# PKG_DESTDIR (keyutils 1.6.3-5 failed exactly there).
	test -e "$PKG_DESTDIR/lib/x86_64-linux-gnu/libcrypt.so.1"
	test -e "$PKG_DESTDIR/usr/lib/pkgconfig/libcrypt.pc"
}
