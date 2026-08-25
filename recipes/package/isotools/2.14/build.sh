#
# isotools -- a self-contained, portable grub-mkrescue/sbsign/xorriso/
# mtools artifact, harvested as a hostbuild (ADR-0056) rather than
# merged into any container image. Closes the real gap ADR-0063 left
# open: those four packages already build from source (grub.recipe,
# sbsigntools.recipe, xorriso.recipe, mtools.recipe) but only ever
# land inside a *container* image's rootfs -- useless to cixd
# itself, which runs unsandboxed on the host and needs real,
# execve()-able binaries at a known host filesystem path to assemble
# an installer ISO (image/src/mkinstalleriso.c) server-side, the same
# way ADR-0057's cix hostbuild needs a real host-side mkbootroot.
#
# --build-image= must already have grub, sbsigntools, xorriso, and
# mtools installed via ordinary `pkg install` first (the "dev" image
# built up over Part 5 of the bare-metal-readiness plan has all four)
# -- a hard precondition documented here in a comment rather than via
# pkg_depends=, the same posture kernel.recipe/cix.recipe already
# established: pkg_depends='s automatic dependency-chain-installer
# resolves against a *target image*, which a hostbuild job has none
# of, so it is deliberately left empty for every hostbuild recipe in
# this project, not just this one.
#
# pkg_source/pkg_sha256/pkg_version below are grub's own real upstream
# tarball -- not because this recipe rebuilds GRUB again (it doesn't;
# pkg_build() below only verifies the already-installed grub-mkrescue
# reports this exact version), but because this recipe's real
# identity is "the grub-mkrescue toolchain, repackaged" and grub is
# the anchor of the four -- the same "real, genuine, checksum-verified
# source even though pkg_install() copies pre-built files instead of
# building from it" posture libc-dev.recipe already established for
# an analogous reason (glibc's own from-source build being its own
# giant undertaking, out of scope there; here, the four source builds
# already exist as their own separate recipes, so redoing them a
# second time here would be a real Parallel Implementation).
#
pkg_name="isotools"
pkg_version="2.14"
pkg_source="https://ftp.gnu.org/gnu/grub/grub-2.14.tar.xz"
pkg_sha256="bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43"
pkg_depends=""

# Fails loudly, not silently, if --build-image= doesn't actually have
# grub/sbsigntools/xorriso/mtools installed yet -- the same "verify
# before trusting" posture libc-dev.recipe's own pkg_build() already
# established, rather than a pkg_install() that would otherwise `cp`
# nothing and produce a silently-empty, broken artifact.
pkg_build() {
	/usr/bin/grub-mkrescue --version | grep -q ' 2.14$' || {
		echo "installed grub-mkrescue is not 2.14 -- update pkg_version/pkg_source above, or install grub onto --build-image= first" >&2
		exit 1
	}
	test -x /usr/bin/sbsign || {
		echo "sbsign not found -- install sbsigntools onto --build-image= first" >&2
		exit 1
	}
	test -x /usr/bin/xorriso || {
		echo "xorriso not found -- install xorriso onto --build-image= first" >&2
		exit 1
	}
	test -x /usr/bin/mformat || {
		echo "mformat not found -- install mtools onto --build-image= first" >&2
		exit 1
	}
	test -d /usr/lib/grub/x86_64-efi || {
		echo "grub's own x86_64-efi module tree is missing" >&2
		exit 1
	}
}

# grub-mkrescue statically links the same in-tree image-generation
# code grub-mkimage.c itself calls (confirmed directly, ADR-0063) --
# grub-mkimage itself is never exec'd as a subprocess, so it isn't
# harvested here. sbverify is harvested alongside sbsign purely so a
# later operator/REST caller can verify a signature too, not because
# mkinstalleriso.c itself calls it.
#
# The shared-library closure below is real and directly confirmed
# (via `ldd` against each binary on this exact build image, not
# guessed) and hardcoded rather than resolved at build time -- the
# same posture image/src/mkinstalleriso.c's own g_lib_closure[] array
# already established, for the identical reason: no `ldd` binary
# exists anywhere on this project's own from-recipe images (only a
# real Debian/system install carries the /usr/bin/ldd wrapper script).
pkg_install() {
	mkdir -p "$PKG_DESTDIR/bin" "$PKG_DESTDIR/lib/grub/x86_64-efi" \
	         "$PKG_DESTDIR/lib64" "$PKG_DESTDIR/lib/x86_64-linux-gnu"

	cp -a /usr/bin/grub-mkrescue "$PKG_DESTDIR/bin/"
	cp -a /usr/lib/grub/x86_64-efi/. "$PKG_DESTDIR/lib/grub/x86_64-efi/"

	cp -a /usr/bin/sbsign /usr/bin/sbverify "$PKG_DESTDIR/bin/"

	cp -a /usr/bin/xorriso "$PKG_DESTDIR/bin/"

	# mcopy/mformat are both symlinks to the one real "mtools" binary
	# (busybox-style multi-call), preserved as symlinks by cp -a.
	cp -a /usr/bin/mtools "$PKG_DESTDIR/bin/"
	ln -s mtools "$PKG_DESTDIR/bin/mcopy"
	ln -s mtools "$PKG_DESTDIR/bin/mformat"

	cp -a /lib64/ld-linux-x86-64.so.2 "$PKG_DESTDIR/lib64/"
	cp -a /lib/x86_64-linux-gnu/libc.so.6 \
	      /lib/x86_64-linux-gnu/liblzma.so.5 /lib/x86_64-linux-gnu/liblzma.so.5.4.1 \
	      /lib/x86_64-linux-gnu/libcrypto.so.3 \
	      /lib/x86_64-linux-gnu/libbz2.so.1.0 /lib/x86_64-linux-gnu/libbz2.so.1.0.4 \
	      /lib/x86_64-linux-gnu/libz.so.1 /lib/x86_64-linux-gnu/libz.so.1.2.13 \
	      "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
