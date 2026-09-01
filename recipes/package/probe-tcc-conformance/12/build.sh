#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# tcc@0.9.28rc-8 added /lib/<triplet> to the library search path, and
# its own gate linked against a library placed there. The cixd
# hostbuild still fails with `library 'crypto' not found`. So either
# the sandbox does not have the tcc that was just built, or libcrypto
# is not where this expects it. Ask the sandbox directly instead of
# reasoning about it.
#
pkg_name="probe-tcc-conformance"
pkg_version="12"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
# the same tools cixd's own recipe declares, so this sees what it sees
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils openssl"
pkg_changelog="12: why does -lcrypto fail in a composed sandbox when tcc searches /lib/<triplet> and openssl is declared"

pkg_build() {
	echo "=== which tcc is actually in this sandbox ==="
	command -v tcc
	tcc -v 2>&1 | head -1

	echo
	echo "=== where is libcrypto ==="
	for d in /lib /usr/lib /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu; do
		printf '  %-32s ' "$d"
		ls "$d"/libcrypto* 2>/dev/null | tr '\n' ' ' || true
		echo
	done

	echo
	echo "=== where is libc (the triplet this build would derive) ==="
	ls /lib/*/libc.so.6 2>/dev/null | sed 's/^/  /' || echo "  none under /lib/*/"
	ls /usr/lib/*/libc.so.6 2>/dev/null | sed 's/^/  /' || echo "  none under /usr/lib/*/"

	echo
	echo "=== does -lcrypto link ==="
	printf 'int main(void){return 0;}\n' > t.c
	if tcc t.c -lcrypto -o t 2>&1; then echo "  ok"; else echo "  FAILED (above)"; fi

	echo
	echo "=== does it link with an explicit -L ==="
	if tcc t.c -L/lib/x86_64-linux-gnu -lcrypto -o t2 2>&1; then echo "  ok with -L/lib/x86_64-linux-gnu"; else echo "  still FAILED"; fi

	echo
	echo "=== what the compiler thinks its search path is ==="
	tcc -print-search-dirs 2>&1 | head -20 | sed 's/^/  /' || echo "  (-print-search-dirs unsupported)"

	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
