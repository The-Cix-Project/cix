#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #224, the pass tally. Probes 1-4 established that a build container
# can host these tests (uid 0, real caps, writable dir, fork, and --
# after v2.10.3 -- a working loopback), and that the whole tree compiles
# (112 binaries, 0 errors) once test_pkg.c's six broken string literals
# were repaired.
#
# The remaining question is the only one that decides how much of the
# gate is worth wiring: how many actually PASS here. Ten chosen to span
# the range rather than to flatter it -- pure HTTP surface, a real
# external binary, PKI, images, volumes, LDAP, NTP, syslog, the process
# table, and one that creates a real BRIDGE (test_networks), which #224
# predicts will fail in an unprivileged-network sandbox and is worth
# confirming rather than assuming.
#
# Bounded deliberately: 90s per test, so a hang costs 90 seconds rather
# than the run.
#
pkg_name="probe-daemon-tests"
pkg_version="5"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.10.6.tar.gz"
pkg_sha256="6a3fcce87612e50f4ef3f405f5685f10daba0e962dc7da83e024643e44c5b99b"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils findutils grep sed gawk dnsmasq"
pkg_changelog="5: how many daemon-linked tests actually pass in a build container (#224)"

pkg_build() {
	echo "=== make all ==="
	make -k all > /run/all.log 2>&1 || true
	echo "  binaries: $(ls build/ | wc -l)   compile errors: $(grep -cE 'error:' /run/all.log)"
	grep -E "error:" /run/all.log | head -5 | sed 's/^/  /'

	TESTS="test_daemon test_dns test_pki test_images test_volume test_ldap test_ntp test_syslogfwd test_hostproc test_networks"
	echo
	echo "=== run (90s cap each) ==="
	pass=0; fail=0; failed=""
	for t in $TESTS; do
		if [ ! -x "build/$t" ]; then
			echo "  $t: NOT BUILT"; fail=$((fail+1)); failed="$failed $t(nobuild)"; continue
		fi
		if timeout 90 ./build/$t > /run/$t.log 2>&1; then
			echo "  $t: PASS"
			pass=$((pass+1))
		else
			rc=$?
			echo "  $t: FAIL (exit $rc)"
			grep -iE 'FAIL|error|cannot|No such' /run/$t.log | head -4 | sed 's/^/      /'
			fail=$((fail+1)); failed="$failed $t"
		fi
	done

	echo
	echo "=== TALLY: $pass passed, $fail failed, of $(echo $TESTS | wc -w) ==="
	[ -n "$failed" ] && echo "  failed:$failed"
	exit 1
}

pkg_install() {
	echo "probe-daemon-tests is a diagnostic; nothing is installed" >&2
	exit 1
}
