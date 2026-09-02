#
# probe-selftest-env 5 -- does a build container have its OWN network
# namespace? (issue #224)
#
# Probe 4 reported host interfaces (cix-ctnet0, cix-test0, vt-a, vt-b)
# visible from inside a build container and I read that as a leak: tests
# "passing" by mutating the build host. Six tests were excluded from the
# build gate on that basis.
#
# That reading is probably wrong, and the correction matters more than
# the original claim. src/container.c says in as many words that every
# build sandbox has its own netns ("a container with its own netns but
# no networks (every build sandbox)"), CLONE_NEWNET is in the default
# clone flags, and the host reports none of those devices afterwards.
#
# /sys/class/net is not per-netns when /sys is bind-mounted from the
# host -- it shows the mounting namespace's devices. /proc/net/dev IS
# per-netns. So the two disagreeing is the whole answer, and it is one
# file read rather than a rebuild of the suite.
#
pkg_name="probe-selftest-env"
pkg_version="5"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="bash coreutils tcc sed grep"
pkg_changelog="5: settle whether a build container has its own netns, by comparing /proc/net/dev (per-netns) against /sys/class/net (not, when /sys is bind-mounted). Probe 4's leak claim rests on the wrong file (#224)"

pkg_build() {
	echo "=== /sys/class/net (NOT per-netns when /sys is bind-mounted from the host)"
	ls /sys/class/net 2>/dev/null | sed 's/^/  /'

	echo
	echo "=== /proc/net/dev (IS per-netns -- this is the authoritative view)"
	cat /proc/net/dev 2>/dev/null | tail -n +3 | cut -d: -f1 | sed 's/^/  /'

	echo
	echo "=== our own netns id, for the record"
	ls -l /proc/self/ns/net 2>/dev/null | sed 's/^/  /'

	echo
	echo "=== what /sys itself is"
	grep -E ' /sys | /proc ' /proc/self/mounts 2>/dev/null | sed 's/^/  /'

	echo
	echo "  If /proc/net/dev shows only lo while /sys/class/net lists host"
	echo "  devices, the container HAS its own netns and probe 4's leak"
	echo "  claim was an artifact of reading the wrong file."
	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
