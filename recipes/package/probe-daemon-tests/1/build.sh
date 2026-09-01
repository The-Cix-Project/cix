#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #224: 67 of 85 test binaries run nowhere. They are the daemon-linked
# ones -- test_daemon, test_dns, test_pkg, test_networks, test_pki --
# and they cover the behaviour that actually breaks. The issue names
# three possible homes and says the first costs one probe to settle:
#
#   "Can a __pkgbuild container fork a process that binds a port and
#    writes a data dir? Probably yes for most. The ones that create
#    real containers, bridges or namespaces will not."
#
# "Probably" is the word this recipe exists to remove. It measures the
# capabilities a daemon-linked test actually needs, then builds and RUNS
# two real ones, because a capability being present is not the same
# claim as a test passing -- this session already shipped a document
# that compiled, passed every gate, and did not parse.
#
# Deliberately picks two tests at different points on the spectrum:
# test_dns forks a cixd and exercises records over HTTP with no
# containers at all, and test_daemon is the broader HTTP surface. If
# the cheap end fails there is no point costing the rest.
#
pkg_name="probe-daemon-tests"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.10.2.tar.gz"
pkg_sha256="c94c71217cce5e77ded2df9349a9e9c50034fff92a6bba5acf4e4d87dc460f67"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils findutils grep sed gawk"
pkg_changelog="1: can a build container run a daemon-linked test at all (#224)"

pkg_build() {
	echo "=== who are we, and with what ==="
	echo "  uid=$(id -u) euid=$(id -un 2>/dev/null || echo '?')"
	grep -E '^(CapEff|CapBnd)' /proc/self/status 2>/dev/null | sed 's/^/  /' || echo "  (no /proc/self/status)"

	echo
	echo "=== the three things a daemon-linked test needs ==="

	printf '  writable temp data dir   : '
	if d=$(mktemp -d /tmp/cix_probe_XXXXXX 2>/dev/null) && echo x > "$d/x" 2>/dev/null; then
		echo "yes ($d)"; rm -rf "$d"
	else
		printf 'no in /tmp; '
		if d=$(mktemp -d /run/cix_probe_XXXXXX 2>/dev/null) && echo x > "$d/x" 2>/dev/null; then
			echo "yes in /run ($d)"; rm -rf "$d"
		else
			echo "NO -- and these images have no /tmp, which is already documented"
		fi
	fi

	printf '  bind a TCP port          : '
	cat > /run/portprobe.c <<'PROBE_EOF'
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) { perror("socket"); return 1; }
	a.sin_family = AF_INET;
	a.sin_port = htons(28642);
	a.sin_addr.s_addr = htonl(0x7f000001);
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
	if (listen(s, 1) != 0) { perror("listen"); return 1; }
	close(s);
	return 0;
}
PROBE_EOF
	if tcc /run/portprobe.c -o /run/portprobe 2>/dev/null && /run/portprobe; then
		echo "yes (127.0.0.1:28642)"
	else
		echo "NO -- every daemon-linked test binds one"
	fi

	printf '  fork + waitpid a child   : '
	if (sleep 0 &) 2>/dev/null; then echo "yes"; else echo "NO"; fi

	echo
	echo "=== build two real daemon-linked tests ==="
	if ! make build/test_dns build/test_daemon 2>&1 | tail -5; then
		echo "  BUILD FAILED -- cannot answer the question from here"
		exit 1
	fi
	ls -la build/test_dns build/test_daemon 2>/dev/null | sed 's/^/  /'

	echo
	echo "=== RUN them (the actual question) ==="
	for t in test_dns test_daemon; do
		echo "--- $t ---"
		if ./build/$t >/run/$t.log 2>&1; then
			echo "  $t: PASS"
		else
			echo "  $t: FAIL (exit $?) -- last 25 lines:"
			tail -25 /run/$t.log | sed 's/^/    /'
		fi
	done

	echo
	echo "=== verdict recorded above; failing on purpose so this log is kept ==="
	exit 1
}

pkg_install() {
	echo "probe-daemon-tests is a diagnostic; nothing is installed" >&2
	exit 1
}
