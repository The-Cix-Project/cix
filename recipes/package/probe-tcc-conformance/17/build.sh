#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #37: "Root-cause and eliminate g_pkgbuild_rootfs's undocumented
# ambient gcc contamination". Recipes pinning CC=tcc were still getting
# gcc, because a shared, cumulative build sandbox had one lying in it
# and autoconf found it.
#
# ADR-0199 replaced that shared sandbox with an environment composed
# from exactly what a recipe declares. If that worked, a recipe which
# does not declare gcc should not be able to find one -- and the whole
# class of contamination goes with it.
#
# This recipe declares NO compiler but tcc. Anything it finds is
# contamination.
#
pkg_name="probe-tcc-conformance"
pkg_version="17"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_changelog="17: is ambient gcc still reachable from a build sandbox that does not declare it (#37)"

pkg_build() {
	echo "=== compilers reachable from a sandbox declaring only tcc ==="
	for c in gcc cc g++ c++ clang cc1 x86_64-linux-gnu-gcc; do
		printf '  %-24s ' "$c"
		if command -v "$c" >/dev/null 2>&1; then
			echo "PRESENT at $(command -v "$c")  <-- contamination"
		else
			echo "absent"
		fi
	done

	echo
	echo "=== anything gcc-shaped on disk at all ==="
	found=$(ls -d /usr/lib/gcc /usr/libexec/gcc /usr/lib64/libgcc_s.so.1 \
	              /usr/local/go /usr/local/rustup /usr/local/cargo 2>/dev/null)
	if [ -z "$found" ]; then
		echo "  none of the usual toolchain/workstation directories exist"
	else
		echo "$found" | sed 's/^/  PRESENT: /'
	fi

	echo
	echo "=== what autoconf would pick, which is the actual #37 failure ==="
	printf 'int main(void){return 0;}\n' > t.c
	printf '  %-24s ' "CC unset, plain 'cc'"
	if command -v cc >/dev/null 2>&1; then echo "resolves to $(command -v cc)"; else echo "absent -- autoconf must be told"; fi

	echo
	echo "=== how big is this sandbox (issue #168's measure) ==="
	echo "  files under /usr: $(find /usr -xdev -type f 2>/dev/null | wc -l)"
	echo "  files in total:   $(find / -xdev -type f 2>/dev/null | wc -l)"

	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
