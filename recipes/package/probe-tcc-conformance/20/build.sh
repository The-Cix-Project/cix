#
# probe-tcc-conformance 18 -- ground truth for the four TCC defects
# about to be fixed (#208, #209, #219, #235).
#
# Written because the issue text for each predates the 0.9.28rc upgrade
# (ADR-0223), which fixed some of what those issues describe and left
# the rest. Fixing what an issue SAYS is broken, rather than what the
# compiler currently does, is how a recipe grows a workaround for a bug
# that no longer exists -- btrfs-progs carried exactly that for months.
#
# Fails on purpose: a probe's output is its product, and the build log
# is where it is read.
#
pkg_name="probe-tcc-conformance"
pkg_version="20"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="20: re-measure after tcc 0.9.28rc-16, which supplies __builtin_bswap* (#208)"

probe() { printf '  %-42s ' "$1"; }

pkg_build() {
	echo "=== compiler under test ==="
	tcc -v 2>&1 | head -2 | sed 's/^/  /'

	echo
	echo "=== #208: builtins, and whether each LINKS (not just compiles) ==="
	for b in \
		"__builtin_ffs(x)" "__builtin_clz((unsigned)x)" "__builtin_clzll((unsigned long long)x)" \
		"__builtin_popcount((unsigned)x)" "__builtin_bswap16((unsigned short)x)" \
		"__builtin_bswap32((unsigned)x)" "__builtin_bswap64((unsigned long long)x)"
	do
		probe "$b"
		printf 'int main(void){volatile int x=1; return (int)(%s);}\n' "$b" > b.c
		if ! tcc -c b.c -o b.o 2>/dev/null; then
			echo "COMPILE FAILED"
		elif ! tcc b.c -o b.bin 2>/dev/null; then
			echo "compiles, LINK FAILED -- undefined external"
		elif ./b.bin; then
			echo "ok (links and runs)"
		else
			echo "ok (links and runs)"
		fi
	done

	echo
	echo "  the dangerous shape: an undefined symbol is LEGAL in a shared library,"
	echo "  so a missing builtin ships silently rather than failing the link."
	printf 'unsigned s(unsigned v){return __builtin_bswap32(v);}\n' > so.c
	if tcc -shared so.c -o libso.so 2>/dev/null; then
		probe "bswap32 in a .so"
		if nm -D --undefined-only libso.so 2>/dev/null | grep -q bswap32; then
			echo "UNDEFINED IN THE .so  <-- the #176/#208 failure"
		else
			echo "resolved"
		fi
	fi

	echo
	echo "=== #219: constructor/destructor attribute, with and without priority ==="
	probe "bare constructor"
	printf 'void f(void) __attribute__((constructor));\nvoid f(void){}\nint main(void){return 0;}\n' > c1.c
	tcc c1.c -o c1 2>/dev/null && echo "ok" || echo "COMPILE FAILED"
	probe "constructor(300)"
	printf 'void f(void) __attribute__((constructor(300)));\nvoid f(void){}\nint main(void){return 0;}\n' > c2.c
	tcc c2.c -o c2 2>/dev/null && echo "ok" || echo "COMPILE FAILED  <-- #219"
	probe "priority ORDER honoured (100 then 900)"
	cat > c3.c <<'C3'
#include <stdio.h>
void a(void) __attribute__((constructor(100)));
void b(void) __attribute__((constructor(900)));
void a(void){ printf("early "); }
void b(void){ printf("late "); }
int main(void){ printf("main\n"); return 0; }
C3
	if tcc c3.c -o c3 2>/dev/null; then ./c3 | sed 's/^/output: /'; else echo "COMPILE FAILED"; fi

	echo
	echo "=== #209: -Wp, comma lists ==="
	printf '#ifdef FOO\n#ifdef BAR\nint main(void){return 0;}\n#endif\n#endif\n' > wp.c
	probe "-Wp,-DFOO (single)"
	tcc -Wp,-DFOO -DBAR wp.c -o wp1 2>/dev/null && echo "ok" || echo "FAILED"
	probe "-Wp,-DFOO,-DBAR (comma list)"
	tcc -Wp,-DFOO,-DBAR wp.c -o wp2 2>/dev/null && echo "ok" || echo "FAILED  <-- #209"

	echo
	echo "=== #235: what the version macro claims vs what works ==="
	probe "__STDC_VERSION__"
	printf '#include <stdio.h>\nint main(void){printf("%%ld\\n",(long)__STDC_VERSION__);return 0;}\n' > v.c
	tcc v.c -o v 2>/dev/null && ./v || echo "FAILED"
	for feat in \
		'_Static_assert(1,"x"); int main(void){return 0;}' \
		'#include <stdatomic.h>\natomic_int a;\nint main(void){atomic_store(&a,1);return atomic_load(&a)-1;}' \
		'#include <stdalign.h>\nint main(void){return (int)alignof(int)-4;}' \
		'#include <stdnoreturn.h>\nnoreturn void f(void);\nvoid f(void){for(;;);}\nint main(void){return 0;}' \
		'int main(void){int x=1; return _Generic(x, int: 0, default: 1);}' \
		'_Thread_local int t; int main(void){t=1;return t-1;}'
	do
		probe "$(printf '%b' "$feat" | head -1 | cut -c1-40)"
		printf '%b\n' "$feat" > f.c
		if tcc f.c -o f 2>/dev/null && ./f; then echo "works"; else echo "NOT AVAILABLE"; fi
	done

	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
