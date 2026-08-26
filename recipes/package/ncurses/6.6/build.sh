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
pkg_version="6.6"
pkg_source="https://invisible-island.net/archives/ncurses/ncurses-6.6.tar.gz"
pkg_sha256="355b4cbbed880b0381a04c46617b7656e362585d52e9cf84a67e2009b749ff11"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/ncurses-6.6.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="c8ace3b85a940ab2022b65618d78a11ff2afcf72938d692ad720375bbeffbc57"
pkg_depends=""

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
pkg_build() {
	CC=tcc ./configure --prefix=/usr --with-shared --without-normal \
	            --without-debug --without-ada --enable-widec \
	            --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig
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
}
