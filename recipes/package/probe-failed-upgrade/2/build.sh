#
# probe-failed-upgrade: what a failed UPGRADE does to the package that
# was already installed (#275).
#
# Revision 1 installs cleanly and leaves one file behind, so there is a
# real installed package with a real manifest to lose. Revision 2 is
# identical except that its pkg_sha256 is deliberately wrong, so the
# upgrade fails at the safest possible moment -- before a single byte
# of the fetched source is trusted.
#
# Deliberately not tcc: the observation that prompted #275 was made
# while upgrading the compiler in the toolchain image, which destroyed
# it. A probe that reproduces the same thing must not be able to do the
# same damage.
#
pkg_name="probe-failed-upgrade"
pkg_version="2"
pkg_source="https://curl.se/ca/cacert.pem"
pkg_sha256="0000000000000000000000000000000000000000000000000000000000000000"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="2: pkg_sha256 is deliberately wrong so the upgrade fails at the checksum gate, which is the safest failure there is -- it happens before any fetched byte is trusted. If an installed package cannot survive THAT, it cannot survive anything (#275)."

pkg_build() {
	echo "probe-failed-upgrade: revision 2 built"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-failed-upgrade"
	echo "revision 2" > "$PKG_DESTDIR/usr/share/probe-failed-upgrade/marker"
}
