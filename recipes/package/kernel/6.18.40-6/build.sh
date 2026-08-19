pkg_name="kernel"
pkg_version="6.18.40-6"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/thinc/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -6: -4/-5's own HOSTCFLAGS="-I/usr/include" theory
# (gcc's own fixincludes producing a broken include-fixed/limits.h
# that shadows the real header) was wrong -- the identical
# "'PATH_MAX' undeclared" error in scripts/kconfig/conf.o/confdata.o
# reproduced byte-for-byte even with that flag added, ruling out a
# header-search-order explanation entirely (a real, wrong header would
# still have been found either way; the flag would have changed which
# one won).
#
# Real cause: the kernel top Makefile's own HOSTCC default is the bare
# string "gcc", resolved via $PATH at exec time -- and CLAUDE.md's own
# already-documented environment gotcha applies exactly here: "gcc
# must be invoked by its absolute path (/usr/bin/gcc), never a bare
# gcc resolved via $PATH ... bare-name invocation makes gcc compute
# its own installation prefix as a *relative* path", which in turn
# breaks its own internal header/subprogram search (confirmed there
# via `gcc -v`, not re-verified independently here, but the symptom
# class matches exactly: a compile that runs and produces output, not
# a hard "compiler not found" failure, with a header search silently
# resolving wrong). scripts/kconfig's own two host tools are exactly
# the kind of very-early, minimal host-compile this would surface on
# first, before anything else in the build gets a chance to.
#
# Fixed by pinning HOSTCC (and CC, for the same reason, since this
# project never cross-compiles and the kernel's own default target CC
# is the identical bare "gcc") to the absolute path on every make
# invocation -- matches every other recipe in this project that shells
# out to gcc directly. HOSTCFLAGS="-I/usr/include" is dropped: it did
# not fix the real problem and keeping a flag whose own justifying
# theory was disproven would be exactly the kind of doc/code drift
# this project's own Documentation Map exists to prevent.
pkg_build() {
	cp /build/extra/qemu-part1.config .config
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
		: > /build/extra/kmod-extra.config
		for sym in $THINC_KMOD_EXTRA_SYMBOLS; do
			echo "${sym}=m" >> /build/extra/kmod-extra.config
		done
	fi
	make ARCH=x86_64 SHELL=/usr/bin/bash HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc allnoconfig
	if [ -n "$THINC_KMOD_EXTRA_SYMBOLS" ]; then
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
