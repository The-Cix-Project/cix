#
# SCRATCH DIAGNOSTIC ONLY -- never installed. Exits nonzero so its
# output is kept.
#
# THE QUESTION, AND IT IS URGENT
#
# tcc was upgraded to upstream mob 2ba12e83 (0.9.28rc-7), which fixes
# #216: perl's miniperl no longer segfaults, and a TCC build of perl
# now gets past DynaLoader and the whole Unicode pass -- far further
# than it has ever reached.
#
# It then died linking an XS module, with TCC ITSELF segfaulting:
#
#   tcc[21821]: segfault at 48 ip 0x466a9a in tcc[65a9a,415000+58000]
#   make[1]: *** [.../Bzip2.so] Segmentation fault
#
# The command was a `-shared` link. Many packages in this set build
# shared libraries with tcc -- libmnl, libnl, libnftnl, iptables,
# ipset, elfutils. If the new compiler crashes on -shared links
# generally, the upgrade breaks far more than it fixes and has to be
# reverted. If it needs something specific to that link, it is a
# narrow issue to file.
#
# So isolate it: a trivial shared library, then progressively closer to
# what perl was doing (many objects, -O2, an archive of real code).
#
pkg_name="probe-tcc-conformance"
pkg_version="11"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="11: does tcc 0.9.28rc crash on -shared links in general, or only on the specific one perl's Bzip2 XS module does? Decides whether the upgrade is safe or must be reverted"

pkg_build() {
	echo "=== compiler under test ==="
	tcc -v 2>&1 | head -1

	mkdir -p /run/p11 && cd /run/p11

	echo
	echo "=== 1. trivial shared library ==="
	printf 'int add(int a,int b){return a+b;}\n' > a.c
	if tcc -shared a.c -o liba.so 2>&1; then
		echo "  ok  ($(stat -c%s liba.so) bytes)"
	else
		echo "  FAILED (exit $?)"
	fi

	echo
	echo "=== 2. shared library from separate objects, like a real build ==="
	printf 'int add(int a,int b){return a+b;}\n'      > o1.c
	printf 'int sub_(int a,int b){return a-b;}\n'     > o2.c
	printf 'extern int add(int,int);\nint mul(int a,int b){int r=0,i;for(i=0;i<b;i++)r=add(r,a);return r;}\n' > o3.c
	for f in o1 o2 o3; do tcc -c "$f.c" -o "$f.o" 2>&1 || echo "  compile $f FAILED"; done
	if tcc -shared o1.o o2.o o3.o -o libo.so 2>&1; then
		echo "  ok  ($(stat -c%s libo.so) bytes)"
	else
		echo "  FAILED (exit $?)"
	fi

	echo
	echo "=== 3. the same with -O2, which perl's link used ==="
	for f in o1 o2 o3; do tcc -O2 -c "$f.c" -o "$f.oo" 2>&1 || echo "  compile $f FAILED"; done
	if tcc -shared -O2 o1.oo o2.oo o3.oo -o libo2.so 2>&1; then
		echo "  ok  ($(stat -c%s libo2.so) bytes)"
	else
		echo "  FAILED (exit $?)"
	fi

	echo
	echo "=== 4. eight objects, matching the shape of the failing link ==="
	rm -f many*.o
	i=0
	while [ $i -lt 8 ]; do
		printf 'int f%d(int x){return x+%d;}\nstatic int helper%d(int x){return x*2;}\nint g%d(int x){return helper%d(x);}\n' "$i" "$i" "$i" "$i" "$i" > "many$i.c"
		tcc -O2 -c "many$i.c" -o "many$i.o" 2>&1 || echo "  compile many$i FAILED"
		i=$((i+1))
	done
	if tcc -shared -O2 many0.o many1.o many2.o many3.o many4.o many5.o many6.o many7.o -o libmany.so 2>&1; then
		echo "  ok  ($(stat -c%s libmany.so) bytes)"
	else
		echo "  FAILED (exit $?)"
	fi

	echo
	echo "=== 5. and can a program actually LOAD one ==="
	cat > use.c <<'EOF'
#include <stdio.h>
extern int add(int,int);
int main(void){ printf("  add(2,3)=%d\n", add(2,3)); return add(2,3)==5?0:1; }
EOF
	if tcc use.c -L. -la -Wl,-rpath,. -o use 2>&1; then
		LD_LIBRARY_PATH=. ./use && echo "  ok" || echo "  built but ran WRONG"
	else
		echo "  link against the .so FAILED"
	fi

	echo
	echo "=== probe complete -- failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	:
}
