#
# GNU gettext -- internationalization/translation tools (msgfmt,
# xgettext, msgmerge, ...) and libintl. A real, confirmed build-time
# dependency of grub.recipe (Part 5, bare-metal-readiness plan) --
# listed among GRUB 2.14's own upstream "hard requirements" (confirmed
# directly against the real GRUB source tree's own INSTALL file), used
# by GRUB's own build to generate/update its own translated strings.
# Not needed by any thinC container image at runtime -- staged onto
# the build/toolchain image only, the same "build-tool, not a runtime
# dependency" role m4/bison/flex/autoconf/etc. already have here.
#
pkg_name="gettext"
pkg_version="1.0"
pkg_source="https://ftp.gnu.org/gnu/gettext/gettext-1.0.tar.xz"
pkg_sha256="71132a3fb71e68245b8f2ac4e9e97137d3e5c02f415636eb508ae607bc01add7"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-java --disable-native-java \
		--disable-openmp --without-emacs
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/doc" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/info"
}
