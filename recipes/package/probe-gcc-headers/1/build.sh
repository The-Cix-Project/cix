#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# Prints exactly which limits.h /usr/bin/gcc resolves for a trivial
# PATH_MAX probe (both bare "gcc" and absolute "/usr/bin/gcc"), to
# root-cause why scripts/kconfig/conf.c's `#include <limits.h>` isn't
# seeing a real PATH_MAX definition on this build image despite
# libc-dev@2.36 staging a complete usr/include/limits.h. Deliberately
# exits nonzero at the end so its own diagnostic output lands in
# cixd's build-failure log without needing a real package artifact.
#
pkg_name="probe-gcc-headers"
pkg_version="1"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""

pkg_build() {
	echo "=== which gcc ==="
	which gcc || echo "gcc not in PATH"
	echo "=== /usr/bin/gcc -v ==="
	/usr/bin/gcc -v 2>&1
	echo "=== gcc -print-search-dirs ==="
	/usr/bin/gcc -print-search-dirs 2>&1
	echo "=== probe.c ==="
	cat > probe.c <<'EOF'
#include <limits.h>
int x = PATH_MAX;
EOF
	cat probe.c
	echo "=== /usr/bin/gcc -E -v probe.c (header search trace) ==="
	/usr/bin/gcc -E -v probe.c 2>&1 | grep -A30 "search starts here"
	echo "=== /usr/bin/gcc -H -c probe.c (headers actually included) ==="
	/usr/bin/gcc -H -c probe.c -o probe.o 2>&1
	echo "=== does /usr/include/limits.h exist? ==="
	ls -la /usr/include/limits.h 2>&1
	echo "=== first 40 lines of /usr/include/limits.h ==="
	head -40 /usr/include/limits.h 2>&1
	echo "=== does gcc private include-fixed/limits.h exist? ==="
	ls -la /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h 2>&1
	echo "=== its content ==="
	cat /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h 2>&1
	echo "=== forcing failure so this reaches build logs ==="
	exit 1
}

pkg_install() {
	exit 1
}
