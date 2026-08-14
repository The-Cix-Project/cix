#
# screen -- terminal multiplexer (task #729, the jump box recipe set).
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against a second independent GNU mirror -- byte-identical, same
# sha256.
#
pkg_name="screen"
pkg_version="5.0.2"
pkg_source="https://ftp.gnu.org/gnu/screen/screen-5.0.2.tar.gz"
pkg_sha256="ca9a2c7e240919bc7ac12124593ae4529bb4eb5f7349d8857829b7e3f0b3b332"
pkg_depends="ncurses"

# --disable-pam: no PAM recipe exists in this project. --disable-pam
# without setuid would normally be a real functional gap (screen's
# own configure warns loudly: "FOR screen TO WORK IT WILL NEED TO RUN
# AS SUID root BINARY") -- setuid is what lets one user's screen
# session chown a pty device so ANOTHER user can attach to it
# (multi-user session sharing). This jump box is single-operator by
# design (each SSH login is its own container-level identity, task
# #731) -- a user creating and attaching to their own screen sessions
# never needs that cross-user chown, only the setuid multi-user
# sharing case does. So: build without PAM, and deliberately strip the
# setuid bit `make install` applies unconditionally (see pkg_install
# below) rather than leaving an unused, unaudited setuid-root binary
# sitting in the image -- a real, documented security posture, not an
# oversight.
# --disable-socket-dir: this project's minimal images have no
# multi-user /tmp convention (confirmed elsewhere in this project:
# minimal images have no /tmp at all) that a shared global socket
# directory would need to defend against; screen's own per-user
# $HOME-relative default socket path is simpler and sufficient here.
#
# Screen's own AC_SEARCH_LIBS([tgetent], [curses termcap termlib
# ncursesw tinfow ncurses tinfo]) call (confirmed directly in
# configure.ac) already includes ncursesw as a real fallback candidate
# -- a from-scratch build container with only ncurses.recipe's own
# libncursesw.so present (no ambient system libcurses.so alias) will
# correctly reach and use it, confirmed by reading the actual
# candidate order rather than trusting this sandbox's own local build
# (which has ambient system ncurses too, and picks the earlier
# "curses" candidate instead purely because that alias happens to
# exist here).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --disable-pam --disable-socket-dir
	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local build: the versioned binary (screen-5.0.2) plus the unversioned
# usr/bin/screen symlink screen's own Makefile creates, the man page,
# and the utf8encodings data files screen's own multi-byte terminal
# support reads at runtime. `make install` applies `chmod 4755`
# (setuid root) to the binary unconditionally, regardless of the
# --disable-pam build above -- explicitly reverted to plain 0755 here,
# per the security posture explained above.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	chmod 0755 "$PKG_DESTDIR/usr/bin/screen-5.0.2"
	rm -rf "$PKG_DESTDIR/usr/share/man"
}
