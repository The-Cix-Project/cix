#
# probe-gnu-mirror -- can a Cix host still fetch from ftp.gnu.org?
#
# 26 packages at their current revision fetch from ftp.gnu.org, which is
# essentially this platform's entire GNU base. CLAUDE.md records that host
# as not answering this site at all (measured 2026-09-02: TCP connects on
# 443 and 80, then an empty reply after ~12 s, while mirrors.kernel.org
# answers in under a second). If that is still true, none of those 26 can
# be rebuilt, and nothing would reveal it until one of them next needed a
# source build.
#
# Measured from here rather than from the dev sandbox on purpose: the
# daemon fetches sources host-side, so the box's own egress is the only
# egress that decides whether a package can be built. The sandbox answers
# a different question.
#
# The source is GNU hello, the smallest real GNU tarball there is, chosen
# because this probe is about reaching the host, not about the contents.
# A fetch failure shows up before pkg_build() ever runs, which is the
# whole result: this recipe reaching its build step at all is the answer.
#
pkg_name="probe-gnu-mirror"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="1: first probe. Re-measures whether ftp.gnu.org answers a Cix host, since CLAUDE.md records it as dead to this site and 26 packages at their current revision fetch from it."

pkg_build() {
	echo "probe-gnu-mirror: ftp.gnu.org IS reachable from this host -- the fetch succeeded"
	echo "  tree: $(ls | head -5 | tr '\n' ' ')"
	echo "probe-gnu-mirror: failing on purpose so this reaches the log store"
	exit 1
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/doc/probe-gnu-mirror"
	echo "probe only" > "$PKG_DESTDIR/usr/share/doc/probe-gnu-mirror/README"
}
