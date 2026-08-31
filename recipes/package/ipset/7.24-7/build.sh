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
pkg_version="7.24-7"
pkg_changelog="7.24-7: declares libmnl instead of copying it into the package (#206, #207). 7.24-6: strip the --version-script link flag TCC cannot honour (#206, #207)"
pkg_source="https://deb.debian.org/debian/pool/main/i/ipset/ipset_7.24.orig.tar.bz2"
pkg_sha256="fbe3424dff222c1cb5e5c34d38b64524b2217ce80226c14fdcbb13b29ea36112"
pkg_depends="libmnl"

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
	#
	# Same TCC limitation libmnl hit, and the third occurrence in this
	# catalogue after nss-pam-ldapd and zlib 1.3.2-6:
	#
	#   CCLD     libipset.la
	#   tcc: error: unsupported linker option '--version-script=../lib/libipset.map'
	#
	# Dropping it costs symbol versioning and nothing else -- same
	# soname, same exported symbols, and nothing here links a specific
	# symbol version. Edited in the generated Makefiles so upstream's
	# own tree stays unmodified, and asserted afterwards rather than
	# assumed: a sed that silently matched nothing would leave the
	# original failure to be rediscovered at link time.
	#
	sed -i 's/-Wl,--version-script[=,][^ ]*//g' lib/Makefile src/Makefile 2>/dev/null || true
	grep -rq -- '--version-script' lib/Makefile && exit 1

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
#
# -7 stops copying libmnl into the package and declares it instead.
#
# -6 built correctly against Cix's own libmnl and then shipped a COPY of
# whatever sat at /lib/x86_64-linux-gnu/libmnl.so.0 in the build
# sandbox. On this box that copy was Cix's own -- libmnl is in
# pkg_build_depends, so it is what is installed there -- so unlike iw
# and keepalived this was not shipping a Debian binary. It was still the
# wrong shape, and worth fixing rather than leaving because the copy is
# indistinguishable from the contaminated case by inspection: nothing in
# the artifact records which libmnl those bytes came from. Declaring the
# dependency makes the answer checkable instead of inferred, and means
# an image resolves one shared libmnl rather than each consumer carrying
# a private copy no upgrade can reach.
#
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir"
	cp src/ipset "$dir/"

	# Assert the linkage rather than trusting make's exit status (#113).
	echo "=== ipset DT_NEEDED ==="
	readelf -d "$dir/ipset" | grep NEEDED
	readelf -d "$dir/ipset" | grep -q 'NEEDED.*libmnl\.so\.0' || {
		echo "ipset is not linked against libmnl.so.0" >&2
		exit 1
	}
}
