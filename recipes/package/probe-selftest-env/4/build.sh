#
# probe-selftest-env 1 -- where can the 67 daemon-linked tests actually
# run? (issue #224)
#
# #224 classifies the suite (14 run in the build gate, 67 fork a real
# cixd, 4 need privileged namespaces) and says the open question is
# WHERE, to be answered "with a measurement rather than a guess", and
# that option 1 -- inside a build container -- "costs one probe to
# settle". This is that probe.
#
# It runs three daemon-linked tests chosen to bracket the privilege
# boundary rather than to pass:
#
#   test_web        forks cixd, binds a port, serves files over HTTP.
#                   Needs a writable data dir and a socket, nothing more.
#   test_daemon     the same, and then creates REAL containers --
#                   clone3, mount, pivot_root.
#   test_networks   the same, and then creates REAL bridges via
#                   rtnetlink.
#
# Whichever of those three stops working is the answer, and it is a more
# useful answer than a yes/no: it says how much of the suite a build
# container can carry, which decides whether option 1 is a partial
# solution worth taking now or a dead end.
#
# Fails on purpose -- the build log is the product.
#
pkg_name="probe-selftest-env"
pkg_version="4"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.15.4.tar.gz"
pkg_sha256="f611e49cadadfa346a992f56cd4bd75c15e8be493956be353be1348724e9efbd"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk"
pkg_changelog="4: probe 3 established the boundary -- a build container is uid 0 with no CLONE_NEWNET, no CLONE_NEWNS, no mount and no cgroup tree, and test_web passes there while test_daemon and test_networks do not. This one builds the WHOLE suite and runs every binary, so the answer is an exact list of which tests can join the build gate rather than an estimate (#224). 3: probe 2 never ran -- it declared procps and iproute2, which are installed in no image, so composing its build environment failed before the build started (\"declared build tool procps is not installed anywhere\"). The capability checks now use a small C program compiled with the tcc already being declared, which tests the syscalls the tests actually make rather than shelling out to tools that may not exist. 2: probe 1 answered the first half -- test_web PASSES in a build container, so forking a real cixd, binding a port and writing a data dir all work there. Its own diagnostics used sed/awk/ip, which the sandbox does not have unless declared, so test_daemon's failure was masked. Declares them and reports the real reason (#224)"

pkg_build() {
	echo "=== who am I, and what can I do ==="
	echo "  uid=$(id -u) gid=$(id -g)"
	echo "  CapEff: $(grep -i '^CapEff' /proc/self/status 2>/dev/null | cut -f2)"
	echo "  writable /tmp: $( (mkdir -p /tmp/probe.$$ && echo yes && rmdir /tmp/probe.$$) 2>/dev/null || echo NO )"
	echo "  /dev/net/tun: $( [ -e /dev/net/tun ] && echo present || echo absent )"
	echo "  --- syscall capability, tested directly rather than via shell tools ---"
	cat > cap_probe.c <<'CAP_EOF'
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * The three things the daemon-linked tests actually need, asked of the
 * kernel rather than of a shell tool that may not be installed (#224).
 */
int main(void)
{
	printf("    unshare(CLONE_NEWNET): %s\n",
	       unshare(CLONE_NEWNET) == 0 ? "yes" : "NO -- network tests cannot run here");
	printf("    unshare(CLONE_NEWNS):  %s\n",
	       unshare(CLONE_NEWNS) == 0 ? "yes" : "NO -- mount-namespace tests cannot run here");
	mkdir("/tmp/capmp", 0700);
	printf("    mount(tmpfs):          %s\n",
	       mount("none", "/tmp/capmp", "tmpfs", 0, NULL) == 0 ? "yes"
	                                                          : "NO -- container tests cannot run here");
	return 0;
}
CAP_EOF
	tcc cap_probe.c -o cap_probe && ./cap_probe
	echo
	echo "=== building the whole suite ==="
	make -j"$(nproc)" all 2>&1 | tail -3

	echo
	echo "=== running every test binary; PASS here means it can join the build gate ==="
	pass=0; fail=0; skip=0
	for t in build/test_*; do
		[ -x "$t" ] || continue
		name=$(basename "$t")
		printf '  %-34s ' "$name"
		if timeout 180 "./$t" > "/tmp/$name.log" 2>&1; then
			echo "PASS"
			pass=$((pass+1))
		else
			rc=$?
			if [ "$rc" = "124" ]; then
				echo "TIMEOUT"
			else
				echo "fail rc=$rc  |  $(tail -1 "/tmp/$name.log" | cut -c1-70)"
			fi
			fail=$((fail+1))
		fi
	done
	echo
	echo "  ---- $pass PASS, $fail fail/timeout, $skip skipped ----"
	echo
	echo "  The PASS list is the answer to #224 option 1: those tests need"
	echo "  nothing a build container lacks, so they can run on every build"
	echo "  at no infrastructure cost. The rest need option 2 or 3."
	echo
	echo "=== leftovers a killed run would strand (CLAUDE.md records both) ==="
	echo "  net devices visible: $(ls /sys/class/net 2>/dev/null | tr '\n' ' ')"

	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
