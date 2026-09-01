#
# SCRATCH DIAGNOSTIC ONLY -- never installed. Exits nonzero so its
# output is kept in the build-failure log.
#
# THE QUESTION
#
# The upgrade to upstream mob is decided (probes 8 and 9). This asks
# what tcc.recipe's own local work is still FOR afterwards, so the new
# revision keeps what is needed and drops what is not, rather than
# carrying all of it forward untested or dropping it all and finding
# out later.
#
# tcc@0.9.27-14 carries six local changes:
#
#   1. atomic.o added to libtcc1.a          -- runtime for C11 atomics
#   2. dso_handle.o added to libtcc1.a      -- __dso_handle, which seven
#                                              recipes each stubbed
#   3. __ATOMIC_* macros predefined
#   4. bcheck.c malloc-hooks fix for glibc >= 2.34
#   5. the #122 do-while codegen fix
#   6. the #211 compound-literal fix
#
# 4 is ALREADY answered: probes 8 and 9 built mob with none of these
# seds applied, so it is not needed. 6 is answered by probe 9 (mob
# handles it natively). This measures 1, 2, 3 and 5.
#
pkg_name="probe-tcc-conformance"
pkg_version="10"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="10: does upstream mob supply what tcc.recipe's local work supplies -- __dso_handle, the __ATOMIC_* macros, the atomics runtime, the #122 do-while fix, __has_include"

props() {
	CC="$1"; CCX="$2"
	echo "  --- $($CC $CCX -v 2>&1 | head -1) ---"

	echo "  1. __has_include, all three answers"
	cat > hi.c <<'EOF'
#if __has_include(<stdio.h>)
int present = 1;
#else
#error "said an existing header is absent"
#endif
#if __has_include(<cix_definitely_not_real.h>)
#error "said a nonexistent header is present"
#endif
#if __has_include("cix_not_here_either.h")
#error "said a nonexistent quoted header is present"
#endif
int main(void){ return present ? 0 : 1; }
EOF
	out=$($CC $CCX -o hi hi.c 2>&1) || true
	if [ -n "$out" ]; then echo "     FAILS -- $(printf '%s' "$out" | head -1)"
	else ./hi && echo "     ok" || echo "     built but ran WRONG"; fi

	echo "  2. do-while whose body ends unreachable (#122)"
	cat > dw.c <<'EOF'
#include <stdio.h>
static int n = 0;
static int cond(void){ n++; return n < 3; }
int main(void){
	int iters = 0;
	do {
		iters++;
		if (iters > 10) break;
		continue;
	} while (cond());
	printf("     iterations=%d (want 3)\n", iters);
	return iters == 3 ? 0 : 1;
}
EOF
	if $CC $CCX -o dw dw.c 2>&1; then ./dw && echo "     ok" || echo "     WRONG -- condition dropped"; else echo "     did not compile"; fi

	echo "  3. __ATOMIC_* predefined macros"
	cat > am.c <<'EOF'
#ifndef __ATOMIC_SEQ_CST
#error "__ATOMIC_SEQ_CST not predefined"
#endif
#ifndef __ATOMIC_RELAXED
#error "__ATOMIC_RELAXED not predefined"
#endif
#ifndef __ATOMIC_ACQUIRE
#error "__ATOMIC_ACQUIRE not predefined"
#endif
int main(void){ return __ATOMIC_SEQ_CST == 5 ? 0 : 1; }
EOF
	out=$($CC $CCX -o am am.c 2>&1) || true
	if [ -n "$out" ]; then echo "     MISSING -- $(printf '%s' "$out" | head -1)"
	else ./am && echo "     ok (and SEQ_CST == 5)" || echo "     predefined but SEQ_CST != 5"; fi

	echo "  4. __dso_handle at link time"
	cat > dh.c <<'EOF'
#include <stdio.h>
extern void *__dso_handle;
int main(void){ printf("     __dso_handle=%p\n", &__dso_handle); return 0; }
EOF
	out=$($CC $CCX -o dh dh.c 2>&1) || true
	if [ -n "$out" ]; then echo "     MISSING -- $(printf '%s' "$out" | head -1)"
	else ./dh && echo "     ok"; fi

	echo "  5. C11 atomics actually RUN (needs the atomics runtime)"
	cat > at.c <<'EOF'
#include <stdatomic.h>
#include <stdio.h>
int main(void){
	atomic_int a;
	atomic_init(&a, 1);
	atomic_fetch_add(&a, 41);
	printf("     atomic result=%d (want 42)\n", atomic_load(&a));
	return atomic_load(&a) == 42 ? 0 : 1;
}
EOF
	out=$($CC $CCX -o at at.c 2>&1) || true
	if [ -n "$out" ]; then echo "     FAILS -- $(printf '%s' "$out" | head -1)"
	else ./at && echo "     ok" || echo "     built but ran WRONG"; fi
}

pkg_build() {
	mkdir -p /run/p10 && cd /build/src
	echo "=== building upstream mob 2ba12e83 with Cix's tcc, NO local patches ==="
	./configure --prefix=/run/newtcc --cc=tcc > /run/p10/cfg.log 2>&1 || { echo "configure FAILED"; tail -20 /run/p10/cfg.log; exit 1; }
	make -j"$(nproc)" > /run/p10/make.log 2>&1 || { echo "make FAILED"; tail -30 /run/p10/make.log; exit 1; }
	make install > /run/p10/inst.log 2>&1 || true
	echo "  ok"
	echo "  libtcc1.a members mentioning atomic/dso:"
	ar t /run/newtcc/lib/tcc/libtcc1.a 2>/dev/null | grep -Ei 'atomic|dso' | sed 's/^/    /' || echo "    (none)"

	cd /run/p10
	echo
	echo "############ CIX TCC 0.9.27-14 (with all local patches) ############"
	props tcc ""
	echo
	echo "############ UPSTREAM MOB, UNPATCHED ############"
	props /run/newtcc/bin/tcc "-B/run/newtcc/lib/tcc"

	echo
	echo "=== probe complete -- failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	:
}
