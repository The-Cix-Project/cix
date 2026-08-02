#
# procps -- ps/top/free/kill/pgrep/pkill/pidof/pidwait/pmap/pwdx/
# slabtop/tload/uptime/vmstat/w/watch/hugetop, plus sysctl (also
# available via iproute2's own sysctl-adjacent tooling, but this is
# the canonical one) -- the standard Linux process/system-inspection
# toolset. Same recipe contract as bash.recipe -- see that file's own
# header comment for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is upstream procps-ng's own GitLab release tag archive (real,
# reproducible git-archive snapshot of the tagged release, the same
# kind of source distros already build straight from -- no separate
# "orig tarball" repackaging step exists for this project the way
# Debian's own packaging does for e.g. ipset/iputils). Checksum
# computed directly from the downloaded bytes (sha256sum), not taken
# from any third party.
#
pkg_name="procps"
pkg_version="4.0.6"
pkg_source="https://gitlab.com/procps-ng/procps/-/archive/v4.0.6/procps-v4.0.6.tar.gz"
pkg_sha256="1bbe8ff21dcd05a6adcda99a67d2e99cbd515c9e3a78fd3cc915b12aeb330d40"
pkg_depends=""

# Autotools, not meson -- confirmed directly (configure.ac/Makefile.am,
# no meson.build). A git-archive snapshot ships no pre-generated
# ./configure, so autogen.sh must run first; that needs `autopoint`
# (from the separate Debian "autopoint" package, NOT bundled into the
# "gettext" package itself -- confirmed the hard way, `gettext` alone
# left autogen.sh still failing with the same "you must have autopoint
# installed" error) staged into the toolchain, itself needing
# /usr/share/gettext (autopoint's own data files, not part of the
# wholesale /usr/{include,lib,lib64,bin,libexec} copy) and
# /usr/share/aclocal (the gettext.m4/iconv.m4/etc. macros autoreconf
# expands during autogen.sh) as two new targeted toolchain extras --
# see test/test_image_fixture.c's own extras[] list, the same
# "found by a real build failing, not guessed at" precedent
# /usr/share/bison already established for iproute2's own bison-needing
# grammar. --prefix=/usr matches every other recipe in this set;
# --disable-nls keeps this container-image build simple (no locale
# infrastructure needed for a diagnostic CLI toolset) -- confirmed via
# ./configure --help, no other non-default flag needed. ncurses (for
# top/watch) stays at its real upstream default (enabled) -- already
# real via this toolchain's own libncurses-dev.
pkg_build() {
	./autogen.sh
	./configure --prefix=/usr --disable-nls
	make -j"$(nproc)"
}

# ps/top/free/kill/pgrep/pkill/pidof/pidwait/pmap/pwdx/slabtop/tload/
# uptime/vmstat/w/watch/hugetop link against this package's own
# libproc2.so.1 (confirmed via ldd against a real `make install
# DESTDIR=...` -- top/watch additionally need libtinfo.so.6, already
# part of pkg_seed_image_baseline()'s own global runtime seeding, not
# copied again here). libproc2 is this recipe's own shared library, not
# a system one -- staged from this exact build's own DESTDIR output,
# not the host. sysctl lands in usr/sbin, matching where every other
# *-management binary in this project's own images already lives.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin" "$PKG_DESTDIR/usr/sbin" "$PKG_DESTDIR/usr/lib"
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share" "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/lib/pkgconfig" \
	       "$PKG_DESTDIR/usr/lib/libproc2.a" "$PKG_DESTDIR/usr/lib/libproc2.la" \
	       "$PKG_DESTDIR/usr/lib/libproc2.so"
}
