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
pkg_version="5"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/c0883afc.tar.gz"
pkg_sha256="4bdc97066dc070324133a6b27b87b64d5fc75340c0d4e78888eecc071b4a340d"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils make tcc linux-headers"
pkg_changelog="5: also runs test_cixinit_table, the daemon-side table builder, before it joins SELFTESTS. 4: 3 hung on a blocking EOF read the test itself got wrong; the check now polls after cix-init exits. 3: re-measured after close_range marks every fd from 3 up close-on-exec; 2 found a service listing fds 4 and 5 it was never given. 2: re-measured after the fd-hygiene and SIGPIPE fixes; tests 10 and 11 are new. 1: test_cix_init measured in a build container before it joins SELFTESTS."

pkg_build() {
	echo "=== compiler ==="; tcc -v 2>&1 | head -1
	echo "=== build cix-init and its test ==="
	make build/cix-init build/test_cix_init build/test_cixinit_table
	ls -la build/cix-init
	echo "=== run ==="
	rc=0
	./build/test_cix_init || rc=$?
	echo "=== test_cix_init exit $rc ==="
	rc2=0; ./build/test_cixinit_table || rc2=$?
	echo "=== test_cixinit_table exit $rc2 ==="
	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
