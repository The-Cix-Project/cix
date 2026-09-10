#
# probe-run-trigger-dep 3 -- exercises ADR-0273's roll gate against a
# real daemon, which no test in SELFTESTS can do: holding and releasing
# a rebuild needs a real rebuild, and test_pkg (which drives installs)
# is not in that list because a build container cannot create the build
# environments it needs (#224). Same reasoning as probe-missing-tool/1.
#
# Why this package and not probe-run-trigger: that one cannot currently
# build. It fails "checksum mismatch (source 0)" against a source and
# sha this package fetches successfully in the same minutes, which is
# #380 and is unexplained. The gate test needs a package that reliably
# reaches "installed", so it uses the half that does.
#
# EXPECTED, with two images both tracking this package rolling:
#   1. gate_roll off  -> publishing this rebuilds both, no approval
#   2. gate_roll on   -> publishing queues both and rebuilds NEITHER;
#      each reports blocked with blocked_on {kind:"approval",name:"roll"}
#      and appears in GET /v1/pipeline/approvals pending
#   3. approve ONLY THE SECOND image -> it rebuilds and the first stays
#      held. That is the assertion that matters most: a drain that
#      stopped at the head of its queue instead of skipping would let
#      one unapproved image freeze every other image's convergence.
#   4. the grant is consumed -- granted is empty afterwards, and the run
#      is recorded with trigger "rolling" (ADR-0272)
#
pkg_name="probe-run-trigger-dep"
pkg_version="4"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.49.tar.gz"
pkg_sha256="6042eeebf80880a82e06eefbb15c4ecc7272b759258e72b617dd6198f734e11e"
pkg_depends=""
pkg_build_depends="bash coreutils"
pkg_changelog="4: exercises ADR-0273's roll gate end to end against a real daemon -- hold, report as blocked_on approval, approve one of two queued images, and confirm the other stays held rather than the queue stalling behind it."

pkg_build() {
	echo "probe: probe-run-trigger-dep revision 4, nothing to build"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger-dep"
	echo "probe-run-trigger-dep revision 4" > "$PKG_DESTDIR/usr/share/probe-run-trigger-dep/REVISION"
}
