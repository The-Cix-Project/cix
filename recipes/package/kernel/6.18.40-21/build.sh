#
# 6.18.40-18: no change to what is built. This revision exists to verify
# that kernel-builder still builds a kernel after libc-dev was removed
# from it (ADR-0217 phase 3) -- the image's kernel UAPI headers now come
# from linux-headers, installed there first for exactly this reason.
#
# Verification is doing the job, not reporting success: an image that
# assembles is not an image that builds, and this project has been
# taught that twice by roots that assembled cleanly and then panicked.
#
pkg_name="kernel"
pkg_version="6.18.40-21"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=2689d63f8c5450c6b8f06101bfa926aebf93e27e"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 562e048b66f7c3cd988b6438c0e77f583d01fee4d25acb9e3d7e48b41bff1ad5"
pkg_depends=""
pkg_toolchain="gcc"
pkg_toolchain_reason="build system requires GCC internals: the Linux kernel build requires GCC and its extensions throughout"
pkg_changelog="6.18.40-21: tmpfs extended attributes, KSM and per-cgroup hugetlb accounting, in one rebuild (#259, #50, #53). TMPFS_XATTR is a real defect rather than a new capability: CONFIG_TMPFS alone gives a tmpfs that cannot store an xattr, overlayfs needs trusted.overlay.* on its upperdir, and without them every overlay on a tmpfs mounts degraded -- redirect_dir off, which is what makes renaming a lower-only directory work. ADVISE_SYSCALLS accompanies KSM and is a finding of its own: it gates madvise itself, so madvise has always returned ENOSYS here, and KSMs per-region opt-in IS madvise MADV_MERGEABLE -- the same shape as SCSI_LOWLEVEL in the previous revision, a capability that cannot be selected without the thing it lives behind. HUGETLBFS and HUGETLB_PAGE accompany CGROUP_HUGETLB because the accounting knob cannot stand alone. Nothing is turned on by any of this: KSM idles until sysfs says otherwise and nr_hugepages defaults to zero. A gate now asserts each requested symbol survived olddefconfig, because merge_config warns and carries on. 6.18.40-20: a real hardware storage config (#242). This config file's own header said QEMU boot-test target only, not the real target hardware's eventual config -- and that eventual config never happened, so the config written to boot a QEMU test is the config every Cix host runs. A virtio-scsi disk attached to a real host was therefore invisible to the kernel rather than filtered by Cix, while a virtio-blk disk added at the same moment appeared instantly. Adds SCSI_LOWLEVEL, which is not a driver but the menu the low-level host adapters live behind, so without it none of the others could be selected at all and they could not be added one at a time; SCSI_VIRTIO, the reported case; and the controllers real servers ship with -- MEGARAID_SAS for Dell PERC, SCSI_MPT3SAS for LSI SAS2 and SAS3 HBAs, SCSI_SMARTPQI and SCSI_HPSA for HPE Smart Array. All built in rather than modules, because an EFI-stub kernel with no initramfs cannot load a module off a disk it cannot yet see. BLK_DEV_LOOP and the DM family are deliberately left out despite being listed on the issue: neither has any consumer in this platform and neither makes a disk visible. 6.18.40-19: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

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
	# -17: the gcc version is ASKED FOR, not hardcoded, and this recipe
	# builds the kernel again instead of fetching one.
	#
	# Two things were wrong, and they compounded.
	#
	# First, -15 wrote this correction to .../12.5.0/include-fixed/ --
	# the gcc that happened to be installed when it was written -- so the
	# first build against an image pinning a different gcc died with
	#   /build/recipe.sh: line 96:
	#     /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h:
	#     No such file or directory
	# naming a path nobody had thought about since. elfutils 0.192-8
	# already fixed exactly this, for exactly this correction, against
	# exactly this gcc install; the kernel recipe was never updated to
	# match. It only surfaced now because kernel-builder (ADR-0208,
	# gcc 16.2.0-11) is the first image that could build a kernel at all.
	#
	# Second, and the reason the first went unnoticed: a -16 exists on
	# 192.168.15.95 that is not in this repository. Its own comment says
	# what it is -- "192.168.15.95 has no gcc-bearing dev build image
	# (its kernels have always come from the dev-machine build), so the
	# btrfs kernel was built on the dev machine ... and published to the
	# artifact cache; this line lets pkg hostbuild kernel on the box take
	# the artifact tier and skip the on-box build it cannot run." That is
	# a kernel compiled with a foreign toolchain, published to the cache
	# and booted, which the Build Provenance Mandate rules out in as many
	# words. It also kept the hardcoded path above unreachable, because
	# nothing on the box ever ran this function.
	#
	# -17 approves no prebuilt artifact, deliberately. The kernel is
	# built here, on a Cix host, by Cix, or it is not shipped. The
	# premise that made -16 seem necessary is gone: kernel-builder now
	# exists and holds a real gcc toolchain.
	GCC_VERSION=$(/usr/bin/gcc -dumpversion) || {
		echo "kernel: no /usr/bin/gcc in this build environment -- it is required" >&2
		exit 1
	}
	GCC_FIXED_DIR="/usr/lib/gcc/x86_64-pc-linux-gnu/$GCC_VERSION/include-fixed"
	test -d "$GCC_FIXED_DIR" || {
		echo "kernel: gcc $GCC_VERSION has no $GCC_FIXED_DIR to correct" >&2
		exit 1
	}
	cat > "$GCC_FIXED_DIR/syslimits.h" <<'EOF'
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

	#
	# Assert the capabilities this revision exists for actually landed.
	#
	# merge_config warns about a symbol it could not set and carries on,
	# and olddefconfig will silently drop one whose dependencies are not
	# met. That is not a theoretical risk here: #242 shipped a config
	# whose HBA drivers could not be selected at all until
	# SCSI_LOWLEVEL -- the menu they live behind -- was added, and the
	# only reason it was caught was that someone looked. A capability
	# that is requested and quietly absent is worse than one nobody
	# asked for, because everything downstream assumes it is there.
	#
	for sym in CONFIG_TMPFS_XATTR CONFIG_KSM CONFIG_ADVISE_SYSCALLS \
	           CONFIG_HUGETLBFS CONFIG_HUGETLB_PAGE CONFIG_CGROUP_HUGETLB \
	           CONFIG_IPC_NS CONFIG_IKCONFIG CONFIG_IKCONFIG_PROC; do
		if ! grep -q "^${sym}=y$" .config; then
			echo "kernel: ${sym} did not survive olddefconfig -- requested and not set" >&2
			echo "  what .config actually has:" >&2
			grep -E "^(# )?${sym}" .config >&2 || echo "    (nothing at all)" >&2
			exit 1
		fi
		echo "  ${sym}=y confirmed in .config"
	done
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
