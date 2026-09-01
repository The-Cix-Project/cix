#
# vim -- the real, terminal-only editor for the jump box (no GUI/X11,
# nothing this project's own minimal images could ever use anyway).
# Real GitHub release tag, ships a pre-generated ./configure (vim
# checks its own autoconf output into git, unlike most projects), no
# autoreconf/bootstrap step needed.
#
pkg_name="vim"
pkg_version="9.1.1428-3"
pkg_source="https://github.com/vim/vim/archive/refs/tags/v9.1.1428.tar.gz"
pkg_sha256="d96a8208f7756958d5bdb2464e9fdb770a8400e3aafac328e58c81c2477124f1"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/vim-9.1.1428.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends="ncurses"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils ncurses"
pkg_changelog="9.1.1428-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 9.1.1428-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# --without-x/--disable-gui: no X11 anywhere in this project's own
# images, would just be dead weight. --disable-nls: locale/translation
# support, not a real need for a jump box's own terminal editor.
# --disable-channel/--disable-netbeans: vim's own inter-process/IDE
# integration features, no use case here. terminfo left at its real
# default (auto-detected via ncurses, already a dependency).
pkg_build() {
	CC=tcc ./configure --prefix=/usr --without-x --disable-gui --disable-nls \
	            --disable-channel --disable-netbeans
	make -j"$(nproc)"
}

pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc" \
	       "$PKG_DESTDIR/usr/share/locale"
}
