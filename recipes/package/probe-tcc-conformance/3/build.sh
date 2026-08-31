#
# SCRATCH DIAGNOSTIC ONLY (v2: v1 aborted at the first failing
# command -- recipes run under set -e, so `out=$(failing-cmd)` ends the
# script. Every capture is now `|| true`.)
#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# Same pattern as probe-gcc-headers.recipe: it exits nonzero at the end
# so its output lands in cixd's build-failure log without producing an
# artifact.
#
# WHY THIS EXISTS
#
# A batch of TCC conformance claims (#208 missing builtins, #209 driver
# flags, #210 glibc header parsing, #211 compound literals, and a
# CLAUDE.md note about inline linkage) were probed against a DEV
# SANDBOX's Debian-packaged tcc rather than against Cix's own
# tcc@0.9.27-10. Those are different compilers, and it is already
# proven they diverge: the Debian one rejects the anonymous union in
# the kernel's bpf.h that Cix's parses, and compiles a bpf_insn
# reduction that Cix's rejects with "index too large".
#
# So every one of those claims is unverified against the compiler this
# platform actually uses. This recipe re-runs all of them on a Cix
# host, with Cix's toolchain, which is the only place the answers mean
# anything.
#
# The source tarball is irrelevant -- it is only here because a recipe
# needs one. Same one probe-gcc-headers uses, with the same checksum.
#
pkg_name="probe-tcc-conformance"
pkg_version="3"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"

pkg_build() {
	echo "=== which tcc, and what version ==="
	tcc -v 2>&1 | head -2

	cd /run || cd .
	mkdir -p probe && cd probe

	echo
	echo "=== 1. BUILTINS (#208) ==="
	probe_builtin() {
		name="$1"; body="$2"
		printf '%s\n' "$body" > b.c
		out=$(tcc -c b.c -o b.o 2>&1) || true
		if printf '%s' "$out" | grep -q "implicit declaration"; then
			echo "  MISSING  $name"
		elif [ -n "$out" ]; then
			echo "  ERROR    $name -- $(printf '%s' "$out" | head -1)"
		else
			echo "  ok       $name"
		fi
	}
	probe_builtin __builtin_ffs        'int f(unsigned v){ return __builtin_ffs(v); }'
	probe_builtin __builtin_clz        'int f(unsigned v){ return __builtin_clz(v); }'
	probe_builtin __builtin_clzl       'int f(unsigned long v){ return __builtin_clzl(v); }'
	probe_builtin __builtin_clzll      'int f(unsigned long long v){ return __builtin_clzll(v); }'
	probe_builtin __builtin_ctzl       'int f(unsigned long v){ return __builtin_ctzl(v); }'
	probe_builtin __builtin_popcount   'int f(unsigned v){ return __builtin_popcount(v); }'
	probe_builtin __builtin_bswap16    'unsigned short f(unsigned short v){ return __builtin_bswap16(v); }'
	probe_builtin __builtin_bswap32    'unsigned int f(unsigned int v){ return __builtin_bswap32(v); }'
	probe_builtin __builtin_bswap64    'unsigned long long f(unsigned long long v){ return __builtin_bswap64(v); }'
	probe_builtin __builtin_add_overflow 'int f(int a,int b,int*c){ return __builtin_add_overflow(a,b,c); }'
	probe_builtin __builtin_constant_p 'int f(void){ return __builtin_constant_p(1); }'
	probe_builtin __builtin_expect     'int f(int x){ return __builtin_expect(x,1); }'
	probe_builtin __builtin_memcpy     'void f(void*a,void*b){ __builtin_memcpy(a,b,8); }'
	probe_builtin __builtin_memcmp     'int f(void*a,void*b){ return __builtin_memcmp(a,b,8); }'
	probe_builtin __builtin_alloca     'void *f(void){ return __builtin_alloca(16); }'
	probe_builtin __builtin_memset     'void f(void*a){ __builtin_memset(a,0,8); }'
	probe_builtin __builtin_memmove    'void f(void*a,void*b){ __builtin_memmove(a,b,8); }'
	probe_builtin __builtin_offsetof   'struct s{int a;int b;}; unsigned long f(void){ return __builtin_offsetof(struct s,b); }'
	probe_builtin __builtin_types_compatible_p 'int f(void){ return __builtin_types_compatible_p(int,int); }'
	probe_builtin __builtin_choose_expr 'int f(void){ return __builtin_choose_expr(1,2,3); }'

	echo
	echo "=== 2. INLINE LINKAGE ACROSS TWO TUs (CLAUDE.md note) ==="
	for kind in "static inline" "static" "inline" "extern inline"; do
		printf '#ifndef H\n#define H\n%s int helper(int x){ return x+1; }\n#endif\n' "$kind" > h.h
		printf '#include "h.h"\nint pa(int x){ return helper(x); }\n' > p.c
		printf '#include "h.h"\nint pa(int);\nint main(void){ return pa(helper(1))==3?0:1; }\n' > q.c
		tcc -c p.c -o p.o 2>/dev/null || true
		tcc -c q.c -o q.o 2>/dev/null || true
		sym=$(nm p.o 2>/dev/null | grep " helper$" | awk '{print $2}')
		out=$(tcc -o prog p.o q.o 2>&1) || true
		if [ -n "$out" ]; then
			echo "  '$kind': symbol=${sym:-?}  LINK FAILED -- $(printf '%s' "$out" | head -1)"
		else
			r=ran-WRONG; ./prog && r=ran-ok || true
			echo "  '$kind': symbol=${sym:-?}  links ok, $r"
		fi
	done

	echo
	echo "=== 3. DRIVER FLAGS (#209) ==="
	printf '#include <stdio.h>\nint main(void){\n#ifdef FOO\nputs("FOO defined");\n#else\nputs("FOO NOT defined");\n#endif\nreturn 0;}\n' > w.c
	out=$(tcc -Wp,-DFOO -o w1 w.c 2>&1) || true
	[ -n "$out" ] && echo "  -Wp,-DFOO           FAILS -- $out" || { printf '  -Wp,-DFOO           ok -- '; ./w1 || true; }
	out=$(tcc -Wp,-MMD,dep.d,-MT,w.o -c w.c -o w.o 2>&1) || true
	[ -n "$out" ] && echo "  -Wp,-MMD,a,-MT,b    FAILS -- $out" || echo "  -Wp,-MMD,a,-MT,b    accepted"
	printf 'int main(void){ return 0; }\n' > t.c
	out=$(tcc -pthread t.c -o t1 2>&1) || true
	[ -n "$out" ] && echo "  -pthread            FAILS -- $out" || echo "  -pthread            accepted"

	echo
	echo "=== 4. GLIBC HEADER DECLARATIONS (#210) ==="
	for m in __REDIRECT __REDIRECT_NTH __REDIRECT_NTHNL; do
		printf '#include <sys/cdefs.h>\nstruct g{int n;};\nextern int %s (myfn,(const char *p, struct g *pg), myfn64);\nint main(void){return 0;}\n' "$m" > v.c
		out=$(tcc -c v.c -o v.o 2>&1) || true
		[ -n "$out" ] && echo "  $m  FAILS -- $(printf '%s' "$out" | head -1)" || echo "  $m  ok"
	done
	for h in stdio.h fcntl.h unistd.h sys/stat.h dirent.h glob.h ftw.h stdlib.h; do
		printf '#include <%s>\nint main(void){return 0;}\n' "$h" > hh.c
		out=$(tcc -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -c hh.c -o hh.o 2>&1) || true
		[ -n "$out" ] && echo "  -D_FILE_OFFSET_BITS=64 <$h>  FAILS -- $(printf '%s' "$out" | tail -1)" \
		               || echo "  -D_FILE_OFFSET_BITS=64 <$h>  ok"
	done

	echo
	echo "=== 5. DESIGNATED COMPOUND LITERAL AS ARRAY ELEMENT (#211) ==="
	cat > cl.c <<'EOF'
#include <stddef.h>
struct insn { unsigned char code; unsigned char dst:4; unsigned char src:4; short off; int imm; };
struct sk { unsigned bound_dev_if; unsigned family; };
#define MOV(D,I) ((struct insn){ .code=0xb7, .dst=D, .src=0, .off=0, .imm=I })
int main(void){
	struct insn prog[] = {
		MOV(6,1), MOV(3,2),
		MOV(2, offsetof(struct sk, bound_dev_if)),
		MOV(0,1),
	};
	return prog[0].imm;
}
EOF
	out=$(tcc -c cl.c -o cl.o 2>&1) || true
	[ -n "$out" ] && echo "  compound literal array init  FAILS -- $(printf '%s' "$out" | head -1)" \
	               || echo "  compound literal array init  ok"

	echo
	echo "=== 5b. THE SAME, WITH ARRAY-INDEX DESIGNATORS (#211, nftables shape) ==="
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
	out=$(tcc -c cl2.c -o cl2.o 2>&1) || true
	[ -n "$out" ] && echo "  indexed compound literal array init  FAILS -- $(printf '%s' "$out" | head -1)" \
	               || echo "  indexed compound literal array init  ok"

	echo
	echo "=== PROBE COMPLETE -- failing deliberately so this log is kept ==="
	exit 1
}
