#
# v2.57.136: every tool the installer runs is visible on the screen
# (bare-metal install)
#
# A real install failed with "/usr/sbin/sfdisk failed (status 0x100)"
# and nothing else -- not sfdisk's error, not even its usual "Created a
# new partition" lines. The operator had a monitor, the failure was
# readable thanks to the PID-1 parking fix, and it still could not be
# diagnosed, because the one thing that knew what went wrong never
# reached the screen.
#
# The cause is the console. The installer boots with console=tty0
# console=ttyS0, and with more than one console= the kernel points
# /dev/console at the LAST one -- ttyS0. Our own messages appear only
# because dual_printf() writes to /dev/tty0 and /dev/ttyS0 explicitly
# rather than trusting fd 1. A child inherits fd 1 and 2 and cannot do
# that, so sfdisk, mkfs.vfat, mkfs.btrfs and mokutil have all been
# writing to a serial port a monitor-attached machine does not have.
#
# Both runners now give the child a pipe for stdout AND stderr and
# relay it through dual_printf(), prefixed with the tool's own name.
# stderr especially: a tool's diagnosis is usually there.
#
# The two remaining writes that bypassed the dual console go with it --
# the "partitioning ... MiB total" summary (a plain printf) and
# auto_partition()'s "could not read the size of" error (an fprintf to
# stderr). Both were invisible on exactly the hardware they were
# written for; none are left in the file.
#
# This does NOT explain the sfdisk failure. It is what makes the next
# attempt able to answer it in one trip. Two causes were eliminated
# first, by reproducing against a 40 GiB image carrying a
# Windows-shaped GPT and a real NTFS signature: sfdisk rewrote that
# table and exited 0, so neither an existing table nor a leftover
# signature is the trigger, and no speculative --wipe or --force was
# added on a guess.
#
#
pkg_name="cix"
pkg_version="v2.57.136"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.136.tar.gz"
pkg_sha256="49611b2e0ca6bcd1968aadc0b0406d12cbaee38572dbef43ff5f0d65fb049cee"
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
