#
# 7.2.3-8: the regulatory database is built INTO the kernel (#342).
#
# cfg80211 is =y and asks for regulatory.db during its own init, which
# on this platform is before any root filesystem exists. Measured on a
# real boot:
#
#   faux_driver regulatory: Direct firmware load for regulatory.db failed with error -2
#   cfg80211: failed to load regulatory.db
#
# -2 is ENOENT and the file is genuinely present -- rtw88's own blob
# loads from the same /lib/firmware seconds later on the same boot. The
# difference is only WHEN each is asked for. So this was never a
# staging bug and ADR-0263's firmware image was never at fault; a file
# on disk cannot answer a request made before there is a disk.
#
# CONFIG_EXTRA_FIRMWARE copies both the database and its detached
# signature into the kernel image at build time. Both are required:
# CFG80211_REQUIRE_SIGNED_REGDB is =y and, as 7.2.3-7 established the
# hard way, cannot be turned off from .config at all.
#
# The one thing this revision needs that the recipe does not say:
# wireless-regdb must exist in the BUILD IMAGE, at
# CONFIG_EXTRA_FIRMWARE_DIR. It is in kernel-builder 1.5.0 rather than
# in pkg_build_depends below, because a host build's sandbox is the
# build image's rootfs and build_depends composes nothing there
# (ADR-0199) -- the same reason openssl and perl live there.
#
#
# 7.2.3-7: satisfy the regulatory-database signature check instead of
# trying to switch it off.
#
# 7.2.3-6 tried to disable CFG80211_REQUIRE_SIGNED_REGDB and failed in
# exactly the same place, because that config line did nothing.
# Measured with the real config procedure (probe-wifi-driver/4), the
# symbol came out =y regardless: its prompt is conditional on
# CFG80211_CERTIFICATION_ONUS, and a symbol with no active prompt
# cannot be set from .config -- olddefconfig forces it back to its
# `default y`. The same trap as a symbol behind an unselected menu,
# inverted.
#
# The same probe also showed it is the ONLY enabled selector of
# SYSTEM_DATA_VERIFICATION, so certs/ exists in this kernel purely to
# verify the regulatory database -- which is a thing worth verifying,
# and cheap to satisfy: openssl joins the build dependencies below
# (certs/extract-cert is a HOST tool wanting openssl/bio.h, and this
# platform's openssl package installs headers), and wireless-regdb
# ships upstream's regulatory.db.p7s beside the database it rebuilds.
#
# 7.2.3-6: 7.2.3-5's wireless config was right and its build was not.
#
# The olddefconfig gate confirmed all ten wireless symbols =y,
# RTW88_8822BU included, and then the build died in a HOST tool:
# certs/extract-cert.c could not find openssl/bio.h. certs/ had never
# been built here before.
#
# Measured, not inferred (probe-wifi-driver/3): net/wireless/Kconfig:92
# is `select SYSTEM_DATA_VERIFICATION`, sitting under
# `config CFG80211_REQUIRE_SIGNED_REGDB` at line 89, which defaults to
# y -- so turning on cfg80211 turned on the kernel's certificate
# machinery. This revision turns that one option off, which removes
# certs/ entirely rather than teaching the build image to satisfy it.
# cfg80211 still loads regulatory.db via request_firmware(); it just no
# longer demands regulatory.db.p7s beside it. No change to any wireless
# symbol.
#
# 7.2.3-5: wireless, so this platform can run an access point (#30).
#
# The site's USB 802.11ac adapter reported `driver: usb` -- the generic
# USB core had claimed it because this kernel had no wireless support
# of any kind. Which driver actually wants it was measured rather than
# recalled, because vendor 0x2357 covers several unrelated Realtek
# parts and a wrong guess costs a full rebuild of this package:
# rtw8822bu.c:56 carries USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e),
# so it is an RTL8822BU on rtw88 (recipes/package/probe-wifi-driver/1).
#
# RTW88_8822BU's own selects -- RTW88_CORE, RTW88_USB, RTW88_8822B --
# were read verbatim from the tree rather than assumed
# (probe-wifi-driver/2) and are asserted individually by the gate
# below, along with the two menu gates CONFIG_WIRELESS and CONFIG_WLAN.
# Those two matter: this config has twice been bitten by a symbol
# behind an unselected menu simply not appearing, rather than appearing
# as =n.
#
# Firmware is NOT solved by this revision and the distinction is worth
# stating plainly: rtw8822b.c:2614 declares exactly one blob,
# rtw88/rtw8822b_fw.bin, which cannot be built from source by anyone.
# This kernel can drive the radio; whether the radio comes up depends
# on a Build Provenance decision the owner has not yet made.
#
# 7.2.3-4: FUSE (#336), and the config gate finally points at the
# drivers that decide whether the box boots (#319).
#
# CONFIG_FUSE_FS is what a virtualised /proc needs. A process inside a
# container reads /proc/meminfo and sees the host's memory, which is
# not a cosmetic problem here: #278/#279 was a build sizing itself by
# host memory against a 2 GiB cgroup ceiling it could not see, and
# nproc, free and any JVM picking a heap all read the same files. The
# server itself is host-side and is not in this recipe; this is the
# kernel support it cannot exist without.
#
# The gate change is the by-product and it closes a real hole. It
# asserted the eight capabilities the revision that added it existed
# for and not one of VIRTIO_NET, VIRTIO_BLK, VIRTIO_PCI, SCSI_VIRTIO,
# SATA_AHCI, the four HBA drivers or DEVTMPFS -- so a kernel that could
# not see its own NIC would have passed every assertion and shipped. A
# host like that boots, is unreachable, never panics, and therefore
# never rolls back; the outage ends with a person at the machine. All
# thirteen are already =y in the fragment, so this asserts what is
# already true and keeps it true across the next version bump.
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
pkg_version="7.2.3-8"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.3.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=cf9a560d94fdbc96191b183b07668c8cd4d66934"
pkg_sha256="8ba259e8e7b13ec6ef0941c8a39ad90b24bd4a4d6c0010ba6bafb794550ecd03 ccf06f2f9c94fc89e081d0298f6a98dae81cf11140ea6b14d37a39b627789a20"
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
pkg_build_depends="bash bc binutils bison bzip2 coreutils diffutils elfutils findutils flex gawk gcc glibc grep gzip kmod m4 make openssl sed tar xz zlib"
#
# And say so, rather than leaving it to whoever runs the hostbuild
# (ADR-0230): this recipe's own comments already name kernel-builder in
# three places as the environment it belongs in, which is convention
# until it is a field.
#
pkg_build_image="kernel-builder"
pkg_toolchain="gcc"
pkg_toolchain_reason="build system requires GCC internals: the Linux kernel build requires GCC and its extensions throughout"
pkg_changelog="7.2.3-8: builds the regulatory database into the kernel with CONFIG_EXTRA_FIRMWARE (#342). cfg80211 is =y and requests regulatory.db during its own init, before any root filesystem is mounted -- measured on a real boot as \"Direct firmware load for regulatory.db failed with error -2\" while rtw88's own blob loaded successfully from the same /lib/firmware seconds later, because a USB probe happens after root is up. So the file was always present and staging could never have fixed it; only the timing differs. Without the database cfg80211 falls back to the built-in world domain, which makes any country code fail outright (hostapd reached COUNTRY_UPDATE then \"Failed to set country code\" and refused to start) and marks every 5 GHz band NO-IR -- what an AP does -- confining the platform access point to 2.4 GHz. Both the database and its detached signature are embedded, because CFG80211_REQUIRE_SIGNED_REGDB is =y and 7.2.3-7 established it cannot be turned off. wireless-regdb is added to kernel-builder 1.5.0 rather than to pkg_build_depends, because a host build sandbox is the build image rootfs and build_depends composes nothing there (ADR-0199). The gate gains exact-line assertions for the two string symbols and a presence check for the two files, since a database silently not built in presents as a working kernel. No wireless symbol changed. 7.2.3-7: adds openssl to the build dependencies so certs/extract-cert can build, after 7.2.3-6 tried to disable the regulatory-database signature check and failed identically. That config line did nothing: measured with the real config procedure, CFG80211_REQUIRE_SIGNED_REGDB came out =y regardless, because its prompt is conditional on CFG80211_CERTIFICATION_ONUS and a symbol with no active prompt cannot be set from .config -- olddefconfig restores its default y. The same measurement showed it is the only enabled selector of SYSTEM_DATA_VERIFICATION, so certs/ is here purely to verify the regulatory database, which is worth verifying: wireless-regdb ships upstream's regulatory.db.p7s beside the database it rebuilds byte-for-byte, so that signature validates over this platform's own build. No wireless symbol changed. 7.2.3-6: fixes the 7.2.3-5 build, whose wireless config was already correct -- the gate confirmed all ten symbols =y and the build then failed in a host tool, certs/extract-cert.c unable to find openssl/bio.h. Measured rather than inferred: net/wireless/Kconfig:92 selects SYSTEM_DATA_VERIFICATION under CFG80211_REQUIRE_SIGNED_REGDB, which defaults to y, so enabling cfg80211 enabled the kernel certificate machinery. Turning that one option off removes certs/ entirely instead of teaching the build image to satisfy a signature check over a database this platform does not ship; cfg80211 still loads regulatory.db through request_firmware and simply stops demanding regulatory.db.p7s. No wireless symbol changed. 7.2.3-5: wireless, so the platform can run an access point (#30). The site USB 802.11ac adapter reported driver 'usb' because this kernel had no wireless support at all -- no CFG80211, no MAC80211, no WLAN. Which driver claims it was measured rather than recalled, since vendor 0x2357 covers several unrelated Realtek parts and a wrong guess costs a full rebuild: rtw8822bu.c:56 carries USB_DEVICE_AND_INTERFACE_INFO(0x2357, 0x012e), so it is an RTL8822BU on rtw88. RTW88_8822BU selects RTW88_CORE, RTW88_USB and RTW88_8822B, read verbatim from the tree and each asserted by the olddefconfig gate; CONFIG_WIRELESS and CONFIG_WLAN are the menu gates, included because a symbol behind an unselected menu does not appear as =n, it does not appear at all -- the same shape as SCSI_LOWLEVEL in -20 and IP_ADVANCED_ROUTER in 7.2.3-3. Firmware is deliberately not addressed: rtw8822b.c:2614 declares exactly one blob, rtw88/rtw8822b_fw.bin, which nobody can build from source, and shipping it is a Build Provenance decision. This kernel can drive the radio; bringing it up needs that file. 7.2.3-4: CONFIG_FUSE_FS, and the olddefconfig gate extended to the thirteen symbols that decide whether a host boots and can be reached (#336, #319). FUSE is the kernel support a virtualised /proc needs -- a process in a container reads /proc/meminfo and sees the host, which cost this platform a real incident in #278/#279 where a build sized itself by host memory against a cgroup ceiling it could not see. The server that uses it is host-side and is not in this recipe. The gate change is the by-product and closes a hole that mattered more than the feature: the gate asserted the eight symbols the revision that introduced it cared about and none of VIRTIO_NET, VIRTIO_BLK, VIRTIO_PCI, SCSI_VIRTIO, SATA_AHCI, the four HBA drivers or DEVTMPFS, so a kernel that could not see its own network card would have passed every assertion and shipped. Such a host boots, is unreachable, and never panics -- so panic=10 never fires, the boot-assessment counter never drains, systemd-boot never rolls back, and recovery is a person at a shell-less machine. All thirteen are already =y in the fragment, so this asserts what is already true and keeps it true across the next version bump. 7.2.3-3: equal-cost multipath routing (CONFIG_IP_ROUTE_MULTIPATH) and the CONFIG_IP_ADVANCED_ROUTER menu it is gated behind. Found by running BIRD on a real router container, not by reading the config: every one of the nine prefixes it learned over RIPv2 arrived from two site gateways with equal cost, so every route it tried to install was a multipath route, and the kernel refused all of them with Netlink: Invalid argument once a minute while BIRD reported 10 exported, 0 rejected. IP_ROUTE_MULTIPATH was absent from /proc/config.gz entirely rather than set to n, because a symbol behind an unselected menu does not appear at all -- the same shape as SCSI_LOWLEVEL in -20 and ADVISE_SYSCALLS in -21. Both symbols added to the olddefconfig gate. 6.18.40-24: declares its build tools and its build image, so the kernel can be rebuilt through the ordinary install path instead of being refused before it starts (#206). The tool list is kernel-builder own manifest verbatim -- the environment this kernel has actually built in -- rather than a guess at what kbuild reaches for. No change to the kernel or its config. 6.18.40-23: -22 requested CONFIG_IPC_NS and its own gate refused the build, which is the gate working. IPC_NS depends on SYSVIPC or POSIX_MQUEUE and this kernel has neither, so it can never be set -- and that also corrects the premise: there is no System V IPC and there are no POSIX message queues here, so containers were not sharing the hosts, because there is nothing to share. Nothing in this codebase calls shmget, semget, msgget or mq_open. Adding those facilities purely so a namespace could isolate them would be the enable-everything-plausible default this config avoids. Keeps CONFIG_IKCONFIG and IKCONFIG_PROC so a running host can be asked what it was actually built with, which is what made this answerable at all. 6.18.40-21: tmpfs extended attributes, KSM and per-cgroup hugetlb accounting, in one rebuild (#259, #50, #53). TMPFS_XATTR is a real defect rather than a new capability: CONFIG_TMPFS alone gives a tmpfs that cannot store an xattr, overlayfs needs trusted.overlay.* on its upperdir, and without them every overlay on a tmpfs mounts degraded -- redirect_dir off, which is what makes renaming a lower-only directory work. ADVISE_SYSCALLS accompanies KSM and is a finding of its own: it gates madvise itself, so madvise has always returned ENOSYS here, and KSMs per-region opt-in IS madvise MADV_MERGEABLE -- the same shape as SCSI_LOWLEVEL in the previous revision, a capability that cannot be selected without the thing it lives behind. HUGETLBFS and HUGETLB_PAGE accompany CGROUP_HUGETLB because the accounting knob cannot stand alone. Nothing is turned on by any of this: KSM idles until sysfs says otherwise and nr_hugepages defaults to zero. A gate now asserts each requested symbol survived olddefconfig, because merge_config warns and carries on. 6.18.40-20: a real hardware storage config (#242). This config file's own header said QEMU boot-test target only, not the real target hardware's eventual config -- and that eventual config never happened, so the config written to boot a QEMU test is the config every Cix host runs. A virtio-scsi disk attached to a real host was therefore invisible to the kernel rather than filtered by Cix, while a virtio-blk disk added at the same moment appeared instantly. Adds SCSI_LOWLEVEL, which is not a driver but the menu the low-level host adapters live behind, so without it none of the others could be selected at all and they could not be added one at a time; SCSI_VIRTIO, the reported case; and the controllers real servers ship with -- MEGARAID_SAS for Dell PERC, SCSI_MPT3SAS for LSI SAS2 and SAS3 HBAs, SCSI_SMARTPQI and SCSI_HPSA for HPE Smart Array. All built in rather than modules, because an EFI-stub kernel with no initramfs cannot load a module off a disk it cannot yet see. BLK_DEV_LOOP and the DM family are deliberately left out despite being listed on the issue: neither has any consumer in this platform and neither makes a disk visible. 6.18.40-19: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

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
	# The boot-critical set is here for #319, and its argument is
	# stronger than the one above: a missing CONFIG_KSM gives a host
	# without page merging, while a missing CONFIG_VIRTIO_NET gives a
	# host that boots and is unreachable. It does not panic, so panic=10
	# never fires, the Automatic Boot Assessment counter never drains,
	# and systemd-boot never falls back -- recovery is a human at a
	# shell-less box. This matters most on exactly the kind of bump this
	# revision's lineage just made (6.18 -> 7.2), where a symbol can be
	# renamed or moved behind a new menu and the build still succeeds.
	# The check is ^SYM=y$, so a driver demoted to =m reads as absent,
	# which is the right answer for a kernel with no initramfs.
	for sym in CONFIG_TMPFS_XATTR CONFIG_KSM CONFIG_ADVISE_SYSCALLS \
	           CONFIG_HUGETLBFS CONFIG_HUGETLB_PAGE CONFIG_CGROUP_HUGETLB \
	           CONFIG_IKCONFIG CONFIG_IKCONFIG_PROC \
	           CONFIG_IP_ADVANCED_ROUTER CONFIG_IP_ROUTE_MULTIPATH \
	           CONFIG_FUSE_FS \
	           CONFIG_VIRTIO_NET CONFIG_VIRTIO_BLK CONFIG_VIRTIO_PCI \
	           CONFIG_SCSI_VIRTIO CONFIG_SATA_AHCI CONFIG_SCSI_LOWLEVEL \
	           CONFIG_MEGARAID_SAS CONFIG_SCSI_MPT3SAS CONFIG_SCSI_SMARTPQI \
	           CONFIG_SCSI_HPSA CONFIG_DEVTMPFS CONFIG_DEVTMPFS_MOUNT \
	           CONFIG_WIRELESS CONFIG_CFG80211 CONFIG_MAC80211 CONFIG_WLAN \
	           CONFIG_WLAN_VENDOR_REALTEK CONFIG_RTW88 CONFIG_RTW88_CORE \
	           CONFIG_RTW88_USB CONFIG_RTW88_8822B CONFIG_RTW88_8822BU; do
		if ! grep -q "^${sym}=y$" .config; then
			echo "kernel: ${sym} did not survive olddefconfig -- requested and not set" >&2
			echo "  what .config actually has:" >&2
			grep -E "^(# )?${sym}" .config >&2 || echo "    (nothing at all)" >&2
			exit 1
		fi
		echo "  ${sym}=y confirmed in .config"
	done
	# The two string symbols, which the loop above cannot check: its
	# test is ^SYM=y$ and these carry values. Asserted by exact line,
	# because a database that is silently NOT built in is precisely the
	# failure this revision exists to fix, and it presents as a working
	# kernel that quietly falls back to the world regulatory domain --
	# no build error, no boot error beyond one line, just an access
	# point that cannot be given a country.
	for line in 'CONFIG_EXTRA_FIRMWARE="regulatory.db regulatory.db.p7s"' \
	            'CONFIG_EXTRA_FIRMWARE_DIR="/lib/firmware"'; do
		if ! grep -qxF "$line" .config; then
			echo "kernel: ${line} did not survive olddefconfig" >&2
			echo "  what .config actually has:" >&2
			grep -E "^(# )?CONFIG_EXTRA_FIRMWARE" .config >&2 || echo "    (nothing at all)" >&2
			exit 1
		fi
		echo "  ${line} confirmed in .config"
	done
	# And the files themselves must be where the config says, or kbuild
	# fails late with a bare "No rule to make target". Checked here so
	# the message names the real problem: the build image is missing
	# wireless-regdb.
	for f in /lib/firmware/regulatory.db /lib/firmware/regulatory.db.p7s; do
		if [ ! -f "$f" ]; then
			echo "kernel: ${f} is absent -- the build image needs wireless-regdb" >&2
			exit 1
		fi
		echo "  ${f} present ($(wc -c < "$f") bytes)"
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
