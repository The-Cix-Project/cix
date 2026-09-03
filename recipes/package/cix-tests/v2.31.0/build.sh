#
# cix-tests -- run the WHOLE test suite on a real Cix host (#224).
#
# `make selftest` runs 15 tests as part of every cix build. The other 70
# have never been executed anywhere: they fork a real cixd and need root
# and a writable data directory, which the dev sandbox is forbidden to
# provide (it builds cixctl and nothing else) and which nobody had
# arranged on a host.
#
# The measurement this exists to produce is per-test: which of them
# actually run in a build container on a real host, and which genuinely
# need something a container cannot give.
#
# WHY THIS IS SAFE TO RUN ON A LIVE HOST, which was the objection to
# running the suite host-side at all: a package build container is
# created with CLONE_NEWNET, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWUTS and
# CLONE_NEWCGROUP (daemon/src/pkg.c). Every bridge, container, mount and
# process a nested cixd creates in here belongs to those namespaces and
# goes away with them. The leaked-bridge failure mode CLAUDE.md
# documents comes from running tests directly on a host, not from
# running them inside a container that owns its own network namespace.
# Each daemon-linked test additionally spawns its cixd against a fresh
# mkdtemp data directory (ADR-0209), so they cannot collide with each
# other either.
#
# REPORTS, DOES NOT GATE. This revision deliberately exits 0 whatever
# the results: there is no baseline yet, and a gate without a baseline
# is just a red build. Once the passing set is known it becomes a real
# gate on that set -- a test that passes today and fails tomorrow is
# the signal worth having.
#
pkg_name="cix-tests"
pkg_version="v2.31.0"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.31.0.tar.gz"
pkg_sha256="e1a82bbe33b7aa85127f396c2bde330ecee0713e849c7508eb85673591c83518"
pkg_depends=""
#
# The cix recipe's own tools, plus what the tests themselves reach for
# at RUN time rather than build time: sfdisk/mkfs for the disk tests,
# iproute2 for the networking ones, and timeout from coreutils, without
# which one hung test would take the whole run with it.
#
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils util-linux e2fsprogs btrfs-progs iproute2 findutils sed grep gawk"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="toolchain"
pkg_changelog="v2.31.0: first run of the full suite anywhere (#224). Builds every test target and runs each one under a timeout inside a build container, which owns its own network, mount, pid, uts and cgroup namespaces -- so the bridges and containers the daemon-linked tests create cannot escape onto the host, which was the objection to running them host-side. Reports per-test results and exits 0 regardless: there is no baseline yet, and a gate without one is just a red build."

pkg_build() {
	#
	# Build everything. `all` is the only target that produces the
	# daemon-linked tests and their helper binaries -- several tests
	# exec a helper they do not link (daemon_child, net_child,
	# volume_child ...), and a test that needs a fixture the build did
	# not produce fails for a reason that has nothing to do with the
	# code under test.
	#
	make CIX_VERSION="$pkg_version" all

	pass=0; fail=0; timedout=0; total=0
	passed=""; failed=""; hung=""

	echo "=== running every test binary (300s each) ==="
	for t in build/test_*; do
		[ -x "$t" ] || continue
		name=$(basename "$t")
		total=$((total + 1))
		#
		# Each test gets its own log, and only a failure prints one.
		# A passing suite that dumps 70 logs is unreadable, and the
		# build log has a size limit that the noise would exhaust
		# before reaching the tests that actually failed.
		#
		if timeout 300 "./$t" > "/run/$name.log" 2>&1; then
			pass=$((pass + 1)); passed="$passed $name"
			printf '  %-34s PASS\n' "$name"
		else
			rc=$?
			if [ "$rc" -eq 124 ]; then
				timedout=$((timedout + 1)); hung="$hung $name"
				printf '  %-34s TIMEOUT\n' "$name"
			else
				fail=$((fail + 1)); failed="$failed $name"
				printf '  %-34s FAIL (exit %s)\n' "$name" "$rc"
			fi
		fi
	done

	echo
	echo "=== summary ==="
	echo "  total    : $total"
	echo "  passed   : $pass"
	echo "  failed   : $fail"
	echo "  timed out: $timedout"
	echo
	echo "=== failing ==="
	for n in $failed; do
		echo "--- $n"
		tail -12 "/run/$n.log" 2>/dev/null | sed 's/^/    /'
	done
	echo
	echo "=== timed out ==="
	for n in $hung; do echo "  $n"; done
	echo
	echo "=== passing ==="
	for n in $passed; do echo "  $n"; done

	#
	# Exit 0 on purpose -- see this recipe's own header. The product of
	# this build is the log, not an installed file.
	#
	echo "=== measurement complete ==="
}

pkg_install() {
	#
	# Nothing is installed. This package exists to run, not to ship:
	# putting 70 test binaries onto a host would add a large attack
	# surface to every machine to serve a job only a test host has.
	#
	mkdir -p "$PKG_DESTDIR"
}
