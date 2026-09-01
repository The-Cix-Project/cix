#
# bird -- the BIRD Internet Routing Daemon (v2). Same recipe contract
# as bash.recipe -- see that file's own header comment for the
# metadata-scanner-vs-sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their bird2 package -- the
# exact, unmodified upstream release tarball (Debian repackages
# nothing into "orig" tarballs, by their own packaging policy), used
# here rather than bird.network.cz's own download page directly
# because that page 403s any non-browser User-Agent. Checksum verified
# against Debian's own published .dsc Checksums-Sha256 field for this
# exact file, not just self-computed.
#
pkg_name="bird"
pkg_version="2.19.1-8"
#
# Source moved from Debian's pool to BIRD's own release archive,
# because the pool URL now 404s: Debian's pool holds only what is
# CURRENTLY packaged, so a superseded version simply disappears. That
# makes it unsuitable as a pkg_source -- the recipe stops being
# buildable through no change of its own.
#
# Note bird.network.cz 403s any non-browser User-Agent (already
# recorded in this recipe's history); bird.nic.cz serves the same
# archive and does not. Checksum recomputed from the downloaded bytes,
# since this is upstream's .tar.gz rather than Debian's repackaged
# .orig.tar.xz -- a different byte sequence for the same release.
#
pkg_source="https://bird.nic.cz/download/bird-2.19.1.tar.gz"
pkg_sha256="e91aedac07da4f2718f269f969f65dbcd12f4b58248a784fd32e587b7b4dd7e4"
pkg_depends="ncurses readline"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="gcc binutils make linux-headers bash coreutils sed grep gawk findutils diffutils autoconf automake libtool m4 bison flex ncurses readline"
pkg_changelog="2.19.1-8: declares readline, and stops copying libreadline out of the build sandbox's ambient filesystem into the package -- a Build Provenance Mandate violation that checksum verification could never have caught, since a checksum attests to bytes and not to their origin. readline is a real Cix package now. 2.19.1-7: declares ncurses. bird's client (birdc) needs it, and configure refuses rather than degrading: 'The client requires ncurses library.' Building without the client was the alternative and was not taken -- birdc is how an operator talks to the daemon, so dropping it would be a real capability loss to declare, not a build convenience. 2.19.1-6: declares bison and flex. bird has its own configuration-file grammar and lexer, and configure stops with 'Bison is missing.' without them -- a gap only visible once the compiler identity check stopped failing first, which is the ordinary shape of a composed build environment (ADR-0199) surfacing one undeclared tool at a time. 2.19.1-5: built with gcc, a declared Tier 3 exception (ADR-0222). bird's configure rejects any compiler that is not GCC by identity rather than by capability, so the C11 that the compiler upgrade brought was never going to help -- confirmed by retrying against 0.9.28rc and getting the byte-identical refusal. Adds a gate that configure recorded the compiler asked for, and another that runs the built binary. 2.19.1-4: first attempt since the compiler upgrade (ADR-0223). 2.19.1-3 failed at 'configure: error: This program requires the GNU C Compiler' -- an identity check, not a capability gap, so it was never going to be fixed by adding features. Retried because the upgraded tcc may present itself differently; if it still refuses, the answer is a declared Tier 3 gcc exception rather than more guessing. 2.19.1-3: sources from BIRD's own archive instead of Debian's pool, which 404s once a version is superseded; declares build tools (#206)"

# Confirmed directly: this tarball (Debian's own "orig" repackaging of
# BIRD's real upstream source) ships configure.ac but no pre-generated
# ./configure or sysdep/autoconf.h.in -- despite being a real release
# tarball, not a git checkout (it even still carries .gitlab-ci.yml).
# `autoreconf -fi` (force regeneration; install any missing standard
# auxiliary files) runs whichever of autoconf/autoheader/aclocal are
# actually needed, in the right order -- found empirically after
# discovering the individual tools one failure at a time (autoconf
# alone gets ./configure but not sysdep/autoconf.h, which needs
# autoheader too). --prefix=/usr matches every other recipe in this
# set; BIRD's own build otherwise defaults to installing under
# /usr/local/{sbin,etc}, which would land in the wrong place once
# merged into the shared base image. --runstatedir=/run: confirmed
# directly (grep against this exact release's own configure.ac/
# Makefile.in) that BIRD's own CONTROL_SOCKET is literally
# "$(runstatedir)/bird.ctl" -- without this override, autoconf's
# default runstatedir is ${localstatedir}/run, and --prefix=/usr alone
# makes that /usr/var/run (real, confirmed live, ADR-0041/Phase 24: no
# image ships that path, so birdc's control socket had nowhere to go).
# ADR-0041 also gives every image a real, empty /run generically, but
# fixing BIRD's own default is the actual root fix -- /usr/var/run
# still wouldn't exist without it even with a populated /run elsewhere.
#
# Built with gcc: a declared Tier 3 exception (ADR-0222), and the
# reason is unusual enough to be worth stating plainly.
#
# bird's configure does not test for a capability. It tests for an
# identity:
#
#     configure: error: This program requires the GNU C Compiler.
#
# So no amount of compiler work makes this pass. Re-measured after the
# 0.9.28rc upgrade (ADR-0223), on the chance the new compiler presented
# itself differently -- it does not, and the failure is byte-identical.
# That is worth having tried: every other package in #212 was blocked
# by a real capability gap that the upgrade closed, and assuming this
# one was the same would have been wrong in the other direction.
#
# The alternative was patching configure to assert compatibility. That
# is a claim this project cannot honestly make on bird's behalf --
# bird asks for GCC and we would be telling it a different compiler is
# GCC, with a routing daemon as the thing that finds out. Using the
# compiler it asks for is the smaller, more honest move.
#
# Not a provenance compromise: this gcc is Cix's own, three-stage
# bootstrapped with stage 2 proven byte-identical to stage 3.
#
# Absolute path, never a bare `gcc` off $PATH: a bare invocation makes
# gcc compute its installation prefix relatively and fail cc1 with a
# misleading "posix_spawnp: No such file or directory" (CLAUDE.md).
#
pkg_build() {
	autoreconf -fi
	CC=/usr/bin/gcc ./configure --prefix=/usr --runstatedir=/run

	#
	# Confirm configure recorded the compiler we asked for. bird's
	# configure is what rejected tcc, so a silent fallback to some
	# other compiler is exactly the failure worth catching here.
	#
	grep -E "^CC = ?/usr/bin/gcc" Makefile >/dev/null || {
		echo "bird: configure did not record /usr/bin/gcc as CC" >&2
		grep -E "^CC ?=" Makefile >&2 || true
		exit 1
	}

	make -j"$(nproc)"

	#
	# Run what was built. A make that exits 0 is not evidence the
	# binary works -- the lesson of #122 and #216 both.
	#
	out=$(./bird --version 2>&1) || {
		echo "bird: the binary just built does not run:" >&2
		echo "$out" >&2
		exit 1
	}
	case "$out" in
	*2.19.1*) echo "  built bird runs: $out" ;;
	*)
		echo "bird: --version did not report the expected version, got: $out" >&2
		exit 1
		;;
	esac
}

# `bird`/`birdcl` need nothing beyond what pkg_seed_image_baseline()'s
# global bootstrap set already covers, but `birdc` (the readline-
# enabled control client -- interactive line editing/history) links
# libreadline, confirmed via ldd against the real build -- never
# exercised until this recipe's own control client was actually run,
# not caught up front. libtinfo (readline's own transitive dependency)
# is already part of that global set; only libreadline itself needed
# staging here.
#
# No library is copied out of the build sandbox here any more.
#
# Earlier revisions did:
#
#     cp -a /lib/x86_64-linux-gnu/libreadline.so.8 ... "$PKG_DESTDIR/..."
#
# which took whatever libreadline happened to be lying in the build
# environment and shipped it inside a Cix package. That is the exact
# thing the Build Provenance Mandate forbids -- host content entering a
# package -- and it verified perfectly by checksum the whole time,
# because a checksum attests to bytes, not to where they came from.
#
# readline is a real Cix package now (readline@8.3-2, built for this),
# declared in pkg_depends, so the dependency arrives the way every
# other one does.
#
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
}
