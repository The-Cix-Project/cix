#
# SCRATCH DIAGNOSTIC ONLY -- never installed, exits nonzero so the log
# is kept.
#
# #224, second half. Probe 1 settled the capability question and found
# one blocker: a fresh netns has lo DOWN, so nothing could bind
# 127.0.0.1. Fixed in v2.10.3 (container_net_child_loopback_up), and
# probe 1 re-run against it now reports:
#
#   bind a TCP port          : yes (127.0.0.1:28642)
#
# Two failures remained, both trivial and both predicted: test_dns
# stages a real dnsmasq binary, which probe 1 did not declare, and
# test_daemon execs build/daemon_child, which a targeted `make
# build/test_daemon` does not build.
#
# This probe fixes both and widens the sample, because "can one test
# run" and "how many of the 67 pass" are different questions and only
# the second one decides whether the build gate is worth wiring. The
# six chosen span the range deliberately: HTTP surface only
# (test_daemon), a real external binary (test_dns), PKI and image
# state, package machinery, and one that creates a real BRIDGE
# (test_networks) -- which is the case #224 predicts will fail, and is
# worth confirming rather than assuming.
#
pkg_name="probe-daemon-tests"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.10.3.tar.gz"
pkg_sha256="42d3fd8b075a64d6ee87280b13279742a425b87eebd69c91ae07aa26b32ae1a7"
pkg_depends=""
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils findutils grep sed gawk dnsmasq"
pkg_changelog="2: with loopback up and the two trivial gaps closed, how many daemon-linked tests actually pass (#224)"

pkg_build() {
	printf '  loopback bind: '
	cat > /run/p.c <<'P_EOF'
#include <netinet/in.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>
int main(void)
{
	struct sockaddr_in a;
	int s = socket(AF_INET, SOCK_STREAM, 0);
	a.sin_family = AF_INET; a.sin_port = htons(28643);
	a.sin_addr.s_addr = htonl(0x7f000001);
	if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) != 0) { perror("bind"); return 1; }
	close(s); return 0;
}
P_EOF
	tcc /run/p.c -o /run/p 2>/dev/null && /run/p && echo "yes" || echo "NO"

	printf '  dnsmasq present: '
	command -v dnsmasq >/dev/null 2>&1 && echo "$(command -v dnsmasq)" || {
		for c in /usr/sbin/dnsmasq /usr/bin/dnsmasq; do
			[ -x "$c" ] && { echo "$c"; break; }
		done
	}

	TESTS="test_daemon test_dns test_pki test_images test_pkg test_networks"
	echo
	echo "=== build them, plus the helper children they exec ==="
	if ! make build/daemon_child $(for t in $TESTS; do printf 'build/%s ' "$t"; done) 2>&1 | tail -4; then
		echo "  BUILD FAILED"
		exit 1
	fi

	echo
	echo "=== run ==="
	pass=0; fail=0; failed=""
	for t in $TESTS; do
		if timeout 600 ./build/$t >/run/$t.log 2>&1; then
			echo "  $t: PASS"
			pass=$((pass+1))
		else
			rc=$?
			echo "  $t: FAIL (exit $rc)"
			tail -8 /run/$t.log | sed 's/^/      /'
			fail=$((fail+1)); failed="$failed $t"
		fi
	done

	echo
	echo "=== TALLY: $pass passed, $fail failed ==="
	[ -n "$failed" ] && echo "  failed:$failed"
	exit 1
}

pkg_install() {
	echo "probe-daemon-tests is a diagnostic; nothing is installed" >&2
	exit 1
}
