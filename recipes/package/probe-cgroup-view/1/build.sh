#
# probe-cgroup-view 1 -- what does a build container actually see, and
# what would a virtualised /proc tell it instead? (#336, ADR-0262)
#
# ADR-0262 proposes serving a per-container /proc/meminfo and
# /proc/cpuinfo synthesised from the requester's cgroup, mounted by
# DEFAULT. The default is the whole point -- a virtualised /proc that a
# recipe has to remember to ask for is useless, because nobody knows
# they need it until something has already sized itself wrong.
#
# But the default is also the risk, and the build container is where it
# bites. /cix-workload's budget is cpu.max "100000 100000" -- one CPU.
# A cpuinfo derived from that quota makes nproc report 1, and every
# recipe that runs `make -j$(nproc)` becomes -j1. The kernel build goes
# from tens of minutes to hours, and nothing in the build output would
# say why.
#
# So this measures, before the server is written, the four numbers that
# decide the design:
#
#   nproc                     what recipes actually call today
#   cpu.max                   the quota a naive cpuinfo would follow
#   cpuset.cpus.effective     a real parallelism bound, if one is set
#   memory.max / memory.current  what meminfo would report
#
# Two designs are on the table and this picks between them: either build
# containers opt out of cpuinfo, or cpuinfo follows cpuset.cpus.effective
# (which genuinely bounds parallelism) while only meminfo follows the
# quota. If cpuset.cpus.effective is the full host set while cpu.max is
# one CPU, the second design is safe and the first is unnecessary.
#
# Fails on purpose at the end, like every probe here -- the log is the
# product. The lines to read are the ones prefixed CGROUP-VIEW.
#
pkg_name="probe-cgroup-view"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.55.25.tar.gz"
pkg_sha256="c6b847f00098e607e6d5d627e23cbbdf118e9a981bf3f6e95cf231d256313b12"
pkg_build_image="cix-builder"
pkg_build_depends="bash coreutils"
pkg_changelog="1: measure nproc, cpu.max, cpuset.cpus.effective and memory.max inside a real build container, so ADR-0262 can choose whether a virtualised cpuinfo may be on by default without turning every make -j into -j1."

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
	echo "=== the cgroup's own answer ==="
	for d in /sys/fs/cgroup /sys/fs/cgroup/cix-workload; do
		echo "--- $d"
		show "cpu.max" "$d/cpu.max"
		show "cpuset.cpus.effective" "$d/cpuset.cpus.effective"
		show "memory.max" "$d/memory.max"
		show "memory.current" "$d/memory.current"
	done

	echo "=== the verdict inputs ==="
	echo "CGROUP-VIEW host_cpus_online: $(cat /sys/devices/system/cpu/online 2>&1)"
	echo "CGROUP-VIEW fuse_present: $([ -e /dev/fuse ] && echo yes || echo no)"
	echo "CGROUP-VIEW fuse_in_filesystems: $(grep -c fuse /proc/filesystems 2>&1)"

	echo "probe complete -- failing on purpose so nothing installs"
	exit 1
}
