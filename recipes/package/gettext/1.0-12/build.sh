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
pkg_version="1.0-12"
pkg_source="https://mirrors.kernel.org/gnu/gettext/gettext-1.0.tar.xz"
pkg_sha256="71132a3fb71e68245b8f2ac4e9e97137d3e5c02f415636eb508ae607bc01add7"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168).
pkg_build_depends="bash coreutils make tcc binutils linux-headers sed grep gawk"
pkg_changelog="1.0-12: back to tcc, because the reason 1.0-11 gave for gcc is disproven. gcc fails the same way: undefined references to xasprintf, xstrdup, xrealloc and xconcatenated_filename from libgettextsrc, which are gnulib symbols in gettext-tools own convenience archive -- the same family as the set_program_name and close_stdout failures under tcc, one level up from the libtextstyle merge. So this is NOT a tcc property and declaring a gcc exception for it would record a false reason in the one place ADR-0226 asks for a true one. What IS established and kept: the libtextstyle convenience archives genuinely are not merged, measured in 1.0-7 as 412 renamed definitions in libcroco_rpl.a against zero in libtextstyle.a, and the ar merge here fixes that under both compilers. gettext still does not build, and the remaining failure is a second, compiler-independent problem in how gettext-tools own libraries are linked. Recorded on #220 rather than guessed at a sixth time; nothing installed depends on gettext and procps is the only consumer waiting. 1.0-11: builds with gcc (ADR-0226). Four TCC revisions converged on one root cause rather than four problems: libtool does not merge noinst convenience archives when the compiler is TCC. 1.0-7 measured it (libcroco_rpl.a held 412 renamed definitions, libtextstyle.a held zero), 1.0-9 merged them by hand with ar and proved the diagnosis by getting 412 into libtextstyle.a, and the build then failed the same way one level up on gnulib own convenience archive inside gettext-tools -- set_program_name, close_stdout, _gl_start_options. Merging every such archive across every subdirectory of a package this size is battling a build system this project neither owns nor wrote, which is the situation ADR-0226 exists for. The manual merge and its measurements are kept below rather than deleted: they are what identified the cause, they cost nothing under gcc where libtool merges correctly, and their assertion would fail loudly if that ever stopped being true. 1.0-10: sets ac_cv_func_posix_spawn_file_actions_addchdir=yes, the fix m4 1.4.20-2 already recorded for this exact failure. With the libtextstyle merge working, the build got as far as gettext-tools and stopped on spawn.h:1496 incompatible types for redefinition of posix_spawn_file_actions_addchdir -- the documented glibc-2.44 signature. glibc exports the np-suffixed symbol for real but provides the POSIX-2024 name only as a header-level asm alias, so autoconf link probe, which deliberately declares the function itself rather than including the header, links a bare name that does not exist and concludes it is absent; gnulib then emits its own prototype after including the system header and the two collide. Correcting the cache variable is truthful, because the function IS available to any translation unit that includes spawn.h, which is the only legitimate way C code calls it, and it lands on gnulib REPLACE branch where the name is defined to rpl_ and gnulib compiles its own. addfchdir is aliased identically and is set alongside it. 1.0-9: builds libtextstyle/lib before merging, not the whole libtextstyle subdirectory. 1.0-8 got the diagnosis right and the sequencing wrong: it ran make in libtextstyle, which builds adhoc-tests too, and adhoc-tests/hello links against libtextstyle.la and therefore failed on exactly the symbols the merge was about to supply -- before the merge could run. The library itself was never the problem. Building lib alone, merging, then building the rest lets adhoc-tests link against a libtextstyle.a that is complete. That log also showed libtextstyle_xml symbols failing alongside the cr_ ones, confirming this is every bundled convenience archive rather than libcroco specifically, which is why all three are merged. 1.0-8: merges the bundled convenience archives into libtextstyle.a, which is where #220 actually goes wrong. 1.0-7 measured it rather than guessing a fourth time and the answer is unambiguous: config.h carries 412 generated renames, libcroco_rpl.a carries 412 renamed definitions, and libtextstyle.a carries ZERO. The bundled libcroco is compiled correctly and then never reaches the library the tools link against -- the same library built and then discarded pattern as libipset and libelf, which is the third of the three possibilities #220 listed and the one nobody had tested. libtool is supposed to merge a noinst convenience archive into the library that lists it in LIBADD; here it does not, so the recipe does it with ar, which is the same class of recipe-level compensation for a real toolchain gap as sysklogd carrying a __dso_handle stub. The merge is asserted rather than assumed: the renamed symbol count in libtextstyle.a must be non-zero afterwards, so a future libtool that does its own merging cannot make this silently redundant without the assertion still passing. 1.0-7: moves the source to mirrors.kernel.org, and instruments the libtextstyle link so a failure reports its own cause. The mirror is not a cosmetic change: ftp.gnu.org accepts this site's TCP connections and then answers nothing, so the previous URL could not be fetched at all, and pkg_sha256 is unchanged because the mirror serves the byte-identical tarball -- a mirror cannot substitute different bytes without failing a gate that already exists. On the unresolved libtextstyle_cr_ symbols of #220: 1.0-6 attributed those to configure deriving INCLUDED_LIBCROCO as no, and that attribution is wrong. gnulib-m4/libcroco.m4 ends with, when no system libcroco is found, gl_cv_libcroco_use_included=yes -- it sets itself, so forcing it changed nothing, which is consistent with the failure surviving 1.0-6 unchanged. The renaming also proves the conditional was already true: lib/Makefile.am hides internal symbols by compiling every unit, listing the symbols each one DEFINES via exported.sh, and generating a define of each to a libtextstyle_ prefixed name; a symbol only acquires that prefix if some compiled object defined it, so the prefix on cr_ symbols is itself evidence that the bundled libcroco was compiled. The remaining question is where those definitions go afterwards, so this revision prints the three measurements that separate the possibilities rather than guessing a fourth time. 1.0-6: forces gl_cv_libcroco_use_included=yes. 1.0-5's --disable-curses was tried first and changed nothing, so it is not carried forward. 1.0-5: builds with --disable-curses. The final link failed on unresolved libtextstyle_cr_ references -- libtextstyle's bundled libcroco, used only to apply CSS styling to terminal output. Nothing here needs coloured gettext output; gettext is packaged because grub's own build calls into it. A declared capability loss under ADR-0222. Adds a gate that requires autopoint to exist and msgfmt to run. 1.0-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

pkg_build() {
	#
	# gl_cv_libcroco_use_included=yes is carried forward from 1.0-6
	# deliberately, even though libcroco.m4 shows it is a no-op here.
	# It is truthful (the bundled libcroco genuinely is the one to use,
	# because this platform has no other), it is harmless, and this
	# build exists to measure one unknown -- adding a second change
	# alongside it would make the result ambiguous.
	#
	#
	# ac_cv_func_posix_spawn_file_actions_add{chdir,fchdir}: correct an
	# answer autoconf cannot reach on its own (see m4 1.4.20-2, which
	# recorded this first). glibc 2.44 exports the _np name as a real
	# symbol and provides the POSIX-2024 name only as a header-level
	# asm alias, so AC_CHECK_FUNC -- which declares the function itself
	# by design, so a macro cannot fake a positive -- links a bare name
	# that genuinely does not exist and concludes the function is
	# absent. gnulib then emits its own prototype after including the
	# system header, and the two declarations collide:
	#
	#   spawn.h:1496: error: incompatible types for redefinition of
	#                 'posix_spawn_file_actions_addchdir'
	#
	# Saying yes is truthful: the function IS available to any
	# translation unit that includes <spawn.h>, which is the only
	# legitimate way to call it. It also lands on gnulib's strictly
	# safer REPLACE branch, where the name is defined to rpl_ and
	# gnulib compiles its own, so nothing depends on the alias.
	#
	CC=tcc gl_cv_libcroco_use_included=yes \
		ac_cv_func_posix_spawn_file_actions_addchdir=yes \
		ac_cv_func_posix_spawn_file_actions_addfchdir=yes \
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
	#
	# Build the libraries first, then merge, then build the tools that
	# link against them. libtextstyle.a has to exist before it can be
	# corrected, and the tools must not be linked before it is.
	#
	#
	# lib only. Building the whole libtextstyle subdirectory also builds
	# adhoc-tests, whose own "hello" links against libtextstyle.la and
	# so fails on precisely the symbols the merge below is about to
	# supply -- before it can run. The library is not what breaks.
	#
	if ! make -j"$(nproc)" -C libtextstyle/lib; then
		echo "gettext: libtextstyle/lib itself failed to build" >&2
		exit 1
	fi

	#
	# Merge the bundled convenience archives into libtextstyle.a (#220).
	#
	# libtool is supposed to do this: libcroco, libglib and libxml are
	# noinst_LTLIBRARIES listed in libtextstyle's own LIBADD, and a
	# convenience archive exists precisely to be absorbed by the library
	# that names it. Measured on 1.0-7, it is not absorbed --
	# libcroco_rpl.a holds 412 renamed definitions and libtextstyle.a
	# holds none -- so every tool linking libtextstyle fails on
	# libtextstyle_cr_* symbols that were compiled and then thrown away.
	#
	# Doing it with ar is the same kind of recipe-level compensation for
	# a real toolchain gap as sysklogd's __dso_handle stub: the fix
	# belongs here rather than in unmodified third-party source, and it
	# is asserted rather than assumed below.
	#
	L=libtextstyle/lib/.libs
	merged=0
	for a in "$L/libcroco_rpl.a" "$L/libglib_rpl.a" "$L/libxml_rpl.a"; do
		[ -f "$a" ] || continue
		tmp=$(mktemp -d)
		abs="$(pwd)/$a"
		( cd "$tmp" && ar x "$abs" ) || {
			echo "gettext: could not unpack $a" >&2
			exit 1
		}
		if ls "$tmp"/*.o >/dev/null 2>&1; then
			ar r "$L/libtextstyle.a" "$tmp"/*.o || {
				echo "gettext: could not merge $a into libtextstyle.a" >&2
				exit 1
			}
			merged=$((merged + 1))
		fi
		rm -rf "$tmp"
	done
	ranlib "$L/libtextstyle.a" || exit 1
	echo "  merged $merged convenience archive(s) into libtextstyle.a"

	#
	# Assert the merge actually happened. This is the whole change, and
	# a silent no-op here would put the failure back exactly where it
	# was -- at the final link, hundreds of lines later, naming symbols
	# rather than the archive they should have come from.
	#
	renamed=$(nm "$L/libtextstyle.a" 2>/dev/null | grep -c ' T libtextstyle_cr_')
	if [ "$renamed" -eq 0 ]; then
		echo "gettext: libtextstyle.a still defines no libtextstyle_cr_ symbols after the merge" >&2
		nm "$L/libtextstyle.a" 2>/dev/null | grep -c ' T ' >&2
		exit 1
	fi
	echo "  libtextstyle.a now defines $renamed renamed libcroco symbols"

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
