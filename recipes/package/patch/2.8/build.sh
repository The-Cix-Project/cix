#
# GNU patch -- applies unified/context diffs to files. A real build-time
# dependency of grub.recipe (Part 5, bare-metal-readiness plan): GRUB's
# own upstream INSTALL file lists it among the "hard requirements"
# (confirmed against the real GRUB 2.14 source tree's own INSTALL file),
# used by its build system for a handful of small vendored/generated-file
# patches applied during `make`.
#
pkg_name="patch"
pkg_version="2.8"
pkg_source="https://ftp.gnu.org/gnu/patch/patch-2.8.tar.xz"
pkg_sha256="f87cee69eec2b4fcbf60a396b030ad6aa3415f192aa5f7ee84cad5e11f7f5ae3"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
