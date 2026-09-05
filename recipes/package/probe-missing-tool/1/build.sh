#
# probe-missing-tool -- does the daemon refuse a build that exits 0
# while the shell reported a missing command? (#302, ADR-0250)
#
# This exists because the regression test for that gate cannot run
# where the gate runs. test/test_pkg.c drives the case end to end, but
# test_pkg is not in the Makefile's SELFTESTS list: that list is the
# measured set of tests a build container CAN run (probe-selftest-env/4
# -- 35 of 86), and test_pkg needs to create real build environments,
# which a build container cannot do (no CLONE_NEWNS, no mount, no
# cgroup -- see #224). So the gate would otherwise ship with its own
# test never having executed against a real daemon.
#
# The shape is exactly the failure the gate is for: a command that
# genuinely is not there, followed by a successful exit. Deliberately a
# real missing command rather than an echo of the phrase -- an echo
# would prove the scanner reads text, not that it catches what actually
# happens.
#
# EXPECTED RESULT: this install FAILS, with failure_kind "build" and an
# error naming cix_no_such_tool_302. A SUCCESSFUL install means the
# gate is not working.
#
# Source is GNU hello, the same tarball probe-gnu-mirror already uses:
# a probe needs a real, checksum-verified fetch to get as far as
# pkg_build(), and reusing one already proven to fetch keeps this
# probe about the one thing it is testing.
#
pkg_name="probe-missing-tool"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="1: probes the #302 missing-command gate against a real daemon, because the regression test for it cannot run where the gate runs -- test_pkg is not in SELFTESTS, since a build container cannot create the build environments that test needs (#224). Expected to FAIL the install, naming the missing tool."

pkg_build() {
	echo "probe: invoking a command that does not exist, then exiting 0"
	cix_no_such_tool_302 || true
	echo "probe: build step finished successfully (exit 0)"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-missing-tool"
	echo "if this file was installed, the #302 gate did not fire" \
		> "$PKG_DESTDIR/usr/share/probe-missing-tool/RESULT"
}
