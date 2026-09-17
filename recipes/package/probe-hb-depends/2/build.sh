#
# probe-hb-depends -- does a hostbuild accept a recipe that declares
# pkg_depends, carry the declaration, and not resolve it? (#465,
# ADR-0303)
#
# This exists because the regression test for that behaviour cannot run
# where the behaviour runs. test/test_pkg.c step 17 drives the case end
# to end, but test_pkg is in none of the Makefile's SELFTESTS lists:
# that list is the measured set of tests a build container CAN execute
# (probe-selftest-env/4 -- 35 of 86), and test_pkg needs to create real
# build environments, which a build container cannot do (#224). Same
# reasoning as probe-missing-tool/1, and the same convention.
#
# The dependency named below is deliberately a name no published recipe
# has. That is what makes one run answer three questions at once:
#
#   - if anything on the hostbuild path tried to RESOLVE it, the job
#     could only fail ("no such recipe"), so reaching state=installed
#     proves it was not resolved;
#   - GET /v1/pkg/hostbuild/probe-hb-depends reporting
#     depends="nosuchdep" proves it was not silently discarded;
#   - GET /v1/pkg/nosuchdep answering 404 proves nothing installed it.
#
# pkg_build_depends names the tools this build actually uses, because
# a hostbuild now composes its build container from exactly them
# (ADR-0304, #482) and from nothing else. Revision 1 declared it EMPTY
# on purpose, with a comment saying that declaring build tools "would
# suggest a mechanism that does not apply" -- true when it was written,
# and precisely the defect #482 fixed. An empty declaration now falls
# back to the shared build sandbox, which is its own remaining
# exception rather than something to rely on here.
#
# EXPECTED RESULT, on a daemon carrying ADR-0303: POST /v1/pkg/hostbuild
# returns 202 and the job reaches state=installed. On the daemon before
# it: a bare 400, which is issue #465 itself.
#
# Source is GNU hello: a probe needs a real, checksum-verified fetch to
# get as far as pkg_build() at all. Served from mirrors.kernel.org
# rather than ftp.gnu.org, which is intermittently reachable from this
# site (#409) and is not what this probe is measuring.
#
pkg_name="probe-hb-depends"
pkg_version="2"
pkg_source="https://mirrors.kernel.org/gnu/hello/hello-2.12.1.tar.gz"
pkg_sha256="8d99142afd92576f30b0cd7cb42a8dc6809998bc5d607d88761f512e26c7db20"
pkg_depends="nosuchdep"
pkg_build_depends="bash coreutils"
pkg_changelog="2: declares its build tools (bash coreutils), because ADR-0304 (#482) made a hostbuild compose its build container from pkg_build_depends like every other build -- revision 1 declared it empty deliberately, which was correct then and is exactly what that issue fixed. 1: probes ADR-0303 against a real daemon -- a hostbuild accepts a declared pkg_depends, carries it onto the entry, and never resolves it. The regression test for this cannot run where it runs: test_pkg is in no SELFTESTS list (#224)."

pkg_build() {
	echo "probe: a hostbuild whose recipe declares pkg_depends=\"nosuchdep\""
	echo "probe: reaching this line at all means the declaration was not resolved"
	true
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/share/probe-hb-depends"
	echo "a hostbuild ran with pkg_depends=\"nosuchdep\" declared and unresolved" \
		> "$PKG_DESTDIR/usr/share/probe-hb-depends/RESULT"
}
