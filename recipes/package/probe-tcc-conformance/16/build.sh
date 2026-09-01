#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# htop's configure fails with:
#
#   checking for NaN support... no
#   configure: error: cannot find required macros: NAN, isgreater()
#                     and isgreaterequal()
#
# Two guesses have already been wrong: that -DNAN=(__builtin_nanf(""))
# covered it, and that adding __builtin_isgreater/isgreaterequal would
# finish the job. Rather than guess a third time, test each piece of
# what that check needs, separately, so the answer names the piece.
#
pkg_name="probe-tcc-conformance"
pkg_version="16"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="16: which of NAN, __builtin_nanf, isgreater and isgreaterequal does TCC actually lack (#87)"

probe() {
	name="$1"; body="$2"; extra="$3"
	printf '%s\n' "$body" > p.c
	printf '  %-42s ' "$name"
	if out=$(tcc $extra -o p p.c -lm 2>&1); then
		./p >/dev/null 2>&1 && echo "ok (compiles and runs)" || echo "compiles, RUNS WRONG"
	else
		echo "FAILS: $(printf '%s' "$out" | head -1)"
	fi
}

pkg_build() {
	mkdir -p /run/p16 && cd /run/p16
	echo "=== $(tcc -v 2>&1 | head -1) ==="

	probe "NAN from <math.h>, unmodified" \
'#include <math.h>
int main(void){ double d = NAN; return d == d ? 1 : 0; }' ""

	probe "__builtin_nanf" \
'int main(void){ float f = __builtin_nanf(""); return f == f ? 1 : 0; }' ""

	probe "NAN via -DNAN (the current recipe)" \
'#include <math.h>
int main(void){ double d = NAN; return d == d ? 1 : 0; }' '-DNAN=(__builtin_nanf(""))'

	probe "isgreater from <math.h>, unmodified" \
'#include <math.h>
int main(void){ return isgreater(2.0, 1.0) ? 0 : 1; }' ""

	probe "isgreaterequal from <math.h>" \
'#include <math.h>
int main(void){ return isgreaterequal(2.0, 2.0) ? 0 : 1; }' ""

	probe "isgreater with __builtin_ shimmed" \
'#include <math.h>
int main(void){ return isgreater(2.0, 1.0) ? 0 : 1; }' '-D__builtin_isgreater(x,y)=((x)>(y))'

	echo
	echo "=== htop's actual check, all three together ==="
	cat > all.c <<'EOF'
#include <math.h>
int main(void)
{
	double n = NAN;
	if (n == n) return 1;
	if (!isgreater(2.0, 1.0)) return 2;
	if (!isgreaterequal(2.0, 2.0)) return 3;
	return 0;
}
EOF
	for flags in "" '-DNAN=(__builtin_nanf(""))' '-DNAN=(__builtin_nanf("")) -D__builtin_isgreater(x,y)=((x)>(y)) -D__builtin_isgreaterequal(x,y)=((x)>=(y))'; do
		printf '  flags=%-72s ' "${flags:-<none>}"
		if out=$(tcc $flags -o all all.c -lm 2>&1); then
			./all >/dev/null 2>&1 && echo "PASSES" || echo "compiles, exits $?"
		else
			echo "FAILS: $(printf '%s' "$out" | head -1)"
		fi
	done

	echo
	echo "=== gcc, as the reference ==="
	/usr/bin/gcc -o allg all.c -lm 2>&1 && { ./allg >/dev/null 2>&1 && echo "  gcc: PASSES" || echo "  gcc: exits $?"; }

	echo
	echo "=== probe complete -- failing on purpose ==="
	exit 1
}

pkg_install() {
	:
}
