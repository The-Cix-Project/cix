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
pkg_version="4.0.49-2"
pkg_source="https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz"
pkg_sha256="10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 4.0.49.
# Plain ./configure && make: autoconf's configure drives sed/grep/awk, make drives tcc against libc-dev, and binutils covers any archive step the build system makes internally.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils"

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
