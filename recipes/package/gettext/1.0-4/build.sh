#
# GNU gettext -- internationalization/translation tools (msgfmt,
# xgettext, msgmerge, ...) and libintl. A real, confirmed build-time
# dependency of grub.recipe (Part 5, bare-metal-readiness plan) --
# listed among GRUB 2.14's own upstream "hard requirements" (confirmed
# directly against the real GRUB source tree's own INSTALL file), used
# by GRUB's own build to generate/update its own translated strings.
# Not needed by any Cix container image at runtime -- staged onto
# the build/toolchain image only, the same "build-tool, not a runtime
# dependency" role m4/bison/flex/autoconf/etc. already have here.
#
pkg_name="gettext"
pkg_version="1.0-4"
pkg_source="https://ftp.gnu.org/gnu/gettext/gettext-1.0.tar.xz"
pkg_sha256="71132a3fb71e68245b8f2ac4e9e97137d3e5c02f415636eb508ae607bc01add7"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 1.0.
# Plain ./configure && make.
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils"
pkg_changelog="1.0-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	# --disable-shared: libtool builds libgnuintl with -version-script to
	# attach a symbol-version map, and TCC rejects the flag outright --
	#   tcc: error: unsupported linker option '-version-script'
	# Nothing here needs a versioned shared libintl: gettext is present
	# so grub's build can run msgfmt, a build-time tool, and a static
	# library serves that identically.
	CC=tcc ./configure --prefix=/usr --disable-shared --disable-java \
		--disable-native-java --disable-openmp --without-emacs
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/doc" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/info"
}
