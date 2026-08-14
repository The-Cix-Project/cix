#
# libuuid -- RFC 4122 UUID generation library, part of the util-linux
# source tree but built standalone here via util-linux's own real,
# documented `--disable-all-programs --enable-libuuid` convention
# (confirmed directly against the real util-linux 2.42.2 configure.ac).
# A real, confirmed build-time dependency of sbsigntools (Part 5,
# bare-metal-readiness plan) -- sbsigntools' own configure.ac checks
# for `uuid >= ...` via pkg-config, used by sbvarsign/sbsiglist/
# sbkeysync for generating/parsing UEFI variable GUIDs.
#
pkg_name="libuuid"
pkg_version="2.42.2"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_depends=""

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-all-programs --enable-libuuid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
