#
# probe-stuck-fetch -- a deliberately unreachable source, for verifying that
# a stuck fetch can be cancelled (#239).
#
# pkg_source points at a host that ACCEPTS the TCP connection and then never
# answers, so curl genuinely hangs. That distinction is the whole point: a
# first attempt at this test used TEST-NET-1 (192.0.2.1), which is rejected
# immediately, so the fetch failed fast and proved nothing about cancelling
# one that never finishes. Stand up a listener that accepts and never writes,
# point this at it, install, and the entry sits in "fetching" indefinitely --
# which is the state #239 was about.
#
# Never installable by design. Published so the reproduction lives in the
# repo rather than in somebody's shell history.
#
pkg_name="probe-stuck-fetch"
pkg_version="2"
pkg_source="http://192.168.15.31:18443/hangs-forever.tar.gz"
pkg_sha256="0000000000000000000000000000000000000000000000000000000000000000"
pkg_build_depends="bash coreutils"
pkg_changelog="2: a source that accepts the connection and never answers, to verify a stuck fetch can be cancelled (#239)"

pkg_build() { :; }

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share"
}
