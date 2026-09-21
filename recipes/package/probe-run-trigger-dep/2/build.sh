#
# probe-run-trigger-dep 2 -- proves ADR-0272's `trigger` field for BOTH values,
# which no test in SELFTESTS can do (a run needs a real build, and
# test_pkg is excluded -- see probe-missing-tool/1 for the same
# reasoning about #224).
#
# The dependency half. Deliberately trivial: this is about the run RECORD, not about building anything.
#
# Two corrections to the earlier revisions, both found by running them
# on 192.168.15.95 on 2026-09-10:
#
#  1. The source moved off ftp.gnu.org. probe-run-trigger/2's rebuild
#     failed "checksum mismatch (source 0)" while revision 1 had
#     installed fine -- revision 1 was a cache hit and never fetched.
#     The daemon's log shows real `curl: (6) Could not resolve host:
#     ftp.gnu.org` failures from build containers on this box. This
#     uses the cix tarball from git.home.arpa instead, whose bytes were
#     fetched twice and hashed identically while cutting v2.57.49, and
#     which that release then really did build from.
#
#  2. A rolling rebuild does NOT pull an already-installed dependency.
#     resolve_chain() skips one that is installed at ANY version --
#     "if (!force && existing != NULL && existing->state ==
#     PKG_STATE_INSTALLED) return 0;" -- it never compares versions, and
#     only the package the manifest names is forced. So the dependency
#     has to be UNINSTALLED before the rebuild for the two-atom case to
#     happen at all.
#
# EXPECTED, from GET /v1/pipeline/runs:
#   1. install probe-run-trigger deliberately -> two atoms, both "request"
#   2. uninstall probe-run-trigger-dep only
#   3. publish a new revision -> the rolling drain rebuilds, resolve_chain()
#      finds the dependency missing and queues it, and BOTH atoms say
#      "rolling". Before the chain fix only dep_queue[0] saw "rolling"
#      and everything after it recorded "request".
#
pkg_name="probe-run-trigger-dep"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.49.tar.gz"
pkg_sha256="6042eeebf80880a82e06eefbb15c4ecc7272b759258e72b617dd6198f734e11e"
pkg_artifact_sha256="acd784f46df30e0dbfc7a8b82a0b27d69a2dc7f9983cb62c419e0d91581128f4"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="2: source moved to git.home.arpa (ftp.gnu.org does not resolve from a build container on this host, and revision 1's apparent success was a cache hit); records that resolve_chain() skips an already-installed dependency regardless of version, so the probe uninstalls it first."

pkg_build() {
	echo "probe: probe-run-trigger-dep revision 2, nothing to build"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger-dep"
	echo "probe-run-trigger-dep revision 2" > "$PKG_DESTDIR/usr/share/probe-run-trigger-dep/REVISION"
}
