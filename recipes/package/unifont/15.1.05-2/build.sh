#
# GNU Unifont -- the glyph source grub-mkfont converts into the .pf2
# font grub-mkrescue embeds in the installer media.
#
# Nothing is compiled: upstream's source tarball carries a finished BDF
# under font/precompiled/, and this package's whole job is to put it
# where grub.recipe can find it. Unifont specifically because it is
# what GRUB's own build uses, so the font our ISO carries is the one
# upstream intends rather than an arbitrary substitute.
#
# The source is the full tarball rather than the standalone .bdf.gz
# upstream also publishes: pkg_source is extracted with `tar -xf`
# (ADR-0036), and a bare .gz is not a tar archive, so the standalone
# file cannot be a pkg_source at all. Confirmed by reading pkg.c's own
# extraction call rather than by watching it fail.
#
# Fetched from unifoundry.com rather than a GNU mirror deliberately:
# ftp.gnu.org is unreachable over TLS from this network (issue #167).
#
pkg_name="unifont"
pkg_version="15.1.05-2"
pkg_source="https://unifoundry.com/pub/unifont/unifont-15.1.05/unifont-15.1.05.tar.gz"
pkg_sha256="d275f55f4358750e0f86305b92e87b88eb330aa46c15f553d2edf047fb1c23fa"
pkg_depends=""

pkg_build_depends="bash coreutils gzip grep findutils"

pkg_build() {
	:
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/unifont"
	gzip -dc font/precompiled/unifont-15.1.05.bdf.gz \
		> "$PKG_DESTDIR/usr/share/unifont/unifont.bdf"

	# A BDF font begins with STARTFONT. Asserted rather than assumed: a
	# truncated or wrong-format file would otherwise surface much later,
	# as grub-mkfont failing in the middle of an ISO build.
	head -1 "$PKG_DESTDIR/usr/share/unifont/unifont.bdf" | grep -q '^STARTFONT' || {
		echo "unifont: decompressed file is not a BDF font" >&2
		exit 1
	}
	echo "unifont.bdf: $(wc -c < "$PKG_DESTDIR/usr/share/unifont/unifont.bdf") bytes"
}
