#
# probe-tcc-conformance 27 -- can the pinned tcc build a FREESTANDING
# executable that runs, and what shape is it? (ADR-0260)
#
# 26 answered every live question -- _start, syscalls, a delivered
# signal through SA_RESTORER, fork/execve/poll/wait4, implicit memcpy --
# and then its own harness died: try_run executed the binary as a bare
# statement, so exit 42 ended the script under set -e before the ELF
# shape was read. The same class of bug 23 had. Guarded here; nothing
# else changes.
#
# 25 asked whether tcc can link -static against glibc and got its answer
# before the compiler was touched: there is no libc.a on this platform,
# by design (ADR-0251's finalize phase drops static archives from every
# artifact), so that route is closed. What ADR-0260 actually needs from
# cix-init is a binary that resolves nothing at run time. A freestanding
# one -- no libc, no headers, raw syscalls, its own _start -- gives
# exactly that without an archive glibc will never ship.
#
# Whether the pinned compiler can do it is not a question the dev
# sandbox can answer (its tcc is Debian's), and the failure modes are
# specific, so this probe tests them in the order they would bite:
#
#   1. a file-scope __asm__ _start that hands argc/argv to C and calls
#      exit_group -- NOT a C function with asm inside, whose prologue
#      would break the stack layout
#   2. a syscall trampoline written in asm, so nothing depends on tcc
#      honouring register constraints it may not implement
#   3. rt_sigaction with SA_RESTORER and a restorer that does
#      rt_sigreturn, DELIVERED: raise SIGUSR1 to self and check the
#      handler ran -- a missing restorer crashes on return from the
#      handler, not at install
#   4. pipe2 / fork / dup2 / execve / poll / wait4 -- the calls a
#      supervisor lives on
#   5. a struct assignment and a memset, to force tcc's implicit memcpy/
#      memset calls and prove our own definitions resolve them
#   6. readelf: no PT_INTERP, no DT_NEEDED, no UND symbols, and the size
#
# No varargs anywhere: with -nostdlib tcc also skips libtcc1.a, and
# whether x86_64 va_arg needs a helper from it is a question cix-init
# can avoid instead of answering.
#
# Fails on purpose at the end, like every probe in this series -- the
# build log is the product.
#
pkg_name="probe-tcc-conformance"
pkg_version="27"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="27: 26 with its try_run guarded against set -e so the ELF shape section runs. 26: freestanding executable on the pinned tcc -- asm _start, syscall trampoline, delivered signal with SA_RESTORER, fork/execve/poll/wait4, implicit memcpy -- the shape ADR-0260's cix-init needs, since 25 showed static glibc is not on the table."

hdr() { echo; echo "=== $1 ==="; }

# Every command whose non-zero exit is a legitimate answer is guarded:
# 23 died on exactly this under set -e.
try_link() {
	out="$1"; shift
	rm -f "$out"
	if tcc "$@" -o "$out" 2>/run/link.err; then
		echo "  link: accepted (exit 0)"
	else
		rc=$?
		echo "  link: REJECTED (exit $rc): $(head -3 /run/link.err | tr '\n' ' ')"
	fi
}

try_run() {
	if [ ! -x "$1" ]; then
		echo "  run: NOT BUILT"
		return 0
	fi
	if "$1" a b; then rc=0; else rc=$?; fi
	if [ "$rc" -ge 128 ]; then
		echo "  run: DIED with signal $((rc - 128)) (exit $rc)"
	else
		echo "  run: exit $rc (expected 42)"
	fi
	return 0
}

shape() {
	if [ ! -f "$1" ]; then
		echo "  shape: NOT BUILT"
		return 0
	fi
	interp=$(readelf -lW "$1" 2>/dev/null | grep -c INTERP || true)
	needed=$(readelf -dW "$1" 2>/dev/null | grep -c NEEDED || true)
	und=$(readelf -sW "$1" 2>/dev/null | grep -c ' UND ' || true)
	echo "  shape: PT_INTERP=${interp:-0} DT_NEEDED=${needed:-0} UND symbols=${und:-0} size=$(stat -c %s "$1") bytes"
	echo "  program headers:"; readelf -lW "$1" 2>/dev/null | grep -E '^\s+(LOAD|INTERP|DYNAMIC|GNU_|TLS|PHDR)' | sed 's/^/    /' || true
	if [ "${und:-0}" != "0" ]; then echo "  UND list:"; readelf -sW "$1" 2>/dev/null | grep ' UND ' | sed 's/^/    /' || true; fi
	return 0
}

pkg_build() {
	hdr "compiler under test"
	tcc -v 2>&1 | head -2 | sed 's/^/  /'
	command -v readelf >/dev/null 2>&1 && echo "  readelf: $(readelf --version 2>&1 | head -1)" || echo "  readelf: NOT PRESENT"

	cat > /run/free.c <<'EOF'
/* Freestanding: no #include anywhere in this file, on purpose. */

typedef unsigned long size_t;
typedef long ssize_t;

/* 1. _start: the kernel enters here with argc at (%rsp). */
__asm__(
	".text\n"
	".globl _start\n"
	"_start:\n"
	"	xor %rbp, %rbp\n"
	"	mov (%rsp), %rdi\n"
	"	lea 8(%rsp), %rsi\n"
	"	and $-16, %rsp\n"
	"	call cix_main\n"
	"	mov %eax, %edi\n"
	"	mov $231, %eax\n"
	"	syscall\n"
	"	hlt\n"
);

/* 2. syscall trampoline: (n, a, b, c, d, e, f) per the SysV C ABI in,
 * the kernel's rdi/rsi/rdx/r10/r8/r9 out. */
__asm__(
	".text\n"
	".globl cix_syscall6\n"
	"cix_syscall6:\n"
	"	mov %rdi, %rax\n"
	"	mov %rsi, %rdi\n"
	"	mov %rdx, %rsi\n"
	"	mov %rcx, %rdx\n"
	"	mov %r8, %r10\n"
	"	mov %r9, %r8\n"
	"	mov 8(%rsp), %r9\n"
	"	syscall\n"
	"	ret\n"
);
long cix_syscall6(long n, long a, long b, long c, long d, long e, long f);

/* 3. the signal restorer the kernel returns through (SA_RESTORER). */
__asm__(
	".text\n"
	".globl cix_sigreturn\n"
	"cix_sigreturn:\n"
	"	mov $15, %eax\n"
	"	syscall\n"
);
void cix_sigreturn(void);

#define SYS_read 0
#define SYS_write 1
#define SYS_close 3
#define SYS_poll 7
#define SYS_rt_sigaction 13
#define SYS_dup2 33
#define SYS_getpid 39
#define SYS_fork 57
#define SYS_execve 59
#define SYS_wait4 61
#define SYS_kill 62
#define SYS_prctl 157
#define SYS_pipe2 293

static long sc0(long n) { return cix_syscall6(n, 0, 0, 0, 0, 0, 0); }
static long sc1(long n, long a) { return cix_syscall6(n, a, 0, 0, 0, 0, 0); }
static long sc2(long n, long a, long b) { return cix_syscall6(n, a, b, 0, 0, 0, 0); }
static long sc3(long n, long a, long b, long c) { return cix_syscall6(n, a, b, c, 0, 0, 0); }
static long sc4(long n, long a, long b, long c, long d) { return cix_syscall6(n, a, b, c, d, 0, 0); }

/* 5. the implicit calls tcc emits for struct copies and clears. */
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *dp = d; const unsigned char *sp = s;
	while (n--) *dp++ = *sp++;
	return d;
}
void *memset(void *d, int c, size_t n)
{
	unsigned char *dp = d;
	while (n--) *dp++ = (unsigned char)c;
	return d;
}

static size_t cix_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static void out(const char *s) { sc3(SYS_write, 1, (long)s, (long)cix_strlen(s)); }
static void out_num(long v)
{
	char buf[24]; int i = 23; int neg = v < 0;
	buf[i] = '\0';
	if (v == 0) buf[--i] = '0';
	if (neg) v = -v;
	while (v > 0) { buf[--i] = (char)('0' + v % 10); v /= 10; }
	if (neg) buf[--i] = '-';
	out(buf + i);
}

struct k_sigaction {
	void (*handler)(int);
	unsigned long flags;
	void (*restorer)(void);
	unsigned long mask;
};
#define SA_NOCLDSTOP 0x00000001UL
#define SA_RESTORER  0x04000000UL
#define SA_RESTART   0x10000000UL
#define SIGUSR1 10
#define SIGCHLD 17

static volatile int g_usr1, g_chld;
static void on_usr1(int s) { (void)s; g_usr1 = 1; }
static void on_chld(int s) { (void)s; g_chld = 1; }

static int install(int sig, void (*h)(int))
{
	struct k_sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.handler = h;
	sa.flags = SA_RESTORER | SA_RESTART | SA_NOCLDSTOP;
	sa.restorer = cix_sigreturn;
	return (int)sc4(SYS_rt_sigaction, sig, (long)&sa, 0, 8);
}

struct pollfd { int fd; short events; short revents; };
#define POLLIN 1

struct big { long a[8]; };

int cix_main(long argc, char **argv)
{
	int fds[2];
	long pid, rc, st = 0;
	char buf[64];
	struct big x, y;
	struct pollfd p;
	long n;

	out("1. _start ok: argc="); out_num(argc); out(" argv[1]="); out(argc > 1 ? argv[1] : "(none)"); out("\n");
	out("2. syscall ok: getpid="); out_num(sc0(SYS_getpid)); out("\n");

	if (install(SIGUSR1, on_usr1) != 0) { out("3. rt_sigaction FAILED\n"); return 3; }
	if (install(SIGCHLD, on_chld) != 0) { out("3. rt_sigaction(SIGCHLD) FAILED\n"); return 3; }
	sc2(SYS_kill, sc0(SYS_getpid), SIGUSR1);
	out("3. SIGUSR1 delivered through SA_RESTORER: handler ran="); out_num(g_usr1); out("\n");
	if (!g_usr1) return 3;

	/* PR_SET_CHILD_SUBREAPER = 36: a supervisor wants orphans. */
	rc = sc2(SYS_prctl, 36, 1);
	out("4a. prctl(PR_SET_CHILD_SUBREAPER) -> "); out_num(rc); out("\n");

	if (sc2(SYS_pipe2, (long)fds, 0) != 0) { out("4. pipe2 FAILED\n"); return 4; }
	pid = sc0(SYS_fork);
	if (pid < 0) { out("4. fork FAILED\n"); return 4; }
	if (pid == 0) {
		static char *const cargv[] = { "sh", "-c", "echo child-says-hi; exit 7", 0 };
		static char *const cenvp[] = { "PATH=/usr/bin:/bin", 0 };
		sc1(SYS_close, fds[0]);
		sc2(SYS_dup2, fds[1], 1);
		sc3(SYS_execve, (long)"/bin/sh", (long)cargv, (long)cenvp);
		sc3(SYS_write, 2, (long)"execve failed\n", 14);
		cix_syscall6(231, 21, 0, 0, 0, 0, 0);
	}
	sc1(SYS_close, fds[1]);
	p.fd = fds[0]; p.events = POLLIN; p.revents = 0;
	rc = sc3(SYS_poll, (long)&p, 1, 5000);
	out("4b. poll -> "); out_num(rc); out("\n");
	n = sc3(SYS_read, fds[0], (long)buf, sizeof(buf) - 1);
	if (n > 0) { buf[n] = '\0'; out("4c. child wrote: "); out(buf); }
	rc = sc4(SYS_wait4, pid, (long)&st, 0, 0);
	out("4d. wait4 -> pid "); out_num(rc); out(" status=0x");
	{ char h[9]; int i; for (i = 7; i >= 0; i--) { h[i] = "0123456789abcdef"[st & 15]; st >>= 4; } h[8] = 0; out(h); }
	out(" (0x700 expected) SIGCHLD seen="); out_num(g_chld); out("\n");

	memset(&x, 0x5a, sizeof(x));
	y = x; /* forces the implicit memcpy */
	out("5. struct copy via implicit memcpy: "); out_num(y.a[7] == 0x5a5a5a5a5a5a5a5aL ? 1 : 0); out("\n");
	if (y.a[7] != 0x5a5a5a5a5a5a5a5aL) return 5;

	out("6. all good, exiting 42\n");
	return 42;
}
EOF

	hdr "A. tcc -nostdlib -static"
	try_link /run/free.static -nostdlib -static -Wall -Werror /run/free.c
	try_run /run/free.static
	shape /run/free.static

	hdr "B. tcc -nostdlib (no -static)"
	try_link /run/free.dyn -nostdlib -Wall -Werror /run/free.c
	try_run /run/free.dyn
	shape /run/free.dyn

	hdr "C. does tcc pull libtcc1.a in for anything here? (nm of the object)"
	rm -f /run/free.o; tcc -nostdlib -c -o /run/free.o /run/free.c 2>/dev/null || true
	[ -f /run/free.o ] && { readelf -sW /run/free.o | grep ' UND ' | sed 's/^/  /' || echo "  no UND in the object at all"; }

	hdr "probe complete -- failing on purpose so nothing installs"
	exit 1
}
