#
# python -- CPython 3.13, --enable-shared (a real, shared libpython, not
# a single static binary -- needed for anything embedding Python or
# building C extensions against it, the same "real dev toolchain"
# reasoning driving this whole recipe batch).
#
# Source is python.org's own canonical release point (PGP-signed,
# confirmed via the accompanying .asc signature file present alongside
# it), checksum computed directly from the downloaded bytes
# (sha256sum), not taken from any third party.
#
pkg_name="python"
pkg_version="3.13.5"
pkg_source="https://www.python.org/ftp/python/3.13.5/Python-3.13.5.tgz"
pkg_sha256="e6190f52699b534ee203d9f417bdbca05a92f23e35c19c691a50ed2942835385"
pkg_depends=""

# Plain autotools-derived ./configure, confirmed directly.
# --with-ensurepip=no skips the final `python -m ensurepip` install
# step (an extra several-second run of the just-built interpreter
# inside the build container for no benefit -- the ensurepip package
# and its bundled wheels still get installed as real files either way,
# so `python3 -m ensurepip` still works later inside a real container).
# --enable-shared builds libpython3.13.so instead of statically linking
# it into the python binary alone. A first configure run on this
# toolchain (before libffi-dev/libbz2-dev/liblzma-dev/libgdbm-dev/
# uuid-dev/libsqlite3-dev were installed on the build host) left
# ctypes/_sqlite3/_bz2/_lzma/_uuid/_gdbm disabled -- found via this
# build's own "necessary bits ... were not found" summary, not
# guessed at; installing those five real -dev packages on the host
# (flows into this toolchain for free via the existing wholesale
# /usr copy, the same precedent already established for every other
# recipe's own optional-feature detection) and reconfiguring fixed
# all but _tkinter (a GUI toolkit binding with no real use in a
# headless container -- deliberately left out, same judgment call
# this project's own web console already made against xterm.js) and
# _dbm (a legacy ndbm-compat interface; _gdbm, the real modern
# equivalent `dbm.gnu` uses, builds fine and covers the same need).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --enable-shared --with-ensurepip=no
	make -j"$(nproc)"
}

# Confirmed via ldd across the real python3.13 binary and every .so
# under lib-dynload (the compiled extension modules): beyond libc/libm
# (already covered globally) this build needs libbz2, libcrypto/libssl
# (the _ssl/_hashlib modules -- real TLS support, needed by anything
# using urllib/pip/etc.), libffi (ctypes), libgdbm, liblzma,
# libncursesw/libpanelw (the curses module), libreadline (the
# interactive REPL's own line-editing/history), libsqlite3, libuuid,
# libz -- each staged the same SONAME-symlink-plus-real-target pattern
# every other recipe's own runtime libs already use, all copied from
# this exact build container's own toolchain-provided copies (the same
# host libraries python was actually linked against a moment ago).
# libpython3.13.so.1.0 (this build's own shared library, not a system
# one) is kept alongside its libpython3.so/libpython3.13.so dev
# symlinks. usr/lib/python3.13/test (~140MB, CPython's own internal
# test suite -- no real distro ships this either) and __pycache__
# (regenerated automatically on first import, not source) are dropped;
# idlelib/tkinter and the idle3 binaries are dropped too since
# _tkinter isn't built. ensurepip (its own Lib/ensurepip package plus
# bundled pip/setuptools wheels) is kept -- small, and it's what makes
# `python3 -m ensurepip` work for real inside a running container.
# config-3.13-x86_64-linux-gnu (the sysconfig/distutils build data any
# real C-extension build against this python needs) is kept in full,
# matching this whole recipe set's own "a real dev toolchain" purpose.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/lib/python3.13/test" \
	       "$PKG_DESTDIR/usr/lib/python3.13/idlelib" \
	       "$PKG_DESTDIR/usr/lib/python3.13/tkinter" \
	       "$PKG_DESTDIR/usr/bin/idle3" "$PKG_DESTDIR/usr/bin/idle3.13" \
	       "$PKG_DESTDIR/usr/share/man"
	find "$PKG_DESTDIR/usr/lib/python3.13" -name "__pycache__" -type d -exec rm -rf {} +
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libbz2.so.1.0 /lib/x86_64-linux-gnu/libbz2.so.1.0.4 \
	   /lib/x86_64-linux-gnu/libcrypto.so.3 /lib/x86_64-linux-gnu/libssl.so.3 \
	   /lib/x86_64-linux-gnu/libffi.so.8 /lib/x86_64-linux-gnu/libffi.so.8.1.2 \
	   /lib/x86_64-linux-gnu/libgdbm.so.6 /lib/x86_64-linux-gnu/libgdbm.so.6.0.0 \
	   /lib/x86_64-linux-gnu/liblzma.so.5 /lib/x86_64-linux-gnu/liblzma.so.5.4.1 \
	   /lib/x86_64-linux-gnu/libncursesw.so.6 /lib/x86_64-linux-gnu/libncursesw.so.6.4 \
	   /lib/x86_64-linux-gnu/libpanelw.so.6 /lib/x86_64-linux-gnu/libpanelw.so.6.4 \
	   /lib/x86_64-linux-gnu/libreadline.so.8 /lib/x86_64-linux-gnu/libreadline.so.8.2 \
	   /lib/x86_64-linux-gnu/libsqlite3.so.0 /lib/x86_64-linux-gnu/libsqlite3.so.0.8.6 \
	   /lib/x86_64-linux-gnu/libuuid.so.1 /lib/x86_64-linux-gnu/libuuid.so.1.3.0 \
	   /lib/x86_64-linux-gnu/libz.so.1 /lib/x86_64-linux-gnu/libz.so.1.2.13 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
