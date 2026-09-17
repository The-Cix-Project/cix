#
# probe-buildlog-compile -- does test/test_pkg_build_log.c still
# COMPILE?
#
# Written because #476's fix added ~100 lines to that file and nothing
# in this project's build ever reads it. It is not in SELFTESTS (it
# creates a real build container, which a composed build container
# cannot -- #224), and the cix recipe builds named targets plus `make
# selftest`, never `make all`. So the file is compiled by no target any
# release runs, and three artefacts -- the changelog entry, ADR-0297
# and #476's close comment -- said the new cases gate the name-only
# lookup "for anyone who runs it". A file that does not compile gates
# nothing, and that claim would have been false with nobody able to
# notice.
#
# This does the one thing that settles it: fetch the release tarball
# and build that one target. `make build/test_pkg_build_log` depends
# only on the test source, test_image_fixture.c and the client sources,
# so it is a short build rather than a whole tree.
#
# Ends in a deliberate `exit 1`, so nothing installs and no image
# manifest is touched -- same shape as probe-web-syntax-gate/1. A
# COMPILE FAILURE is the finding; the failing exit at the end is not.
#
pkg_name="probe-buildlog-compile"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.197.tar.gz"
pkg_sha256="05722653426f43f77c1cff7fff4d5768775820b874a9dad5499f36393f9f28df"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make tcc linux-headers openssl sed tar gzip grep"
pkg_depends=""
pkg_changelog="1: compiles test/test_pkg_build_log.c, which no target any release builds -- #476 added cases to it and nothing had ever read them."

pkg_build() {
	echo "=== probe-buildlog-compile: building the one target nothing else does ==="
	make build/test_pkg_build_log
	if [ ! -x build/test_pkg_build_log ]; then
		echo "the target did not produce a binary" >&2
		exit 1
	fi
	echo "=== it compiles and links; the new #476 cases are real code ==="
	#
	# And prove the new cases are actually IN the binary, not merely
	# that the file compiled: a compile of the old file would also
	# succeed. The strings are the ones the assertions carry.
	#
	for s in "?name=slowbuild" "?name=nosuchpackage" "no build in progress"; do
		if ! grep -q -- "$s" build/test_pkg_build_log; then
			echo "the built binary does not contain \"$s\" -- the #476 cases are not in it" >&2
			exit 1
		fi
		echo "  present: $s"
	done
	echo "=== done; failing deliberately so nothing installs ==="
	exit 1
}

pkg_install() {
	echo "probe-buildlog-compile: pkg_install must never run -- pkg_build exits 1" >&2
	exit 1
}
