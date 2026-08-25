#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v2 of the same probe (v1's overly verbose output got truncated by
# cixd's own build-output capture, which keeps only the LAST 3800
# bytes -- PKG_BUILD_OUTPUT_CAPTURE_MAX, daemon/src/pkg.c -- so the
# actually-important -H header-search trace never made it into the
# log). Narrowed to just the two -H traces (bare vs explicit -I), the
# one open question left: does an explicit -I/usr/include actually
# change which limits.h gets opened, or is HOSTCFLAGS/-I simply not
# reaching this compile at all.
#
pkg_name="probe-gcc-headers"
pkg_version="2"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	cat > probe.c <<'EOF'
#include <limits.h>
int x = PATH_MAX;
EOF
	echo "=== bare gcc -H -c probe.c (no explicit -I) ==="
	/usr/bin/gcc -H -c probe.c -o probe_bare.o 2>&1
	echo "BARE_EXIT=$?"
	echo "=== gcc -I/usr/include -H -c probe.c ==="
	/usr/bin/gcc -I/usr/include -H -c probe.c -o probe_iflag.o 2>&1
	echo "IFLAG_EXIT=$?"
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
