#
# v2.53.88: the scheduler's wall-clock jobs actually fire.
#
# v2.53.86/87 shipped a scheduler in which no daily or weekly job ever
# ran: schedule_next_run() returns the NEXT occurrence, always strictly
# in the future, and run_due() skipped anything whose "due" was after
# now -- which was every wall-clock job, always. The due test now
# compares the most recent occurrence against what the job has already
# accounted for.
#
# Two supporting fixes: an anchor, so a job created at 20:00 with
# "daily at 02:00" does not fire for this morning; and a skip that
# sticks and is recorded, rather than a one-tick deferral that ran the
# job anyway on the next tick.
#
# v2.53.87: cixctl argv fix for the two commands added today.
#
# dispatch_command() passes the command name separately, so argv[0] is
# the first argument AFTER it. cmd_schedule and cmd_pipeline both read
# one element too far: `schedule actions` fell through to `ls`,
# `schedule set NAME` hit the usage text, and `pipeline --all` ignored
# its only flag. Found against the live daemon.
#
# v2.53.86: ADR-0257 -- one scheduler, and a schedule is structured.
#
# Six periodic timers, four of them because an operator set an interval,
# become one scheduler with schedules as a first-class resource. The
# wire format is structured JSON rather than a cron-like string: the
# decisive property of a schedule syntax is its failure mode, and a
# typo'd cron expression still parses and means something else.
#
# Six new routes, so the count moves 283 -> 289.
#
# ADDITIVE. One action is registered (pkg.refresh-upstreams), and it has
# no existing timer, so nothing double-fires while the legacy timers are
# still alive. The four operator-set intervals migrate later, together,
# with their own contract change.
#
# v2.53.85: the pipeline flow is a grid, not a wrapping flex row.
#
# Verified by screenshotting the deployed v2.53.84 dashboard against the
# live daemon: "AUTHENTICATE" overflowed its own cell. A flex row sizes
# to the shortest name and clips the longest, which here is the one
# stage name a reader is least likely to guess. A grid gives every cell
# the same track width, so the longest name sets it for all of them.
#
# v2.53.84: v2.53.83 plus one line -- GET /pipeline declares
# x-cix-expose [cli, web], not [cli].
#
# The dashboard's Pipeline view calls getPipeline, and test_api_surfaces
# refused the build for exactly that: an endpoint a surface uses but the
# contract does not offer it. Caught by the release selftest on the box
# rather than locally, because the surface test was run before the web
# view was written and not again after.
#
# v2.53.83: ADR-0256 -- the pipeline is the model.
#
# Eleven stages and five statuses replace four unrelated vocabularies:
# the source catalogue's four states, enum pkg_failure_kind's five, a
# build-log line and a boot-entry filename. One (stage, status) pair is
# now spoken verbatim by the API, the CLI and the dashboard.
#
# GET /v1/pipeline is a read-time join over the catalogue, the package
# job records and this host's own boot entry. Route count 282 -> 283.
#
# CONTRACT CHANGE: GET /pkg/{name} and the package list no longer carry
# failure_kind. They carry stage and status. A field that changes
# meaning is worse than a field that is gone.
#
# BEHAVIOUR CHANGE, dormant on an already-installed box: confirming a
# boot now requires the configured uplink to be attached, not merely a
# bound socket. The code path is bootstrap_management_network()'s own
# fresh-install branch, so an existing host with a management network
# already flagged never reaches it and behaves exactly as before.
#
# v2.53.81 -- identical code to v2.53.80. This revision exists only to
# re-run a control-plane assembly, because cix-hosttools 2.2.0 added the
# glibc that image never had and assembly has no trigger of its own
# (#308): it runs as a side effect of a cix hostbuild completing, and a
# completed hostbuild entry cannot be cleared (#310). Two issues filed
# earlier today, both charging their cost here.
#
# v2.53.80 -- test_daemon_net stops hand-rolling cleanup and calls
# test_cleanup_containers_and_network(), which already retries a 409
# network delete. ADR-0180 makes a running container DELETE return 204
# before the child is reaped, so the network really is still in use --
# three hostbuilds were lost to that (#309).
#
# v2.53.79 -- v2.53.78 plus one declaration: probe-gcc-postglibc now
# states pkg_toolchain=gcc, which test_toolchain_policy required and
# which failed that build. ADR-0253 content is otherwise unchanged.
#
# A build output tree is created fresh (ADR-0253), which is
# what finally makes an installer ISO the size of what it carries.
#
# Three output trees were inherited across builds and each caused an
# incident: <artifacts>/cix (a web/ no recipe staged, supplied by an
# older build), the ISO staging tree (#307 -- the seed shipped twice,
# 64.6 MiB wanted plus 69.9 MiB duplicated of 217.9 MiB, and the STALE
# outer copy is the one cix-install installed), and the bootroot image
# root (a control-plane root carrying glibc objects from two builds,
# which panics pid 1).
#
# persist_fresh_output_dir() is the one operation all three now use.
#
# This revision is what puts the fixed mkbootroot and mkinstalleriso
# into the artifact the daemon actually execve()s, so the next assembly
# and the next ISO are built into empty trees.
#
pkg_name="cix"
pkg_version="v2.53.88"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.88.tar.gz"
pkg_sha256="f027fe8b55708e75b3e871079d2d3bf32a47243c6b90433996c6c3cfabc93a8d"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cixd build/cixctl build/mkbootroot build/cix-install \
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
	cp build/cixd build/cixctl build/mkbootroot build/cix-install \
	   build/cix-recover build/mkinstalleriso build/cix-boot.efi build/cix-xorriso \
	   "$PKG_DESTDIR/"
	# The dashboard mkbootroot copies into the assembled root.
	cp -r web "$PKG_DESTDIR/web"
	for f in index.html app.js style.css api.js; do
		if [ ! -f "$PKG_DESTDIR/web/$f" ]; then
			echo "web/$f missing from the artifact -- the assembled control plane would serve a blank dashboard" >&2
			exit 1
		fi
	done
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
