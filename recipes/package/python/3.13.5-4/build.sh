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
pkg_version="3.13.5-4"
pkg_source="https://www.python.org/ftp/python/3.13.5/Python-3.13.5.tgz"
pkg_sha256="e6190f52699b534ee203d9f417bdbca05a92f23e35c19c691a50ed2942835385"
pkg_artifact_sha256="f385ffac7aa87d03dd422503af29b1b538c382694d20e3fe1a9b2232c8f5d26f"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 3.13.5.
# ./configure && make. pkgconf because CPython's configure probes for optional modules through it; findutils because its setup step walks the source tree. Optional C extension modules (ssl, ctypes) may not build under TCC and are not needed here -- grub only needs a working interpreter to run gentpl.py, which is plain stdlib.
pkg_build_depends="bash coreutils make gcc binutils libc-dev sed grep gawk pkgconf findutils m4"

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
	# CC=/usr/bin/gcc, not tcc, and an ABSOLUTE path deliberately.
	#
	# Tier 3 of this project's TCC policy: TCC everywhere, with a small
	# exception list for genuinely infeasible cases (kernel and openssl
	# are already on it). CPython earns its place with a demonstrated
	# gap rather than an assertion -- under TCC the build dies at
	#   ./Include/cpython/pyatomic.h:543: error:
	#     "no available pyatomic implementation for this platform/compiler"
	# because pyatomic.h dispatches on GCC/clang builtins, MSVC, or C11
	# stdatomic.h, and TCC provides none of the three. Atomics are
	# foundational to CPython 3.13, so this is a porting project, not a
	# flag.
	#
	# The compiler used is gcc 16.2.0-11 from this project's OWN
	# artifact cache -- built on a Cix host through gcc.recipe's real
	# three-stage bootstrap, never an ambient host compiler, so the
	# Build Provenance Mandate holds.
	#
	# Absolute path because a bare "gcc" makes gcc compute its own
	# installation prefix relatively, which breaks its cc1 invocation
	# with a misleading "posix_spawnp: No such file or directory" (see
	# CLAUDE.md's environment notes -- real GCC behaviour, found the
	# hard way, not a Cix bug).
	CC=/usr/bin/gcc ./configure --prefix=/usr --enable-shared --with-ensurepip=no
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
	# The block that used to sit here copied ELEVEN libraries out of the
	# build host into this package -- libbz2, libcrypto, libssl, libffi,
	# libgdbm, liblzma, libncursesw, libpanelw, libreadline, libsqlite3,
	# libuuid and libz. That is foreign-OS content shipped inside a Cix
	# package, the same defect as #168/#169 and the same one gawk had,
	# and it only ever "worked" because the old baseline staged those
	# same Debian libraries into every image. With the baseline reduced
	# to the glibc floor (ADR-0209) the copies fail outright:
	#   cp: cannot stat '/lib/x86_64-linux-gnu/libreadline.so.8'
	#
	# They are not replaced by declared dependencies, because nothing
	# here needs them. Python exists in this image for exactly one job:
	# running grub's gentpl.py, which is plain stdlib. The optional C
	# extension modules those libraries back (ssl, sqlite3, readline,
	# curses, bz2, lzma) simply are not built, and their absence costs
	# this use case nothing.
	#
	# What the interpreter itself actually links is verified from the
	# ELF after building, the same discipline gawk 5.3.0-7 used, rather
	# than guessed at here.
}
