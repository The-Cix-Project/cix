#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v4: v3's full `cat include-fixed/limits.h` got tail-truncated by
# cixd's own 3800-byte build-output capture, cutting off exactly the
# part that matters. Narrowed to grep just the MB_LEN_MAX-relevant
# lines out of that file, plus a real preprocess of argp.h's own
# `#include <limits.h>` (confirmed via /usr/include/argp.h: it pulls
# in limits.h at line 26, before elfutils' color.c ever reaches its
# own <stdlib.h> at line 34) to see what MB_LEN_MAX actually resolves
# to along the real include chain color.c takes.
#
pkg_name="probe-gcc-headers"
pkg_version="4"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== MB_LEN_MAX / _GCC_LIMITS_H_ / _GCC_NEXT_LIMITS_H mentions in include-fixed/limits.h ==="
	grep -n "MB_LEN_MAX\|_GCC_LIMITS_H_\|_GCC_NEXT_LIMITS_H" \
	    /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h
	echo "=== line count of that file ==="
	wc -l /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h
	echo "=== real include chain: argp.h then stdlib.h (matches color.c's own order) ==="
	printf '#include <argp.h>\n#include <stdlib.h>\nint y = MB_LEN_MAX;\n' > probe4.c
	/usr/bin/gcc -E probe4.c 2>&1 | grep -n "MB_LEN_MAX\|^int y"
	echo "=== -H trace for that same chain ==="
	/usr/bin/gcc -H -c probe4.c -o probe4.o 2>&1 | grep -i "limits.h"
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
