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
pkg_version="2.42.2-4"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"

# No pkg_artifact_sha256 yet. This revision changes where the library
# is installed, so its bytes are not the previous revision's bytes; a
# checksum is added only once a real Cix host has built and published
# these exact ones (ADR-0122, #325).
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf"
pkg_changelog="2.42.2-4: install the runtime library to /usr/lib, the one library directory (#184). util-linux splits its own output when --prefix=/usr is given and --libdir is not: the real libuuid.so.1 goes to /lib, historically so early boot could reach it before /usr was mounted, while the development symlink, the static archive and the pkgconfig file go to /usr/lib. This platform has one library directory and the loader does not search a bare /lib, so the .so.1 was installed where nothing looks for it. Every consumer that linked it was fine until one actually ran: ldapsearch on the jump host died with \"error while loading shared libraries: libuuid.so.1\" and exit 127, which presented as sshd finding no LDAP keys for a user rather than as a missing library. --libdir=/usr/lib is explicit rather than inherited, so the split cannot come back on a prefix change. No source change. 2.42.2-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.42.2-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --libdir=/usr/lib --disable-all-programs --enable-libuuid
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/lib"/*.la
}
