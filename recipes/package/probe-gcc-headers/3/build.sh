#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# v3: chasing elfutils 0.192-3's real-gcc build failure --
# /usr/include/x86_64-linux-gnu/bits/stdlib.h's fortified wctomb()
# errors "#error \"Assumed value of MB_LEN_MAX wrong\"", meaning
# MB_LEN_MAX is defined to something other than 16 (__STDLIB_MB_LEN_MAX)
# by the time <stdlib.h> is processed -- in a FRESH pkgbuild container
# (this build's own lowerdir, unrelated to kernel.recipe's own
# upperdir-scoped include-fixed/limits.h patch, per g_pkgbuild_rootfs's
# own lowerdir/upperdir split). Confirming directly rather than
# guessing: (1) whether this container's own gcc include-fixed/limits.h
# is already the same broken never-chains-onward template the kernel
# hostbuild found, and (2) what MB_LEN_MAX actually preprocesses to.
#
pkg_name="probe-gcc-headers"
pkg_version="3"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== gcc include-fixed/limits.h content ==="
	cat /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h 2>&1
	echo "=== gcc -print-prog-name and search dirs ==="
	/usr/bin/gcc -print-search-dirs 2>&1 | head -5
	echo "=== preprocessed MB_LEN_MAX via <stdlib.h> ==="
	printf '#include <stdlib.h>\nint y = MB_LEN_MAX;\n' > probe2.c
	/usr/bin/gcc -E probe2.c 2>&1 | grep -n "MB_LEN_MAX\|^int y" | tail -20
	echo "=== -H trace for <stdlib.h> (which limits.h/stdlib.h files get opened) ==="
	/usr/bin/gcc -H -c probe2.c -o probe2.o 2>&1 | grep -i "limits.h\|stdlib.h"
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
