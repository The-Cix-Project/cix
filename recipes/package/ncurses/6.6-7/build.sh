#
# ncurses -- terminal-handling library (wide/Unicode variant only),
# the shared dependency behind htop.recipe/mtr.recipe/screen.recipe's
# own TUI/terminfo needs (task #729, the jump box recipe set).
#
# Source is ncurses' own canonical invisible-island.net release,
# checksum verified against a second independent source (the GNU FTP
# mirror's own copy of the same tarball) -- byte-identical, same
# sha256.
#
pkg_name="ncurses"
pkg_version="6.6-7"
pkg_source="https://invisible-island.net/archives/ncurses/ncurses-6.6.tar.gz"
pkg_sha256="355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11"
# Nothing at runtime: ncurses links against libc alone.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD.
# Each entry earns its place:
#   tcc       the compiler, passed to configure as an argument below
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln throughout configure and make
#   sed       ncurses' configure rewrites its own output with sed
#             constantly, and its Makefiles generate headers with it
#   grep      every feature test that greps a compiler or header
#   gawk      ncurses generates its terminfo tables through awk scripts,
#             on top of config.status's own use of it
#   binutils  ar and ranlib for the static libraries built alongside the
#             shared ones
#   findutils this recipe's own `find . -name Makefile -exec sed` below.
#             6.6-3 forgot it and got `find: command not found` -- the
#             declaration covers what the RECIPE runs, not only what the
#             package's own build system runs
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils"
pkg_changelog="6.6-7: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 6.6-6: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# Wide (Unicode-capable) build only -- confirmed real via a local
# build in this sandbox -- no narrow/non-widechar variant is built
# alongside it, since nothing in this project's own recipe set needs
# one and a second parallel build would just double this recipe's own
# complexity for no real consumer. Deliberately NOT passing
# --with-termlib: that splits terminfo (tgetent/tputs/etc.) into a
# separate libtinfow.so, which breaks a real class of downstream
# software that does a single-library AC_CHECK_LIB probe for curses
# functions (confirmed empirically: screen's/mtr's own configure
# scripts each do exactly this, and correctly find -lncursesw only
# once libncursesw.so is self-contained -- with the split active,
# htop's own multi-candidate AC_SEARCH_LIBS still found ncursesw fine,
# but mtr's single-shot AC_CHECK_LIB([ncursesw],[wprintw]) reported
# "no" because -lncursesw alone couldn't resolve tinfo-only symbols
# without also linking -ltinfow). Since only the wide variant is built
# here at all, there's no narrow-build library-size argument for the
# split either. --enable-pc-files stages real pkg-config files
# (ncursesw.pc et al) for consumers that probe via pkg-config instead
# of AC_CHECK_LIB.
# CC=tcc is passed as a configure ARGUMENT, not an environment
# variable, and that distinction is the whole fix. ncurses' configure is
# a heavily customised autoconf 2.52-era script that runs its own
# AC_PROG_CC-alike without honouring a preset CC from the environment:
# with `CC=tcc ./configure`, the log says
#
#   checking for gcc... gcc
#   checking whether we are using the GNU C compiler... yes
#
# and every subsequent feature test then ran through gcc -- the same
# gcc that segfaults on ordinary C (issue #116), which is why the
# checks came back "no" en masse and configure gave up with "getopt is
# required for building programs". So every ncurses this project has
# installed was built by a compiler the recipe never named, exactly
# like tcc (--cc=) and flex (CC_FOR_BUILD) before it.
#
# An autoconf command-line assignment is authoritative and is recorded
# in config.status, so it survives every sub-configure too.
pkg_build() {
	./configure CC=tcc --prefix=/usr --with-shared --without-normal \
	            --without-debug --without-ada --enable-widec \
	            --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig
	# ncurses links its shared libraries with
	#
	#   -Wl,-soname,`basename $@ .6.6`.6,-stats,-lc
	#
	# and TCC rejects the whole option string because of the last two:
	# `unsupported linker option '-stats,-lc'`. -stats only asks the
	# linker to print size statistics, and -lc is redundant -- the
	# compiler driver links libc into a shared object regardless, and
	# readelf confirms libc.so.6 is a NEEDED entry either way. Dropping
	# exactly those two leaves `-Wl,-soname,NAME`, which TCC supports
	# (verified directly), so the library still gets its correct SONAME.
	#
	# Asserted rather than assumed: if the pattern ever stops appearing,
	# this fails loudly instead of silently linking something else.
	grep -rlq -- ',-stats,-lc' ncurses/Makefile || {
		echo "ncurses: expected ',-stats,-lc' in the generated Makefiles" >&2
		exit 1
	}
	find . -name Makefile -exec sed -i 's/,-stats,-lc//g' {} +

	make -j"$(nproc)"
}

# Real files from this recipe's own DESTDIR install, confirmed via a
# local build: shared libs (libncursesw/libformw/libmenuw/libpanelw,
# each versioned .so.6/.so.6.6 plus the unversioned dev symlink),
# headers (curses.h/ncurses.h/term.h/panel.h/menu.h/form.h/etc,
# installed unprefixed -- overwrite mode is this configure's own
# default, confirmed via a real DESTDIR install showing them directly
# under include/ rather than include/ncursesw/), the terminfo database
# (usr/share/terminfo, ~2900 entries/~7.5MB -- a real runtime
# dependency for any of these tools to recognize more than a
# hardcoded handful of terminal types, kept in full rather than
# trimmed since this is a purpose-built jump box image, not a
# size-constrained embedded one), and the CLI tools (tic/infocmp/
# tput/reset/clear/tabs/etc, genuinely useful on an operator-facing
# jump box in their own right). .a static archives and the C++
# binding (libncurses++w.a, no .so variant exists upstream) are
# dropped -- nothing in this project's own recipe set links ncurses
# statically or needs its C++ API.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -f "$PKG_DESTDIR"/usr/lib/*.a

	# Compatibility names. This build is --enable-widec, so every
	# library is installed under its wide-char name (libncursesw.so.6)
	# and nothing called libncurses.so or libtinfo.so exists at all.
	# Real code does not ask for the wide names: psmisc's configure
	# looks for -ltinfo, then -lncurses, then -ltermcap, finds none of
	# them, and stops with "Cannot find tinfo, ncurses or termcap
	# libraries" -- with a perfectly good ncurses installed. vim, htop,
	# screen and procps all link the same conventional names.
	#
	# Without --with-termlib the terminfo half is compiled INTO
	# libncursesw rather than split out, so a libtinfo.so.6 pointing at
	# it genuinely resolves every symbol a -ltinfo consumer wants.
	# These are the same compatibility links Debian and Fedora ship for
	# the same reason, not a shim invented here.
	ln -sf libncursesw.so.6 "$PKG_DESTDIR/usr/lib/libncurses.so.6"
	ln -sf libncursesw.so.6 "$PKG_DESTDIR/usr/lib/libncurses.so"
	ln -sf libncursesw.so.6 "$PKG_DESTDIR/usr/lib/libtinfo.so.6"
	ln -sf libncursesw.so.6 "$PKG_DESTDIR/usr/lib/libtinfo.so"
	ln -sf ncursesw.pc "$PKG_DESTDIR/usr/lib/pkgconfig/ncurses.pc"
	ln -sf ncursesw.pc "$PKG_DESTDIR/usr/lib/pkgconfig/tinfo.pc"
}
