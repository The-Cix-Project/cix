#
# autoconf -- GNU Autoconf: autoconf/autoheader/autom4te/autoreconf/
# autoscan/autoupdate/ifnames. Every recipe already using autotools in
# this project (procps.recipe's own autogen.sh chain being the deepest
# real example) currently only needs these tools inside the isolated
# BUILD sandbox; this recipe makes them real, runtime-installable
# packages for a target "dev" image instead.
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against two independent mirrors (ftp.gnu.org and mirrors.kernel.org)
# -- byte-identical, same sha256.
#
pkg_name="autoconf"
pkg_version="2.71"
pkg_source="https://ftp.gnu.org/gnu/autoconf/autoconf-2.71.tar.xz"
pkg_sha256="f14c83cfebcc9427f2c3cea7258bd90df972d92eb26752da4ddad81c87a0faa4"
pkg_depends="m4 perl gawk"

# Plain autotools, confirmed directly. Nothing here is actually
# compiled -- autoconf/autoheader/autom4te/autoreconf/autoscan/
# autoupdate/ifnames are all real shell/Perl scripts, sed-substituted
# from .in templates at build time (confirmed via `file`), not C
# binaries.
pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)" MAKEINFO=true
}

# Real, load-bearing runtime dependencies, not just build-time ones,
# confirmed by inspecting the generated scripts directly: autoconf's
# own shebang is a plain #!/bin/sh (already covered by any real shell
# in the target image), but every other tool here is #!/usr/bin/perl
# (hardcoded absolute path -- pkg_depends="... perl ..." above), and
# autoconf's own generated wrapper hardcodes M4=/usr/bin/m4 and
# AWK=gawk (the bare command name, resolved via PATH at runtime, found
# via this build's own AC_PROG_AWK search -- gawk was present ahead of
# mawk on this build host) -- pkg_depends="m4 ... gawk" above covers
# both. Everything under usr/share/autoconf (the Autom4te::* Perl
# modules and every autoconf/autotest/m4sugar .m4 macro file) is real,
# load-bearing runtime data, not documentation -- autoconf cannot run
# without it, so unlike most other recipes' usr/share stripping, this
# one is kept in full.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/info" "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/locale"
}
