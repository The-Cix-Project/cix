#
# findutils -- GNU find/xargs/locate. A genuinely missing base tool
# confirmed empirically (ADR-0056): a real kernel hostbuild against
# the "dev" toolchain image failed building kernel/mm subsystem
# built-in.a archives with "xargs: command not found" (kernel's own
# scripts/Makefile.build pipes object file lists through xargs to ar).
# Not part of coreutils (a separate GNU project), and nothing else in
# this recipe catalog happened to need it before now.
#
# Source is GNU's own canonical ftp.gnu.org release.
#
pkg_name="findutils"
pkg_version="4.10.0"
pkg_source="https://ftp.gnu.org/gnu/findutils/findutils-4.10.0.tar.xz"
pkg_sha256="1387e0b67ff247d2abde998f90dfbf70c1491391a59ddfecb8ae698789f0a4f5"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale"
}
