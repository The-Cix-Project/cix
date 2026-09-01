#
# freetype -- the font rasteriser grub-mkfont needs.
#
# Packaged for one reason, and it is the last input a self-hosted
# installer ISO was missing. grub-mkrescue always embeds a .pf2 font
# for the media label (isolated directly: --fonts= correctly installs
# no menu fonts, and the build still fails until --label-font has one),
# and our grub cannot produce a .pf2 at all -- it was configured
# without freetype, so grub-mkfont was never built. Only its bash
# completion shipped, which is a good indication of the gap.
#
# Built minimal on purpose. GRUB's font conversion needs glyph
# rasterisation and nothing else, so the optional PNG, Brotli, HarfBuzz
# and bzip2 integrations are all switched off rather than pulling four
# more libraries in behind a font.
#
pkg_name="freetype"
pkg_version="2.13.3-3"
pkg_source="https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
pkg_sha256="0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"
pkg_artifact_sha256="b6eb405c0bc3318b5830e309c1bbb1a4d200cf0f12c46eeea7e27f8ef465b3b7"
pkg_depends="zlib"

# ADR-0199/0209: composed from exactly these, no fallback (#168).
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils pkgconf findutils diffutils zlib"
pkg_changelog="2.13.3-3: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	# --disable-shared is what keeps this on TCC rather than pushing it
	# onto the Tier-3 exception list. libtool asks the compiler for
	# -Wl,-version-script when it builds a versioned SHARED library,
	# and TCC does not implement that option:
	#
	#   tcc: error: unsupported linker option '-version-script'
	#
	# Nothing here needs a shared libfreetype. Its one consumer is
	# grub-mkfont, a build-time tool, and linking it statically means
	# the ISO toolchain carries no libfreetype.so to stage or keep in
	# step either -- strictly less to go wrong than the shared build
	# would have given us.
	CC=tcc ./configure --prefix=/usr \
		--disable-shared --enable-static \
		--with-zlib=yes \
		--with-png=no --with-brotli=no --with-harfbuzz=no --with-bzip2=no
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"

	# The .pc file is what grub's configure looks for; pkgconf only
	# searches /usr/lib/pkgconfig and /usr/share/pkgconfig here, so a
	# .pc installed anywhere else is invisible (issue #174).
	pc=$(find "$PKG_DESTDIR" -name 'freetype2.pc' | head -1)
	test -n "$pc" || {
		echo "freetype: no freetype2.pc was installed" >&2
		exit 1
	}
	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	case "$pc" in
	"$PKG_DESTDIR/usr/lib/pkgconfig/freetype2.pc") ;;
	*) mv "$pc" "$PKG_DESTDIR/usr/lib/pkgconfig/freetype2.pc" ;;
	esac
	echo "installed freetype2.pc:"
	cat "$PKG_DESTDIR/usr/lib/pkgconfig/freetype2.pc"
}
