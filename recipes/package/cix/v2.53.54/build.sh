#
# v2.53.54 -- ADR-0246, plus the call-site fix v2.53.48 died on.
#
# registry_create() gained an adopt_if_running parameter and only one of
# its two real call sites was updated; the other creates the package
# build container, where &entry landed in the new int parameter. Caught
# by building here, which is the only place this code meets Cix's own
# toolchain.
#
#
# v2.53.48 -- ADR-0246: the control plane becomes a supervisor and a
# worker, and the worker learns to adopt what is already running.
#
# cixd is the init, the supervisor, the event loop, the REST API, the
# container runtime and the executor of unbounded blocking work. On
# 2026-09-05 one of those blocked reading a pty master and took the
# other five with it, the health endpoint included.
#
# Two measurements taken while amending the ADR changed its shape.
#
# pid 1 on the box IS cixd, so a dead pid 1 would have been a kernel
# panic; the box did not panic and jump served SSH throughout, which
# means the daemon was hung rather than dead. That resolves the question
# the ADR left open.
#
# The running-container registry is memory only -- registry.c does no
# file I/O at all, and only the definitions persist. A restarted worker
# therefore knows nothing about what is live, and would autostart
# duplicates on top of the survivors.
#
# So this adds re-adoption, without which the split would be worse than
# the wedge it replaces: containers kept serving through the incident,
# and a supervisor whose restart killed them would trade a control-plane
# outage for a workload outage. container_adopt() rebuilds a handle from
# kernel state instead of clone3(), anchored on the container cgroup,
# which outlives any daemon; the init is identified by NSpid, which
# survives the reparenting that makes any parent-is-the-daemon rule
# wrong exactly when it matters.
#
# Adoption forces one interface between the two processes. An adopted
# container belongs to the supervisor, so waitid() in the worker fails
# with ECHILD and the exit status would be lost. cix-init forwards every
# child it reaps over a socketpair and the worker matches pids against
# its registry. Adoption is only attempted when that channel exists: a
# worker may only adopt what something else will reap for it, so a
# standalone cixd behaves exactly as before.
#
# stallwatch gains authority to act -- a sustained service stall signals
# pid 1 rather than only writing the failure down behind the API that is
# down whenever it matters.
#
# This build verifies the new code compiles under Cix's own toolchain and
# passes the selftest gates. Nothing sets init=/bin/cix-init yet, so
# cix-init does not run on a host and cixd as pid 1 is unchanged; boot
# integration and the kill -STOP proof the ADR demands come next.
#
pkg_name="cix"
pkg_version="v2.53.54"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.54.tar.gz"
pkg_sha256="c65776baf45fe78d9d89a45a3decab4516f113eda3e5c3be91985c257923aaa9"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="toolchain"
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cixd build/cixctl build/cix-init build/mkbootroot build/cix-install \
	    build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso
	#
	# The contract guards: the generated REST surface and its route
	# count, the documentation indexes, the ADR-0224 gcc-exception
	# count, the ELF install gate, treecopy's device-node handling.
	# Failing here fails the build, which is the point.
	#
	make CIX_VERSION="$pkg_version" selftest
}

pkg_install() {
	cp build/cixd build/cixctl build/cix-init build/mkbootroot build/cix-install \
	   build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso \
	   "$PKG_DESTDIR/"
	# Asserted against the real bytes: this has to be a PE32+ image or
	# the firmware will not load it, and a wrong format would surface
	# only as a machine that does not boot after an install. MZ is the
	# DOS header every PE file begins with.
	magic=$(dd if="$PKG_DESTDIR/cix-boot.efi" bs=1 count=2 2>/dev/null)
	case "$magic" in
	MZ) ;;
	*)
		echo "cix-boot.efi is not a PE image (magic: $magic)" >&2
		exit 1
		;;
	esac
}
