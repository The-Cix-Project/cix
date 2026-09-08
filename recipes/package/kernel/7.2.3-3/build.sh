#
# 7.2.3-1 -- the first kernel recipe on this platform whose version was
# chosen by a release channel rather than by a person, and whose
# checksum was never computed here.
#
# 192.168.15.95 tracked 6.18.40, which is a *longterm* line and was not
# even current for it (kernel.org lists 6.18.49). The stated intent is
# `stable`, and kernel.org's own releases.json resolves that to 7.2.3.
#
# WHERE THE CHECKSUM CAME FROM (ADR-0254, ADR-0255 stages 1-3):
#
#   1. resolve   www.kernel.org/releases.json -> latest_stable 7.2.3
#   2. fetch     .../pub/linux/kernel/v7.x/sha256sums.asc  (clearsigned)
#   3. verify    Good signature, RSA key
#                B8868C80BA62A1FFFAF5FDA9632D3A06589DA6B1 -- byte-equal
#                to the fingerprint pinned at
#                docs/keys/kernel.org-autosigner.asc
#   4. read      linux-7.2.3.tar.xz -> 8ba259e8...50ecd03
#
# It is kernel.org's own published checksum, quoted. Not a value this
# platform computed over its own download, which is exactly the
# downgrade ADR-0193 refused to accept and deferred the whole feature
# rather than take. Verified twice by independent implementations: this
# project's pgpverify.c, and stock gpg against the same pinned
# fingerprint. Both produced this identical checksum.
#
# THE CONFIG IS THE RISK, NOT THE FETCH.
#
# image/kernel/qemu-part1.config was written for 6.18.x under a
# documented allnoconfig+merge procedure, and is carried here unchanged
# -- same URL, same pinned git ref, same checksum, byte for byte. A
# major version moves symbols: some are renamed, some removed, and
# config handling drops silently what it does not recognise. So a clean
# build here is NOT evidence of a bootable kernel, and this project has
# been taught that specific lesson twice by roots that assembled
# cleanly and then panicked at boot.
#
# Install this to the spare A/B slot and boot-test it. Do not make it
# the only kernel on a box until it has actually booted one.
#
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
#
# ADR-0255: this recipe declares HOW it discovers releases, and nothing
# about WHICH one to take.
#
# "kernel.org" names a discovery kind (srcupstream.h) that knows two
# things an operator should never have to restate: how to enumerate
# Linux releases, and that kernel.org publishes mainline, stable and
# longterm in parallel. Which of those a given box tracks, and how far
# back in it to sit, are operator state -- a recipe cannot know which
# line a particular machine is meant to be on. That is the same split
# ADR-0188 already established for artifact policy.
#
# Declaring this is what makes the package ROLLABLE. Without it the
# package is pinned, which is a permanent first-class answer rather
# than a gap -- and it is why "cannot resolve" is loud by construction
# here: a package that never said how it discovers releases is never
# silently left behind, because it was never trying to move.
#
pkg_upstream="kernel.org"
pkg_version="7.2.3-3"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=0ee4c282dcbdb08e2b59c93d8dd829278620e8e2"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03 f98fcc7b5d20c2991b52e53db20f7b4ff5d82e9fb7ac271ecdfe8241cd04ab36"
pkg_depends=""
#
# The tools a kernel build needs, and where this list comes from (#206).
#
# ADR-0199 composes every build environment from a recipe's declared
# tools and nothing else, so a recipe declaring none is refused before it
# starts -- this one was among the last three that could not be built
# through the ordinary path.
#
# The list is not guessed. It is exactly what recipes/image/kernel-builder
# declares, which is the image this kernel has actually been built in
# successfully; ADR-0208 gives that image the single job of building
# kernels and their modules. Deriving it from a manifest that demonstrably
# works beats reasoning about what kbuild might reach for, and if the two
# ever disagree the build says so rather than the list quietly rotting.
#
pkg_build_depends="bash bc binutils bison bzip2 coreutils diffutils elfutils findutils flex gawk gcc glibc grep gzip kmod m4 make sed tar xz zlib"
#
# And say so, rather than leaving it to whoever runs the hostbuild
# (ADR-0230): this recipe's own comments already name kernel-builder in
# three places as the environment it belongs in, which is convention
# until it is a field.
#
pkg_build_image="kernel-builder"
pkg_toolchain="gcc"
pkg_toolchain_reason="build system requires GCC internals: the Linux kernel build requires GCC and its extensions throughout"
pkg_changelog="7.2.3-3: equal-cost multipath routing (CONFIG_IP_ROUTE_MULTIPATH) and the CONFIG_IP_ADVANCED_ROUTER menu it is gated behind. Found by running BIRD on a real router container, not by reading the config: every one of the nine prefixes it learned over RIPv2 arrived from two site gateways with equal cost, so every route it tried to install was a multipath route, and the kernel refused all of them with Netlink: Invalid argument once a minute while BIRD reported 10 exported, 0 rejected. IP_ROUTE_MULTIPATH was absent from /proc/config.gz entirely rather than set to n, because a symbol behind an unselected menu does not appear at all -- the same shape as SCSI_LOWLEVEL in -20 and ADVISE_SYSCALLS in -21. Both symbols added to the olddefconfig gate. 6.18.40-24: declares its build tools and its build image, so the kernel can be rebuilt through the ordinary install path instead of being refused before it starts (#206). The tool list is kernel-builder own manifest verbatim -- the environment this kernel has actually built in -- rather than a guess at what kbuild reaches for. No change to the kernel or its config. 6.18.40-23: -22 requested CONFIG_IPC_NS and its own gate refused the build, which is the gate working. IPC_NS depends on SYSVIPC or POSIX_MQUEUE and this kernel has neither, so it can never be set -- and that also corrects the premise: there is no System V IPC and there are no POSIX message queues here, so containers were not sharing the hosts, because there is nothing to share. Nothing in this codebase calls shmget, semget, msgget or mq_open. Adding those facilities purely so a namespace could isolate them would be the enable-everything-plausible default this config avoids. Keeps CONFIG_IKCONFIG and IKCONFIG_PROC so a running host can be asked what it was actually built with, which is what made this answerable at all. 6.18.40-21: tmpfs extended attributes, KSM and per-cgroup hugetlb accounting, in one rebuild (#259, #50, #53). TMPFS_XATTR is a real defect rather than a new capability: CONFIG_TMPFS alone gives a tmpfs that cannot store an xattr, overlayfs needs trusted.overlay.* on its upperdir, and without them every overlay on a tmpfs mounts degraded -- redirect_dir off, which is what makes renaming a lower-only directory work. ADVISE_SYSCALLS accompanies KSM and is a finding of its own: it gates madvise itself, so madvise has always returned ENOSYS here, and KSMs per-region opt-in IS madvise MADV_MERGEABLE -- the same shape as SCSI_LOWLEVEL in the previous revision, a capability that cannot be selected without the thing it lives behind. HUGETLBFS and HUGETLB_PAGE accompany CGROUP_HUGETLB because the accounting knob cannot stand alone. Nothing is turned on by any of this: KSM idles until sysfs says otherwise and nr_hugepages defaults to zero. A gate now asserts each requested symbol survived olddefconfig, because merge_config warns and carries on. 6.18.40-20: a real hardware storage config (#242). This config file's own header said QEMU boot-test target only, not the real target hardware's eventual config -- and that eventual config never happened, so the config written to boot a QEMU test is the config every Cix host runs. A virtio-scsi disk attached to a real host was therefore invisible to the kernel rather than filtered by Cix, while a virtio-blk disk added at the same moment appeared instantly. Adds SCSI_LOWLEVEL, which is not a driver but the menu the low-level host adapters live behind, so without it none of the others could be selected at all and they could not be added one at a time; SCSI_VIRTIO, the reported case; and the controllers real servers ship with -- MEGARAID_SAS for Dell PERC, SCSI_MPT3SAS for LSI SAS2 and SAS3 HBAs, SCSI_SMARTPQI and SCSI_HPSA for HPE Smart Array. All built in rather than modules, because an EFI-stub kernel with no initramfs cannot load a module off a disk it cannot yet see. BLK_DEV_LOOP and the DM family are deliberately left out despite being listed on the issue: neither has any consumer in this platform and neither makes a disk visible. 6.18.40-19: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

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
	           CONFIG_IKCONFIG CONFIG_IKCONFIG_PROC \
	           CONFIG_IP_ADVANCED_ROUTER CONFIG_IP_ROUTE_MULTIPATH; do
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
