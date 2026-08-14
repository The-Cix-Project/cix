#
# mtools -- FAT filesystem manipulation tools (mcopy/mformat/mdir/...).
# A real, runtime-only dependency of grub-mkrescue (Part 5, bare-metal-
# readiness plan) -- grub-mkrescue.c invokes the literal "mformat" and
# "mcopy" binaries by name via fork/exec, searched on $PATH (confirmed
# directly against GRUB's own util/grub-mkrescue.c source, with no
# override flag for either, unlike its --xorriso= flag) -- not needed
# to *build* GRUB itself, only to *run* grub-mkrescue afterward, to
# populate the ISO's own embedded FAT-formatted EFI system partition
# image.
#
pkg_name="mtools"
pkg_version="4.0.49"
pkg_source="https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz"
pkg_sha256="10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
