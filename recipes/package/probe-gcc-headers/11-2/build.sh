#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v11: gcc-6.4.0's own build (seeded by gcc-4.7.4-6, --image=
# gcc-tcc-bootstrap) hit `configure: error: C preprocessor "/lib/cpp"
# fails sanity check`, real underlying error `/usr/include/limits.h:
# 124:26: error: no include path in which to search for limits.h` --
# the exact same signature already root-caused for the kernel build
# (a broken/never-completed syslimits.h chaining wrapper). Confirming
# directly, in the same --image context gcc-6.4.0's own build used,
# whether gcc-4.7.4-6's own include-fixed/syslimits.h has the same
# defect, since the earlier attempt to inspect a preserved container's
# files via the REST files API returned unreliable/inconsistent 404s.
#
pkg_name="probe-gcc-headers"
pkg_version="11-2"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_toolchain="gcc"
pkg_toolchain_reason="diagnostic: a scratch probe whose entire purpose is to exercise gcc's own headers; not a shipped package"
pkg_changelog="11-2: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

pkg_build() {
	echo "=== which gcc, does it exist ==="
	ls -la /usr/bin/gcc 2>&1
	/usr/bin/gcc --version 2>&1 | head -1
	echo "=== include-fixed dir listing ==="
	ls -la /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed/ 2>&1 | head -20
	echo "=== syslimits.h vs limits.h md5 ==="
	md5sum /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed/syslimits.h \
	       /usr/lib/gcc/x86_64-unknown-linux-gnu/4.7.4/include-fixed/limits.h 2>&1
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
