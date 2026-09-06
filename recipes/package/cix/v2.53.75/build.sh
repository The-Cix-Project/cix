#
# v2.53.75 -- a package artifact carries what the platform runs
# (ADR-0251), and an image recipe is authoritative (ADR-0252).
#
# The glibc artifact measured 56.8 MiB compressed, 146.4 MiB unpacked
# over 2114 entries: libc.so.6 carrying 9.46 MiB of debug sections out
# of 11.42 MiB total, libc.a shipped three times at 22.43 MiB each, and
# 607 i18n locale SOURCE files feeding usr/lib/locale, which has zero
# entries. Fleet-wide, 112 packages and 1004 MiB.
#
# Nothing defined what a package artifact IS. Every recipe ran make
# install DESTDIR= and shipped whatever a general-purpose distribution
# would want, then cleaned up after itself by hand: 37 of 115 latest
# revisions pruned anything at all, in twenty-one different spellings.
# glibc is the worked example and is not ignorant -- it prunes man,
# info and doc and misses locale and i18n; it strips *crt*.o and misses
# every shared object it ships.
#
# daemon/policy/pkg-finalize.sh is that rule, once, embedded in cixd and
# sourced after pkg_install() returns. It strips ELF by output kind,
# drops libfoo.a where libfoo.so* ships beside it, removes *.la, and
# removes usr/share/{man,info,doc,locale,i18n}. Kept archives are left
# alone: !<arch> does not imply an archive of ELF, and go-bootstrap
# ships Go 1.4 archives that strip rejects.
#
# Also carries v2.53.74's #305 work: the platform's device paths are
# resolved rather than hardcoded to /dev/vda, and an A/B loader entry
# names its root by PARTUUID.
#
pkg_name="cix"
pkg_version="v2.53.75"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.53.75.tar.gz"
pkg_sha256="074ed3aff9beaf28fc1605d3e4320d67a10dfd7528dbdebb293490586abf7814"
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
