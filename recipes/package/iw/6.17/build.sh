#
# iw -- the nl80211-based CLI for configuring wireless devices (join a
# network, set a card to AP/monitor/managed mode, inspect link/station
# state) -- the modern replacement for the old ioctl-based
# wireless-tools (iwconfig etc., which this project doesn't package).
# Same recipe contract as bash.recipe -- see that file's own header
# comment for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is upstream's own canonical release location, kernel.org's
# software archive (the real, first-party distribution point for this
# tool -- no 403-on-non-browser-UA workaround needed here, unlike
# bird.recipe/iputils.recipe's own upstreams). Checksum computed
# directly from the downloaded bytes (sha256sum), not taken from any
# third party.
#
pkg_name="iw"
pkg_version="6.17"
pkg_source="https://www.kernel.org/pub/software/network/iw/iw-6.17.tar.xz"
pkg_sha256="7d182e498289ab39b257da6780d562e415377107f50358ee5b55b8cfe40b1e33"
pkg_depends=""

# Hand-rolled Makefile, no configure step -- confirmed directly. Links
# against libnl-3.0/libnl-genl-3.0 via pkg-config, both already real in
# this toolchain (libnl-3-dev/libnl-genl-3-dev, confirmed present on
# this build host and therefore staged by the wholesale
# /usr/{include,lib,lib64,bin,libexec} toolchain copy -- no new extra
# needed, unlike procps.recipe's own /usr/share/gettext+aclocal gap).
# PREFIX=/usr matches every other recipe in this set.
pkg_build() {
	make -j"$(nproc)" CC=tcc PREFIX=/usr
}

# iw links against libnl-3/libnl-genl-3 -- confirmed directly via ldd
# against the real build above. Neither is part of
# pkg_seed_image_baseline()'s own global bootstrap set (ld.so/libc/
# libtinfo -- the bare minimum every built binary needs, not a full
# transitive-dependency closure for every package), so this recipe
# stages both itself, into the exact same /lib/x86_64-linux-gnu/ path
# every other runtime lib in this project already resolves from --
# copied from the isolated build container's own toolchain-provided
# copy of them (the same host libraries iw was actually linked against
# a moment ago). Both are real files behind a versioned SONAME symlink
# (confirmed via `ls -la`), so both the symlink and its real target
# are copied, the same two-file pattern ipset.recipe's own libmnl
# staging already established.
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp iw "$dir/"
	cp -a /lib/x86_64-linux-gnu/libnl-3.so.200 /lib/x86_64-linux-gnu/libnl-3.so.200.26.0 \
	   /lib/x86_64-linux-gnu/libnl-genl-3.so.200 /lib/x86_64-linux-gnu/libnl-genl-3.so.200.26.0 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
