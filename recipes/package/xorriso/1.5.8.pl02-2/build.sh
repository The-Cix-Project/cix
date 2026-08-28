#
# xorriso -- ISO 9660/Rock Ridge/Joliet image manipulation, with
# bundled libburn/libisofs/libisoburn (statically linked, confirmed
# via the real xorriso "standalone" tarball's own documented shape --
# no separate libburnia recipes needed). A real, runtime-only
# dependency of grub-mkrescue (Part 5, bare-metal-readiness plan) --
# grub-mkrescue.c invokes the literal "xorriso" binary by name via
# fork/exec, searched on $PATH (confirmed directly against GRUB's own
# util/grub-mkrescue.c source, not assumed) -- never linked as a
# library, and not needed to *build* GRUB itself, only to *run*
# grub-mkrescue afterward.
#
pkg_name="xorriso"
pkg_version="1.5.8.pl02-2"
pkg_source="https://ftp.gnu.org/gnu/xorriso/xorriso-1.5.8.pl02.tar.gz"
pkg_sha256="b1455ecafbf0692ddafe1d71002a96f2ce2d77f4deae602678261ce033f97bc8"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 1.5.8.pl02.
# Plain ./configure && make, same shape as mtools.
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils"

# --disable-libreadline: this project's images don't stage readline,
# and interactive dialog-mode line-editing is irrelevant to
# grub-mkrescue's own non-interactive use of xorriso.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-libreadline
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
