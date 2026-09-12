#
# Why test_pki does not run in the build gate (#417).
#
# It is excluded because #224's own full-suite matrix put it among the
# 22 that fail in a build container -- but that matrix reported a tally,
# not a per-test cause, and #417 was filed on the strength of "it fails
# there" without anyone reading its output. This runs exactly that one
# binary and prints everything it says, so the fix is chosen from a
# cause rather than from a guess.
#
# The obvious theory is already ruled out and is not re-tested here:
# test_pki execve()s /usr/bin/openssl directly for its own verification,
# but test_https_chain is ALREADY in the gate and cannot get a host
# certificate without the daemon shelling out to the same binary, so
# openssl is present in a build container.
#
# Fails on purpose -- the build log is the product.
#
pkg_name="probe-test-pki"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.117.tar.gz"
pkg_sha256="a5af7804e7bf7a4af2f5ec967d50810c21e2d39c2cb67c707888e93ae99cdda6"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="cix-builder"
pkg_changelog="2: re-run after fixing what probe 1 found -- an uninitialised `off` in pki_cert_create() that sent uninitialised stack to openssl as -addext, so cert issuance failed by stack contents. What still fails here is what decides how test_pki joins the gate (#417). 1: read test_pki's actual failure in a build container, rather than inferring it from #224's tally (#417)"

pkg_build() {
	echo "=== openssl, the theory already ruled out by test_https_chain being gated ==="
	echo "  /usr/bin/openssl: $( [ -x /usr/bin/openssl ] && /usr/bin/openssl version || echo ABSENT )"
	echo
	echo "=== building only what test_pki needs ==="
	make build/cixd build/daemon_child build/test_pki 2>&1 | tail -5
	echo
	echo "=== running it, full output ==="
	./build/test_pki 2>&1 | tail -70
	echo "=== exit status: ${PIPESTATUS[0]} ==="
	echo
	echo "=== and test_https_chain, which IS gated, for comparison in the same container ==="
	make build/test_https_chain 2>&1 | tail -2
	./build/test_https_chain 2>&1 | tail -12
	echo "=== exit status: ${PIPESTATUS[0]} ==="

	echo
	echo "PROBE COMPLETE -- failing deliberately so the log is kept"
	return 1
}

pkg_install() {
	:
}
