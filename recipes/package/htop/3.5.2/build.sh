#
# htop -- interactive process viewer (task #729, the jump box recipe
# set).
#
# Source is htop-dev's own canonical GitHub Releases asset (a real
# dist tarball with configure already generated, not a bare git-tag
# archive) -- this project's own single-source-of-truth GitHub
# organization for the project, no second mirror exists to
# cross-verify against; the release asset itself is immutable once
# published.
#
pkg_name="htop"
pkg_version="3.5.2"
pkg_source="https://github.com/htop-dev/htop/releases/download/3.5.2/htop-3.5.2.tar.xz"
pkg_sha256="225128e697c4a8c8a878fd0078c965ff8bd5fb24913bfc8473b8edbd50f843f8"
pkg_depends="ncurses"

# capabilities/delayacct/sensors/hwloc are all real htop features this
# configure script silently auto-enables whenever it happens to find
# their headers/libs already present -- confirmed the hard way in
# this sandbox (a dev box with libcap-dev installed turned on
# "capabilities: yes" with zero flags passed). None of their backing
# libraries (libcap, libnl for delayacct, libsensors, hwloc) exist as
# a recipe in this project, so a real isolated build container
# wouldn't have found them anyway -- but disabling explicitly here
# documents that as a deliberate scope decision for this jump box's
# own process viewer, not an accident of whatever happens to be lying
# around in a given build container. --with-curses=ncursesw pins the
# real dependency by name rather than letting the default probe guess.
pkg_build() {
	CC=tcc ./configure --prefix=/usr --with-curses=ncursesw \
	            --disable-capabilities --disable-delayacct \
	            --disable-sensors --disable-hwloc
	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local build: just the htop binary, its man page, and its own
# example config under /usr/share/doc -- confirmed link line is
# "-lncursesw -lm" (`ldd htop`, run with LD_LIBRARY_PATH pointed at
# this project's own from-scratch ncurses build rather than this
# sandbox's ambient system copy to get a true reading: only
# libncursesw.so.6/libm.so.6/libc.so.6, nothing else).
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/doc"
}
