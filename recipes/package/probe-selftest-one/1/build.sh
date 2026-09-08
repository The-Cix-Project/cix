#
# probe-selftest-one 1 -- run ONE named test binary in a real build
# container, and by doing so prove the daemon can still build at all.
#
# Two jobs, and the second one is the reason this exists.
#
# 1. THE CHEAP FEEDBACK LOOP. A red release selftest costs a full cix
#    release cycle to re-test: build, assembly, ~260 s plus the selftest
#    itself, and the ADR-0260 cut-over spent four of them finding four
#    real faults one at a time. Building one test binary and running it
#    takes about thirty seconds. The rule this recipe exists to make
#    cheap: a red selftest gets a probe of the failing binary before it
#    gets another release. Change TESTS below and publish a new
#    revision -- a revision IS the parameter here, which is the
#    established convention for probe recipes in this tree.
#
# 2. THE POST-DEPLOY BUILD GATE. This recipe is an ordinary package
#    build, so running it exercises the whole build path: compose the
#    build image, create a build container, run something in it, collect
#    the output. That is precisely what nothing checked after v2.55.24
#    deployed. v2.55.24 was verified across eleven containers, bird on
#    both routers and sshd on the jump host -- and it could not build a
#    single package, because ADR-0260 made a build container's capture
#    pipe the same descriptor cix-init was told to keep and the pre-exec
#    tidy-up closed it (EBADF, reported as "killed by signal 10").
#    Nothing in that verification list ran a build, so the fault shipped
#    and was only found by the next build anyone attempted.
#
#    The release that introduces a change to the build path is itself
#    built by the daemon it replaces, so a cut-over's first real
#    exercise of the new code is always the build AFTER the one that
#    ships it. That is not a fact about ADR-0260; it is a fact about
#    self-hosting, and it will be true of the next one too. Run this
#    after every deploy.
#
# Fails on purpose at the end, like every probe in this tree -- the log
# is the product, and nothing should install. A probe that INSTALLED
# would be a package the platform carries for no reason.
#
pkg_name="probe-selftest-one"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.55.25.tar.gz"
pkg_sha256="c6b847f00098e607e6d5d627e23cbbdf118e9a981bf3f6e95cf231d256313b12"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_changelog="1: run one named test binary in a build container, and prove the daemon can build at all. Written after v2.55.24 shipped a daemon that could not build a package, having been verified only on containers."

#
# The tests to build and run. A daemon-linked test needs the same
# build image and CAP_SYS_ADMIN the cix recipe declares, which is why
# those are set above rather than left to the smaller default a probe
# would otherwise take.
#
TESTS="test_cix_init test_cixinit_table"

pkg_build() {
	echo "=== compiler ==="
	tcc -v 2>&1 | head -1

	echo "=== build ==="
	for t in $TESTS; do
		make "build/$t" || {
			echo "SELFTEST-ONE RESULT: BUILD FAILED for $t"
			exit 1
		}
	done

	echo "=== run ==="
	rc_all=0
	for t in $TESTS; do
		rc=0
		"./build/$t" || rc=$?
		echo "--- $t exit $rc"
		[ "$rc" -eq 0 ] || rc_all=1
	done

	# One line to grep for. The build reaching this point is itself the
	# second measurement: a container was composed, created and run.
	if [ "$rc_all" -eq 0 ]; then
		echo "SELFTEST-ONE RESULT: PASS ($TESTS)"
	else
		echo "SELFTEST-ONE RESULT: FAIL ($TESTS)"
	fi
	echo "BUILD-PATH RESULT: the daemon composed an image, created a build container and ran it"

	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
