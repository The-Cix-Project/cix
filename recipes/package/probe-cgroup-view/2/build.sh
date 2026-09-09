#
# probe-cgroup-view 2 -- revision 1 could not answer its own question,
# and both reasons are worth recording (#336, ADR-0262).
#
# Revision 1 declared pkg_build_depends="bash coreutils" and then called
# grep, which is not in coreutils and is installed in no build image
# unless declared. Four of its eight measurements came back
# "grep: command not found" -- the same trap probe-selftest-env/2 hit
# and CLAUDE.md already documents. grep is declared here.
#
# The second reason is a design error rather than a typo, and it is the
# more useful finding: revision 1 read /sys/fs/cgroup from INSIDE the
# build container, and every file came back absent. A build container
# has no cgroup tree mounted (probe-selftest-env/3 measured the same
# boundary: uid 0, no CLONE_NEWNS, no mount, no cgroup tree). The
# numbers it wanted are host-side.
#
# That does not damage ADR-0262's design -- it confirms it. The server
# there is host-side and resolves the REQUESTER's pid through
# /proc/<pid>/cgroup, precisely because the container cannot see its own
# cgroup. A probe reading its own /sys/fs/cgroup was measuring the one
# side the design does not use.
#
# So the host-side numbers come from the API, where the daemon already
# owns them, and are recorded here rather than re-measured:
#
#   GET /v1/system/pkg-build-config          cpu_max "100000 100000" (1.0 CPU),
#                                            memory_max 2147483648 (2 GiB)
#   GET /v1/system/control-plane-reservation host_cpus 2,
#                                            workload_cpu_max "180000 100000"
#
# What only a probe can say is what a recipe SEES, and that is what this
# one now measures. The decisive line is nproc: it reads
# sched_getaffinity, which a cpuset constrains and a cpu.max quota does
# not. Revision 1 measured nproc=2 against a host of 2 CPUs and a build
# quota of 1.0 CPU -- so no cpuset narrows a build container, and a
# cpuinfo derived from the QUOTA would report 1 and turn every
# make -j$(nproc) into -j1.
#
# That is ADR-0262's second design, chosen on this evidence: cpuinfo
# follows cpuset.cpus.effective, meminfo follows the memory limit, and
# no build-container opt-out is needed.
#
# Fails on purpose at the end -- the log is the product.
#
pkg_name="probe-cgroup-view"
pkg_version="2"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.55.25.tar.gz"
pkg_sha256="c6b847f00098e607e6d5d627e23cbbdf118e9a981bf3f6e95cf231d256313b12"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils grep"
pkg_changelog="2: revision 1 could not answer its own question. It called grep without declaring it, losing four of eight measurements, and it read /sys/fs/cgroup from inside a build container, where no cgroup tree is mounted -- the numbers it wanted are host-side, which is exactly why ADR-0262 puts the server on the host and resolves the requester pid. The host-side values are recorded from the API in this header; what a probe alone can say is what a recipe SEES, and nproc is the decisive one because it reads sched_getaffinity, which a cpuset constrains and a quota does not. 1: measure nproc, cpu.max, cpuset.cpus.effective and memory.max inside a real build container, so ADR-0262 can choose whether a virtualised cpuinfo may be on by default without turning every make -j into -j1."

show() {
	# $1 = label, $2 = path. Says "absent" rather than printing nothing,
	# because an empty line and a missing file read identically in a log
	# and mean completely different things.
	if [ -r "$2" ]; then
		echo "CGROUP-VIEW $1: $(cat "$2" 2>&1 | tr '\n' ' ')"
	else
		echo "CGROUP-VIEW $1: absent ($2)"
	fi
}

pkg_build() {
	echo "=== who am I ==="
	echo "CGROUP-VIEW cgroup: $(cat /proc/self/cgroup 2>&1 | tr '\n' ' ')"

	echo "=== what recipes see today ==="
	echo "CGROUP-VIEW nproc: $(nproc 2>&1)"
	echo "CGROUP-VIEW cpuinfo_processors: $(grep -c '^processor' /proc/cpuinfo 2>&1)"
	echo "CGROUP-VIEW memtotal: $(grep '^MemTotal' /proc/meminfo 2>&1)"
	echo "CGROUP-VIEW memavailable: $(grep '^MemAvailable' /proc/meminfo 2>&1)"

	# nproc reads sched_getaffinity, which a cpuset genuinely constrains
	# and a cpu.max quota does not -- so these two disagreeing is the
	# whole question this probe exists to answer.
	echo "=== the cgroup's own answer: NOT VISIBLE FROM HERE ==="
	# Revision 1 printed nine "absent" lines from this loop. A build
	# container has no cgroup tree mounted at all, so the loop could only
	# ever have printed that -- and reading it as "the limits are unset"
	# rather than "this is the wrong side to look" is the misreading this
	# comment exists to prevent. The real values are in the header above,
	# from GET /v1/system/pkg-build-config.
	echo "CGROUP-VIEW cgroup_tree_mounted: $( [ -d /sys/fs/cgroup ] && [ -e /sys/fs/cgroup/cgroup.controllers ] && echo yes || echo no )"

	echo "=== the verdict inputs ==="
	echo "CGROUP-VIEW host_cpus_online: $(cat /sys/devices/system/cpu/online 2>&1)"
	echo "CGROUP-VIEW fuse_present: $([ -e /dev/fuse ] && echo yes || echo no)"
	echo "CGROUP-VIEW fuse_in_filesystems: $(grep -c fuse /proc/filesystems 2>&1)"

	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
