#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v6: v5's output got tail-truncated again before the interesting
# part (lines 27-40 showing whether `#include "syslimits.h"` is
# present, and syslimits.h's own head). Narrowed to just: whether that
# include line exists, syslimits.h's file size + md5sum compared to
# limits.h's own (to check if it's really a distinct file or an
# identical copy), and its first 15 lines only.
#
pkg_name="probe-gcc-headers"
pkg_version="6"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	D=/usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed
	echo "=== does limits.h #include \"syslimits.h\"? ==="
	grep -n 'syslimits\|_LIBC_LIMITS_H_' "$D/limits.h"
	echo "=== sizes and md5sums ==="
	wc -c "$D/limits.h" "$D/syslimits.h"
	md5sum "$D/limits.h" "$D/syslimits.h"
	echo "=== syslimits.h first 15 lines ==="
	head -15 "$D/syslimits.h"
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
