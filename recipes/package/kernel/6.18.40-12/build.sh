pkg_name="kernel"
pkg_version="6.18.40-12"
pkg_source="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.40.tar.xz https://osakka:c28512d7855f083672567fdee9ba7eb0c0018890@git.home.arpa/api/v1/repos/itdlabs/thinc/raw/image/kernel/qemu-part1.config?ref=473858954a927611fed6ccccc6ec346d4aec821d"
pkg_sha256="3712fc1ec839e4daac981176c8518912e8f452650aaedfe4381da4419613a431 71bb10a4615edbfd4f1d2d65e6d8c4d78ab215280f3a26707519f31783ee1f22"
pkg_depends=""

# Re-pinned to -12: -11's own fetch/build/limits.h/objtool-link chain
# all worked correctly (fetched clean via a live-token substitution
# done BEFORE its first sync, dodging the per-version sync-cache gap
# -9/-10 hit -- see git history on this file for that full trail), and
# got all the way to a real, previously-undiscovered NEW blocker:
# `make[2]: *** [Kbuild:24: include/generated/timeconst.h] Error 127`
# (`bc: command not found` -- a genuinely missing dev-image package,
# fixed by installing the already-existing bc.recipe into dev, never
# needed before this session), immediately followed by tools/objtool's
# own final link failing on `/usr/lib/libz.so.1: undefined reference
# to '__va_start'` -- root-caused and fixed via zlib.recipe 1.3.2-3
# (TCC's own shared-library link never auto-adds its runtime helper
# archive libtcc1.a the way its executable link does, leaving zlib's
# own gzprintf()-driven va_start/va_arg calls genuinely unresolved
# inside libz.so.1 for any non-tcc-linked consumer; fixed at the
# source by statically resolving both symbols into libz.so.1 itself
# via LDFLAGS=/usr/lib/tcc/libtcc1.a at zlib's own configure time --
# see that recipe's own full trail). Both fixes are now installed into
# dev (bc 1.08.1, zlib 1.3.2-3, confirmed via nm -D that libz.so.1 no
# longer has any undefined va_ symbol) -- this re-pin is purely to
# retry the kernel build against a build_image state that now actually
# has every prerequisite objtool needs, not a further recipe-content
# change of its own.
#
# -8's own fix (wholesale-replace gcc's
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
