#
# v2.57.169: fixes a real bug in v2.57.168's own libarchive extraction
# (#411 follow-up): ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS refused
# every single archive entry outright, silently, because dest_dir is
# always absolute -- confirmed live via a real failed `pkg install
# --name=probe-gnu-mirror`. Fixed by checking the archive's own entry
# name for an absolute path BEFORE dest_dir is prefixed onto it,
# instead of relying on that flag. Also adds real error logging
# (archive_error_string() to the log store on every failure path,
# where before there was none at all) and ARCHIVE_EXTRACT_OWNER (GNU
# tar's own default as real root, which the old tar -xf call always
# got for free). See #466 for the test-coverage gap this exposed.
#
# v2.57.168: tar extraction moves in-process via libarchive (#411's
# extraction half; creation is out of scope, byte-identity constrained
# -- see CHANGELOG.md). Also declares libarchive in pkg_build_depends
# (build-time headers) -- runtime needs libarchive installed onto
# cix-hosttools too, so mkbootroot's own CIX_LIB_DIRS_PLATFORM sweep
# bundles the .so into the control-plane root.
#
# v2.57.167: retire rm/sha256sum shell-outs, one openssl call, and the
# kmod-extra.config env var (#352, #351, #412). See CHANGELOG.md.
#
# Also declares sed, tar, gzip and grep in pkg_build_depends -- the
# build failed in turn on '/bin/sh: sed: command not found'
# (build/generated/pkg_finalize.h, ADR-0251, Makefile:714), then 'sh:
# tar: command not found' (test_artifact_export/test_image_recipe/
# test_images building their own fixtures), then 'sh: grep: command
# not found' (test_artifact_export's own tar-content assertions,
# revealed only once tar itself worked). All four tools are installed
# into cix-builder (6.1.0's own image_packages) yet were unavailable to
# this build's own composed environment. Cause not fully established
# -- whatever let earlier builds reach these ambiently stopped holding;
# declaring them is correct regardless (#168/ADR-0199: composed from
# declared tools, not from whatever an image happens to carry).
#
# v2.57.166: fix a use-after-free in the #446 ping_group_range 400
# message (it read freed JSON memory). The 400 logic itself was correct.
#
pkg_name="cix"
pkg_version="v2.57.169"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.169.tar.gz"
pkg_sha256="8d4ec0ac77d5ecd2b524f04b29d33893b55db49178ea0f604f94c0515e94388b"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils minisign sed tar gzip grep libarchive"
pkg_build_caps="CAP_SYS_ADMIN"
# ADR-0208: cix-builder's one job is building Cix. This said
# "toolchain" -- an image no recipe in this repo describes, which
# existed only on one host and died with it. cix-builder is what the
# guide documents and what the taxonomy names (#305).
pkg_build_image="cix-builder"
# Reverted to empty (was briefly "openssl", #465): pkg_hostbuild_start()
# refuses ANY recipe with a non-empty pkg_depends outright
# (PKG_ERR_INVALID_RECIPE) -- "dependency resolution targets 'merge
# into an image', meaningless for a one-shot artifact harvest; every
# prerequisite must already be baked into build_image's own rootfs."
# cix's real deploy path is `pkg hostbuild cix --upgrade --wait
# --deploy` (installs into __hostbuild, elfcheck does not apply there),
# not an ordinary `pkg install --image=X` -- so this field must stay
# empty for the path that actually matters, even though elfcheck can
# legitimately refuse an ordinary `pkg install --name=cix --image=base`
# for the same missing-openssl reason. See #465.
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
