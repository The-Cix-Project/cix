#
# v2.53.61 -- ADR-0246, plus the call-site fix v2.53.48 died on.
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
pkg_version="v2.53.61"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.61.tar.gz"
pkg_sha256="3430a507a621e9086a262f2df326b834b8d29e6765ed2418c630c593bd03863e"
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
	#
	# #296: prove the new console-input scenario actually catches the
	# bug it was written for.
	#
	# The selftest above ran test_console_exec, which now includes an
	# INPUT scenario: it writes a keystroke as a binary websocket frame
	# and requires a real exec'd process to echo it back. A pass proves
	# nothing on its own -- the whole reason #294 shipped is that this
	# file was green while the console ignored every keystroke.
	#
	# v2.53.60 tried to prove it by DELETING the memset, and the test
	# still passed, so that build correctly failed. The reason is worth
	# keeping: not zeroing a malloc() only reproduces #294 when the
	# memory happens to be non-zero, and fresh kernel pages are zeroed,
	# so pty_out_len came up 0 and the code worked. That is also why
	# #294 was environment-dependent rather than constant.
	#
	# So the struct is POISONED instead of merely left unzeroed, which
	# is what uninitialised memory actually looked like on the host that
	# hit this: every field the code below assigns is still assigned,
	# and every field it forgot -- pty_out_len, the buffer -- is
	# garbage. That is exactly #294.
	#
	echo "=== #296: poisoning the console session struct, the input test must now FAIL ==="
	sed -i 's@^\tmemset(sess, 0, sizeof(\*sess));$@\tmemset(sess, 0xff, sizeof(*sess)); /* #296 proof */@' daemon/src/main.c
	grep -q "#296 proof" daemon/src/main.c || {
		echo "could not inject the #294 condition -- the proof is not being run" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
	rc=0
	./build/test_console_exec >/tmp/c296.log 2>&1 || rc=$?
	tail -30 /tmp/c296.log | sed 's/^/  /'
	if [ "$rc" = "0" ]; then
		echo "=== the console-input test PASSED against a build carrying the #294 bug" >&2
		echo "=== it does not detect what it was written for; failing this build" >&2
		exit 1
	fi
	echo "=== the input test detected the injected bug (rc=$rc), so its pass above is real ==="
	sed -i 's@^\tmemset(sess, 0xff, sizeof(\*sess)); /\* #296 proof \*/$@\tmemset(sess, 0, sizeof(*sess));@' daemon/src/main.c
	grep -q "memset(sess, 0, sizeof(\*sess));" daemon/src/main.c || {
		echo "could not restore the #294 fix -- refusing to ship" >&2
		exit 1
	}
	make CIX_VERSION="$pkg_version" build/cixd
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
