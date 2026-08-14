#
# mtr -- combined traceroute/ping network diagnostic tool (task #729,
# the jump box recipe set).
#
# mtr has no formal dist-tarball release (confirmed: its GitHub
# Releases API returns zero release assets, only git tags) -- the
# source below is a plain git-tag archive, which ships configure.ac
# but not a pre-generated ./configure. pkg_build() below runs the
# project's own real bootstrap.sh (aclocal + autoheader + automake +
# autoconf) before configuring, using this project's own already-built
# autoconf/automake/m4/libtool -- these don't need to be listed in
# pkg_depends since they're part of the shared sandboxed toolchain
# image every pkg_build() already runs inside (image/src/
# mktoolchainimage.c), the same reason bird.recipe/keepalived.recipe
# have an empty pkg_depends despite needing a real C toolchain to
# build. Checksum is this exact tag archive's own hash -- no second
# mirror exists for a git-tag-only source, but GitHub's tag archives
# are content-addressed and immutable once generated.
#
pkg_name="mtr"
pkg_version="0.96"
pkg_source="https://github.com/traviscross/mtr/archive/refs/tags/v0.96.tar.gz"
pkg_sha256="73e6aef3fb6c8b482acb5b5e2b8fa7794045c4f2420276f035ce76c5beae632d"
pkg_depends="ncurses"

# --without-gtk/--without-jansson/--without-ipinfo: none of GTK+3,
# libjansson (JSON output), or ipinfo.io lookup support (needs
# libcurl-dev, no recipe for that exists) are real needs for this
# jump box's own terminal-only diagnostic use -- confirmed real via a
# local build in this sandbox, both flags eliminate the corresponding
# checks entirely rather than leaving them to silently succeed/fail
# against whatever happens to be ambient. ncursesw is picked up by
# name (confirmed via configure.ac: AC_CHECK_LIB([ncursesw],[wprintw])
# is tried before the plain ncurses/curses fallbacks, and correctly
# succeeds once ncurses.recipe's own libncursesw.so is self-contained
# -- see ncurses.recipe's own comment on why --with-termlib is
# deliberately not used there).
#
# Linux capabilities (libcap, dropping CAP_NET_RAW down to
# mtr-packet's own unprivileged helper process after opening the raw
# socket) auto-enable if libcap happens to be present at configure
# time -- confirmed empirically in this sandbox (a dev box with
# libcap-dev installed silently turned "cap: yes" on with zero flags
# passed, no --without-cap exists to force it off). No libcap recipe
# exists in this project, so a real isolated build container simply
# won't find it and this correctly falls back to "cap: no" -- mtr then
# needs to run as real root for its raw ICMP socket, the same posture
# this project's own daemon/src/ping.c already has. Deliberate scope
# decision, not an oversight: adding a whole new libcap recipe purely
# for mtr's own optional privilege-drop enhancement is out of
# proportion for this task.
pkg_build() {
	./bootstrap.sh
	CC=tcc ./configure --prefix=/usr --without-gtk --without-jansson --without-ipinfo
	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local DESTDIR install: mtr/mtr-packet land under usr/sbin (this
# project's own images have no separate /sbin, so this is the real,
# final path, not usr/bin) -- mtr (the ncurses UI + traceroute/ping
# engine) and mtr-packet (the small helper process mtr spawns to
# actually open raw sockets, confirmed via ldd it needs no libcap
# here, matching the "cap: no" build above). Man pages
# (usr/share/man/man8) and bash-completion
# (usr/share/bash-completion/completions) dropped -- consistent with
# every other recipe in this set trimming non-essential doc/completion
# output.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/bash-completion"
}
