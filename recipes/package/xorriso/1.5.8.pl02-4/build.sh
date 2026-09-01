#
# xorriso -- ISO 9660/Rock Ridge/Joliet image manipulation, with
# bundled libburn/libisofs/libisoburn (statically linked, confirmed
# via the real xorriso "standalone" tarball's own documented shape --
# no separate libburnia recipes needed). A real, runtime-only
# dependency of grub-mkrescue (Part 5, bare-metal-readiness plan) --
# grub-mkrescue.c invokes the literal "xorriso" binary by name via
# fork/exec, searched on $PATH (confirmed directly against GRUB's own
# util/grub-mkrescue.c source, not assumed) -- never linked as a
# library, and not needed to *build* GRUB itself, only to *run*
# grub-mkrescue afterward.
#
pkg_name="xorriso"
pkg_version="1.5.8.pl02-4"
pkg_source="https://ftp.gnu.org/gnu/xorriso/xorriso-1.5.8.pl02.tar.gz"
pkg_sha256="b1455ecafbf0692ddafe1d71002a96f2ce2d77f4deae602678261ce033f97bc8"
pkg_artifact_sha256="cf957c540b9d4fbb8d863483b53b57a289b2adb97a6234902f956f865fa1652d"
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 1.5.8.pl02.
# Plain ./configure && make, same shape as mtools.
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils"
pkg_changelog="1.5.8.pl02-4: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# --disable-libreadline: this project's images don't stage readline,
# and interactive dialog-mode line-editing is irrelevant to
# grub-mkrescue's own non-interactive use of xorriso.
pkg_build() {
	# -D__STDC_NO_VLA__=1: glibc's <regex.h> declares regexec()'s
	# regmatch_t parameter with a real C99 VLA-in-prototype size
	# expression referencing the NEXT parameter --
	#   regmatch_t __pmatch[_Restrict_arr_ _REGEX_NELTS(__nmatch)]
	# -- and TCC's parser rejects it outright:
	#   /usr/include/regex.h:682: error: '__nmatch' undeclared
	# The header already has an #ifndef __STDC_NO_VLA__ branch for
	# exactly this situation (a compiler without VLA support), so
	# saying so accurately steers it onto a form TCC parses, with no
	# hand-redeclaration of regex_t/regexec/regfree needed. This
	# project hit the identical wall in logstore.c; see CLAUDE.md's
	# environment notes.
	CC=tcc CFLAGS="-D__STDC_NO_VLA__=1" ./configure --prefix=/usr --disable-libreadline
	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share"
}
