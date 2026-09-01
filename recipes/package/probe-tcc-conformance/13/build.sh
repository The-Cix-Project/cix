#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# libcap 2.78-11 is the first third-party package the tcc upgrade
# (#216, ADR-0223) actually broke:
#
#   cap_alloc.c:23: error: ')' expected (got '(')
#
# The line is `__attribute__((constructor (300))) void ...` -- a
# constructor with a PRIORITY argument. tcc 0.9.27 accepted it;
# upstream mob does not parse the priority.
#
# Before choosing between patching libcap (a declared capability loss
# under ADR-0222) and patching the compiler, establish what the new
# compiler actually supports and whether priorities are honoured at
# all. Patching libcap to drop the priority is only safe if the
# ordering it asks for was never real.
#
pkg_name="probe-tcc-conformance"
pkg_version="13"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="13: what does the upgraded tcc do with __attribute__((constructor)) and with a priority argument -- the construct that broke libcap"

run_case() {
	CC="$1"; CCX="$2"; label="$3"; src="$4"; name="$5"
	printf '  %-26s ' "$name"
	if out=$($CC $CCX -o "prog_$name" "$src" 2>&1); then
		./"prog_$name" 2>&1 | tr '\n' ' '
		echo
	else
		echo "COMPILE FAILED: $(printf '%s' "$out" | head -1)"
	fi
}

suite() {
	CC="$1"; CCX="$2"
	echo "  --- $($CC $CCX -v 2>&1 | head -1) ---"

	cat > c_bare.c <<'EOF'
#include <stdio.h>
__attribute__((constructor)) static void ctor(void){ printf("ctor "); }
__attribute__((destructor))  static void dtor(void){ printf("dtor "); }
int main(void){ printf("main "); return 0; }
EOF
	run_case "$CC" "$CCX" x c_bare.c bare

	cat > c_prio.c <<'EOF'
#include <stdio.h>
__attribute__((constructor (300))) static void ctor(void){ printf("ctor300 "); }
__attribute__((destructor  (300))) static void dtor(void){ printf("dtor300 "); }
int main(void){ printf("main "); return 0; }
EOF
	run_case "$CC" "$CCX" x c_prio.c priority

	# are priorities actually ORDERED? 100 must run before 900.
	cat > c_order.c <<'EOF'
#include <stdio.h>
__attribute__((constructor (900))) static void late(void){ printf("late "); }
__attribute__((constructor (100))) static void early(void){ printf("early "); }
int main(void){ printf("main "); return 0; }
EOF
	run_case "$CC" "$CCX" x c_order.c ordering

	# the exact libcap shape: visibility plus a prioritised constructor
	cat > c_libcap.c <<'EOF'
#include <stdio.h>
__attribute__((visibility ("hidden")))
__attribute__((constructor (300))) void init(void){ printf("init "); }
int main(void){ printf("main "); return 0; }
EOF
	run_case "$CC" "$CCX" x c_libcap.c libcap-shape
}

pkg_build() {
	mkdir -p /run/p13 && cd /run/p13
	echo "############ CIX TCC (upgraded) ############"
	suite tcc ""
	echo
	echo "############ GCC, as the reference ############"
	suite /usr/bin/gcc "-O0"
	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
