pkg_name="kernel"
pkg_version="6.18.40-7"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:REPLACE_WITH_REAL_TOKEN@git.home.arpa/api/v1/repos/itdlabs/cix/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -7: -6's HOSTCC=/usr/bin/gcc CC=/usr/bin/gcc fix (the
# bare-"gcc"-relative-prefix gotcha CLAUDE.md already documents) also
# reproduced the identical "'PATH_MAX' undeclared" error byte for byte
# -- ruled that theory out too. Root-caused conclusively via a real,
# throwaway diagnostic build (recipes/package/probe-gcc-headers, not a
# real package, see its own commit history) that ran `gcc -H -c` (GCC's
# own header-inclusion trace) against a trivial `#include <limits.h>`
# probe, both bare and with an explicit `-I/usr/include` added:
#
#   . /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h
#   .. /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h
#
# identical in both cases -- proving `-I/usr/include` has literally
# zero effect on the search order here. This is real, standard (if
# obscure) GCC behavior: when a `-I` directory is *already* one of
# GCC's own built-in standard system directories (which `/usr/include`
# always is), GCC does not re-order it earlier -- the `-I` is
# effectively a no-op for exactly this one directory. GCC's own
# private `include-fixed/limits.h` therefore always wins the search
# for a plain `#include <limits.h>` no matter what -I flags a caller
# adds, and its own content (confirmed directly, via the same probe,
# before this one was narrowed) is the raw, never-fully-processed
# `glimits.h` template (real compiler-intrinsic limits like SCHAR_MAX,
# a conditional `#include_next <limits.h>` gated on
# `_GCC_NEXT_LIMITS_H`, which is never defined) -- GCC ships this
# self-contained "baked" only after its own post-install
# `mkheaders.sh`/fixincludes step successfully stitches it together
# with the real system limits.h; gcc.recipe's own pkg_install()
# comment already independently confirms `fixincl`/`fixinc.sh`/
# `mkheaders` were present but deliberately dropped from the shipped
# package as "used solely during gcc's own build, not afterward" --
# consistent with that step never having actually completed
# successfully at the time of `sed`'s own late arrival in this same
# cumulative pkgbuild sandbox (sed.recipe was added to fix a different,
# unrelated gap, only after gcc.recipe had already finished its own
# `make install`).
#
# The real, permanent fix belongs in gcc.recipe itself (re-run
# mkheaders.sh, now that sed/awk/coreutils are all genuinely present,
# or drop these two files from every future gcc package so
# `<limits.h>` falls straight through to the real, complete
# usr/include/limits.h libc-dev@2.36 already stages) -- deliberately
# NOT done here: gcc.recipe's own pkg_build() is a real multi-hour
# --enable-bootstrap 3-stage self-host, and forcing a full rebuild
# tonight just to fix two header files is disproportionate given the
# goal is finishing this kernel build. Filed as issue #55 instead.
#
# Scoped, documented workaround here: this build's own container
# instance never uses GCC's own fixincludes machinery for anything (no
# third-party vendor-header workarounds needed against a modern
# glibc), so removing the two broken files from *this container's*
# already-installed gcc, before compiling anything, is safe and
# unblocks the build without touching the gcc package/other consumers
# of the same "dev" image. `-f`: harmless no-op if a future gcc
# package build already fixes this and ships neither file.
pkg_build() {
	rm -f /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h \
	      /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/syslimits.h

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
