#
# probe-run-trigger-dep 1 -- proves ADR-0272's `trigger` field is right for BOTH
# values, which no test in SELFTESTS can do (a run needs a real build,
# and test_pkg is excluded -- see probe-missing-tool/1 for the same
# reasoning about #224).
#
# The pair matters more than either half. probe-run-trigger DEPENDS on
# probe-run-trigger-dep, so a rolling rebuild of the first pulls the
# second, and the two atoms are what the original implementation got
# wrong: the cause was a module static consumed when a run opened, so
# only dep_queue[0] ever saw "rolling" and every dependency recorded
# itself as "request". The fix carries it on the chain.
#
# EXPECTED RESULT, read from GET /v1/pipeline/runs:
#   after a deliberate POST /v1/pkg/install -- both atoms "request"
#   after publishing a new revision of both -- both atoms "rolling"
#
# Deliberately trivial: this probe is about the run RECORD, not about
# building anything, so it does the least work that still goes through
# the real fetch/build/install path.
#
pkg_name="probe-run-trigger-dep"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_artifact_sha256="533a68b97b112c54f6dacf750ba681972a5a055e640ffc8d6bb0ffeaacbf239b"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="1: probes ADR-0272's run trigger end to end against a real daemon -- request for a deliberate install, rolling for a publish-driven rebuild, on the dependency as well as on the package asked for."

pkg_build() {
	echo "probe: revision 1, nothing to build"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger-dep"
	echo "probe-run-trigger-dep revision 1" > "$PKG_DESTDIR/usr/share/probe-run-trigger-dep/REVISION"
}
