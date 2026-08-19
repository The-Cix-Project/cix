pkg_name="kernel"
pkg_version="6.18.40-5"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:ad39fb3177e17b310aba8390e14b123ef075e6c0@git.home.arpa/api/v1/repos/itdlabs/thinc/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -5 (functionally identical to -4; -4 was re-synced with a live-substituted real fetch token that the daemon's own per-version sync immutability then silently ignored on re-sync, so this version carries the fix instead): first real hostbuild attempt against a genuinely
# gcc-equipped "dev" image (gcc 12.5.0-10, issue #32 closed) surfaced a
# real HOSTCC header-ordering bug, not a kernel/config issue. `make
# allnoconfig`'s own scripts/kconfig/conf.o HOSTCC compile failed with
# "'PATH_MAX' undeclared", even though this image's libc-dev@2.36
# stages a complete, correct /usr/include/limits.h. Root cause: GCC's
# own fixincludes machinery (which pre-processes and stages a private
# copy of select system headers under
# usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/, searched
# *before* the real /usr/include in GCC's standard include order, each
# entry normally just `#include_next`-ing straight through to the real
# system header behind it) ran as part of gcc.recipe's own bootstrap,
# at a point in that same cumulative pkgbuild sandbox's history before
# `sed` existed there (sed.recipe was only added afterward, this same
# investigation) -- fixincludes shells out to sed/awk internally to
# rewrite known-broken vendor headers, and silently produced an
# incomplete include-fixed/limits.h when it couldn't, which then
# shadows the real header instead of transparently passing through to
# it. Confirmed via the failure signature itself (a macro missing, not
# a file-not-found -- meaning some limits.h *was* found and read, just
# not glibc's real one) and by libc-dev@2.36's own file listing
# already containing a complete, correct usr/include/limits.h
# unrelated to this gap.
#
# Fixed at the kernel-build level, not by touching gcc's own installed
# files or re-bootstrapping it: HOSTCFLAGS="-I/usr/include" puts the
# real glibc header directory ahead of GCC's built-in search path
# (a plain -I flag is always searched before GCC's own standard
# system/include-fixed directories, regardless of command-line
# position) for every host-tool compile this build performs
# (scripts/kconfig/conf.c and confdata.c being the ones that actually
# hit this, but applied uniformly rather than only to the specific
# target that happened to fail first).
pkg_build() {
	cp /build/extra/qemu-part1.config .config
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
		: > /build/extra/kmod-extra.config
		for sym in $THINC_KMOD_EXTRA_SYMBOLS; do
			echo "${sym}=m" >> /build/extra/kmod-extra.config
		done
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCFLAGS="-I/usr/include" allnoconfig
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config \
			/build/extra/kmod-extra.config
	else
		bash ./scripts/kconfig/merge_config.sh -m .config /build/extra/qemu-part1.config
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCFLAGS="-I/usr/include" olddefconfig
	# bzImage and modules built in a single make invocation, not two
	# separate ones: modpost's per-module symbol resolution needs
	# Module.symvers/vmlinux.o from the vmlinux link step still present
	# in the tree, which a second, independent `make modules` afterwards
	# does not reliably see -- confirmed empirically (modpost failed with
	# "vmlinux.o is missing" + hundreds of "undefined!" errors for
	# ordinary exported symbols like kfree/dev_set_name against a split
	# invocation; this project's own established discipline is to fix
	# the confirmed real cause, not guess).
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCFLAGS="-I/usr/include" -j"$(nproc)" bzImage modules
}

pkg_install() {
	cp arch/x86/boot/bzImage "$PKG_DESTDIR/bzImage"
	make ARCH=x86_64 SHELL=/usr/bin/bash INSTALL_MOD_PATH="$PKG_DESTDIR" modules_install
	depmod -b "$PKG_DESTDIR" "$(make -s ARCH=x86_64 kernelrelease)"
}
