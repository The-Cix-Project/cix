#
# readline -- GNU Readline, line editing and history for interactive
# programs.
#
# Packaged because bird's client (birdc) requires it and configure
# refuses to build the client without it. Until now bird's own
# pkg_install copied libreadline.so.8 out of the build sandbox's
# ambient filesystem, which is a provenance violation of exactly the
# kind CLAUDE.md's Build Provenance Mandate names: no host content is
# ever copied into a package. Packaging it properly removes that copy
# rather than moving it somewhere less visible.
#
# Source is the GNU project's own canonical distribution point.
#
pkg_name="readline"
pkg_version="8.3"
pkg_source="https://ftp.gnu.org/gnu/readline/readline-8.3.tar.gz"
pkg_sha256="fe5383204467828cd495ee8d1d3c037a7eba1389c22bc6a041f627976f9061cc"

#
# readline links against a terminfo library for terminal capability
# lookup. ncurses provides it here (configure finds tgetent in
# -ltinfo), so it is a real runtime dependency, not only a build one.
#
pkg_depends="ncurses"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes.
# See docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils ncurses"
pkg_changelog="8.3: first packaging. Needed by bird's client, and it retires an ambient-filesystem copy of libreadline that bird's pkg_install was doing -- a Build Provenance Mandate violation"

pkg_build() {
	#
	# --with-curses so readline links terminfo from the real ncurses
	# package rather than searching for termcap, and shared libraries
	# because that is what consumers link against; the static archive
	# has no consumer here and only inflates every image that installs
	# this.
	#
	CC=tcc ./configure --prefix=/usr --with-curses --enable-shared --disable-static

	make -j"$(nproc)"

	#
	# Prove the library was actually built, and that it carries a real
	# SONAME. A readline whose shared object is missing or unnamed
	# links nothing, and configure is perfectly willing to produce
	# that quietly.
	#
	ls libreadline.so* >/dev/null 2>&1 || {
		echo "readline: no shared library was produced" >&2
		ls -la >&2
		exit 1
	}
	echo "  built: $(ls libreadline.so* | tr '\n' ' ')"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	#
	# Documentation and the examples tree are not runtime data. The
	# headers and the .pc-less link name ARE kept: this exists so
	# other packages can build against it.
	#
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/doc" "$PKG_DESTDIR/usr/share/readline"
}
