#
# psmisc -- fuser/killall/pstree/peekfd, the standard small-utilities
# set for the jump box's own process-management diagnostics.
#
# Source is Debian's own ".orig.tar.xz" (the exact, unmodified
# upstream release, same convention iputils.recipe's own comment
# already established) rather than GitLab's raw tag archive -- the
# first build attempt against that tag archive failed running
# ./autogen.sh: automake's own installed shebang couldn't execute
# cleanly in this project's build sandbox (real toolchain gap, not a
# psmisc bug), so this uses a real release tarball that already ships
# a pre-generated ./configure instead of needing autoreconf at all.
#
pkg_name="psmisc"
pkg_version="23.7-1"
pkg_source="https://deb.debian.org/debian/pool/main/p/psmisc/psmisc_23.7.orig.tar.xz"
pkg_sha256="58c55d9c1402474065adae669511c191de374b0871eec781239ab400b907c327"
pkg_depends="ncurses"

pkg_build() {
	./configure --prefix=/usr
	make -j"$(nproc)"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
