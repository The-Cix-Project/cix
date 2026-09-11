#
# probe-bad-sha -- does a source checksum failure say ENOUGH to find the
# cause? (#332)
#
# Same reason probe-dep-unresolvable and probe-missing-tool exist: the
# regression test belongs in test/test_pkg.c, and test_pkg is not in the
# Makefile's SELFTESTS list -- that list is the measured set of tests a
# build container can actually run (probe-selftest-env/4), and test_pkg
# needs to create real build environments, which a build container
# cannot do (#224). So the behaviour would otherwise ship with nothing
# having exercised it against a real daemon.
#
# The shape is the failure exactly: a real, reachable source, fetched
# successfully, whose declared sha256 is deliberately wrong. That is the
# SUBSTITUTED-BODY case of the three #332 names -- the bytes arrive
# whole and are not what was declared -- and it is the one that can be
# provoked on purpose. The other two (a truncated transfer, a fetch that
# never wrote to the path at all) cannot be staged from a recipe.
#
# What a correct answer looks like. The install must fail, and the
# failure must carry:
#
#   - the byte count actually received, so a truncated transfer is
#     distinguishable from a substituted one
#   - enough of the COMPUTED hash to compare against the declared one
#   - and, in the log store, both hashes in full plus the path and url
#
# Before #332 the whole answer was "checksum mismatch (source 0)",
# which says only that two hashes differ. Every fact that separates a
# truncated transfer from a substituted body from a fetch that never
# landed was discarded, and #380 is what that costs: a package that
# failed this way for two revisions with no way to tell which of the
# three it was.
#
# The declared sha256 below is the correct hash with its last character
# changed from 0 to 1. Deliberately a near-miss rather than obvious
# nonsense: a wrong-looking checksum invites the reader to blame the
# recipe, and the point of this probe is that the message should let
# them rule the recipe out without having to.
#
pkg_name="probe-bad-sha"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db21"
pkg_build_depends="bash coreutils"
pkg_changelog="1: probes #332 against a real daemon -- a source checksum failure must report the bytes received and the computed hash, not just that two hashes differ. Expected to FAIL the install."

pkg_build() {
	echo "probe: never reached -- the source checksum fails before the build"
}

pkg_install() {
	echo "probe: never reached"
}
