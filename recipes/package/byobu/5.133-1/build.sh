#
# byobu -- terminal window manager and status line over screen or tmux
# (the jump box recipe set, alongside screen/htop/btop/bind-utils).
#
# Source is Launchpad's own release tarball, which is the last real
# RELEASE of byobu. Upstream's git repository (dustinkirkland/byobu) is
# still active but has renamed the project: every tag on it is now
# trustmux-v7.x, and those are git-tag archives rather than made
# releases -- the v7.19 archive still unpacks as byobu-trustmux-v7.19/
# and still carries 329 byobu-named paths, so it is the same codebase
# mid-rename. 5.133 is packaged here because it is the newest thing
# upstream actually released as a tarball with a pre-generated
# configure. Revisit when the rename settles and 7.x ships releases.
#
pkg_name="byobu"
pkg_version="5.133-1"
pkg_source="https://launchpad.net/byobu/trunk/5.133/+download/byobu_5.133.orig.tar.gz"
pkg_sha256="4d8ea48f8c059e56f7174df89b04a08c32286bae5a21562c5c6f61be6dab7563"

# Runtime only. byobu compiles NOTHING -- see pkg_build().
#   screen  the multiplexer it drives. tmux is not packaged in this
#           project; see the backend note in pkg_install().
#   python  usr/lib/byobu/include/select-session.py, the session
#           picker byobu runs when more than one session exists.
#   procps  the status widgets shell out to free(1) and ps(1); without
#           it those widgets are simply blank, nothing else breaks.
#   bash    one script (usr/lib/byobu/include/*) is #!/bin/bash -e;
#           everything else is #!/bin/sh.
pkg_depends="screen python procps bash"

# No compiler. byobu's configure.ac declares exactly one program macro,
# AC_PROG_LN_S, and the installed tree is 152 files with zero ELF
# binaries -- confirmed by building it. So tcc, binutils and
# linux-headers are deliberately absent from this list, unlike every
# other recipe in this set.
pkg_build_depends="make bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="5.133-1: first packaging. Launchpad's 5.133 release tarball rather than upstream's git tags, which have renamed to trustmux-v7.x and are archives rather than releases."

# Two things this build needs that are not obvious, both confirmed by
# building it rather than by reading:
#
# 1. The tarball's generated autotools files carry the same timestamp
#    as their sources, so make decides aclocal.m4 is out of date and
#    tries to regenerate it:
#
#      It also requires GNU Autoconf, GNU m4 and Perl in order to run
#      make: *** [Makefile:325: aclocal.m4] Error 127
#
#    None of those are packaged here and none are actually needed --
#    the tarball ships a real 3577-line configure. Touching the
#    generated files in dependency order settles it.
#
# 2. --sysconfdir=/etc. With --prefix=/usr alone, autoconf's default
#    puts sysconfdir at $prefix/etc, and byobu's config lands in
#    /usr/etc/byobu/ where nothing reads it -- byobu's own scripts look
#    in /etc/byobu. Confirmed: without this flag the installed tree has
#    /usr/etc/byobu/backend and no /etc/byobu at all.
pkg_build() {
	touch aclocal.m4
	sleep 1
	touch configure
	sleep 1
	find . -name 'Makefile.in' -exec touch {} +
	if [ aclocal.m4 -nt configure ]; then
		echo "byobu: configure is older than aclocal.m4; make will try to regenerate" >&2
		exit 1
	fi

	./configure --prefix=/usr --sysconfdir=/etc
	make -j"$(nproc)"
}

# The backend is deliberately left UNPINNED.
#
# byobu already picks a backend for itself, in
# usr/lib/byobu/include/common: tmux if it is on PATH, else screen,
# else a clear error. That logic only runs when BYOBU_BACKEND is empty
# -- and the shipped /etc/byobu/backend sets BYOBU_BACKEND="tmux"
# unconditionally, which defeats it. Installed as shipped, byobu on
# this jump box would try a tmux that is not packaged and fail, having
# installed perfectly.
#
# Commenting that line out rather than rewriting it to "screen" is the
# point: byobu then detects what is actually present. Today that is
# screen. If tmux is ever packaged, byobu starts preferring it with no
# change to this recipe and no stale pin to find. A hardcoded "screen"
# would have to be remembered and undone.
#
# Man pages are dropped, as elsewhere in this set.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	sed -i 's/^BYOBU_BACKEND=/#BYOBU_BACKEND=/' "$PKG_DESTDIR/etc/byobu/backend"
	if grep -qE '^BYOBU_BACKEND=' "$PKG_DESTDIR/etc/byobu/backend"; then
		echo "byobu: the backend pin survived; it would override auto-detection" >&2
		exit 1
	fi
	rm -rf "$PKG_DESTDIR/usr/share/man"
}
