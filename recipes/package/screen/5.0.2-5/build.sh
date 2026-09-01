#
# screen -- terminal multiplexer (task #729, the jump box recipe set).
#
# Source is GNU's own canonical ftp.gnu.org release, checksum verified
# against a second independent GNU mirror -- byte-identical, same
# sha256.
#
pkg_name="screen"
pkg_version="5.0.2-5"
pkg_source="https://ftp.gnu.org/gnu/screen/screen-5.0.2.tar.gz"
pkg_sha256="ca9a2c7e240919bc7ac12124593ae4529bb4eb5f7349d8857829b7e3f0b3b332"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/screen-5.0.2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="80bdd3e54f83d496eae5ea84d01f1005b99933b325c4f7e80e61ba5f875d6aa2"
pkg_depends="ncurses"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils ncurses libxcrypt"
pkg_changelog="5.0.2-5: first build attempt since the compiler upgrade (ADR-0223). 5.0.2-4 could not build: configure reported 'checking for tcc option to enable C11 features... unsupported' and the build then died at sched.c:196 (#212). The upgraded compiler accepts -std=c11 and reports __STDC_VERSION__ 201112 under it. Also fixes a duplicated pkg_build_depends whose second copy silently dropped libxcrypt from the first -- the same trap as mtr's, already sprung. 5.0.2-4: rewrites -iquote to -I, which TCC does not implement. 5.0.2-3: declares libxcrypt -- configure fails with 'unable to find crypt() function' without it, since glibc 2.44 moved crypt() out of libc. 5.0.2-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

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
	# TCC does not implement -iquote (GCC's "search only for
	# quoted includes" path form):
	#
	#   tcc: error: invalid option -- '-iquote.'
	#
	# screen's generated Makefile passes `-iquote.` unconditionally.
	# Rewritten to plain -I., which is a superset: -iquote restricts a
	# path to "..." includes, -I applies it to both. Nothing here relies
	# on the narrower form, and the directory is the source root either
	# way.
	sed -i 's/-iquote\([^ ]*\)/-I\1/g' Makefile
	if grep -q -- '-iquote' Makefile; then
		echo "screen: an -iquote survived the rewrite" >&2
		exit 1
	fi

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
