#
# iptables -- the legacy xtables packet filter/NAT CLI (iptables,
# ip6tables, iptables-save/restore, and the full set of match/target
# extension modules -- MASQUERADE/DNAT/SNAT/conntrack/etc., the actual
# reason a router needs this). Same recipe contract as bash.recipe --
# see that file's own header comment for the metadata-scanner-vs-
# sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their iptables package --
# the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than netfilter.org's own download page directly because
# that 403s any non-browser User-Agent, the same workaround bird.recipe's
# own header comment already documents for a different upstream.
# Checksum verified two ways: against Debian's own published .dsc
# Checksums-Sha256 field for this exact file, and independently by
# downloading it and computing sha256 directly -- both matched.
#
pkg_name="iptables"
pkg_version="1.8.13"
pkg_source="https://deb.debian.org/debian/pool/main/i/iptables/iptables_1.8.13.orig.tar.xz"
pkg_sha256="1afcd33da9e8f913ace6a2126788162e207e26f5d5e29c6573c0e581ffc58b99"
pkg_depends=""

# --disable-nftables: confirmed directly, not a corner cut -- this
# toolchain's own libnftnl (from the same apt-installed -dev package
# every other real dependency in this recipe set comes from) is 1.2.4,
# genuinely older than the >=1.2.6 this release's own configure.ac
# requires for nftables-compat support. The legacy xtables backend
# built here (iptables/ip6tables/*-save/*-restore, the multi-call
# xtables-legacy-multi binary, and every match/target extension module)
# is the traditional, still fully functional and widely deployed mode
# -- not a degraded build, a different real one. --enable-shared:
# this release's own default; keeps libxtables/libip4tc/libip6tc as
# real shared libraries the extension modules under
# /usr/lib/xtables/*.so (loaded at runtime by rule type, e.g. -j
# MASQUERADE) link against, confirmed via ldd to need nothing external
# beyond libc once built this way.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --enable-shared --disable-nftables
	make -j"$(nproc)"
}

# `make install` DESTDIR-installs the full real runtime footprint in
# one step -- the multi-call binary and every iptables/ip6tables/
# *-save/*-restore symlink pointing at it, iptables' own three shared
# libraries (with their real versioned names and .so/.soN symlinks,
# libtool's own doing), and the complete /usr/lib/xtables/ extension
# directory. Every match/target module under /usr/lib/xtables/ is a
# real, on-demand-loaded runtime dependency for the specific rule
# types that need it (conntrack, MASQUERADE, DNAT/SNAT, ...), not
# optional -- confirmed via ldd against the real build above that none
# of it needs anything external beyond libc (legacy mode needs neither
# libmnl nor libnfnetlink at runtime). What's pruned afterward
# (headers, .la/.a files, pkgconfig, man pages, the xslt doc helper)
# is real `make install` output too, just not runtime-needed -- this
# is a container image, not a dev environment, the same boundary every
# other recipe in this set already keeps.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/xtables" "$PKG_DESTDIR/usr/lib/pkgconfig"
	rm -f "$PKG_DESTDIR"/usr/lib/*.la
}
