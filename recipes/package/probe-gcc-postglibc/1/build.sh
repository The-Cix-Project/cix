#
# probe-gcc-postglibc -- does gcc still produce a working binary after
# ADR-0251's finalize policy removed libc.a from the glibc package?
#
# The policy drops a static archive whose shared counterpart ships
# beside it, which on this platform means libc.a, libm.a, libpthread.a,
# librt.a and libdl.a all go. That was checked against the recipe tree
# before adoption -- nothing references libc.a by path, and the one
# -static link in the tree (libtool's dlopen-self probe in gcc's build)
# is disabled by that recipe on purpose because it deadlocked for 94
# minutes. What was NOT proven is that gcc's own three-stage bootstrap
# never reaches for libc.a somewhere else, and proving that costs a full
# gcc rebuild.
#
# This is the cheap half of that question, and it is the half that
# matters first: can the installed gcc still compile and link an
# ordinary dynamically-linked program against the stripped glibc? If it
# cannot, nothing else on this platform builds either, and we find out
# in seconds instead of after a multi-hour bootstrap.
#
# Deliberately exits non-zero so its output is kept in the build log --
# the same convention probe-gcc-headers and probe-tcc-conformance use.
#
pkg_name="probe-gcc-postglibc"
pkg_version="1"
pkg_source="http://192.168.15.31:8080/make-4.4.1-6-x86_64.tar.gz"
pkg_sha256="e887b110633256f955029d002323414e1f973d0e2566e0133450f8e39fc02420"
pkg_depends=""
pkg_build_depends="bash coreutils gcc binutils glibc linux-headers"
pkg_toolchain="gcc"
pkg_toolchain_reason="the probe exists to exercise gcc itself -- it asks whether gcc can still compile and link against a glibc the ADR-0251 finalize policy has stripped, so TCC would answer a different question"
pkg_changelog="1: after ADR-0251 stripped glibc, can gcc still compile and link an ordinary dynamic binary? Exits nonzero on purpose so its output is kept in the build log."

pkg_build() {
	echo "=== what the installed glibc actually ships ==="
	for f in /usr/lib/libc.a /lib/x86_64-linux-gnu/libc.a /usr/lib/x86_64-linux-gnu/libc.a; do
		if [ -f "$f" ]; then echo "  PRESENT $f"; else echo "  absent  $f"; fi
	done
	for f in /usr/lib/libc_nonshared.a /usr/lib/libc.so /usr/lib/crt1.o /usr/lib/crti.o; do
		if [ -f "$f" ]; then echo "  PRESENT $f"; else echo "  ABSENT  $f  <-- gcc needs this"; fi
	done
	echo "  libc.so.6: $(ls -l /lib/x86_64-linux-gnu/libc.so.6 2>/dev/null || ls -l /usr/lib/libc.so.6 2>/dev/null || echo missing)"
	echo "  locale sources: $(ls /usr/share/i18n 2>/dev/null | wc -l) entries (expect 0)"

	echo "=== gcc: compile and link a dynamic binary ==="
	mkdir -p /run/probe
	cat > /run/probe/t.c <<'CEOF'
#include <stdio.h>
#include <string.h>
#include <math.h>
int main(void) {
	char b[32];
	snprintf(b, sizeof(b), "%.2f", sqrt(1764.0));
	printf("sqrt=%s strlen=%zu\n", b, strlen(b));
	return 0;
}
CEOF
	/usr/bin/gcc -O2 -o /run/probe/t /run/probe/t.c -lm || {
		echo "FATAL: gcc could not link an ordinary dynamic binary"
		exit 1
	}
	echo "  link: ok"
	/run/probe/t || { echo "FATAL: the binary did not run"; exit 1; }
	echo "  run: ok"

	echo "=== the answer ==="
	echo "gcc still compiles, links and runs against the stripped glibc."
	echo "This does NOT prove gcc's own bootstrap is unaffected -- only a"
	echo "real gcc rebuild does that."
	exit 1
}

pkg_install() {
	:
}
