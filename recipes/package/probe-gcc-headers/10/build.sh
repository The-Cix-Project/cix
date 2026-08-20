#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v10: URGENT sandbox-health check. bash 5.2's own build just failed in
# this shared pkgbuild sandbox with "cannot run C compiled programs"
# during its own configure step's trivial TCC-compiled a.out probe --
# right after installing gcc/4.7.4-3 (built here, in this same shared
# sandbox, via TCC). Since every recipe's pkg_build() runs in this SAME
# shared g_pkgbuild_rootfs sandbox regardless of target image, if that
# install corrupted something shared (ld.so, a libc symlink, etc.),
# every future build is at risk, not just this one experiment.
# Confirming directly, urgently, before anything else.
#
pkg_name="probe-gcc-headers"
pkg_version="10"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== trivial tcc compile+run ==="
	echo 'int main(void){return 42;}' > /build/t.c
	tcc /build/t.c -o /build/t.out 2>&1
	echo "COMPILE_EXIT=$?"
	/build/t.out
	echo "RUN_EXIT=$?"
	echo "=== ldd on the produced binary ==="
	ldd /build/t.out 2>&1
	echo "=== ld.so / libc identity ==="
	ls -la /lib64/ld-linux-x86-64.so.2 2>&1
	ls -la /usr/lib/x86_64-linux-gnu/libc.so.6 2>&1
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
