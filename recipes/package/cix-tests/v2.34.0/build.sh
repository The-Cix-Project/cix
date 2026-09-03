#
# v2.34.0 -- stages build-inputs/bzImage from the published kernel artifact
# so the six boot tests can run, and probes whether overlayfs can be
# stacked on overlayfs, which is the suspected cause of the container
# tests failing. Measured rather than assumed: the previous grouping of
# these failures mistook a startup warning for the cause.
#
#
# v2.33.0 -- a nested cixd can delegate cgroup controllers (#258). The
# cgroup v2 no-internal-process rule blocked it: the daemon sat in its own
# namespace root, so that root could never enable controllers for children.
# It now moves itself into a leaf and retries.
#
#
# v2.32.0 -- containers that run containers get a cgroup2 mount (#257).
# Without it a nested cixd had no cgroup tree to create under, which was
# 25 of the 48 failures the first full suite run found.
#
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
pkg_version="v2.34.0"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.33.0.tar.gz http://192.168.15.31:8080/kernel-6.18.40-20-x86_64.tar.gz"
pkg_sha256="ad4dd380be4c6b2d6f5aef4534e5ca122ecb191e0147de44012a7e0f359037e4 1a929babcf2654cc45d9d47197f0a003b6660573cecf9ae0ce930edcf8b6ab4f"
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

	#
	# The six boot tests need build-inputs/bzImage, which is an INPUT
	# rather than a build output -- `make` does not produce it (#191).
	# Taken from the kernel artifact a Cix host already built and
	# published, which is both faster than building one and the kernel
	# this platform actually ships. Fetched as an ordinary extra
	# pkg_source, so its bytes are checksum-verified like any other
	# source before they are used.
	#
	mkdir -p build-inputs
	if tar xzf /build/extra/kernel-6.18.40-20-x86_64.tar.gz -C build-inputs ./bzImage; then
		echo "staged build-inputs/bzImage ($(wc -c < build-inputs/bzImage) bytes)"
	else
		echo "WARNING: could not stage bzImage -- the boot tests will skip themselves"
	fi

	#
	# Why do the container-creating tests fail? MEASURE it rather than
	# assume: the last classification of these failures was wrong, and
	# read a startup warning as the cause when the real error was an
	# overlay mount.
	#
	# The hypothesis is that overlayfs cannot be stacked on overlayfs.
	# This build container's own rootfs IS an overlay mount, so a test
	# whose data directory sits on it asks the kernel for exactly that
	# and gets EINVAL. If so, putting the data directory on the tmpfs
	# at /run -- which every container gets fresh (mountns.c) -- avoids
	# the stack entirely.
	#
	# Two mounts, same shape, different backing filesystem. The answer
	# decides whether a one-line change to test_data_dir_create() is
	# worth making.
	#
	echo "=== overlay probe (#224) ==="
	for base in /build/ovl-probe /run/ovl-probe; do
		rm -rf "$base"; mkdir -p "$base/lower" "$base/upper" "$base/work" "$base/merged"
		if mount -t overlay overlay 		    -o "lowerdir=$base/lower,upperdir=$base/upper,workdir=$base/work" 		    "$base/merged" 2>/tmp/ovl.err; then
			echo "  $base: OVERLAY OK"
			umount "$base/merged" 2>/dev/null || true
		else
			echo "  $base: OVERLAY FAILED -- $(cat /tmp/ovl.err 2>/dev/null | head -1)"
		fi
		rm -rf "$base"
	done
	echo "=== end overlay probe ==="

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
