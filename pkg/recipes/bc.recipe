#
# bc -- GNU bc, the arbitrary-precision calculator language. A
# genuinely missing base tool confirmed empirically (ADR-0056): a real
# kernel hostbuild against the "dev" toolchain image failed generating
# include/generated/timeconst.h (kernel/time/timeconst.bc, computing
# HZ-related time constants) with "bc: command not found". Not part of
# coreutils (a separate GNU project), and nothing else in this recipe
# catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="bc"
pkg_version="1.08.1"
pkg_source="https://ftp.gnu.org/gnu/bc/bc-1.08.1.tar.gz"
pkg_sha256="b71457ffeb210d7ea61825ff72b3e49dc8f2c1a04102bbe23591d783d1bfe996"
pkg_depends=""

pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info"
}
