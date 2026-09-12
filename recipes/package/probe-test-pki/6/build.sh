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
pkg_version="6"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.119.tar.gz"
pkg_sha256="1e82c8374afeb1f8efffb4dca602e86327bccd979abae943e2ba69589e8621bd"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="cix-builder"
pkg_changelog="6: probe 5 found the real cause and it was the probe -- every container create failed with 'cix-init is not beside this daemon executable', because the probe built cixd without it. The cix recipe builds build/cix-init explicitly before make selftest, so the real gate has it. Builds it here too, which answers whether test_pki can be gated whole (#417). 5: probe 4 reported exit 127 -- err_of sat above the static json_str_field it calls."

pkg_build() {
	echo "=== openssl, the theory already ruled out by test_https_chain being gated ==="
	echo "  /usr/bin/openssl: $( [ -x /usr/bin/openssl ] && /usr/bin/openssl version || echo ABSENT )"
	echo
	echo "=== building only what test_pki needs ==="
	make build/cix-init build/cixd build/daemon_child build/test_pki 2>&1 | tail -5
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
