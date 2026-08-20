pkg_name="kernel"
pkg_version="6.18.40-8"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:REPLACE_WITH_REAL_TOKEN@git.home.arpa/api/v1/repos/itdlabs/thinc/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -8: -7's `rm -f` of gcc's broken include-fixed/limits.h
# (issue #55) overcorrected -- it does not just drop out of the search
# harmlessly, it's a *required* link in a real, mandatory chain.
# glibc's own /usr/include/limits.h (confirmed by reading it directly)
# does, unconditionally (line 122-125, not inside any #ifndef guard):
#
#   #if defined __GNUC__ && !defined _GCC_LIMITS_H_
#   # include_next <limits.h>
#   #endif
#
# -- i.e. whenever __GNUC__ is defined (real gcc, not TCC) and nothing
# has already set _GCC_LIMITS_H_, glibc's own header REQUIRES a second,
# compiler-provided limits.h to exist further down the search path and
# hard-fails the whole compile if none is found -- exactly the "no
# include path in which to search for limits.h" error -7 produced.
# PATH_MAX itself only ever comes from glibc's own later
# `#include <bits/posix1_lim.h>` (line 195, gated on __USE_POSIX,
# unrelated to any of this __GNUC__ machinery) -- but that line is
# never reached if the #include_next above it hard-fails first.
#
# Real, standard fix (this is literally what GCC's own gsyslimits.h
# template is designed to be, not a hack): replace gcc's broken,
# never-fixincludes-finished include-fixed/limits.h with a minimal,
# correct pass-through that defines _GCC_LIMITS_H_ and immediately
# #include_next's onward. When entered via GCC's own private
# include-fixed dir (first in the search order) as `#include <limits.h>`
# from user code, `#include_next <limits.h>` correctly continues the
# search to the next candidate, /usr/include/limits.h -- glibc's real,
# complete header. Control lands there with _GCC_LIMITS_H_ ALREADY
# defined, so glibc's own guarded #include_next (line 122 above) is
# skipped entirely (correct -- there's nothing further to chain to),
# and execution falls straight through to the POSIX limits section
# (bits/posix1_lim.h, real PATH_MAX) same as any ordinary, complete
# glibc installation. syslimits.h is left alone (unreached once
# limits.h itself chains directly; harmless either way).
#
# gcc.recipe's own permanent fix (issue #55) still stands as the right
# long-term home for this -- this remains a scoped, build-container-
# local workaround, not a change to the installed gcc package itself.
pkg_build() {
	cat > /usr/lib/gcc/x86_64-pc-linux-gnu/12.5.0/include-fixed/limits.h <<'EOF'
#ifndef _GCC_LIMITS_H_
#define _GCC_LIMITS_H_
#include_next <limits.h>
#endif
EOF

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
