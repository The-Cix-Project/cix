#
# v2.53.64 -- three changes that need one build to confirm.
#
# 282: an undeclared query parameter is refused rather than ignored.
# DELETE /v1/pkg/htop?image=jumpbox returned 204 and deleted htop from
# the DEFAULT image, because the router strips everything from ? onward
# to match path segments and nothing afterwards was obliged to look at
# it again. apigen now reads each operation's declared parameters out
# of openapi.yaml and the dispatcher refuses anything else with a 400.
# An operation declaring no query parameters accepts none, which is
# what makes deletePkg safe by construction rather than by review.
#
# 300: the log store says so when the kernel ring outruns it. Its
# /dev/kmsg drain collapsed EAGAIN, EINTR and EPIPE into one return,
# and EPIPE is data loss -- kernel records overwritten before the
# reader reached them. Read as nothing new, entries vanished with
# nothing recording a gap, which turns log-store silence into false
# evidence that nothing was written.
#
# 297/298/299: cix-init is removed. ADR-0247 superseded the supervisor
# the day the box disproved it, and the boot entry went back to
# init=/bin/cixd then -- but the binary stayed built, staged into every
# bootroot and installed, shipping everywhere and running nowhere with
# three known defects against it. The reap channel, container_adopt(),
# the registry adopted bookkeeping, stallwatch's SIGUSR1-to-pid-1 path
# and hostproc_kill()'s supervised self-kill allowance go with it.
#
# ADR-0248: gcc runs over our own C as a linter in the selftest.
# -fsyntax-only, so it emits no object and nothing it touches can reach
# an artifact; TCC still compiles every byte this project ships. It
# earns its place by finding ten defects TCC reported nothing about
# under -Wall -Werror, including two int/size_t comparisons and four
# unused variables. Proven rather than assumed: an unused variable
# injected into stallwatch.c compiles clean under TCC and fails the
# gate.
#
# This build is the confirmation that all four hold against Cix's own
# toolchain and the full selftest, which is the only place they meet it.
#
pkg_name="cix"
pkg_version="v2.53.64"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.64.tar.gz"
pkg_sha256="77655930942deee631dd2d944ebda01cd847d2a23fb3c98464e0d0d8e43cd924"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="toolchain"
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
