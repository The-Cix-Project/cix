#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v8: verify zlib 1.3.2-3's own fix actually landed -- __va_start/
# __va_arg should now be resolved (not listed as undefined "U") in
# the real libz.so.1, confirmed via nm -D before trusting a full
# kernel rebuild round on it.
#
pkg_name="probe-gcc-headers"
pkg_version="8"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== undefined symbols in libz.so.1 now ==="
	nm -D /usr/lib/libz.so.1 2>&1 | grep " U " | grep -i "va_" || echo "(none -- resolved)"
	echo "=== does libz.so.1 now itself define va_start/va_arg? ==="
	nm -D /usr/lib/libz.so.1 2>&1 | grep -i "va_"
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
