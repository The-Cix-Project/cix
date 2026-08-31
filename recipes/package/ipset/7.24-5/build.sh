#
# ipset -- userspace CLI for the kernel's IP set framework (used by
# iptables/nftables match extensions, and directly by keepalived's own
# optional ipset integration). Same recipe contract as bash.recipe --
# see that file's own header comment for the metadata-scanner-vs-
# sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.bz2" for their ipset package --
# the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than ipset.netfilter.org/netfilter.org's own download
# page directly because that 403s any non-browser User-Agent, the same
# workaround bird.recipe's own header comment already documents for a
# different upstream. Checksum verified two ways: against Debian's own
# published .dsc Checksums-Sha256 field for this exact file, and
# independently by downloading it and computing sha256 directly --
# both matched.
#
pkg_name="ipset"
pkg_version="7.24-5"
pkg_changelog="7.24-5: point PKG_CONFIG_PATH at where Cix packages put their .pc files (#206, #207)"
pkg_source="https://deb.debian.org/debian/pool/main/i/ipset/ipset_7.24.orig.tar.bz2"
pkg_sha256="fbe3424dff222c1cb5e5c34d38b64524b2217ce80226c14fdcbb13b29ea36112"
pkg_depends=""

# pkg_build_depends (#206). Every entry past the baseline was named by a
# real build rather than guessed:
#   configure: error: The pkg-config script could not be found -> pkgconf
#   line 7431: cmp: command not found                          -> diffutils
#   Package requirements (libmnl >= 1) were not met             -> libmnl
# The last one could not be satisfied at all until libmnl was packaged
# (#207) -- it is why this recipe had been unbuildable rather than
# merely undeclared.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf libmnl"

# --with-kmod=no: ipset's own real source also ships a kernel module
# (default "yes"), needing a real configured kernel source/build tree
# this project doesn't build against -- the kernel's own IP_SET config
# is a separate, host-kernel-config concern, the same split this
# project already keeps for iptables (netfilter kernel modules are
# never something a pkg recipe builds). Only the userspace CLI matters
# here. --prefix=/usr matches every other recipe in this set. Confirmed
# directly via configure.ac: libmnl (already staged into this
# toolchain, see docs/adr/0036's own sibling reasoning for why C
# library deps live in the shared toolchain, not per-recipe) is
# unconditionally required, no graceful degradation -- unlike
# iproute2's own optional libmnl use.
pkg_build() {
	#
	# PKG_CONFIG_PATH is set explicitly because this is the first
	# recipe here to consume another Cix package's .pc file, and the
	# default search path turned out not to be enough on its own: with
	# libmnl installed in the environment and its libmnl.pc present,
	# configure still reported `checking for libmnl >= 1... no`.
	#
	# Both directories are named rather than one: libuuid and libmnl put
	# their .pc under usr/lib/pkgconfig, while a package installing with
	# a multiarch libdir lands in lib/x86_64-linux-gnu/pkgconfig, and a
	# consumer should not have to know which convention its dependency
	# happened to follow.
	#
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	CC=tcc ./configure --prefix=/usr --with-kmod=no
	make -j"$(nproc)"
}

# Only the real, runtime-needed pieces -- the CLI binary itself and
# its one real extra shared-library dependency, libmnl (confirmed via
# ldd; ipset's own libipset gets linked into the binary directly, no
# separate libipset.so ends up in its ldd output). Headers/static lib/
# pkgconfig/man pages from `make install`'s own full output are
# deliberately not copied -- this is a container image, not a dev
# environment, the same boundary every other recipe in this set
# already keeps.
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp src/ipset "$dir/"
	cp -a /lib/x86_64-linux-gnu/libmnl.so.0 /lib/x86_64-linux-gnu/libmnl.so.0.2.0 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
