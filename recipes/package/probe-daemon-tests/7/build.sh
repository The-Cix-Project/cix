#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #482 left three edited test files unverified, and the release
# selftest could not have caught it. SELFTESTS in the Makefile is a
# curated subset (the measured set a build container can actually run,
# #224), and `make selftest` builds only that list -- so `SELFTEST:
# PASS` in the v2.57.207 hostbuild says nothing whatsoever about
# test_pkg, test_pkg_cache or test_kmod_build, all three of which #482
# edited and none of which is in the list. Under -Wall -Werror an
# unbuilt edit is unverified code, and the #482 close comment leaned on
# that PASS. This probe is what makes the claim true or visibly false.
#
# test_kernelrecipe, the fourth file #482 edited, IS in SELFTESTS and
# therefore really was built and run by that hostbuild -- it is built
# here anyway, because a probe that measures three of four when the
# fourth costs nothing is just a narrower claim.
#
# Running them is a second, weaker question. probe-selftest-env/4
# measured 35 of 86 tests passing in a build container: anything that
# needs unshare(CLONE_NEWNS), mount() or a cgroup is out, and these
# three create real build environments. So a run failure here is
# expected and is NOT the signal -- the compile is. Both are reported
# separately so neither can be mistaken for the other.
#
pkg_name="probe-daemon-tests"
pkg_version="7"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.207.tar.gz"
pkg_sha256="248ee9aa84530b9de7fc9f3c324bcd291adec7844a9c74f1b458180074d992b6"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign sed tar gzip grep gawk findutils diffutils libarchive curl quickjs"
pkg_changelog="7: run test_kmod_build and test_pkg_cache with a real build/cixd present (#482)"

pkg_build() {
	TESTS="test_pkg test_pkg_cache test_kmod_build test_kernelrecipe"

	# Probe 6 answered the compile question -- all four build clean with
	# zero warnings. It could not answer the RUN question for two of
	# them, and for one of those the reason was this probe's own fault:
	# test_kmod_build forks a real cixd and got
	# "execve build/cixd: No such file or directory", because probe 6
	# built only the test binaries. That is the documented trap --
	# `make build/test_X` does not build build/cixd -- and it is worth
	# not reporting as a test failure. So cixd is built first here.
	#
	# test_pkg still cannot run in a build container at all: it needs
	# the ADR-0209 floor artifacts from build-inputs/floor-artifacts/,
	# a hand-fetched input that is deliberately not in the source
	# tarball. That is a real limit, not something to work around.
	echo "=== build cixd first (test_kmod_build forks one) ==="
	if make CIX_VERSION="$pkg_version" build/cixd > /run/cixd.build 2>&1; then
		echo "  build/cixd: OK"
	else
		echo "  build/cixd: FAILED"
		grep -E 'error:' /run/cixd.build | head -5 | sed 's/^/      /'
	fi

	echo
	echo "=== compile: the question this probe exists to answer ==="
	rc=0
	for t in $TESTS; do
		if make CIX_VERSION="$pkg_version" "build/$t" > "/run/$t.build" 2>&1; then
			echo "  $t: COMPILES"
		else
			echo "  $t: COMPILE FAILED"
			grep -E 'error:|warning:' "/run/$t.build" | head -8 | sed 's/^/      /'
			rc=1
		fi
	done

	echo
	echo "=== zero-warning check (-Wall -Werror is on, but prove it) ==="
	echo "  warning lines across all four builds: $(cat /run/test_*.build | grep -c 'warning:')"

	echo
	echo "=== run (90s cap each; a failure here is the environment, not the edit) ==="
	for t in $TESTS; do
		[ -x "build/$t" ] || { echo "  $t: NOT BUILT"; continue; }
		if timeout 90 "./build/$t" > "/run/$t.run" 2>&1; then
			echo "  $t: PASS"
		else
			echo "  $t: FAIL (exit $?)"
			tail -12 "/run/$t.run" | sed 's/^/      /'
		fi
	done

	echo
	echo "=== COMPILE VERDICT: $([ $rc -eq 0 ] && echo 'all four compile' || echo 'AT LEAST ONE FAILED') ==="
	exit 1
}

pkg_install() {
	echo "probe-daemon-tests is a diagnostic; nothing is installed" >&2
	exit 1
}
