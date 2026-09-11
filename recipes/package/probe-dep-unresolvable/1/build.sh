#
# probe-dep-unresolvable -- does an install blame the PACKAGE when an
# unresolvable DEPENDENCY is the real cause? (#318)
#
# Same reason probe-missing-tool exists: the regression test for this
# belongs in test/test_pkg.c, and test_pkg is not in the Makefile's
# SELFTESTS list -- that list is the measured set of tests a build
# container can actually run (probe-selftest-env/4), and test_pkg needs
# to create real build environments, which a build container cannot do
# (no CLONE_NEWNS, no mount, no cgroup -- #224). So the behaviour would
# otherwise ship with nothing having exercised it against a real daemon.
#
# The shape is the failure exactly: a recipe that parses perfectly,
# whose own fields are all valid, naming a dependency that does not
# exist. Before #318 this answered
#
#   400 {"error":"no such recipe, or it failed to parse"}
#
# about THIS recipe -- which parses fine -- sending the reader to
# inspect the wrong file. resolve_chain() already builds a precise
# message saying what it could not resolve; that message was being
# discarded at the return.
#
# EXPECTED RESULT: POST /v1/pkg/install answers 400 saying a DEPENDENCY
# could not be resolved, and the daemon log names
# no-such-dependency-xyz. An answer blaming this recipe means the fix
# has regressed.
#
# Nothing is ever fetched or built: dependency resolution runs before
# the fetch, so the install fails before the source is touched. The
# source is nevertheless a real, checksum-verified tarball -- the same
# GNU hello other probes use -- because a recipe must be genuinely
# valid for this probe to be about the dependency rather than about a
# malformed field.
#
pkg_name="probe-dep-unresolvable"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_depends="no-such-dependency-xyz"
pkg_build_depends="bash coreutils"
pkg_changelog="1: probes #318 against a real daemon -- an install whose DEPENDENCY cannot be resolved must say so, rather than reporting the package's own recipe as unparseable. Expected to FAIL the install, naming the dependency."

pkg_build() {
	echo "probe: never reached -- dependency resolution fails before the fetch"
}

pkg_install() {
	echo "probe: never reached"
}
