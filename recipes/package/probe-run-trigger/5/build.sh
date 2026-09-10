#
# probe-run-trigger 5 -- proves the #382 fix on a real host.
#
# What no test in SELFTESTS can do: the drain started the same image
# once per free chain slot, and reproducing that needs a build that
# really stays in flight across drain passes, which needs a build
# container -- exactly what a build container cannot create (#224).
# test_stallwatch covers the other half of #382 (a deleted image's
# queue entry and grant go with it) and says so in the file.
#
# The measurement: an image tracks this package rolling, this revision
# is published, and GET /v1/system/pkg-build-config is sampled every
# 3 seconds until the image converges. active_jobs must be <= 1 at
# EVERY sample. On v2.57.53 the same shape put ten copies of one job
# into all ten slots.
#
# It depends on probe-run-trigger-dep deliberately: a chain that pulls
# a dependency exercises the skip while a DEPENDENCY build holds the
# slot, not just the top-level one. Note (from revision 4) that a
# rolling rebuild does not pull an already-installed dependency at all
# -- resolve_chain() skips one installed at any version -- so the
# dependency has to be uninstalled first for the two-atom case.
#
pkg_name="probe-run-trigger"
pkg_version="5"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.49.tar.gz"
pkg_sha256="6042eeebf80880a82e06eefbb15c4ecc7272b759258e72b617dd6198f734e11e"
pkg_depends="probe-run-trigger-dep"
pkg_build_depends="bash coreutils"
pkg_changelog="5: a new revision to drive one rolling rebuild while active_jobs is sampled, proving the #382 drain fix on a real host."

pkg_build() {
	echo "probe: probe-run-trigger revision 5, nothing to build"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger"
	echo "probe-run-trigger revision 5" > "$PKG_DESTDIR/usr/share/probe-run-trigger/REVISION"
}
