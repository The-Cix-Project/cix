#
# 2.55.0-8: declares zlib as a RUNTIME dependency (#455).
#
# 2.55.0-7 could never install. Its own elfcheck gate refused it:
#
#   usr/bin/git links "libz.so.1", which nothing it declares provides
#   -- add the package supplying it to pkg_depends= (having it in
#   pkg_build_depends= only puts it in the build sandbox, so the link
#   is recorded and the dependency is not)
#
# zlib was listed in pkg_build_depends and not in pkg_depends, which
# is exactly the distinction that message exists to draw: the build
# sandbox had it, so configure found it and the binary linked against
# it, and nothing recorded that the installed package needs it at run
# time. An image built from this recipe would have carried a git that
# could not start.
#
# git links zlib unconditionally -- it is not optional and has no
# NO_ZLIB knob, unlike the NO_RUST/NO_GETTEXT/NO_TCLTK switches below
# which exist precisely because those parts ARE optional. So the fix is
# to declare it, not to disable it.
#
# Found by a real install rather than by reading: there has never been
# a cached artifact for git at any revision, which is what the failure
# looks like from the outside.
#
pkg_name="git"
pkg_version="2.55.0-8"
pkg_source="https://www.kernel.org/pub/software/scm/git/git-2.55.0.tar.xz"
pkg_sha256="457fdb04dc8728e007d4688695e6912e6f680727920f2a40bf11eacc17505357"
pkg_depends="zlib"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf zlib perl tar"
pkg_changelog="2.55.0-7: declares tar. configure found none, set TAR empty, and the install step's 'tar cf - | tar xof -' degraded into 'cf -' and 'xof -' -- reported as 'cf: command not found', which names neither tar nor the real problem. 2.55.0-6: declares perl -- git's install step builds its Perl modules and died with exit 127 without it, after the build itself had fully succeeded. 2.55.0-5: empties FUZZ_OBJS at source -- a command-line FUZZ_PROGRAMS= did not survive config.mak.autogen. Also drops a duplicated pkg_build_depends. 2.55.0-4: excludes the oss-fuzz harnesses, which link with an option TCC does not implement and which this package does not install. 2.55.0-3: -D__STDC_NO_VLA__=1 for glibc regex.h, whose regexec() prototype TCC cannot parse. 2.55.0-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#

# Run with $PWD already at /build/src (the extracted tarball, one
# leading path component already stripped) and PATH=/usr/bin:/bin -- no
# network access. Git ships a pre-generated ./configure (no autoconf
# regeneration needed); zlib/OpenSSL/PCRE2 headers and libs it links
# against are already part of this toolchain's own wholesale
# /usr/{include,lib} staging (test_image_fixture_stage_toolchain()),
# confirmed directly -- configure found everything it needed with no
# extra staging.
pkg_build() {
	#
	# glibc declares regexec()'s array parameter with a real C99
	# VLA-in-prototype size expression referencing the NEXT parameter,
	# and TCC's parser rejects it outright:
	#
	#   /usr/include/.../regex.h:686: error: '__nmatch' undeclared
	#
	# The header has its own __STDC_NO_VLA__ branch for exactly this
	# situation -- a compiler without VLA support -- which TCC parses
	# fine. Defining it is accurate rather than a trick: TCC really
	# does not support this construct. Same fix as
	# daemon/src/logstore.c's own include block.
	#
	export CPPFLAGS="${CPPFLAGS:+$CPPFLAGS }-D__STDC_NO_VLA__=1"

	CC=tcc ./configure --prefix=/usr
	# git's `all` target builds the oss-fuzz harnesses, which link with

	# -Wl,--allow-multiple-definition -- an option TCC does not implement:

	#

	#   tcc: error: unsupported linker option '--allow-multiple-definition'

	#

	# Those harnesses are fuzzing entry points for git's own CI. They are

	# not installed, nothing here runs them, and building them is not

	# what this package is for. Excluded by name rather than by disabling

	# something broader, so the omission is visible and reversible.

	# git's `all` builds the oss-fuzz harnesses, which link with
	# -Wl,--allow-multiple-definition -- an option TCC does not
	# implement. Passing FUZZ_PROGRAMS= on the make command line did NOT
	# suppress them (configure writes config.mak.autogen, which is
	# included after the command line is applied), so the object list is
	# emptied at source, where nothing can re-derive it.
	#
	# These are fuzzing entry points for git's own CI. They are not
	# installed and nothing here runs them. Removed by name so the
	# omission is visible rather than hidden behind a broader switch.
	sed -i 's|^FUZZ_OBJS += .*$||' Makefile
	if grep -q '^FUZZ_OBJS += ' Makefile; then
		echo "git: a FUZZ_OBJS entry survived the strip" >&2
		exit 1
	fi

	make -j"$(nproc)" NO_RUST=1 NO_GETTEXT=1 NO_TCLTK=1
}

# PKG_DESTDIR is set by cixd itself (daemon/src/pkg.c) -- everything
# written under it is what actually gets merged into the target image
# once this build container exits successfully. Lands the real `git`
# binary at usr/bin/git plus its own libexec/git-core/* helper binaries
# (git-upload-pack, git-receive-pack, git-remote-http, ...) every real
# git operation -- clone, fetch, push, smart-HTTP -- actually needs at
# runtime, not just the one top-level binary.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" prefix=/usr NO_RUST=1 NO_GETTEXT=1 NO_TCLTK=1
}
