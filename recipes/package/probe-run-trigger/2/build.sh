#
# probe-run-trigger 2 -- proves ADR-0272's `trigger` field is right for BOTH
# values, which no test in SELFTESTS can do (a run needs a real build,
# and test_pkg is excluded -- see probe-missing-tool/1 for the same
# reasoning about #224).
#
# CORRECTS revision 1's header, which claimed a rolling rebuild of
# probe-run-trigger would pull probe-run-trigger-dep with it. It does
# not, and the reason is worth knowing: resolve_chain() skips a
# dependency that is installed at ANY version --
#
#   if (!force && existing != NULL && existing->state == PKG_STATE_INSTALLED)
#           return 0; /* already satisfied */
#
# -- it never compares versions. Only the package named in the manifest
# is forced. So the test as revision 1 described it would have produced
# a one-atom chain and proved nothing about dependencies. (Revision 1 is
# published and a published recipe is immutable, hence a correction here
# rather than an edit there.)
#
# The dependency case still matters, because it is exactly what the
# first implementation of ADR-0272 got wrong: the cause was a module
# static consumed when a run opened, so only dep_queue[0] ever saw
# "rolling" and every other atom recorded itself as "request". The fix
# carries the cause on the chain.
#
# EXPECTED RESULT, read from GET /v1/pipeline/runs. The dependency has
# to be UNINSTALLED first, which is what makes the rebuild pull it:
#   1. install probe-run-trigger@1 deliberately -> two atoms, both "request"
#   2. uninstall probe-run-trigger-dep only
#   3. publish this revision -> the rolling drain rebuilds the image,
#      resolve_chain() finds the dependency missing and queues it, and
#      BOTH atoms must say "rolling"
#
# Deliberately trivial: this probe is about the run RECORD, not about
# building anything, so it does the least work that still goes through
# the real fetch/build/install path.
#
pkg_name="probe-run-trigger"
pkg_version="2"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_depends="probe-run-trigger-dep"
pkg_build_depends="bash coreutils"
pkg_changelog="2: corrects revision 1's header -- resolve_chain() skips a dependency installed at any version, so a rolling rebuild pulls one only if it is absent, and the probe uninstalls it first. Probes ADR-0272's run trigger end to end against a real daemon -- request for a deliberate install, rolling for a publish-driven rebuild, on the dependency as well as on the package asked for."

pkg_build() {
	echo "probe: revision 2, nothing to build"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-run-trigger"
	echo "probe-run-trigger revision 2" > "$PKG_DESTDIR/usr/share/probe-run-trigger/REVISION"
}
