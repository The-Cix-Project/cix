#
# v2.38.0 -- three environment assumptions fixed in the tests themselves:
# merged-usr-only library paths, overlay scratch dirs on an overlay, and a
# staged library looked for only where Debian keeps it.
#
#
# v2.36.0 -- test data directories move to the tmpfs at /run, so a nested
# daemon is not asking the kernel to stack overlayfs on overlayfs. Adds
# ncurses and squashfs-tools, which the boot tests reached once bzImage
# was staged. The overlay probe is gone: the kernel already answered it,
# saying the filesystem was not supported as an upperdir.
#
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
pkg_version="v2.53.21"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.21.tar.gz http://192.168.15.31:8080/kernel-6.18.40-20-x86_64.tar.gz http://192.168.15.31:8080/bash-5.2.37-2-x86_64.tar.gz http://192.168.15.31:8080/coreutils-9.11-3-x86_64.tar.gz http://192.168.15.31:8080/tcc-0.9.27-7-x86_64.tar.gz http://192.168.15.31:8080/glibc-2.44-12.tar.gz http://192.168.15.31:8080/linux-headers-6.18.40-4-x86_64.tar.gz"
pkg_sha256="843932c185a342ba98aa902a5c973f8ba99f55585596733173d38bfaa4cdb62f 1a929babcf2654cc45d9d47197f0a003b6660573cecf9ae0ce930edcf8b6ab4f 47f08a7eac65b59ce6e6a211ffcf77545993f4b5c52c094dbd79ec367f08bd14 f040e943e222849b9b8b405c16d1abeebf238544c9905328173c2f3ad4fb1009 2e9095069725a251dcecadddc89e179f08627728ff13d376510a854e0e6de26d 6ea13d0c3e998e3e123475cb4ffaadb71a6e028481bc688adea8a20a1365afdd 5cd03e3376c4640bd7a05b578855935ca6ae880a6c6cfe6e516e914ea880e315"
pkg_depends=""
#
# The cix recipe's own tools, plus what the tests themselves reach for
# at RUN time rather than build time: sfdisk/mkfs for the disk tests,
# iproute2 for the networking ones, and timeout from coreutils, without
# which one hung test would take the whole run with it.
#
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils util-linux e2fsprogs btrfs-progs iproute2 ncurses squashfs-tools findutils sed grep gawk"
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
	# The five floor packages the package tests seed from (ADR-0209).
	#
	# test_pkg and its four siblings install real, checksum-verified
	# artifacts rather than fabricated ones, and refuse to run without
	# them -- deliberately, because a package test seeded with invented
	# tarballs proves nothing about installing a package. Like bzImage
	# above these are INPUTS: make does not produce them, and a fresh
	# build container has no way to conjure them.
	#
	# Named unstamped on disk because that is what the fixture's own
	# table looks for; the cache serves some of these arch-stamped and
	# some not, so the copy normalises rather than the fixture guessing.
	#
	# Their versions are pinned by that table, not chosen here -- the
	# table's own comment says an artifact is re-verified rather than
	# the version edited alone, so this recipe follows it rather than
	# leading it.
	#
	mkdir -p build-inputs/floor-artifacts
	floor_ok=1
	for spec in "bash-5.2.37-2-x86_64.tar.gz bash-5.2.37-2.tar.gz" \
	            "coreutils-9.11-3-x86_64.tar.gz coreutils-9.11-3.tar.gz" \
	            "tcc-0.9.27-7-x86_64.tar.gz tcc-0.9.27-7.tar.gz" \
	            "glibc-2.44-12.tar.gz glibc-2.44-12.tar.gz" \
	            "linux-headers-6.18.40-4-x86_64.tar.gz linux-headers-6.18.40-4.tar.gz"; do
		set -- $spec
		if [ -f "/build/extra/$1" ]; then
			cp "/build/extra/$1" "build-inputs/floor-artifacts/$2"
		else
			echo "WARNING: floor artifact $1 was not staged -- the package tests will refuse to run"
			floor_ok=0
		fi
	done
	if [ "$floor_ok" = "1" ]; then
		echo "staged $(ls build-inputs/floor-artifacts | wc -l) floor artifacts ($(du -sm build-inputs/floor-artifacts | cut -f1) MB)"
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
