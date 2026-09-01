#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #222, python's turn. Its recipe records a precise reason:
#
#   under TCC the build dies at
#     ./Include/cpython/pyatomic.h:543: error:
#       "no available pyatomic implementation for this platform/compiler"
#   because pyatomic.h dispatches on GCC/clang builtins, MSVC, or C11
#   stdatomic.h, and TCC provides none of the three.
#
# The pinned compiler (ADR-0223) now has the third. So the reason may be
# stale in the same way btrfs-progs' was -- or it may not, because
# CLAUDE.md records a second fact that bears directly on this: TCC
# reports __STDC_VERSION__ as 199901 even though the C11 features are
# present, so a header that GATES on that macro rather than probing for
# the feature still takes its non-C11 path.
#
# Which of those two is true is a measurement, and the answer changes
# what should be written in python's recipe. Under ADR-0226 the point is
# not to force python onto TCC -- gcc is an ordinary choice for a
# third-party package -- it is that a recorded reason should be true.
#
pkg_name="probe-toolchain-retire"
pkg_version="3"
pkg_source="https://www.python.org/ftp/python/3.13.5/Python-3.13.5.tgz"
pkg_sha256="e6190f52699b534ee203d9f417bdbca05a92f23e35c19c691a50ed2942835385"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc binutils linux-headers sed grep gawk pkgconf findutils m4"
pkg_changelog="3: is python's stated gcc reason (no C11 stdatomic under TCC) still true (#222)"

pkg_build() {
	echo "=== what does this compiler actually claim and provide? ==="
	cat > /run/v.c <<'V_EOF'
#include <stdio.h>
int main(void) {
#ifdef __STDC_VERSION__
	printf("  __STDC_VERSION__ = %ldL\n", (long)__STDC_VERSION__);
#else
	printf("  __STDC_VERSION__ undefined\n");
#endif
	return 0;
}
V_EOF
	tcc /run/v.c -o /run/v && /run/v

	printf '  <stdatomic.h> usable   : '
	cat > /run/a.c <<'A_EOF'
#include <stdatomic.h>
static atomic_int counter;
int main(void) { atomic_store(&counter, 3); return atomic_load(&counter) == 3 ? 0 : 1; }
A_EOF
	if tcc /run/a.c -o /run/a 2>/run/a.err && /run/a; then
		echo "yes (compiles and runs)"
	else
		echo "NO"; sed 's/^/    /' /run/a.err
	fi

	echo
	echo "=== does pyatomic.h's own dispatch accept this compiler? ==="
	# The exact question the recipe's recorded reason is about, asked
	# directly rather than inferred from a full build.
	cat > /run/p.c <<'P_EOF'
#include "Include/Python.h"
int main(void) { return 0; }
P_EOF
	if tcc -I. -IInclude -c /run/p.c -o /run/p.o 2>/run/p.err; then
		echo "  Python.h (and pyatomic.h through it): compiles under tcc"
	else
		echo "  Python.h under tcc:"
		grep -iE 'pyatomic|no available|error' /run/p.err | head -6 | sed 's/^/    /'
	fi

	echo
	echo "=== and the real build, recipe's own configure line, only CC changed ==="
	if ! CC=tcc ./configure --prefix=/usr --enable-shared --with-ensurepip=no \
	     > /run/conf.log 2>&1; then
		echo "  configure FAILED under tcc:"
		tail -12 /run/conf.log | sed 's/^/    /'
		exit 1
	fi
	echo "  configure: ok"
	if ! make CC=tcc -j"$(nproc)" > /run/make.log 2>&1; then
		echo "  make FAILED under tcc -- first errors:"
		grep -iE 'error|Error [0-9]' /run/make.log | head -10 | sed 's/^/    /'
		exit 1
	fi
	echo "  make: SUCCEEDED under tcc"
	exit 1
}

pkg_install() {
	echo "probe-toolchain-retire is a diagnostic; nothing is installed" >&2
	exit 1
}
