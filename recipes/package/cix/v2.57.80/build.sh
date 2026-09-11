#
# v2.57.80: a package must declare every library it links (#389, ADR-0276)
#
# The same tree as v2.57.56, which is a dead version. Its recipe was
# produced by copying v2.57.55's and bumping pkg_version= and
# pkg_source= -- and not pkg_sha256=, which stayed pointed at the
# previous tag's bytes. The build failed at once with "checksum mismatch
# (source 0)", which is the gate doing exactly its job.
#
# It could not be corrected in place: a published recipe's build
# instructions are immutable (ADR-0107), and the single permitted edit
# is adding pkg_artifact_sha256= where none exists -- an APPROVAL of
# bytes, not an instruction. So v2.57.56 is published, unbuildable and
# permanent; there is no recipe delete. The three fields that change
# between cix releases are pkg_version, pkg_source and pkg_sha256, and
# a copy-and-bump that touches two of them produces exactly this.
#
# cmake@4.4.3-2 declared pkg_depends="" and built against openssl, so it
# linked DT_NEEDED libssl.so.3 and was installed recording no runtime
# dependency. It ran fine in cix-builder, where openssl happened to be
# installed for unrelated reasons. An hour later fastfetch failed with a
# 148-byte log naming neither cmake nor the missing declaration:
#
#     cmake: error while loading shared libraries: libssl.so.3
#
# buildenv_add_tool() composes a build environment from each declared
# tool's recorded depends -- correct recursion, fed a lie. Blame landed
# on a package whose own recipe was right; whether it reproduced
# depended on which image you looked at; and correcting the recorded
# field cost a full C++ bootstrap rebuild, because only a reinstall
# rewrites it.
#
# The gate refuses an install whose staged tree needs a soname that
# neither the tree itself, nor the transitive closure of its declared
# pkg_depends, nor the C library provides. The check is about the
# DECLARATION: asking whether libssl.so.3 exists in the target image
# would have passed the package that caused this. A question it cannot
# answer -- unresolvable dependency, unreadable tree -- logs why it did
# not run and proceeds; a gate that fails an install fires on evidence.
#
# Expect it to refuse an existing recipe on its next build. Nothing
# re-checks what is installed, so no running host changes.
#
# Also in this build, both found by the v2.57.55 selftest failing:
#
#   - ADR-0274/0275 carried the "# ADR-NNNN: Title" H1 this corpus no
#     longer accepts, which test_docindex gates. Every release built
#     from those commits failed.
#   - test_toolchain_policy counted 26 gcc recipes against 28 expected,
#     and both missing names -- cmake and fastfetch -- were correct
#     recipes declaring pkg_toolchain="gcc". The detector scanned
#     pkg_build() for compiler command text, and neither names one:
#     cmake runs ./bootstrap, fastfetch runs cmake. A declaration is now
#     authoritative and the scan is the backstop for gcc used without
#     being declared.
#
# And ADR-0275 amended before implementation (#388): the phase plan had
# no home, the phase vocabulary is closed at six, and markers do not
# generalise to CBS -- plan from `cbs explain --json`, position needs
# cix-build-system#131, which does not exist yet.
#
# Carried from v2.57.55, which never deployed: /etc/os-release on the
# host root and in every container image (#387, ADR-0274). One
# osrelease_render(), called from mkbootroot and
# pkg_seed_image_baseline(), because two literals would drift and that
# drift is a host and its containers disagreeing about what they are.
# ID=cix; no VERSION_ID, since Cix is rolling-release.
#
pkg_name="cix"
pkg_version="v2.57.80"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/v2.57.80.tar.gz"
pkg_sha256="9aad815c8e8900cec8a8a116c4f4d6367f3137d6f88dcfe9ae119f7bc9d7227c"
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
