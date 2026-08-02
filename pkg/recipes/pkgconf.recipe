#
# pkgconf -- the modern, actively-maintained pkg-config implementation
# (the original freedesktop.org pkg-config project has had no release
# in years; pkgconf is what current real distros -- Alpine, Arch, and
# increasingly Debian -- ship instead). Needed by any real C/C++
# project's own build (most autotools/meson/cmake projects probe for
# `pkg-config` to find library CFLAGS/LDFLAGS) -- this project's own
# iproute2.recipe/keepalived.recipe/hostapd.recipe etc. all rely on the
# BUILD sandbox's own pkg-config; this recipe makes it a real,
# installable runtime package for a target image too.
#
# Source is upstream's own canonical distfiles.ariadne.space release
# point, cross-checked against GitHub Releases for the same tag --
# byte-identical, same sha256. (An earlier, stale version guess at
# pkgconf-2.3.0 was caught and discarded here: that tag doesn't exist
# upstream any more, real current latest is 3.0.5 -- confirmed via the
# GitHub releases API directly, not assumed.)
#
pkg_name="pkgconf"
pkg_version="3.0.5"
pkg_source="https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.5.tar.xz"
pkg_sha256="3acd3a8a3cce65a8d620321855d92fb602e026cbe8e13ee36bdec58483b59ace"
pkg_depends=""

# Ships both a meson.build and a plain ./configure in its release
# tarball -- autotools used here for consistency with the rest of this
# recipe set, confirmed to work directly.
pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

# pkgconf links dynamically against its own libpkgconf.so.8 (confirmed
# via ldd -- "not found" only because it isn't on this host's own
# system library path, resolves fine once staged at its real install
# location inside a target image), so both the real file and its
# SONAME symlink are kept; static archive/libtool file dropped, same
# as every other recipe's own static-lib stripping. usr/bin/pkg-config
# is added as a symlink to pkgconf -- the real, standard convention
# every distro shipping pkgconf uses, since virtually every build
# system that probes for pkg-config looks for that exact command name,
# never "pkgconf" itself. usr/share/aclocal/pkg.m4 (the PKG_CHECK_MODULES
# autoconf macro) is load-bearing runtime data for autotools-based
# consumers, kept in full. bomtool/pccritic/spdxtool (pkgconf's own
# SBOM/license-auditing CLI utilities) are small and real, kept rather
# than arbitrarily trimmed.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	ln -sf pkgconf "$PKG_DESTDIR/usr/bin/pkg-config"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib/libpkgconf.a" "$PKG_DESTDIR/usr/lib/libpkgconf.la"
}
