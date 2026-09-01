#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# Exits nonzero at the end so its output lands in cixd's build-failure
# log without producing an artifact (probe-gcc-headers' pattern).
#
# THE QUESTION
#
# Probe 8 established that upstream tcc mob (0.9.28rc, commit 2ba12e83)
# fixes #216 -- the stack-slot aliasing that miscompiled perl -- and
# that it builds cleanly under Cix's own tcc, so upgrading costs
# nothing in self-hosting.
#
# That makes the upgrade worth considering on its own. But before
# committing to it, find out what ELSE it settles. Four other TCC
# issues are open against the 2017 release:
#
#   #208  seven missing builtins, emitted as undefined externals
#         rather than rejected -- silent in a shared library
#   #209  driver flags: -Wp,a,b,c comma lists, and -pthread, which
#         is not merely ignored but drops the source file argument
#   #210  glibc header constructs (__REDIRECT, _FILE_OFFSET_BITS=64)
#   #211  designated compound literals as array elements
#   #212  no C11 at all -- blocks libuv and screen
#
# Every test below is probe 4's, unchanged in substance, but run
# against BOTH compilers so the two columns can be compared directly.
# Probe 4's answers are the baseline; this says which of them move.
#
# Pinned to one commit, not to `mob`: a branch is not a reproducible
# source. Same commit and checksum probe 8 verified against two
# independent fetches.
#
pkg_name="probe-tcc-conformance"
pkg_version="9"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="9: run the whole #208-#212 suite against BOTH Cix's tcc 0.9.27 and upstream mob 2ba12e83, to find out how many issues the #216 upgrade would close at once"

suite() {
	CC="$1"
	CCX="$2"

	echo "  --- version ---"
	$CC $CCX -v 2>&1 | head -1 | sed 's/^/    /'

	echo "  --- 1. BUILTINS (#208) ---"
	probe_builtin() {
		printf '%s\n' "$2" > b.c
		out=$($CC $CCX -c b.c -o b.o 2>&1) || true
		if printf '%s' "$out" | grep -q "implicit declaration"; then echo "    MISSING  $1"
		elif [ -n "$out" ]; then echo "    ERROR    $1 -- $(printf '%s' "$out" | head -1)"
		else echo "    ok       $1"; fi
	}
	probe_builtin __builtin_ffs      'int f(unsigned v){ return __builtin_ffs(v); }'
	probe_builtin __builtin_clz      'int f(unsigned v){ return __builtin_clz(v); }'
	probe_builtin __builtin_clzl     'int f(unsigned long v){ return __builtin_clzl(v); }'
	probe_builtin __builtin_clzll    'int f(unsigned long long v){ return __builtin_clzll(v); }'
	probe_builtin __builtin_ctzl     'int f(unsigned long v){ return __builtin_ctzl(v); }'
	probe_builtin __builtin_popcount 'int f(unsigned v){ return __builtin_popcount(v); }'
	probe_builtin __builtin_bswap16  'unsigned short f(unsigned short v){ return __builtin_bswap16(v); }'
	probe_builtin __builtin_bswap32  'unsigned int f(unsigned int v){ return __builtin_bswap32(v); }'
	probe_builtin __builtin_bswap64  'unsigned long long f(unsigned long long v){ return __builtin_bswap64(v); }'
	probe_builtin __builtin_memcpy   'void f(void*a,void*b){ __builtin_memcpy(a,b,8); }'
	probe_builtin __builtin_memcmp   'int f(void*a,void*b){ return __builtin_memcmp(a,b,8); }'
	probe_builtin __builtin_memset   'void f(void*a){ __builtin_memset(a,0,8); }'
	probe_builtin __builtin_memmove  'void f(void*a,void*b){ __builtin_memmove(a,b,8); }'
	probe_builtin __builtin_alloca   'void *f(void){ return __builtin_alloca(16); }'
	probe_builtin __builtin_offsetof 'struct s{int a;int b;}; unsigned long f(void){ return __builtin_offsetof(struct s,b); }'

	echo "  --- 2. INLINE LINKAGE ACROSS TWO TUs ---"
	for kind in "static inline" "inline" "extern inline"; do
		printf '#ifndef H\n#define H\n%s int helper(int x){ return x+1; }\n#endif\n' "$kind" > h.h
		printf '#include "h.h"\nint pa(int x){ return helper(x); }\n' > p.c
		printf '#include "h.h"\nint pa(int);\nint main(void){ return pa(helper(1))==3?0:1; }\n' > q.c
		$CC $CCX -c p.c -o p.o 2>/dev/null || true
		$CC $CCX -c q.c -o q.o 2>/dev/null || true
		sym=$(nm p.o 2>/dev/null | grep " helper$" | awk '{print $2}')
		out=$($CC $CCX -o prog p.o q.o 2>&1) || true
		if [ -n "$out" ]; then
			echo "    '$kind': symbol=${sym:-?}  LINK FAILED"
		else
			r=ran-WRONG; ./prog && r=ran-ok || true
			echo "    '$kind': symbol=${sym:-?}  links ok, $r"
		fi
	done

	echo "  --- 3. DRIVER FLAGS (#209) ---"
	printf '#include <stdio.h>\nint main(void){\n#ifdef FOO\nputs("FOO defined");\n#else\nputs("FOO NOT defined");\n#endif\nreturn 0;}\n' > w.c
	out=$($CC $CCX -Wp,-DFOO -o w1 w.c 2>&1) || true
	if [ -n "$out" ]; then echo "    -Wp,-DFOO        FAILS -- $(printf '%s' "$out" | head -1)"
	else printf '    -Wp,-DFOO        ok -- '; ./w1 || true; fi
	printf 'int main(void){ return 0; }\n' > t.c
	out=$($CC $CCX -pthread t.c -o t1 2>&1) || true
	if [ -n "$out" ]; then echo "    -pthread         FAILS -- $(printf '%s' "$out" | head -1)"
	else echo "    -pthread         accepted, binary=$([ -x t1 ] && echo yes || echo NO)"; fi

	echo "  --- 4. GLIBC HEADERS (#210) ---"
	for m in __REDIRECT __REDIRECT_NTH; do
		printf '#include <sys/cdefs.h>\nstruct g{int n;};\nextern int %s (myfn,(const char *p, struct g *pg), myfn64);\nint main(void){return 0;}\n' "$m" > v.c
		out=$($CC $CCX -c v.c -o v.o 2>&1) || true
		[ -n "$out" ] && echo "    $m  FAILS -- $(printf '%s' "$out" | head -1)" || echo "    $m  ok"
	done
	for h in stdio.h fcntl.h unistd.h sys/stat.h; do
		printf '#include <%s>\nint main(void){return 0;}\n' "$h" > hh.c
		out=$($CC $CCX -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -c hh.c -o hh.o 2>&1) || true
		[ -n "$out" ] && echo "    _FILE_OFFSET_BITS=64 <$h>  FAILS" || echo "    _FILE_OFFSET_BITS=64 <$h>  ok"
	done

	echo "  --- 5. COMPOUND LITERALS (#211) ---"
	cat > cl.c <<'EOF'
#include <stddef.h>
struct insn { unsigned char code; unsigned char dst:4; unsigned char src:4; short off; int imm; };
struct sk { unsigned bound_dev_if; unsigned family; };
#define MOV(D,I) ((struct insn){ .code=0xb7, .dst=D, .src=0, .off=0, .imm=I })
int main(void){
	struct insn prog[] = { MOV(6,1), MOV(3,2), MOV(2, offsetof(struct sk, bound_dev_if)), MOV(0,1) };
	return prog[0].imm;
}
EOF
	out=$($CC $CCX -c cl.c -o cl.o 2>&1) || true
	[ -n "$out" ] && echo "    compound literal array init          FAILS -- $(printf '%s' "$out" | head -1)" \
	               || echo "    compound literal array init          ok"
	cat > cl2.c <<'EOF'
enum { IDX_A, IDX_B, IDX_C };
struct opt { const char *name; int val; const char *arg; const char *help; };
#define OPT(n,v,a,h) (struct opt){ .name=n, .val=v, .arg=a, .help=h }
static const struct opt options[] = {
        [IDX_A] = OPT("help",    1, 0, "show help"),
        [IDX_B] = OPT("version", 2, 0, "show version"),
        [IDX_C] = OPT("file",    3, "<f>", "read input"),
};
int main(void){ return options[0].val; }
EOF
	out=$($CC $CCX -c cl2.c -o cl2.o 2>&1) || true
	[ -n "$out" ] && echo "    indexed compound literal array init  FAILS -- $(printf '%s' "$out" | head -1)" \
	               || echo "    indexed compound literal array init  ok"

	echo "  --- 6. C11 (#212) ---"
	printf '#include <stdio.h>\nint main(void){\n#ifdef __STDC_VERSION__\nprintf("%%ld\\n",(long)__STDC_VERSION__);\n#else\nprintf("undefined\\n");\n#endif\nreturn 0;}\n' > sv.c
	if $CC $CCX -o sv sv.c 2>/dev/null; then printf '    __STDC_VERSION__ = '; ./sv || true
	else echo "    __STDC_VERSION__ probe did not build"; fi
	for h in stdatomic.h stdalign.h stdnoreturn.h threads.h; do
		printf '#include <%s>\nint main(void){return 0;}\n' "$h" > h11.c
		out=$($CC $CCX -c h11.c -o h11.o 2>&1) || true
		[ -n "$out" ] && echo "    MISSING  <$h>" || echo "    ok       <$h>"
	done
	printf 'int main(void){ _Atomic int x = 0; return x; }\n' > at.c
	out=$($CC $CCX -c at.c -o at.o 2>&1) || true
	[ -n "$out" ] && echo "    _Atomic          MISSING" || echo "    _Atomic          ok"
	printf 'int main(void){ _Static_assert(1, "ok"); return 0; }\n' > sa.c
	out=$($CC $CCX -c sa.c -o sa.o 2>&1) || true
	[ -n "$out" ] && echo "    _Static_assert   MISSING" || echo "    _Static_assert   ok"

	echo "  --- 7. STACK SLOT ALIASING (#216) ---"
	cat > r.c <<'EOF'
#include <stdio.h>
void *opaque(int); int pick(void);
static int A(int which){
	switch (which) { case 0: return -1;
	{ void *p; void *q;
	case 1:
		if (pick()) p = opaque(1); else p = opaque(2);
		q = NULL;
		if (p == NULL) { printf("    A FAIL  p is NULL (shared slot)\n"); return 1; }
		printf("    A ok\n"); return 0;
	} }
	return -1;
}
void *opaque(int n){ static char buf[8]; return buf + (n & 1); }
int pick(void){ return 1; }
int main(void){ return A(1); }
EOF
	if $CC $CCX -o r r.c 2>&1; then ./r || true; else echo "    (did not compile)"; fi
}

pkg_build() {
	mkdir -p /run/probe && cd /build/src

	echo "=== building upstream tcc mob 2ba12e83 with Cix's tcc ==="
	./configure --prefix=/run/newtcc --cc=tcc > /run/probe/cfg.log 2>&1 || { echo "configure FAILED"; tail -20 /run/probe/cfg.log; exit 1; }
	make -j"$(nproc)" > /run/probe/make.log 2>&1 || { echo "make FAILED"; tail -30 /run/probe/make.log; exit 1; }
	make install > /run/probe/inst.log 2>&1 || true
	echo "  built: $(ls -la /run/newtcc/bin/tcc 2>&1)"

	cd /run/probe

	echo
	echo "############ A. CIX'S CURRENT TCC (0.9.27, Dec 2017) ############"
	suite tcc ""

	echo
	echo "############ B. UPSTREAM MOB 2ba12e83 (0.9.28rc) ############"
	suite /run/newtcc/bin/tcc "-B/run/newtcc/lib/tcc"

	echo
	echo "=== probe complete -- failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	:
}
