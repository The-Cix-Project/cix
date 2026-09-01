#
# GNU gettext -- internationalization/translation tools (msgfmt,
# xgettext, msgmerge, ...) and libintl. A real, confirmed build-time
# dependency of grub.recipe (Part 5, bare-metal-readiness plan) --
# listed among GRUB 2.14's own upstream "hard requirements" (confirmed
# directly against the real GRUB source tree's own INSTALL file), used
# by GRUB's own build to generate/update its own translated strings.
# Not needed by any Cix container image at runtime -- staged onto
# the build/toolchain image only, the same "build-tool, not a runtime
# dependency" role m4/bison/flex/autoconf/etc. already have here.
#
pkg_name="gettext"
pkg_version="1.0-6"
pkg_source="https://ftp.gnu.org/gnu/gettext/gettext-1.0.tar.xz"
pkg_sha256="71132a3fb71e68245b8f2ac4e9e97137d3e5c02f415636eb508ae607bc01add7"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 1.0.
# Plain ./configure && make.
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils"
pkg_changelog="1.0-6: forces gl_cv_libcroco_use_included=yes. gettext bundles libcroco and compiles it only under the INCLUDED_LIBCROCO conditional, which configure derived as 'no' -- and with no system libcroco either, nothing defined the 28 libtextstyle_cr_* symbols the code still referenced. 1.0-5's --disable-curses was tried first and changed nothing, so it is not carried forward. 1.0-5: builds with --disable-curses. The final link failed on unresolved libtextstyle_cr_* references -- libtextstyle's bundled libcroco, used only to apply CSS styling to terminal output. Nothing here needs coloured gettext output; gettext is packaged because procps' autogen.sh calls autopoint. A declared capability loss under ADR-0222. Adds a gate that requires autopoint to exist and msgfmt to run. 1.0-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	#
	# gl_cv_libcroco_use_included=yes, and that one variable is the fix.
	#
	# The link failed on a wall of unresolved references:
	#
	#   tcc: error: unresolved reference to 'libtextstyle_cr_cascade_destroy'
	#   tcc: error: unresolved reference to 'libtextstyle_cr_style_new'
	#   ... 28 in all
	#
	# Those cr_ symbols are libcroco, a CSS parser that libtextstyle
	# uses to style terminal output. gettext bundles a copy, and
	# libtextstyle/lib/Makefile.am compiles it only under
	#
	#     if INCLUDED_LIBCROCO
	#
	# which configure sets from the cache variable
	# gl_cv_libcroco_use_included. Here it resolved to "no" -- there is
	# no system libcroco either -- so nothing at all defined the
	# symbols the code still referenced, and the failure arrived at the
	# very end of the build with no mention of libcroco in it.
	#
	# Forcing the cache variable is truthful, not a trick: the bundled
	# libcroco genuinely is the one to use, because this platform has
	# no other. It is the same mechanism m4's recipe uses to correct a
	# gnulib probe that reaches the wrong conclusion, and CLAUDE.md
	# records that pattern.
	#
	# --disable-curses was tried first and changed nothing -- the
	# styling code is compiled regardless of whether a terminal library
	# is available -- so it is not carried forward. A flag that does
	# not do what it appears to is worse than no flag.
	#
	CC=tcc gl_cv_libcroco_use_included=yes \
		./configure --prefix=/usr --disable-shared --disable-java \
		--disable-native-java --disable-openmp --without-emacs

	make -j"$(nproc)"

	#
	# Run what was built, and specifically the part that is the reason
	# gettext is packaged: autopoint. A gettext that builds but cannot
	# hand procps its autopoint is of no use here, and nothing else
	# would have noticed.
	#
	if [ ! -x gettext-tools/misc/autopoint ]; then
		echo "gettext: autopoint was not produced -- it is the only reason this package exists" >&2
		ls -la gettext-tools/misc/ >&2 || true
		exit 1
	fi
	out=$(gettext-tools/src/msgfmt --version 2>&1 | head -1) || {
		echo "gettext: msgfmt does not run:" >&2
		echo "$out" >&2
		exit 1
	}
	echo "  built: $out"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/doc" "$PKG_DESTDIR/usr/share/man" \
	       "$PKG_DESTDIR/usr/share/info"
}
