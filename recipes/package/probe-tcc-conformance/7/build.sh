#
# SCRATCH DIAGNOSTIC ONLY -- never meant to succeed or be installed.
# Exits nonzero at the end so its output lands in cixd's build-failure
# log without producing an artifact (probe-gcc-headers' pattern).
#
# WHY THIS ONE EXISTS
#
# #216: TCC-built miniperl segfaults deterministically. Root-caused from
# the real binary (kept via ADR-0175's keep_on_failure, disassembled at
# the faulting address) to TCC giving two simultaneously-live locals the
# SAME stack slot.
#
# The site is perl's regexec.c, in S_regmatch:
#
#     8130    {
#     8131        SV      *ret;
#     8132        REGEXP  *re_sv;
#     ...
#     8138    case GOSUB:            <-- a case label INSIDE the block
#     ...
#     8356        if (...) ret = &PL_sv_undef;
#     8359        else   { ret = POPs; PUTBACK; }
#     8370        re_sv = NULL;      <-- emitted into ret's slot
#     8383        if (SvGMAGICAL(ret))   <-- reads NULL, faults
#
# Both were assigned -0x820(%rbp). The shape that appears to trigger it
# is a case label of the ENCLOSING switch sitting inside a block that
# declares locals -- control jumps into the middle of the block, past
# the declarations.
#
# This probe tests that shape directly, at runtime, on Cix's own tcc.
# It is deliberately not tested against the dev sandbox's Debian tcc:
# that is a different compiler and has produced wrong answers twice
# (see probe-tcc-conformance/4's own header).
#
# The source tarball is irrelevant -- a recipe needs one. Same tarball
# and checksum the other probes use.
#
pkg_name="probe-tcc-conformance"
pkg_version="7"
pkg_source="https://ftp.gnu.org/gnu/sed/sed-4.9.tar.gz"
pkg_sha256="d1478a18f033a73ac16822901f6533d30b6be561bcbce46ffd7abce93602282e"
pkg_depends=""
pkg_build_depends="tcc gcc binutils make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_changelog="7: declare gcc, so section D actually produces the reference answer instead of /usr/bin/gcc: No such file or directory -- a composed build environment holds exactly the declared tools (ADR-0199)"

pkg_build() {
	echo "=== which tcc, and what version ==="
	tcc -v 2>&1 | head -2

	cd /run || cd .
	mkdir -p probe216 && cd probe216

	#
	# A: the reduction. Two pointer locals in a block that also
	# contains a case label of the enclosing switch. `p` is assigned
	# on both arms of an if/else, then `q` is set to NULL, then `p`
	# is read. If they share a slot, p reads NULL.
	#
	# opaque()/sink() are external so nothing can be constant-folded,
	# and the NULL store cannot be reasoned away.
	#
	cat > a.c <<'EOF'
#include <stdio.h>

void *opaque(int);
int   pick(void);

static int check(int which)
{
	switch (which) {
	case 0:
		return -1;

	{
		void *p;
		void *q;

	case 1:
		if (pick())
			p = opaque(1);
		else
			p = opaque(2);

		q = NULL;

		if (p == NULL) {
			printf("A: FAIL p is NULL after q=NULL (shared slot)\n");
			return 1;
		}
		if (q != NULL) {
			printf("A: FAIL q is not NULL\n");
			return 1;
		}
		printf("A: ok p=%p q=%p\n", p, q);
		return 0;
	}
	}
	return -1;
}

void *opaque(int n) { static char buf[8]; return buf + (n & 1); }
int   pick(void)    { return 1; }

int main(void) { return check(1); }
EOF
	echo
	echo "=== A. two live locals, case label inside their block (#216) ==="
	if tcc -o a a.c 2>&1; then
		./a || true; echo "   exit=$?"
	else
		echo "   (did not compile)"
	fi

	#
	# B: the same two locals with NO case label inside the block --
	# the control question. If B passes and A fails, the case label
	# jumping into the block is what breaks slot assignment, which
	# tells whoever fixes tcc where to look.
	#
	sed 's/^	case 1:$//; s/^	case 0:$/	case 0:/' a.c > /dev/null 2>&1 || true
	cat > b.c <<'EOF'
#include <stdio.h>

void *opaque(int);
int   pick(void);

static int check(void)
{
	{
		void *p;
		void *q;

		if (pick())
			p = opaque(1);
		else
			p = opaque(2);

		q = NULL;

		if (p == NULL) {
			printf("B: FAIL p is NULL after q=NULL (shared slot)\n");
			return 1;
		}
		printf("B: ok p=%p q=%p\n", p, q);
		return 0;
	}
}

void *opaque(int n) { static char buf[8]; return buf + (n & 1); }
int   pick(void)    { return 1; }

int main(void) { return check(); }
EOF
	echo
	echo "=== B. CONTROL: same locals, no case label in the block ==="
	if tcc -o b b.c 2>&1; then
		./b || true; echo "   exit=$?"
	else
		echo "   (did not compile)"
	fi

	#
	# C: show the stack slots directly, which is how this was found in
	# miniperl. If p and q print the same address they share a slot,
	# and that is the bug regardless of what the checks above happen
	# to observe.
	#
	cat > c.c <<'EOF'
#include <stdio.h>

int pick(void);

static int check(int which)
{
	switch (which) {
	case 0:
		return -1;

	{
		void *p;
		void *q;

	case 1:
		if (pick()) p = (void *)1; else p = (void *)2;
		q = NULL;
		printf("C: &p=%p &q=%p  %s\n", (void *)&p, (void *)&q,
		       ((void *)&p == (void *)&q) ? "SAME SLOT -- BUG" : "distinct");
		return ((void *)&p == (void *)&q) ? 1 : 0;
	}
	}
	return -1;
}

int pick(void) { return 1; }

int main(void) { return check(1); }
EOF
	echo
	echo "=== C. do the two locals occupy the same address ==="
	if tcc -o c c.c 2>&1; then
		./c || true; echo "   exit=$?"
	else
		echo "   (did not compile)"
	fi

	#
	# D: same three, built with gcc, as the reference answer. perl
	# builds correctly under gcc, so gcc is expected to pass all of
	# them; if it does not, the reduction is wrong rather than tcc.
	#
	echo
	echo "=== D. REFERENCE: the same three under gcc ==="
	for f in a b c; do
		if /usr/bin/gcc -O0 -o "g$f" "$f.c" 2>&1; then
			"./g$f" || true; echo "   gcc $f exit=$?"
		else
			echo "   gcc $f did not compile"
		fi
	done

	echo
	echo "=== probe complete -- failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	:
}
