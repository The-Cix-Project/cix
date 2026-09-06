#
# perl -- Perl 5. Needed as a real runtime by autoconf/automake (both
# already hardcode #!/usr/bin/perl into their generated tools -- see
# autoconf.recipe/automake.recipe's own pkg_depends="... perl ..."),
# and useful as its own real scripting language for anything built in
# a "dev" image.
#
# Source is CPAN's own canonical distribution point, checksum verified
# against a second, independent CPAN mirror (cpan.metacpan.org's own
# authors/id path) -- byte-identical, same sha256. (5.40.1, not 5.40.0:
# an initial guess at .0 was corrected after failing to find a second
# independent source to cross-check it against; .1 is the real current
# stable patch release and has one.)
#
pkg_name="perl"
pkg_version="5.40.1-7"
pkg_source="https://www.cpan.org/src/5.0/perl-5.40.1.tar.gz"
pkg_sha256="02f8c45bb379ed0c3de7514fad48c714fd46be8f0b536bfd5320050165a1ee26"
pkg_artifact_sha256="4eed9d1a24e93061c32477ee87f7bc4900de8aef1ef39ab37d1f41dde40f6d39"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/perl-5.40.1.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends="libxcrypt"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
# gcc rather than tcc, and binutils because gcc drives a real as/ld.
#
pkg_toolchain="gcc"
pkg_toolchain_reason="codegen/parse: an XS module's link step crashes tcc itself; miniperl builds correctly since #216 was fixed"
pkg_build_depends="gcc binutils libxcrypt make linux-headers bash coreutils sed grep gawk findutils diffutils"
pkg_changelog="5.40.1-7: installs with perl own install.perl target instead of install, so installman never runs. Every previous revision put 943 preformatted man pages directly in the image root -- reported by an operator running ls / in the jumpbox container (#267). Not fixable through Configure: perl records none as an empty installman1dir, and installman prepends DESTDIR to it BEFORE testing for that empty marker, so DESTDIR turns no man pages into the destdir root itself. A gate now fails the build if any regular file lands at the top of the destdir. 5.40.1-6: declare pkg_toolchain=gcc and its reason (#222, ADR-0226)"

#
# Why gcc, when this project's default is TCC.
#
# Tier 3 of the 3-tier TCC policy (CLAUDE.md): Cix's own code is always
# TCC; third-party recipes are TCC by default and get real effort; a
# short, explicit, declared exception list exists for cases where that
# effort has been spent and the answer is still no. perl joins kernel,
# openssl and gcc on that list. It is a declared capability loss, which
# ADR-0222 already covers -- and NOT a provenance compromise: the gcc
# used here is Cix's own, three-stage bootstrapped with stage 2 proven
# byte-identical to stage 3. Nothing foreign enters this build.
#
# The effort was genuinely spent. Three revisions built perl with TCC,
# and each one found and fixed a real, precisely-isolated gap:
#
#   -1  perl.h's PERL_DIAG_STR_(x) expands to a PARENTHESIZED string-
#       literal concatenation, `("" x "")`, used as a char[] array
#       initializer. TCC's array-initializer parser rejects the parens
#       ("character array initializer must be a literal"); GCC accepts
#       them. Isolated to two minimal probes: `char x[] = ("" "s" "");`
#       fails, the identical line without parens succeeds.
#
#   -2  hints/linux.sh sets ccdlflags to `-Wl,-E` (export-dynamic),
#       which TCC's linker frontend does not accept under that spelling.
#       Load-bearing, not spurious: stripping it linked fine and then
#       every dynamically-loaded XS module failed at runtime with
#       "Cwd.so: undefined symbol: Perl_croak_nocontext". TCC has the
#       capability under its own name, -rdynamic, so a wrapper
#       translated it.
#
#   -3  That wrapper translated a bare `-E` as well, which is a
#       different flag entirely -- preprocess-and-stop, not
#       export-dynamic. Configure's preprocessor probe got handed a link
#       flag, concluded no C preprocessor existed, and stopped to ask a
#       human to name one. Two builds sat at that prompt for 110 and 125
#       minutes (#214).
#
# With -3 in place the build finally ran to completion territory --
# through Configure, mktables, the whole Unicode property pass -- and
# then miniperl segfaulted. Deterministically: same faulting address
# (ip 0x697d9a), same offset-12 read off a null-looking pointer, across
# separate processes. miniperl is the interpreter perl builds first and
# then uses to build the rest of itself, so this stops perl dead.
#
# That is a compiler codegen fault, and there is no recipe-level fix for
# it. It is tracked as #216 and is worth chasing on its own merits,
# because this project has twice been bitten by the QUIET version of the
# same class of bug -- TCC-built tar silently listing one member per
# archive while exiting 0 (#122), and squashfs-tools silently
# byte-swapping every on-disk field while reporting success. A segfault
# is the loud version. Fixing #216 is not a prerequisite for perl, but
# perl should return to TCC once it is fixed.
#
# gcc is invoked by ABSOLUTE path. A bare `gcc` off $PATH makes gcc
# compute its own installation prefix relatively, which breaks cc1
# invocation with a misleading "posix_spawnp: No such file or directory"
# (CLAUDE.md).
#
# -Dcc= is Perl's own documented Configure flag for choosing a compiler.
# Configure is hand-rolled, not autotools, and under -des it does not
# reliably honor a bare CC= from the environment the way ./configure
# does. -Dusethreads matches the build host's own perl.
#
pkg_build() {
	#
	# stdin closed: Configure is interactive by nature. `-des` accepts
	# every default it *can* answer itself, but a probe that fails
	# outright still falls through to a prompt, and a prompt with a
	# terminal-less stdin inherited from the builder waits forever --
	# which is exactly how #214 presented (110 and 125 minutes, no
	# output). With stdin at /dev/null such a prompt reads EOF and
	# Configure dies, so the same class of fault fails loudly and fast
	# instead of silently holding a build slot.
	#
	./Configure -des -Dcc=/usr/bin/gcc -Dprefix=/usr -Dusethreads </dev/null

	#
	# Verify the preprocessor Configure actually RECORDED, not that
	# Configure merely finished. Configure caches its answers in
	# config.sh and a wrong one is accepted silently, then produces
	# broken output much later (the #113 failure shape). So read the
	# recorded command back out and preprocess a real file with it:
	# a string match would only prove the variable is set, this proves
	# the thing it names works.
	#
	for v in cpprun cppstdin; do
		cmd=$(sed -n "s/^$v='\(.*\)'$/\1/p" config.sh)
		if [ -z "$cmd" ]; then
			echo "FATAL: Configure recorded no $v in config.sh" >&2
			exit 1
		fi
		echo "  config.sh $v=$cmd"
		printf '#define CIX_PROBE 1\nint cix = CIX_PROBE;\n' > /run/cix-cpp-probe.c
		if ! eval "$cmd" /run/cix-cpp-probe.c > /run/cix-cpp-probe.out 2>/run/cix-cpp-probe.err; then
			echo "FATAL: recorded $v failed to run:" >&2
			cat /run/cix-cpp-probe.err >&2
			exit 1
		fi
		if ! grep -q 'int cix = 1;' /run/cix-cpp-probe.out; then
			echo "FATAL: recorded $v ran but did not preprocess." >&2
			echo "       expected 'int cix = 1;' in its output, got:" >&2
			head -20 /run/cix-cpp-probe.out >&2
			exit 1
		fi
		echo "  $v preprocesses correctly"
	done

	make -j"$(nproc)"

	#
	# Run the perl that was just built, and load a real XS module
	# through it.
	#
	# A `make` that exits 0 is not evidence the binary works -- #216 is
	# exactly that gap seen from the other side, and #122 (tar listing
	# one member per archive, exit 0) is the same lesson from a
	# different package. This gate executes the interpreter and makes it
	# dlopen a compiled module, which is also the only real test of the
	# export-dynamic linkage that revision -2 found the hard way: the
	# link succeeded there too, and every XS module still failed at
	# runtime with "undefined symbol: Perl_croak_nocontext".
	#
	# Cwd is core, always built, and genuinely XS.
	#
	out=$(./perl -Ilib -e 'use Cwd; print "XS-OK:", (Cwd::getcwd() ne "" ? 1 : 0);' 2>&1) || {
		echo "FATAL: the perl just built failed to run:" >&2
		echo "$out" >&2
		exit 1
	}
	case "$out" in
	*XS-OK:1*) echo "  built perl runs and loads a real XS module" ;;
	*)
		echo "FATAL: built perl ran but its XS load did not report success." >&2
		echo "       got: $out" >&2
		exit 1
		;;
	esac

	#
	# crypt() actually works.
	#
	# Configure probes for -lcrypt and silently carries on without it,
	# so a perl with no crypt() support builds, installs and passes
	# every check above. Revision -4 was exactly that: libxcrypt was
	# not declared, so it was not in the composed sandbox (ADR-0199),
	# and nothing said so.
	#
	# This runs the real builtin through the perl just built. crypt()
	# with a known salt has a known answer, so it proves the library is
	# linked and working, not merely present.
	#
	cout=$(./perl -Ilib -e 'my $h = crypt("password","ab"); print "CRYPT:", (defined $h && $h =~ /^ab/ ? 1 : 0);' 2>&1) || {
		echo "FATAL: crypt() failed in the perl just built:" >&2
		echo "$cout" >&2
		exit 1
	}
	case "$cout" in
	*CRYPT:1*) echo "  built perl has working crypt()" ;;
	*)
		echo "FATAL: built perl has no working crypt() -- libxcrypt was not linked." >&2
		echo "       got: $cout" >&2
		exit 1
		;;
	esac
}

# Confirmed via ldd against the real perl binary and every one of its
# core XS (compiled) modules under lib/perl5: everything links only
# against libm, libcrypt (crypt()/password hashing, a real, separate
# library, not part of libc itself), and libc.
#
# libcrypt now arrives as a declared dependency on libxcrypt, not as a
# copy. Revisions up to -4 did `cp /lib/x86_64-linux-gnu/libcrypt.so.1`
# from the build host, which was wrong twice over: no Cix package ships
# a libcrypt.so.1 at all (libxcrypt provides .so.2), so on a composed
# build environment holding exactly the declared tools (ADR-0199) the
# file was simply absent and the build died on this line -- and had it
# been present, it would have been the host's own copy, staged into a
# package without provenance. Declaring the dependency fixes both, and
# leaves one owner for the library instead of two. usr/lib/perl5
# (the real standard library -- core modules, not documentation) is
# kept in full; it's what makes perl actually usable, the same
# "load-bearing runtime data" reasoning autoconf.recipe's own
# usr/share/autoconf staging already established.
#
# --- Man pages, and why this uses install.perl (#267) ---------------
#
# Every revision before this one installed 943 preformatted man pages
# straight into the IMAGE ROOT. An operator found it by running `ls /`
# in the jumpbox container and seeing a screen of `Time::tm.0`,
# `perlre.0`, `Encode::JP.0`. Every top-level file in that image was
# one of these.
#
# The old `rm -rf "$PKG_DESTDIR/usr/share/man"` here shows man pages
# were already judged unwanted -- no image in this project's own set
# ships man infrastructure for anything else. It removed a directory
# the pages had never been written to.
#
# It is NOT fixable through Configure, which is the obvious thing to
# try and would have cost a full perl build to disprove. Telling
# Configure `-Dman1dir=none` is how perl records "no man pages", and
# what it records is an EMPTY installman1dir -- which is already what
# this build has (`perl -V:installman1dir` on the installed binary
# reports ''). The failure is in installman's own ordering, read out
# of perl-5.40.1/installman directly:
#
#     $opts{man1dir} //= $opts{destdir} . $Config{installman1dir};
#     ...
#     if ($mandir eq ' ' or $mandir eq '') { return }
#
# The empty-means-none marker is tested AFTER destdir has been
# prepended to it, so with DESTDIR set the marker is destroyed: "" and
# " " both become "$PKG_DESTDIR", which is a real directory, and every
# page is written into it. A package built without DESTDIR would never
# show this, which is why it is upstream and unnoticed.
#
# So do not run installman at all. `install.perl` is perl's own target
# for exactly this split -- `make install` is `install.perl` plus
# `install.man`, and only the latter formats and installs man pages
# (Makefile.SH: `install.man: all installman`). The binaries, the
# standard library and the packlist all come from installperl, which
# `install.perl` runs unchanged.
pkg_install() {
	#
	# install.perl, NOT install: see the comment above. `make install`
	# would additionally run installman, which cannot be told to skip
	# when DESTDIR is set.
	#
	make install.perl DESTDIR="$PKG_DESTDIR"

	#
	# Nothing belongs at the top of a package's destdir -- every file
	# a package ships lives under a real directory. This is a class of
	# packaging error that builds cleanly, checksums correctly,
	# installs without complaint and is only ever noticed by a human
	# running `ls /`, so it gets a gate rather than trust.
	#
	strays=$(find "$PKG_DESTDIR" -maxdepth 1 -type f | wc -l)
	if [ "$strays" -ne 0 ]; then
		echo "FATAL: $strays file(s) landed at the top of the destdir." >&2
		echo "       These would be installed into the image ROOT (#267)." >&2
		find "$PKG_DESTDIR" -maxdepth 1 -type f | head -10 >&2
		exit 1
	fi
	echo "  destdir root is clean -- no man pages leaked into /"

	#
	# And the standard library really did install, since the whole
	# change here is which install target runs. A perl with no
	# usr/lib/perl5 would still satisfy the check above.
	#
	if [ ! -d "$PKG_DESTDIR/usr/lib/perl5" ]; then
		echo "FATAL: install.perl produced no usr/lib/perl5" >&2
		exit 1
	fi
	echo "  standard library installed: $(find "$PKG_DESTDIR/usr/lib/perl5" -type f | wc -l) files"
}
