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
pkg_version="2.5-3"
pkg_source="https://ftp.gnu.org/gnu/inetutils/inetutils-2.5.tar.xz"
pkg_sha256="87697d60a31e10b5cb86a9f0651e1ec7bee98320d048c0739431aac3d5764fb6"
pkg_artifact_sha256="a58f7606914a7618cae2b823d21c2b7d3c7ba9886096e21aaeaa60d6a71ea4f4"
pkg_depends="ncurses"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils ncurses"
pkg_changelog="2.5-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.5-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-servers --disable-clients --enable-telnet
	make -j"$(nproc)"
}

pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/bin"
	cp telnet/telnet "$PKG_DESTDIR/usr/bin/telnet"
}
