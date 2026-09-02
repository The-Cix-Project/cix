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
pkg_version="1.0-7"
pkg_source="https://mirrors.kernel.org/gnu/gettext/gettext-1.0.tar.xz"
pkg_sha256="71132a3fb71e68245b8f2ac4e9e97137d3e5c02f415636eb508ae607bc01add7"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168).
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils"
pkg_changelog="1.0-7: moves the source to mirrors.kernel.org, and instruments the libtextstyle link so a failure reports its own cause. The mirror is not a cosmetic change: ftp.gnu.org accepts this site's TCP connections and then answers nothing, so the previous URL could not be fetched at all, and pkg_sha256 is unchanged because the mirror serves the byte-identical tarball -- a mirror cannot substitute different bytes without failing a gate that already exists. On the unresolved libtextstyle_cr_ symbols of #220: 1.0-6 attributed those to configure deriving INCLUDED_LIBCROCO as no, and that attribution is wrong. gnulib-m4/libcroco.m4 ends with, when no system libcroco is found, gl_cv_libcroco_use_included=yes -- it sets itself, so forcing it changed nothing, which is consistent with the failure surviving 1.0-6 unchanged. The renaming also proves the conditional was already true: lib/Makefile.am hides internal symbols by compiling every unit, listing the symbols each one DEFINES via exported.sh, and generating a define of each to a libtextstyle_ prefixed name; a symbol only acquires that prefix if some compiled object defined it, so the prefix on cr_ symbols is itself evidence that the bundled libcroco was compiled. The remaining question is where those definitions go afterwards, so this revision prints the three measurements that separate the possibilities rather than guessing a fourth time. 1.0-6: forces gl_cv_libcroco_use_included=yes. 1.0-5's --disable-curses was tried first and changed nothing, so it is not carried forward. 1.0-5: builds with --disable-curses. The final link failed on unresolved libtextstyle_cr_ references -- libtextstyle's bundled libcroco, used only to apply CSS styling to terminal output. Nothing here needs coloured gettext output; gettext is packaged because grub's own build calls into it. A declared capability loss under ADR-0222. Adds a gate that requires autopoint to exist and msgfmt to run. 1.0-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	#
	# gl_cv_libcroco_use_included=yes is carried forward from 1.0-6
	# deliberately, even though libcroco.m4 shows it is a no-op here.
	# It is truthful (the bundled libcroco genuinely is the one to use,
	# because this platform has no other), it is harmless, and this
	# build exists to measure one unknown -- adding a second change
	# alongside it would make the result ambiguous.
	#
	CC=tcc gl_cv_libcroco_use_included=yes \
		./configure --prefix=/usr --disable-shared --disable-java \
		--disable-native-java --disable-openmp --without-emacs

	#
	# The bundled libcroco is built as a libtool convenience archive
	# (noinst_LTLIBRARIES) and merged into libtextstyle. Three things
	# have to hold for that to work, and the failure mode of #220 is
	# consistent with any one of them being false. Measure all three
	# rather than infer: whether the renames were generated at all,
	# whether the definitions carry them, and whether they survive the
	# merge into the library the tools actually link against. This is
	# the same class of failure as the libipset and libelf cases --
	# a library built and then discarded -- so the merge is the
	# specific step under suspicion.
	#
	if ! make -j"$(nproc)"; then
		L=libtextstyle/lib
		echo "=== gettext #220 evidence ==="
		echo "renames generated in config.h: $(grep -c 'libtextstyle_cr_' $L/config.h 2>/dev/null || echo NO-CONFIG-H)"
		for a in $L/.libs/libcroco_rpl.a $L/.libs/libtextstyle.a; do
			if [ -f "$a" ]; then
				echo "$a: renamed-defs=$(nm "$a" 2>/dev/null | grep -c ' T libtextstyle_cr_') plain-defs=$(nm "$a" 2>/dev/null | grep -c ' T cr_')"
			else
				echo "$a: ABSENT"
			fi
		done
		ls -la $L/.libs/ 2>/dev/null | head -12
		echo "=== end evidence ==="
		exit 1
	fi

	#
	# Run what was built, and specifically the part that is the reason
	# gettext is packaged. A gettext that builds but cannot hand grub
	# its tools is of no use here, and nothing else would notice.
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
