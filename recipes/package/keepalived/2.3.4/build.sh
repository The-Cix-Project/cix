#
# keepalived -- VRRP (a shared "floating" IP a router pair fails over
# between them) plus LVS/IPVS load-balancing health checking. Same
# recipe contract as bash.recipe -- see that file's own header comment
# for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.gz" for their keepalived package
# -- the exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than github.com/acassen/keepalived's own release page
# directly for consistency with the rest of this recipe set (several
# of whose real upstreams 403 non-browser User-Agents; Debian's own
# mirror sidesteps needing to check that project by project). Checksum
# verified two ways: against Debian's own published .dsc
# Checksums-Sha256 field for this exact file, and independently by
# downloading it and computing sha256 directly -- both matched.
#
pkg_name="keepalived"
pkg_version="2.3.4"
pkg_source="https://deb.debian.org/debian/pool/main/k/keepalived/keepalived_2.3.4.orig.tar.gz"
pkg_sha256="6afd95ddb7d3e0d3b8b8e5b3a489144131b61a01b06d29e883d0c44acc8a36bf"
pkg_depends=""

# No flags needed -- confirmed directly, this release's own configure
# auto-detects every optional integration (OpenSSL for VRRP auth/
# checksums, libnl-3 for accurate netlink interface tracking, libiptc/
# libnftnl/libmnl for its own iptables/nftables integration, kernel
# IPVS headers for LVS) from what this toolchain's own already-real
# -dev packages provide, the same "probe, degrade gracefully if
# genuinely absent" pattern iproute2.recipe's own header comment
# already documents -- every one of them was actually found here, so
# this is a real, full-featured build, not a stripped one.
pkg_build() {
	CC=tcc ./configure --prefix=/usr
	make -j"$(nproc)"
}

# The binary plus its full real runtime footprint, confirmed via ldd
# against the real build above: OpenSSL (VRRP authentication/
# checksums), libnl-3/libnl-genl-3 (netlink interface tracking),
# libip4tc/libip6tc (its own iptables integration -- the same two
# libraries iptables.recipe stages too; harmless, ABI-compatible
# overlap if both land in the same image, and this recipe stays
# correct standing alone if iptables.recipe isn't installed), libnftnl
# + libmnl (its own nftables integration). libipset is dlopen()'d at
# runtime, not linked (confirmed: absent from ldd's own output despite
# LIBIPSET_DYNAMIC showing up in keepalived --version's config list --
# graceful if ipset.recipe isn't installed into the same image, real
# if it is). Sample configs/man pages/systemd units from `make
# install`'s own full output are deliberately not copied -- this is a
# container image, not a dev environment, the same boundary every
# other recipe in this set already keeps.
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp keepalived/keepalived "$dir/"
	cp -a /lib/x86_64-linux-gnu/libssl.so.3 /lib/x86_64-linux-gnu/libcrypto.so.3 \
	   /lib/x86_64-linux-gnu/libnl-3.so.200 /lib/x86_64-linux-gnu/libnl-3.so.200.26.0 \
	   /lib/x86_64-linux-gnu/libnl-genl-3.so.200 /lib/x86_64-linux-gnu/libnl-genl-3.so.200.26.0 \
	   /lib/x86_64-linux-gnu/libip4tc.so.2 /lib/x86_64-linux-gnu/libip4tc.so.2.0.0 \
	   /lib/x86_64-linux-gnu/libip6tc.so.2 /lib/x86_64-linux-gnu/libip6tc.so.2.0.0 \
	   /lib/x86_64-linux-gnu/libnftnl.so.11 /lib/x86_64-linux-gnu/libnftnl.so.11.6.0 \
	   /lib/x86_64-linux-gnu/libmnl.so.0 /lib/x86_64-linux-gnu/libmnl.so.0.2.0 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
