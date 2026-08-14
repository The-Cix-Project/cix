#
# inetutils -- built here only for its real telnet client, for the
# jump box's own diagnostic toolset. GNU's own canonical ftp.gnu.org
# release, standard autotools with a pre-generated ./configure.
#
# --disable-servers/--disable-clients turn off every one of
# inetutils' many client/server programs by default (ftp, rcp, rexec,
# rlogin, rsh, talk, tftp, whois, and their server-side counterparts,
# none of which this jump box needs); --enable-telnet re-enables just
# the one client this recipe exists for (each tool gets its own
# --enable-<tool>/--disable-<tool> flag via inetutils' own
# IU_ENABLE_CLIENT/IU_ENABLE_SERVER autoconf macros, confirmed
# directly in am/enable.m4). Base utilities outside the clients/
# servers grouping (hostname, ping, logger, ...) still build --
# harmless, simply not installed below (iputils.recipe/coreutils
# already cover ping/hostname for this box).
#
pkg_name="inetutils"
pkg_version="2.5"
pkg_source="https://ftp.gnu.org/gnu/inetutils/inetutils-2.5.tar.xz"
pkg_sha256="87697d60a31e10b5cb86a9f0651e1ec7bee98320d048c0739431aac3d5764fb6"
pkg_depends="ncurses"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-servers --disable-clients --enable-telnet
	make -j"$(nproc)"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp telnet/telnet "$PKG_DESTDIR/usr/bin/telnet"
}
