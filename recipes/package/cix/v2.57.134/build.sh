#
# v2.57.134: an installer failure is readable, loopback is a valid
# management choice, and a gateway is optional (#131, bare-metal)
#
# All three from one real bare-metal attempt: the installer could not
# see the machine's built-in Ethernet, then "went away for a long time"
# and crashed with nothing on screen.
#
# cix-install had 31 error paths that returned from main() while running
# as PID 1. Returning from PID 1 is an instant "Attempted to kill init!"
# panic whose stack trace scrolls the installer's own message -- the one
# saying what went wrong -- off the top of the screen. The success path
# already parked on dual_console_wait_for_key(); only the failures did
# not, so the outcome that most needed to be readable was the one that
# destroyed its own evidence. install_main() is now wrapped by a main()
# that parks on a banner, rather than rebooting, since a reboot takes
# the error off screen as surely as the panic did. Same convention as
# cix-recover's main().
#
# lo is now a listed, labelled management interface with its own
# defaults (127.0.0.1, /8, no gateway). It was skipped on the reasoning
# that the list shows real NICs -- but on real hardware the built-in
# Ethernet is often not there either, because E1000E/IGB/IXGBE/R8169/
# TIGON3 are all =m and this installer carries no module tree, so only
# virtio_net appears. Hiding the one choice that always works, on the
# screen where the operator needs one, was the gap.
#
# That is a deliberate state, not a degraded one: cixd's DEFAULT_BIND is
# 127.0.0.1 (main.c:157, used at :27740) and boot_init() brings lo
# administratively up precisely so that bind can succeed. Checking it
# corrected a comment of mine that had claimed cixd binds every
# interface when unpinned.
#
# A gateway is optional in all three places that had to agree: the
# installer's completeness test (interface, address and prefix), 
# parse_net_conf(), and bootstrap_management_network() -- which treated
# an empty gateway as a parse failure, and returning -1 there fails
# boot_init() and exits cixd, which as PID 1 is the same panic again. A
# present-but-invalid gateway stays an error, and no default route is
# added when there is none.
#
#
pkg_name="cix"
pkg_version="v2.57.134"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.134.tar.gz"
pkg_sha256="7fda202daa596c6dcc87cebf45879a0687b2cf3366eaafc42cec5ed0047b405a"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
pkg_depends=""

pkg_build() {
	make CIX_VERSION="$pkg_version" \
	    build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
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
	cp build/cix-init build/cixd build/cixctl build/mkbootroot build/cix-install \
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
