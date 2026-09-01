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
pkg_version="2.13.3-4"
pkg_source="https://download.savannah.gnu.org/releases/freetype/freetype-2.13.3.tar.xz"
pkg_sha256="0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289"
pkg_depends="zlib"

# ADR-0199/0209: composed from exactly these, no fallback (#168).
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils pkgconf findutils diffutils zlib"
pkg_changelog="2.13.3-4: -fno-asynchronous-unwind-tables -- tcc 0.9.28rc emits every .eh_frame FDE relocation as .text+0, so GNU ld rejects the archive with \"overlapping FDEs\" and grub silently loses grub-mkfont"

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
	# -fno-asynchronous-unwind-tables, and why a font depends on it.
	#
	# tcc 0.9.27 emitted no .eh_frame at all. The 0.9.28rc snapshot
	# (ADR-0223) turns unwind tables on by default and emits every FDE
	# relocation as ".text + 0" rather than each function's own offset:
	#
	#   Relocation section '.rela.eh_frame':
	#     R_X86_64_PC32   .text + 0      <- all eight, one per function
	#
	# So every FDE claims to describe code starting at .text+0 with a
	# different length, they all overlap, and GNU ld refuses the object:
	#
	#   ld: .eh_frame_hdr refers to overlapping FDEs
	#   ld: final link failed: bad value
	#
	# tcc links its own output fine, so libfreetype.a builds, installs
	# and verifies -- the failure only appears when GCC links against
	# it. That is how it surfaced: grub's configure probes freetype
	# with BUILD_CC, the probe failed to link, configure set
	# enable_grub_mkfont='no', no unicode.pf2 was generated, and grub's
	# own gate caught it at the far end of a four-minute build. The
	# archive also doubled, 2.6 MB to 5.1 MB, entirely in unwind data
	# nothing here can use.
	#
	# This is the compiler's own documented option (tcc.c:
	# "asynchronous-unwind-tables  create eh_frame section [on]"), not
	# a post-hoc strip: it sets unwind_tables=0 and tccdbg.c skips
	# creating the section, which is exactly the 0.9.27 shape that
	# worked. freetype is C with no exceptions and nothing here
	# unwinds through it. The compiler bug itself is #227.
	# -O2 is spelled out because setting CFLAGS at all displaces the
	# default configure would otherwise choose; it is stated rather
	# than silently dropped.
	CC=tcc CFLAGS="-O2 -fno-asynchronous-unwind-tables" ./configure --prefix=/usr \
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
	# Asserted, not assumed: an archive carrying .eh_frame is one GNU
	# ld will refuse, and the only symptom downstream is a feature
	# quietly not built. See #227.
	if readelf -SW "$PKG_DESTDIR/usr/lib/libfreetype.a" 2>/dev/null | grep -q '\.eh_frame'; then
		echo "freetype: libfreetype.a still carries .eh_frame -- GNU ld will reject it (#227)" >&2
		exit 1
	fi

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
