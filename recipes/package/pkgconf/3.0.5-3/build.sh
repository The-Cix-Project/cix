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
pkg_version="3.0.5-3"
pkg_source="https://distfiles.ariadne.space/pkgconf/pkgconf-3.0.5.tar.xz"
pkg_sha256="3acd3a8a3cce65a8d620321855d92fb602e026cbe8e13ee36bdec58483b59ace"
# Nothing at runtime: pkgconf links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from exactly
# these and nothing else, so this list is not documentation -- it is
# the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with CC=tcc
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       an autoconf configure rewrites its own output with sed on
#             essentially every substitution it makes
#   grep      the same, for every feature test that greps a compiler or
#             header for a pattern
#   gawk      config.status generates every Makefile through awk --
#             measured, not assumed: a build environment without it
#             fails at `config.status: line NNNN: awk: command not
#             found` (see diffutils 3.10-3)
#   binutils  the static libpkgconf archive the tool links against
#   findutils configure probes `xargs -n`, and fails the check without it
#   diffutils configure's "a working dd" test compares with `cmp`
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils findutils diffutils"


# Ships both a meson.build and a plain ./configure in its release
# tarball -- autotools used here for consistency with the rest of this
# recipe set, confirmed to work directly.
# --disable-shared: libtool builds libpkgconf.so with `-version-script`,
# which TCC rejects outright ("unsupported linker option"). That flag
# exists to control which symbols a SHARED library exports, and this
# package does not ship one to control -- pkg_install() below already
# deletes the headers and the static archive, so what is installed is
# the pkgconf tool and the pkg-config symlink to it. Nothing in this
# catalog links against libpkgconf.
#
# So this is not a workaround for the missing flag; it is declining to
# build a shared library nobody consumes, and the flag stops being
# relevant as a consequence.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-shared
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
