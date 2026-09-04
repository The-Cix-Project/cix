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
pkg_version="3"
pkg_source="https://curl.se/ca/cacert.pem"
pkg_sha256="0000000000000000000000000000000000000000000000000000000000000000"
pkg_depends=""
pkg_build_depends="probe-failed-upgrade bash coreutils"
pkg_changelog="3: revision 2 with one difference -- it build-depends on ITSELF, which is the only structural difference between tcc and the probe that did not reproduce #275. tcc build-depends on tcc, so an upgrade queues tcc as a dependency of itself and a second job resolves onto the same package entry. The checksum is wrong here too, so the upgrade fails at the same safe gate revision 2 failed at and the only variable is the self-dependency."

pkg_build() {
	echo "probe-failed-upgrade: revision 2 built"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-failed-upgrade"
	echo "revision 2" > "$PKG_DESTDIR/usr/share/probe-failed-upgrade/marker"
}
