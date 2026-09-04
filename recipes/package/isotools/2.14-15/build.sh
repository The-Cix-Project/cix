#
# 2.14-9: no change to what is built. This revision exists to verify
# that iso-builder still builds the tools the installer media is made
# from, after libc-dev was removed from that image (ADR-0217 phase 3).
# Its C headers now come from glibc and its kernel UAPI headers from
# linux-headers, both already installed there.
#
# Verifying by building the thing the image exists to build, rather
# than by inspecting what is installed in it: an image that looks right
# is not an image that works, which this project has been taught twice
# by control-plane roots that assembled cleanly and then panicked.
#
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
pkg_version="2.14-15"
pkg_changelog="2.14-15: install to /usr/lib rather than the multiarch directory (#184 stage 2). The multiarch triplet is a Debian convention for letting several architectures share one filesystem and a Cix image has one architecture. /usr/lib is already on the loader search path, being glibc libdir since 2.44-7. The artifact approval is dropped because those bytes came from the previous revision. The libraries this artifact carries are searched rather than hardcoded, because they come from the build sandbox and glibc and openssl move under this recipe's feet. The loader keeps its own psABI path under lib64, which is fixed and not ours to choose. 2.14-14: declares the packages providing the libraries it stages into the ISO -- glibc, libxcrypt, efivar, keyutils, openssl and zlib. It was copying ten shared libraries out of the build sandbox without naming where any of them came from (#110). They all come from real Cix packages, so this is a missing declaration rather than ambient contamination, but the distinction was invisible: an undeclared copy looks identical whether its source is a package or a Debian leftover, which is exactly why #110 could not be assessed by reading. Declared, the composer supplies them and the copy is from a package. 2.14-13: declares dosfstools for mkfs.fat, the last of the tools isotools checks for. Derived by reading every test -x in the recipe at once rather than adding one per failed build: grub, sbsigntools, xorriso, mtools, mokutil, shim and dosfstools. Its own checks are good -- each names the tool and says to install it -- but they fire one at a time at run time, where a declaration lets the composer supply them all up front (ADR-0199). 2.14-12: declares shim too. isotools checks for each of its tools in turn and exits on the first one missing, so they surface one per build; grub, sbsign, xorriso, mformat, mokutil and shim are all real inputs to a signed bootable ISO. 2.14-11: mokutil: isotools checks for it with test -x and exits telling the operator to install it onto the build image first. Declaring it is that same requirement expressed where the composer can satisfy it. 2.14-10: declares its build tools so it can be rebuilt through the ordinary install path (#206). isotools assembles a bootable ISO by invoking grub-mkrescue, sbsign, xorriso and mformat. It already checks for each with test -x and exits with an instruction to install it onto the build image first -- declaring them is the same requirement, expressed where the composer can act on it (ADR-0199) instead of at run time."
pkg_source="https://ftp.gnu.org/gnu/grub/grub-2.14.tar.xz"
pkg_sha256="bc8d3c73535b8838d8c8e2654d73edc4e6ae8c8acdb45d5df5dc9a1547446d43"
pkg_depends=""
#
# Build tools derived from what this recipe's own pkg_build() actually
# invokes, plus the baseline the declaring recipes converge on. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils grub sbsigntools xorriso mtools mokutil shim dosfstools glibc libxcrypt efivar keyutils openssl zlib"

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
	# The installer-image content this artifact now also carries -- see
	# pkg_install() below for why it moved here from mkinstalleriso's
	# own hardcoded host paths.
	test -x /usr/bin/mokutil || {
		echo "mokutil not found -- install mokutil onto --build-image= first" >&2
		exit 1
	}
	test -f /usr/lib/shim/shimx64.efi.signed || {
		echo "shim not found -- install shim onto --build-image= first" >&2
		exit 1
	}
	test -f /usr/lib/shim/mmx64.efi.signed || {
		echo "MokManager not found -- install shim onto --build-image= first" >&2
		exit 1
	}
	# mkfs.fat, for the same reason shim and mokutil moved here: it is
	# an installer input that a Cix control-plane root has no reason to
	# carry, so mkinstalleriso's old /usr/sbin read could only ever work
	# on a Debian development box.
	test -x /usr/sbin/mkfs.fat || {
		echo "mkfs.fat not found -- install dosfstools onto --build-image= first" >&2
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

# The shared-library closure below is hardcoded rather than resolved
# at build time -- the same posture image/src/mkinstalleriso.c's own
# g_lib_closure[] array already established, for the identical reason:
# no `ldd` binary exists anywhere on this project's own from-recipe
# images (only a real Debian/system install carries the /usr/bin/ldd
# wrapper script).
#
# 2.14's closure was wrong, and wrong in a way that says where it came
# from. It staged liblzma.so.5.4.1, libbz2.so.1.0.4 and libz.so.1.2.13
# out of /lib/x86_64-linux-gnu -- those exact minor-version filenames
# are Debian's, and no package in this recipe set installs any of them.
# It could only ever have been read off the old shared build sandbox
# from before ADR-0199, the one seeded with the build host's entire
# /usr (issue #168). Against a real composed image those `cp` calls
# name files that do not exist, so this recipe could not have built
# at all since that sandbox was retired.
#
# This revision's closure is MEASURED, not copied: readelf -d on each
# real published artifact, following every DT_NEEDED to a fixed point.
#   grub-mkrescue        libc.so.6
#   xorriso              libz.so.1  libc.so.6
#   mtools               libc.so.6
#   sbsign, sbverify     libcrypto.so.3  libc.so.6
# and then the second level, which is where the closure actually
# closes:
#   libcrypto.so.3       libc.so.6      (only -- glibc 2.36 folded
#                                       libdl into libc, so the
#                                       -ldl -pthread that openssl
#                                       links with adds no DT_NEEDED)
#   libz.so.1.3.2        libc.so.6
# So the real set is libc.so.6, libz.so.1, libcrypto.so.3, plus the
# loader. liblzma and libbz2 are not in it at any level: nothing here
# links either.
#
# sbsign pulls in libcrypto and NOT libuuid, which is worth stating
# because it is not the obvious reading of sbsigntools' configure.ac
# -- that file does check for libuuid, but src/Makefile.am adds
# $(uuid_LIBS) only to sbvarsign/sbsiglist/sbkeysync, none of which
# this artifact harvests. Confirmed against the built binary, not
# inferred from configure.
#
# Source paths matter as much as the file list, and they are no longer
# fixed. Each of these libraries comes from a package that is moving to
# /usr/lib as this platform collapses its library directories (#184),
# and they do not arrive together. zlib was ALWAYS in /usr/lib -- which
# is what broke 2.14, since it looked in the multiarch directory for a
# file that had never been there -- while libc.so.6 and libcrypto.so.3
# are in it until glibc and openssl move. So the source is SEARCHED
# rather than named, and a library that is genuinely absent fails the
# build here rather than at execve() on a deployed host.
#
# The destination is one directory now:
# mkinstalleriso.c sets PATH=<root>/bin and
# LD_LIBRARY_PATH=<root>/usr/lib (ADR-0154 -- these
# binaries are execve()'d directly off the bare host, not inside a
# chroot, so without that the loader silently prefers whatever the
# host already has at a system path). Only the file set and where each
# file is copied FROM have changed.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/bin" "$PKG_DESTDIR/lib/grub/x86_64-efi" \
	         "$PKG_DESTDIR/lib64" "$PKG_DESTDIR/usr/lib"

	cp -a /usr/bin/grub-mkrescue "$PKG_DESTDIR/bin/"
	cp -a /usr/lib/grub/x86_64-efi/. "$PKG_DESTDIR/lib/grub/x86_64-efi/"

	# 2.14-7 is a rebuild, not a recipe change: 2.14-6 harvested this
	# directory faithfully and it contained only grub-mkconfig_lib,
	# because the grub it harvested from could not generate fonts at
	# all. grub 2.14-7 fixed that (freetype + unifont), so re-harvesting
	# is what actually puts unicode.pf2 in the artifact.
	#
	# grub-mkrescue's own data files -- unicode.pf2 above all. It reads
	# them from a pkgdatadir compiled in at /usr/share/grub, which
	# exists on the machine that built GRUB and on no Cix control-plane
	# root, so an ISO build got all the way to the final step and then
	# died on "cannot open /usr/share/grub/unicode.pf2". mkinstalleriso
	# points GRUB's own "pkgdatadir" environment variable here.
	mkdir -p "$PKG_DESTDIR/share/grub"
	cp -a /usr/share/grub/. "$PKG_DESTDIR/share/grub/"

	cp -a /usr/bin/sbsign /usr/bin/sbverify "$PKG_DESTDIR/bin/"

	cp -a /usr/bin/xorriso "$PKG_DESTDIR/bin/"

	# mcopy/mformat are both symlinks to the one real "mtools" binary
	# (busybox-style multi-call), preserved as symlinks by cp -a.
	cp -a /usr/bin/mtools "$PKG_DESTDIR/bin/"
	ln -s mtools "$PKG_DESTDIR/bin/mcopy"
	ln -s mtools "$PKG_DESTDIR/bin/mformat"

	# Every cp below names a file that must exist: a missing one fails
	# the build here rather than producing an artifact that dies at
	# execve() on a deployed host, where the same fault would surface
	# as a bare "exit 1" from grub-mkrescue with nothing naming a
	# library.
	cp -a /lib64/ld-linux-x86-64.so.2 "$PKG_DESTDIR/lib64/"
	# Found rather than hardcoded (#184). These come from the build
	# sandbox, and this platform is collapsing its library directories,
	# so glibc and openssl move under this recipe. zlib was ALREADY in
	# /usr/lib while the other two were not -- which is exactly the
	# split this search removes.
	for lib in libc.so.6 libcrypto.so.3 libz.so.1 libz.so.1.3.2; do
		found=""
		for dir in /usr/lib /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu; do
			if [ -e "$dir/$lib" ]; then found="$dir/$lib"; break; fi
		done
		[ -n "$found" ] || {
			echo "isotools: $lib is not in the build sandbox" >&2
			exit 1
		}
		cp -a "$found" "$PKG_DESTDIR/usr/lib/"
	done

	# --- what the INSTALLER image needs, as opposed to the ISO tools ---
	#
	# Everything above is a tool this artifact runs to build an ISO.
	# Everything below is content mkinstalleriso stages INTO the
	# installer image, which until now it read from absolute paths on
	# whatever machine happened to run it: /usr/bin/mokutil,
	# /usr/lib/shim/*.efi.signed and a hardcoded library list. None of
	# it exists on a Cix control-plane root, which is why POST
	# /v1/system/iso has never been servable on a real host.
	#
	# Carrying it here makes those inputs come from packages -- shim
	# 16.1-1, mokutil 0.7.2-2, efivar 39-3, keyutils 1.6.3-6,
	# libxcrypt 4.4.36-4 -- harvested out of an image, which is exactly
	# what this hostbuild exists to do for the ISO tools already.
	mkdir -p "$PKG_DESTDIR/shim"
	cp -a /usr/lib/shim/shimx64.efi.signed /usr/lib/shim/mmx64.efi.signed \
	      "$PKG_DESTDIR/shim/"
	cp -a /usr/bin/mokutil "$PKG_DESTDIR/bin/"

	# The ESP formatter. Built to need nothing beyond libc (mkfs.fat has
	# no dependencies at all), asserted against the real ELF in its own
	# recipe -- so unlike mokutil below, it contributes nothing to the
	# closure this artifact carries.
	#
	# sbin/, not bin/, and deliberately: mkinstalleriso resolves this as
	# <isotools-root>/sbin/mkfs.fat, and <isotools-root> is a plain
	# "/usr" for a bare dev-machine invocation. Staging it under bin/
	# (as 2.14-4 did) made that resolve to /usr/bin/mkfs.fat, which
	# exists on no development box -- the Cix path kept working while
	# the dev fallback silently broke.
	#
	# 2.14-4 also harvested fdisk, for cix-install's interactive
	# partitioning path. ADR-0214 removed that path, and fdisk with it.
	#
	mkdir -p "$PKG_DESTDIR/sbin"
	cp -a /usr/sbin/mkfs.fat "$PKG_DESTDIR/sbin/"

	# mokutil's closure, kept in this artifact's own lib directory
	# alongside the ISO tools' libraries.
	#
	# mkinstalleriso no longer carries a list of which of these to stage
	# into the installer image: it reads mokutil's real DT_NEEDED
	# entries and resolves them against this directory, transitively. So
	# what matters here is only that everything mokutil links is
	# PRESENT -- the tool works out which, and fails loudly naming any
	# library it cannot find.
	#
	# MEASURED off the binary this project builds, not copied from
	# Debian's, because the two genuinely differ:
	#
	#   ours     libssl.so.3 libcrypto.so.3 libefivar.so.1
	#            libkeyutils.so.1 libcrypt.so.2 libc.so.6
	#   Debian's libcrypto.so.3 libefivar.so.1 libkeyutils.so.1
	#            libcrypt.so.1 libc.so.6 libdl.so.2
	#
	# Ours pulls libssl because openssl.pc's Libs names both libraries,
	# and libcrypt.so.2 because our libxcrypt drops the obsolete DES/NIS
	# ABI -- a different soname, not a different version. Ours does not
	# link libdl at all; glibc folded it into libc at 2.34.
	#
	# Each SONAME symlink is copied with its real target, since cp -a
	# preserves a symlink as a symlink and a dangling one is worse than
	# a missing file -- it fails at load time, not at build time.
	# Same search as above (#184): every one of these comes from a
	# package that has moved or is moving to /usr/lib, so naming a
	# fixed directory would break as each one lands.
	for lib in libssl.so.3 \
	           libefivar.so.1 libefivar.so.1.39 \
	           libkeyutils.so.1 libkeyutils.so.1.10 \
	           libcrypt.so.2 libcrypt.so.2.0.0; do
		found=""
		for dir in /usr/lib /lib/x86_64-linux-gnu /usr/lib/x86_64-linux-gnu; do
			if [ -e "$dir/$lib" ]; then found="$dir/$lib"; break; fi
		done
		[ -n "$found" ] || {
			echo "isotools: $lib is not in the build sandbox" >&2
			exit 1
		}
		cp -a "$found" "$PKG_DESTDIR/usr/lib/"
	done
}
