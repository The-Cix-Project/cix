pkg_name="kernel"
pkg_version="6.18.40"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz http://127.0.0.1:8901/qemu-part1.config"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 a7c0f454fecec70388ea2790bad5d8eb898d247be9854fe27a865d548faa8f7f"
pkg_depends=""

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
# ADR-0159 Phase B: an optional KANXEO_KMOD_EXTRA_SYMBOLS environment
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
	if [ -n "$KANXEO_KMOD_EXTRA_SYMBOLS" ]; then
		: > /build/extra/kmod-extra.config
		for sym in $KANXEO_KMOD_EXTRA_SYMBOLS; do
			echo "${sym}=m" >> /build/extra/kmod-extra.config
		done
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash allnoconfig
	if [ -n "$KANXEO_KMOD_EXTRA_SYMBOLS" ]; then
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
