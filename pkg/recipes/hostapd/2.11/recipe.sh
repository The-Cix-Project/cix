#
# hostapd -- the userspace IEEE 802.11 access point / authentication
# server daemon (turns a wireless card with AP-mode support into a
# real WiFi access point, WPA2/WPA3-Personal or -Enterprise). Same
# recipe contract as bash.recipe -- see that file's own header comment
# for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is upstream's own canonical release location, w1.fi (the
# hostap project's real, first-party distribution point -- hostapd and
# wpa_supplicant are released from the same source tree; only the
# hostapd/ subdirectory is built here). Checksum computed directly
# from the downloaded bytes (sha256sum), not taken from any third party.
#
pkg_name="hostapd"
pkg_version="2.11"
pkg_source="https://w1.fi/releases/hostapd-2.11.tar.gz"
pkg_sha256="2b3facb632fd4f65e32f4bf82a76b4b72c501f995a4f62e330219fe7aed1747a"
pkg_depends=""

# Not a ./configure-driven build -- confirmed directly: hostapd's own
# Makefile reads a hand-provided hostapd/.config file (its own
# Kconfig-lite convention, the same shape this project's own kernel
# config work already deals with, just for a userspace daemon instead
# of a kernel). Starts from upstream's own defconfig (already a real,
# complete, distro-grade baseline: CONFIG_DRIVER_NL80211 -- the modern
# driver this project's own device model needs, not the legacy
# CONFIG_DRIVER_HOSTAP wext path; CONFIG_LIBNL32; WPA2-Enterprise EAP;
# IPv6; DPP/WiFi-Easy-Connect) plus two additions confirmed to compile
# cleanly against this toolchain: CONFIG_IEEE80211W (management frame
# protection) and CONFIG_SAE (WPA3-Personal) -- both real, current
# hostapd features, not exotic ones, and a WiFi AP recipe with no
# WPA3-Personal path in 2026 would be a real, avoidable gap. Builds
# from the hostapd/ subdirectory only -- wpa_supplicant/ (the client-
# side counterpart) is a separate concern, not built here.
pkg_build() {
	cd hostapd
	cp defconfig .config
	printf 'CONFIG_IEEE80211W=y\nCONFIG_SAE=y\n' >> .config
	make -j"$(nproc)"
}

# hostapd links against libnl-3/libnl-genl-3 (CONFIG_LIBNL32's own
# nl80211 driver backend) and libssl/libcrypto (CONFIG_EAP's own TLS
# stack) -- confirmed directly via ldd against the real build above.
# None of the four are part of pkg_seed_image_baseline()'s own global
# bootstrap set (ld.so/libc/libtinfo -- the bare minimum every built
# binary needs, not a full transitive-dependency closure for every
# package), so this recipe stages all four itself, into the exact same
# /lib/x86_64-linux-gnu/ path every other runtime lib in this project
# already resolves from -- copied from the isolated build container's
# own toolchain-provided copy of them (the same host libraries hostapd
# was actually linked against a moment ago). libnl-3/libnl-genl-3 are
# real files behind a versioned SONAME symlink (confirmed via
# `ls -la`), so both the symlink and its real target are copied, the
# same two-file pattern ipset.recipe's own libmnl staging already
# established; libssl/libcrypto are plain real files, one copy each.
# hostapd's own Makefile has no usable `install` target for this
# project's purposes (no BINDIR set at all, confirmed directly) -- the
# two real binaries are copied by hand instead, the same precedent
# iputils.recipe's own manual copy already set for a from-source build
# with an install step that doesn't fit this project's DESTDIR
# convention cleanly. pkg_build() and pkg_install() run in the same
# shell session/cwd (daemon/src/pkg.c: ". recipe.sh; cd src && pkg_build
# && pkg_install"), so pkg_build()'s own `cd hostapd` is still in effect
# here -- these two paths are bare (`hostapd`/`hostapd_cli`, not
# `hostapd/hostapd`), matching that already-descended cwd. Found the
# hard way: the first version of this recipe used the top-level-relative
# `hostapd/hostapd` path here, which silently failed (no `set -e` in
# this recipe contract, so the failing cp didn't abort pkg_install()) --
# the install still reported "installed" with the two absolute-path lib
# cp's below succeeding, but usr/sbin/hostapd was never actually copied,
# caught only by chrooting in and checking the real destdir output
# against the daemon's own reported package manifest.
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir" "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp hostapd hostapd_cli "$dir/"
	cp -a /lib/x86_64-linux-gnu/libnl-3.so.200 /lib/x86_64-linux-gnu/libnl-3.so.200.26.0 \
	   /lib/x86_64-linux-gnu/libnl-genl-3.so.200 /lib/x86_64-linux-gnu/libnl-genl-3.so.200.26.0 \
	   /lib/x86_64-linux-gnu/libssl.so.3 /lib/x86_64-linux-gnu/libcrypto.so.3 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
