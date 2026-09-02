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
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.15.4.tar.gz"
pkg_sha256="f611e49cadadfa346a992f56cd4bd75c15e8be493956be353be1348724e9efbd"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk iproute2 procps"
pkg_changelog="2: probe 1 answered the first half -- test_web PASSES in a build container, so forking a real cixd, binding a port and writing a data dir all work there. Its own diagnostics used sed/awk/ip, which the sandbox does not have unless declared, so test_daemon's failure was masked. Declares them and reports the real reason (#224)"

pkg_build() {
	echo "=== who am I, and what can I do ==="
	echo "  uid=$(id -u) gid=$(id -g)"
	echo "  /proc/self/status CapEff: $(grep -i '^CapEff' /proc/self/status 2>/dev/null | awk '{print $2}')"
	echo "  writable /tmp: $( (mkdir -p /tmp/probe.$$ && echo yes && rmdir /tmp/probe.$$) 2>/dev/null || echo NO )"
	echo "  /dev/net/tun: $( [ -e /dev/net/tun ] && echo present || echo absent )"
	echo "  unshare(NEWNET) via a real syscall is what test_networks needs"
	echo "  can we unshare a net namespace here?"
	if unshare --net true 2>/dev/null; then echo "    yes"; else echo "    NO -- test_networks and friends cannot work here"; fi
	echo "  can we mount a tmpfs (what pivot_root-based tests need)?"
	if mkdir -p /tmp/mp.$$ && mount -t tmpfs none /tmp/mp.$$ 2>/dev/null; then
		echo "    yes"; umount /tmp/mp.$$ 2>/dev/null; rmdir /tmp/mp.$$ 2>/dev/null
	else
		echo "    NO -- container-creating tests cannot work here"; rmdir /tmp/mp.$$ 2>/dev/null
	fi

	echo
	echo "=== building the daemon and three bracketing tests ==="
	make build/cixd build/test_web build/test_daemon build/daemon_child build/test_networks \
		2>&1 | tail -4

	echo
	echo "=== running them, least privileged first ==="
	for t in test_web test_daemon test_networks; do
		printf '  %-16s ' "$t"
		if [ ! -x "build/$t" ]; then
			echo "NOT BUILT"
			continue
		fi
		if timeout 300 "./build/$t" > "/tmp/$t.log" 2>&1; then
			echo "PASS"
		else
			rc=$?
			echo "FAIL rc=$rc"
			echo "      --- last 6 lines:"
			tail -6 "/tmp/$t.log" | sed 's/^/      /'
		fi
	done

	echo
	echo "=== leftovers a killed run would strand (CLAUDE.md records both) ==="
	echo "  bridges: $(ip -o link show type bridge 2>/dev/null | wc -l)"
	echo "  stray cixd: $(pgrep -c cixd 2>/dev/null || echo 0)"

	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
