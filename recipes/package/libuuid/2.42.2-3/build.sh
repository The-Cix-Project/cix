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
pkg_version="2.42.2-3"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/libuuid-2.42.2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf"
pkg_changelog="2.42.2-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.42.2-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-all-programs --enable-libuuid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
