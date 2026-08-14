#
# elfutils -- provides libelf, the ELF object file library. A
# genuinely missing dependency confirmed empirically (ADR-0056): a
# real kernel hostbuild against the "dev" toolchain image failed at
# `tools/objtool` (needed for ORC/stack-validation metadata) with
# "cannot find -lelf". Nothing else in this recipe catalog happened to
# need it before now.
#
# Compression support (zlib/bzlib/lzma) and debuginfod are all opt-in
# via --with-*/--enable-* flags (confirmed via ./configure --help) --
# a plain configure needs nothing beyond this project's existing
# libc-dev + gcc toolchain, no dependency cascade.
#
# Source is elfutils' own canonical sourceware.org release.
#
pkg_name="elfutils"
pkg_version="0.192"
pkg_source="https://sourceware.org/elfutils/ftp/0.192/elfutils-0.192.tar.bz2"
pkg_sha256="616099beae24aba11f9b63d86ca6cc8d566d968b802391334c91df54eab416b4"
pkg_depends=""

pkg_build() {
	./configure --prefix=/usr --disable-debuginfod --disable-libdebuginfod
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/lib"/*.a
}
