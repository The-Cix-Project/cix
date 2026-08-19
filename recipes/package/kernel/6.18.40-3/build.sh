pkg_name="kernel"
pkg_version="6.18.40-3"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:REPLACE_WITH_REAL_TOKEN@git.home.arpa/api/v1/repos/itdlabs/thinc/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -3 (issue #46/#47's own overnight gcc-bootstrap
# investigation, 2026-08-19): image/kernel/qemu-part1.config gains
# CONFIG_BLK_CGROUP=y + CONFIG_CGROUP_WRITEBACK=y -- found missing
# while diagnosing a real kernel Oops pair (two page-cache/xarray
# faults in filemap_get_folios_tag, hit by jbd2 and a writeback
# kworker 23 seconds apart on the identical address) that silently
# stalled an in-progress gcc 12.5.0-4 build on 192.168.15.95 for over
# 9 hours before being caught. See that commit's own message (and the
# config fragment's own new comment) for the full reasoning -- not a
# confirmed fix for the Oops itself (this kernel still has zero
# memory-debugging instrumentation compiled in, so the actual
# corruption event can't be pinpointed without a live kernel debugger
# or a diagnostic KASAN build, neither done here), but real,
# independently-justified hardening: it also fixes a separate,
# already-known gap where every pkgbuild container's own
# disk.read_bytes/write_bytes had always read 0 (no io-cgroup
# controller was ever compiled in to report through).
#
# Also switches the config-file half of pkg_source from -2's own ref
# to the exact commit that landed this change, same established
# pattern.
#
# A hostbuild recipe (ADR-0056) -- pkg_install() places its output at a
# fixed, well-known filename ($PKG_DESTDIR/bzImage) rather than merging
# into any container image's rootfs. Reproduces the exact
# allnoconfig+merge_config.sh+olddefconfig+make bzImage sequence
# image/kernel/qemu-part1.config's own header comment documents, proven
# working against this project's own real gcc-equipped "dev" image
# (Phase 33) as --build-image=.
#
# Part 3 (bare-metal-readiness plan, kernel module loading): also builds
# and harvests the real .ko module tree image/kernel/qemu-part1.config's
# own =m entries produce, and runs a real depmod over it -- this needs
# kmod.recipe already installed on --build-image= (a hard precondition,
# same shape gcc.recipe/libc-dev.recipe/etc. already have among each
# other -- see pkg/recipes/README.md for the "install this before that"
# convention). depmod's own version argument must be the freshly-built
# kernel's own `make kernelrelease` output, never the build container's
# running uname -r (a bare `depmod` with no -F/version argument defaults
# to that instead, which will not match this kernel version at all).
#
# ADR-0159 Phase B: an optional THINC_KMOD_EXTRA_SYMBOLS environment
# variable (a space-separated list of bare CONFIG_* names, set only by
# POST /v1/system/kmod-build's own daemon-side pkg_hostbuild_start()
# call -- every other caller of this same recipe, e.g. an ordinary
# `pkg hostbuild kernel`, never sets it) adds extra modules to the
# curated qemu-part1.config set for this one build, each forced to `=m`
# via a second merge_config.sh fragment -- reuses the exact same
# allnoconfig+merge_config.sh+olddefconfig sequence unmodified when
# unset, so this is strictly additive, never a second code path.
pkg_build() {
	# SHELL=/usr/bin/bash on every invocation: GNU Make ignores an
	# inherited $SHELL on Unix and always spawns /bin/sh internally for
	# each recipe line unless overridden on its own command line.
	# /bin/sh now exists on the build image (a symlink to /usr/bin/bash,
	# see bash.recipe) so this is no longer strictly required, but kept
	# explicit for clarity and to not depend on that symlink existing.
	#
	# /bin/sh existing at all is the other, harder-won requirement here:
	# the kernel's own scripts/kconfig/preprocess.c calls popen() to
	# evaluate Kconfig's $(shell ...) macros, and popen() is POSIX-
	# specified to always exec "/bin/sh" literally -- no override
	# mechanism exists, not $SHELL, not a SHELL= make variable. Without
	# it, that popen() call fails deep inside glibc's posix_spawn fast
	# path and (misleadingly) surfaces as "Cannot allocate memory"
	# instead of the expected "No such file or directory" -- confirmed
	# empirically the hard way (ADR-0056), not assumed. This project's
	# own images deliberately have no /bin otherwise (usr/bin-only FHS
	# convention), so bash.recipe now provides this one standard path
	# any real build-toolchain image needs.
	cp /build/extra/qemu-part1.config .config
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
		: > /build/extra/kmod-extra.config
		for sym in $THINC_KMOD_EXTRA_SYMBOLS; do
			echo "${sym}=m" >> /build/extra/kmod-extra.config
		done
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash allnoconfig
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config \
			/build/extra/kmod-extra.config
	else
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash olddefconfig
	# bzImage and modules built in a single make invocation, not two
	# separate ones: modpost's per-module symbol resolution needs
	# Module.symvers/vmlinux.o from the vmlinux link step still present
	# in the tree, which a second, independent `make modules` afterwards
	# does not reliably see -- confirmed empirically (modpost failed with
	# "vmlinux.o is missing" + hundreds of "undefined!" errors for
	# ordinary exported symbols like kfree/dev_set_name against a split
	# invocation; this project's own established discipline is to fix
	# the confirmed real cause, not guess).
	make ARCH=x86_64 SHELL=/usr/bin/bash -j"$(nproc)" bzImage modules
}

pkg_install() {
	cp arch/x86/boot/bzImage "$PKG_DESTDIR/bzImage"
	make ARCH=x86_64 SHELL=/usr/bin/bash INSTALL_MOD_PATH="$PKG_DESTDIR" modules_install
	depmod -b "$PKG_DESTDIR" "$(make -s ARCH=x86_64 kernelrelease)"
}
