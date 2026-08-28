pkg_name="kernel"
pkg_version="6.18.40-15"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=9c967196d9ce65ea3937db298f851d83a7447f1f"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 a7d7d225fdf6c17cd8e2b7e18e5b89f9ea06416050b58222dee8c206b9cb5946"
pkg_depends=""

# Re-pinned to -15 (ADR-0207 phase 4, #161): CONFIG_BTRFS_FS=y -- the
# platform storage substrate itself. Found by the first real QEMU
# test_installer run after the installer's btrfs flip, not by review:
# mkfs.btrfs (userspace) succeeded, then the installer panicked at
# "mount config: No such device". The fragment's own new comment block
# carries the full account; its Kconfig selects pull the checksum/
# compression deps under the documented allnoconfig+merge procedure,
# so the fragment ref bump is the entire change here.
#
# Re-pinned to -14 (issue #114): CONFIG_BINFMT_SCRIPT=y. Without it the
# kernel does not understand "#!" at all, so execve() of any script
# returns ENOEXEC -- and because POSIX makes a shell whose execve()
# returns ENOEXEC run the file itself, every shell script kept working
# by coincidence while every script with any other interpreter was
# quietly handed to bash. Found when diffutils' bundled help2man Perl
# script produced bash syntax errors with a working /usr/bin/perl
# sitting right there in the build container. Same allnoconfig trap the
# config file already documents above CONFIG_BINFMT_ELF: ELF was caught
# because nothing boots without it, SCRIPT was not because its absence
# degrades rather than fails.
#
# (-10 through -13 were published straight to the box's catalog and
# never committed here; -14 is the next free number rather than a
# thirteenth revision of anything.)
#
# Re-pinned to -9: -8's own fix (wholesale-replace gcc's
# include-fixed/limits.h with a bare passthrough) got the kernel's own
# HOSTCC past its PATH_MAX gap, but was later proven incomplete while
# root-causing a DIFFERENT, unrelated package's build failure
# (elfutils 0.192-4 through -6's own history has the full trail): that
# wholesale replacement silently discards gcc's own correct ISO limit
# macros (UCHAR_MAX, INT_MAX, etc., all derived from real compiler
# builtins like __SCHAR_MAX__/__INT_MAX__) along with the one thing
# that was actually broken -- kernel.recipe -8 never noticed because
# none of the HOSTCC-compiled kconfig/objtool tooling it happened to
# exercise needed those particular macros from <limits.h>, but that
# was luck, not correctness, and left a known-inferior fix in place
# once the real defect was understood (contra this project's own "no
# hacks" standard).
#
# The real, fully root-caused defect (probe-gcc-headers v3 through
# v6): gcc's own include-fixed/limits.h content is CORRECT and matches
# a real working reference gcc byte-for-byte -- it is NOT broken. The
# actual bug is include-fixed/syslimits.h: confirmed via direct
# md5sum, it is a byte-for-byte IDENTICAL COPY of limits.h, instead of
# the short, distinct `#include_next`-chaining wrapper a complete
# fixincludes/mkheaders run is supposed to generate there. limits.h's
# own `#include "syslimits.h"` (right after defining _GCC_LIMITS_H_)
# therefore re-enters what is really just itself, with _GCC_LIMITS_H_
# already set -- it takes the `#else` branch, finds `_GCC_NEXT_LIMITS_H`
# was never defined (only the real wrapper sets it), and does nothing:
# glibc's real /usr/include/limits.h (MB_LEN_MAX=16, PATH_MAX via
# bits/posix1_lim.h, etc.) is never reached through <limits.h> at all.
#
# Real, minimal, correct fix (elfutils 0.192-5 established this):
# replace ONLY syslimits.h with the standard wrapper -- limits.h
# itself is left completely untouched, so its own correct ISO limit
# macros keep working AND glibc's real header becomes reachable again.
# Applied here as this recipe's own scoped, build-container-local
# workaround (this build's own upperdir), same posture as -8.
#
# tools/objtool's own earlier `cannot find -lelf` failure this same
# session (ADR-0056) is addressed by elfutils 0.192-6 (libelf only),
# but NOT via pkg_depends here -- pkg_hostbuild_start() (daemon/src/
# pkg.c) explicitly rejects any hostbuild recipe with a non-empty
# pkg_depends (dependency *resolution* targets "merge into an image",
# meaningless for a one-shot artifact harvest); every prerequisite
# must already be baked into --build-image's own rootfs via an
# ordinary `pkg install` first, confirmed the hard way when this
# recipe's own first version of this comment still had
# pkg_depends="elfutils" set and hostbuild rejected it outright with
# "no such recipe, or it failed to parse" (PKG_ERR_INVALID_RECIPE).
#
# gcc.recipe's own permanent fix (issue #55, updated with this fuller
# syslimits.h root cause) still needs its mkheaders/fixincludes step
# re-run properly, needing a full --enable-bootstrap rebuild to
# verify -- not done here.
pkg_build() {
	cat > /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h <<'EOF'
#ifndef _GCC_NEXT_LIMITS_H
#define _GCC_NEXT_LIMITS_H
#include_next <limits.h>
#undef _GCC_NEXT_LIMITS_H
#endif
EOF

	cp /build/extra/qemu-part1.config .config
	if [ -n "$CIX_KMOD_EXTRA_SYMBOLS" ]; then
		: > /build/extra/kmod-extra.config
		for sym in $CIX_KMOD_EXTRA_SYMBOLS; do
			echo "${sym}=m" >> /build/extra/kmod-extra.config
		done
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc allnoconfig
	if [ -n "$CIX_KMOD_EXTRA_SYMBOLS" ]; then
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config \
			/build/extra/kmod-extra.config
	else
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc olddefconfig
	# bzImage and modules built in a single make invocation, not two
	# separate ones: modpost's per-module symbol resolution needs
	# Module.symvers/vmlinux.o from the vmlinux link step still present
	# in the tree, which a second, independent `make modules` afterwards
	# does not reliably see -- confirmed empirically (modpost failed with
	# "vmlinux.o is missing" + hundreds of "undefined!" errors for
	# ordinary exported symbols like kfree/dev_set_name against a split
	# invocation; this project's own established discipline is to fix
	# the confirmed real cause, not guess).
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc -j"$(nproc)" bzImage modules
}

pkg_install() {
	cp arch/x86/boot/bzImage "$PKG_DESTDIR/bzImage"
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc \
	    INSTALL_MOD_PATH="$PKG_DESTDIR" modules_install
	depmod -b "$PKG_DESTDIR" "$(make -s ARCH=x86_64 kernelrelease)"
}
