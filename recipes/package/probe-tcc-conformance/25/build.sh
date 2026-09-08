#
# probe-tcc-conformance 25 -- can the pinned tcc produce a working
# statically linked executable? (ADR-0260)
#
# ADR-0260 makes cix-init pid 1 in every container and links it
# -static, the one named exception to this platform's "never -static"
# rule. The reasoning there is sound only if the compiler can actually
# do it: a static link needs the libc archives (libc.a, and whatever
# crt objects glibc's static start-up wants) to be present in the build
# image, and it needs tcc's own linker to resolve a static glibc, which
# carries IFUNC relocations (R_X86_64_IRELATIVE) for the string and
# memory routines. Both of those are things a dev sandbox cannot answer
# about this platform's compiler -- its tcc is Debian's, and this
# project has twice been burned treating that as an answer here.
#
# Static linking is load-bearing for ADR-0260 in two ways, so a gap has
# to be found as a finding before Stage A rather than as a surprise
# during it: it dissolves the bootstrap cycle (a build container is a
# container, so a cix-init PACKAGE would need a container that already
# had cix-init), and it removes glibc skew (an image's own glibc is not
# in the picture for a binary that resolves nothing at run time).
#
# Six questions:
#   1. is libc.a present in the build image at all, and where?
#   2. does tcc -static accept the flag and produce an executable?
#   3. does that executable RUN, and return the value it computes?
#      (a static glibc that links but crashes in an IFUNC resolver is
#      the failure to expect, and it presents as a plain SIGSEGV)
#   4. does it carry no PT_INTERP and no DT_NEEDED -- i.e. is it
#      actually static, rather than tcc silently ignoring the flag?
#   5. does the shape cix-init needs work under -static: sigaction,
#      waitpid, poll, socketpair/send with MSG_DONTWAIT, execve --
#      the calls in 594ec1fc^:init/src/cix_init.c
#   6. how big is it -- the number that decides whether staging one
#      copy per container at create time is an ordinary cost
#
# Fails on purpose at the end, like every probe in this series -- the
# build log is the product.
#
pkg_name="probe-tcc-conformance"
pkg_version="25"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="tcc make bash coreutils sed grep gawk binutils findutils"
pkg_changelog="25: can the pinned tcc link -static, does the result run, and is it genuinely static -- ADR-0260 makes cix-init depend on the answer."

hdr() { echo; echo "=== $1 ==="; }

# One link attempt, reporting acceptance and the reason for a refusal
# separately -- a rejected flag and a silently ignored one are different
# answers and only the exit status tells them apart.
try_link() {
	out="$1"; shift
	rm -f "$out"
	if tcc "$@" -o "$out" 2>/run/link.err; then
		echo "  link: accepted (exit 0)"
		return 0
	fi
	echo "  link: REJECTED (exit $?): $(head -3 /run/link.err | tr '\n' ' ')"
	return 1
}

# Run a built binary and report its exit status honestly, including a
# signal death -- bash reports those as 128+n and the distinction is the
# whole point of question 3.
try_run() {
	if [ ! -x "$1" ]; then
		echo "  run: NOT BUILT"
		return
	fi
	"$1"; rc=$?
	if [ "$rc" -ge 128 ]; then
		echo "  run: DIED with signal $((rc - 128)) (exit $rc)"
	else
		echo "  run: exit $rc"
	fi
}

# Is the file really static? No PT_INTERP, no DT_NEEDED.
static_shape() {
	if [ ! -f "$1" ]; then
		echo "  shape: NOT BUILT"
		return
	fi
	interp=$(readelf -lW "$1" 2>/dev/null | grep -c INTERP || true)
	needed=$(readelf -dW "$1" 2>/dev/null | grep -c NEEDED || true)
	irel=$(readelf -rW "$1" 2>/dev/null | grep -c IRELATIVE || true)
	echo "  shape: PT_INTERP=${interp:-0} DT_NEEDED=${needed:-0} IRELATIVE relocs=${irel:-0} size=$(stat -c %s "$1") bytes"
	if [ "${interp:-0}" = "0" ] && [ "${needed:-0}" = "0" ]; then
		echo "  shape: STATIC"
	else
		echo "  shape: NOT STATIC -- the flag did not do what it says"
	fi
}

pkg_build() {
	hdr "compiler under test"
	tcc -v 2>&1 | head -2 | sed 's/^/  /'
	command -v readelf >/dev/null 2>&1 && echo "  readelf: $(readelf --version 2>&1 | head -1)" || echo "  readelf: NOT PRESENT -- shape checks below cannot report"

	hdr "1. is libc.a in the build image?"
	found=$(find / -xdev -name 'libc.a' 2>/dev/null | head -5 || true)
	if [ -n "$found" ]; then
		echo "$found" | sed 's/^/  /'
		echo "  crt objects beside it:"
		for f in $found; do ls -1 "$(dirname "$f")" 2>/dev/null | grep -E '^(crt1|crti|crtn|libc_nonshared)\.' | sed 's/^/    /' || true; done
	else
		echo "  NONE -- no static libc anywhere; -static cannot succeed and the fix is in the glibc recipe, not the compiler"
	fi

	hdr "2. a trivial program, tcc -static"
	cat > /run/trivial.c <<'EOF'
int probe_symbol(int n) { return n + 41; }
int main(void) { return probe_symbol(1); }
EOF
	try_link /run/trivial.bin -static /run/trivial.c
	hdr "3. does it run? (expected exit 42)"
	try_run /run/trivial.bin
	hdr "4. is it actually static?"
	static_shape /run/trivial.bin

	hdr "5. the shape cix-init needs, tcc -static"
	cat > /run/initshape.c <<'EOF'
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_chld;
static void on_chld(int s) { (void)s; g_chld = 1; }

int main(void)
{
	struct sigaction sa;
	int sv[2];
	struct pollfd pfd;
	pid_t pid;
	int status;
	char buf[16];

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_chld;
	sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	if (sigaction(SIGCHLD, &sa, NULL) != 0) return 10;
	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) return 11;
	pid = fork();
	if (pid < 0) return 12;
	if (pid == 0) {
		close(sv[0]);
		if (send(sv[1], "child", 5, MSG_DONTWAIT | MSG_NOSIGNAL) != 5) _exit(20);
		execl("/bin/sh", "sh", "-c", "exit 7", (char *)0);
		_exit(21);
	}
	close(sv[1]);
	pfd.fd = sv[0]; pfd.events = POLLIN; pfd.revents = 0;
	if (poll(&pfd, 1, 5000) <= 0) return 13;
	if (recv(sv[0], buf, sizeof(buf), MSG_DONTWAIT) != 5) return 14;
	if (waitpid(pid, &status, 0) != pid) return 15;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 7) return 16;
	/* strerror pulls in the locale machinery -- the part of a static
	 * glibc most likely to be missing or to need dlopen. */
	if (strerror(ENOENT) == NULL) return 17;
	printf("sigaction/socketpair/fork/execve/poll/recv/waitpid all fine; g_chld=%d\n", (int)g_chld);
	return 42;
}
EOF
	try_link /run/initshape.bin -static /run/initshape.c
	echo "  (expected exit 42 and one line of output)"
	try_run /run/initshape.bin
	static_shape /run/initshape.bin

	hdr "6. for contrast: the same two programs linked dynamically"
	try_link /run/trivial.dyn /run/trivial.c;   try_run /run/trivial.dyn;   static_shape /run/trivial.dyn
	try_link /run/initshape.dyn /run/initshape.c; try_run /run/initshape.dyn; static_shape /run/initshape.dyn

	hdr "probe complete -- failing on purpose so nothing installs"
	exit 1
}
