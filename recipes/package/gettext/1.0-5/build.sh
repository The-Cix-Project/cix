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
pkg_version="1.0-5"
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
pkg_changelog="1.0-5: builds with --disable-curses. The final link failed on unresolved libtextstyle_cr_* references -- libtextstyle's bundled libcroco, used only to apply CSS styling to terminal output. Nothing here needs coloured gettext output; gettext is packaged because procps' autogen.sh calls autopoint. A declared capability loss under ADR-0222. Adds a gate that requires autopoint to exist and msgfmt to run. 1.0-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	#
	# --disable-curses, and it is the point of this revision.
	#
	# 1.0-4 built until the final link and then failed with a wall of:
	#
	#   tcc: error: unresolved reference to 'libtextstyle_cr_cascade_destroy'
	#   tcc: error: unresolved reference to 'libtextstyle_cr_style_new'
	#   ... and three more of the same shape
	#
	# Those cr_ symbols are libtextstyle's bundled copy of libcroco, a
	# CSS parser, renamed with a prefix. libtextstyle uses it to apply
	# CSS-described styling to terminal output -- colour in gettext's
	# own tools. The objects were being referenced and not linked.
	#
	# Nothing in this platform needs styled gettext output. gettext is
	# here because procps' autogen.sh calls autopoint (#206/#220), and
	# autopoint is a shell script that copies files.
	#
	# So this is a declared capability loss under ADR-0222 rather than
	# a link to be fixed: gettext's tools lose coloured terminal
	# output, and gain the ability to be built at all.
	#
	CC=tcc ./configure --prefix=/usr --disable-shared --disable-java \
		--disable-native-java --disable-openmp --without-emacs \
		--disable-curses

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
