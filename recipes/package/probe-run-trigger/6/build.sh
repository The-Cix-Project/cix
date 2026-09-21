#
# probe-run-trigger 6 -- the #382 measurement with a window wide enough
# to mean something.
#
# Revision 5 passed and proved little: both artifacts were cache hits,
# the entry went fetching -> installed in 9 seconds, the build stage was
# never reached, and active_jobs was sampled 3 times. The bug it exists
# to catch needs a build that STAYS in flight while the drain runs again
# and again -- on v2.57.53 that is when the same image was started once
# per free chain slot until all ten were gone.
#
# So pkg_build() sleeps 90 seconds. With a 3-second sample interval that
# is ~30 drain opportunities with the slot held, against the 10 slots the
# old code would have consumed.
#
# EXPECTED: active_jobs == 1 at every sample, for the whole 90 seconds.
#
pkg_name="probe-run-trigger"
pkg_version="6"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.49.tar.gz"
pkg_sha256="6042eeebf80880a82e06eefbb15c4ecc7272b759258e72b617dd6198f734e11e"
pkg_artifact_sha256="14f4835f2d2220f571f0b19549dbf4910abeb6a96f11a100176157cf678e3b99"
pkg_depends="probe-run-trigger-dep"
pkg_build_depends="bash coreutils"
pkg_changelog="6: revision 5 converged from cache in 9s and never reached the build stage, so it never opened the multi-pass window #382 lived in; this one sleeps 90s in pkg_build() so the slot is genuinely held while the drain runs many times."

pkg_build() {
	# Deliberately slow. Revision 5 proved nothing: its artifact was
	# cached, so the entry went fetching -> installed in 9 seconds and
	# the drain only ran a handful of times. #382 needs the slot held
	# across MANY passes -- that is when the old drain took a second,
	# third and tenth slot for the same image.
	echo "probe: holding a chain slot for 90s so the drain runs repeatedly"
	sleep 90
	echo "probe: done holding"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger"
	echo "probe-run-trigger revision 6" > "$PKG_DESTDIR/usr/share/probe-run-trigger/REVISION"
}
