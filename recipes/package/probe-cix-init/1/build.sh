#
# probe-cix-init 1 -- does test_cix_init pass inside a composed build
# container? (ADR-0260)
#
# The Makefile's SELFTESTS list is the measured set of tests a build
# container can run (#224), and its own rule is that a test is listed
# only after it has been seen passing there. test_cix_init creates no
# container, mounts nothing and needs no capability -- it forks
# build/cix-init as a plain child over the real wire format -- so it
# should qualify. This probe is the measurement, run where the gate
# runs, on the platform's own tcc.
#
# Fails on purpose at the end, like every probe -- the log is the
# product; the line to read is "CIX-INIT RESULT".
#
pkg_name="probe-cix-init"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/35c40516.tar.gz"
pkg_sha256="eaf06bbba14930edbe9cf382a58e03b00dbba26f682387cca551f0cd05f090bc"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make tcc linux-headers"
pkg_changelog="1: test_cix_init measured in a build container before it joins SELFTESTS."

pkg_build() {
	echo "=== compiler ==="; tcc -v 2>&1 | head -1
	echo "=== build cix-init and its test ==="
	make build/cix-init build/test_cix_init
	ls -la build/cix-init
	echo "=== run ==="
	rc=0
	./build/test_cix_init || rc=$?
	echo "=== test_cix_init exit $rc ==="
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
