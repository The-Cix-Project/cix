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
pkg_version="4.0.49-3"
pkg_source="https://ftp.gnu.org/gnu/mtools/mtools-4.0.49.tar.gz"
pkg_sha256="10cd1111da87bf2400a380c1639a6cba8bfb937a24f9c51f5f88d393ae5f6f76"
pkg_artifact_sha256="d4c358fbe8cef660f6a86edfacc2efcef1f40c42d20f4e4e43185e606a384cfe"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 4.0.49.
# Plain ./configure && make: autoconf's configure drives sed/grep/awk, make drives tcc against libc-dev, and binutils covers any archive step the build system makes internally.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils"

pkg_build() {
	#
	# ac_cv_header_iconv_h=no builds mtools onto its own non-iconv code
	# path, and it is a correctness fix rather than a preference.
	#
	# grub-mkrescue drives mformat/mcopy to populate the installer's
	# EFI System Partition, and on a real Cix host that died with
	#
	#   Error converting to codepage 850 Invalid argument
	#
	# mtools converts FAT filenames through iconv_open("CP850"), and
	# glibc's iconv resolves codepages through gconv modules that live
	# in /usr/lib/x86_64-linux-gnu/gconv. Nothing in this platform has
	# them -- checked directly inside the build image, the directory
	# does not exist -- and they could not simply be copied in from a
	# Debian host without importing foreign binaries, which the Build
	# Provenance Mandate rules out.
	#
	# mtools already handles this: charsetConv.c carries a real
	# #ifndef HAVE_ICONV_H implementation built on wcrtomb(), which is
	# in libc itself and needs no gconv modules. Characters it cannot
	# represent become '_'. Every filename this project puts on an ESP
	# is ASCII, so the two paths produce identical output here.
	#
	# There is no --without-iconv switch, so the detection is overridden
	# through autoconf's own cache variable -- the documented way to
	# tell configure the answer to a probe.
	#
	ac_cv_header_iconv_h=no CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
