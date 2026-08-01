#
# iproute2 -- ip/ss/tc and the rest of the standard Linux networking
# toolset. Same recipe contract as bash.recipe -- see that file's own
# header comment for the metadata-scanner-vs-sourced-shell-script split.
#
# Source verified against kernel.org's own published sha256sums.asc
# for this exact release.
#
pkg_name="iproute2"
pkg_version="6.18.0"
pkg_source="https://www.kernel.org/pub/linux/utils/net/iproute2/iproute2-6.18.0.tar.xz"
pkg_sha256="6ba520e1975e4c50dc931eeae91ea37c198b8a173744885f8895b84325f9d456"
pkg_depends=""

# iproute2's own ./configure is a plain shell script (not autoconf) --
# it probes the pkgbuild toolchain image's headers/libraries (kernel
# uapi headers, optionally libmnl/libelf via pkg-config if present) and
# writes the Config file the top-level Makefile requires; it degrades
# gracefully (smaller feature set, not a build failure) when an
# optional dependency isn't found, exactly like every other
# --without-*-by-default open-source build in this recipe set.
pkg_build() {
	./configure
	make -j"$(nproc)"
}

# Once the toolchain actually had libmnl/libelf/libcap-dev available
# (added for ipset/iptables/keepalived, ADR-0036's own sibling
# packages), `ip`'s own optional feature detection picked them up too
# -- confirmed via ldd against a real build: libelf (BPF program
# loading), libmnl, libcap, and libz (a transitive dep of libelf's own
# compressed-debug-info support), none of which pkg_seed_image_
# runtime()'s global bootstrap set covers. Staged the same way every
# other recipe in this set stages its own real extra runtime deps.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libelf.so.1 /lib/x86_64-linux-gnu/libelf-0.188.so \
	   /lib/x86_64-linux-gnu/libmnl.so.0 /lib/x86_64-linux-gnu/libmnl.so.0.2.0 \
	   /lib/x86_64-linux-gnu/libcap.so.2 /lib/x86_64-linux-gnu/libcap.so.2.66 \
	   /lib/x86_64-linux-gnu/libz.so.1 /lib/x86_64-linux-gnu/libz.so.1.2.13 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
