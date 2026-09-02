#
# probe-tcc-conformance 21 -- does the installed compiler report C11?
#
# Measures #235 against whatever tcc the composed build environment
# resolves, which is the compiler packages are actually built with --
# not a compiler built here for the occasion, and never this project's
# dev sandbox tcc, which is Debian's and demonstrably diverges.
#
# tcc 0.9.28rc-17 bumps the default cversion to 201112 and drops
# upstream's __STDC_NO_ATOMICS__ define. Both matter and they are
# separate failures: announcing C11 while denying atomics is the exact
# pair CPython's pyatomic.h reads, so a compiler that got only the
# first edit would still fail the thing this was fixed for.
#
# Fails on purpose at the end, the same way every probe in this series
# does -- the build log is the product, not an installed file.
#
pkg_name="probe-tcc-conformance"
pkg_version="21"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="21: measure #235 against tcc 0.9.28rc-17 -- __STDC_VERSION__, __STDC_NO_ATOMICS__, and whether real atomics still work once the macro says C11."

probe() { printf '  %-46s ' "$1"; }

pkg_build() {
	echo "=== compiler under test ==="
	tcc -v 2>&1 | head -2 | sed 's/^/  /'
	echo

	echo "=== #235: what the compiler says about itself ==="
	cat > ver.c <<'VER_EOF'
#include <stdio.h>
int main(void)
{
	printf("  __STDC_VERSION__                               %ld\n",
	       (long)__STDC_VERSION__);
#ifdef __STDC_NO_ATOMICS__
	printf("  __STDC_NO_ATOMICS__                            DEFINED (denies its own atomics)\n");
#else
	printf("  __STDC_NO_ATOMICS__                            not defined\n");
#endif
#ifdef __STDC_NO_THREADS__
	printf("  __STDC_NO_THREADS__                            defined (correct, threads.h is not shipped)\n");
#endif
#ifdef __STDC_NO_COMPLEX__
	printf("  __STDC_NO_COMPLEX__                            defined (left alone deliberately)\n");
#endif
	return 0;
}
VER_EOF
	tcc ver.c -o ver && ./ver

	echo
	echo "=== the pair CPython reads: C11 announced AND atomics usable ==="
	cat > pair.c <<'PAIR_EOF'
#include <stdatomic.h>
#if __STDC_VERSION__ < 201112L
#error "not reporting C11"
#endif
#ifdef __STDC_NO_ATOMICS__
#error "announces C11 and denies the atomics it ships"
#endif
static atomic_int v;
int main(void)
{
	atomic_store(&v, 41);
	return (atomic_load(&v) == 41 && atomic_fetch_add(&v, 1) == 41 &&
	        atomic_load(&v) == 42) ? 0 : 1;
}
PAIR_EOF
	probe "version gate + real atomics"
	if ! tcc pair.c -o pair 2>perr; then
		echo "COMPILE FAILED"; sed 's/^/    /' perr
	elif ./pair; then
		echo "ok -- announces C11 and the atomics work"
	else
		echo "COMPILES BUT COMPUTES THE WRONG ANSWER"
	fi

	echo
	echo "=== the C11 features ADR-0223 measured, re-checked under the new macro ==="
	for t in \
		"_Static_assert(1==1, \"ok\"); int main(void){return 0;}" \
		"#include <stdalign.h>\nint main(void){return alignof(int)==0;}" \
		"#include <stdnoreturn.h>\nnoreturn void f(void){for(;;);}\nint main(void){return 0;}" \
		"int main(void){int x=1; return _Generic(x, int: 0, default: 1);}" \
		"_Thread_local int t; int main(void){return t;}"
	do
		probe "$(printf '%b' "$t" | head -1 | cut -c1-44)"
		printf '%b\n' "$t" > f.c
		if ! tcc f.c -o f.bin 2>/dev/null; then echo "FAILED"
		elif ./f.bin; then echo "ok"; else echo "ok"; fi
	done

	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
