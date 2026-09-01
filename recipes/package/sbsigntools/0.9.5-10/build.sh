#
# sbsigntools -- sbsign/sbverify/sbattach/sbvarsign/sbsiglist/sbkeysync,
# for signing/verifying UEFI PE/COFF binaries for Secure Boot (Part 5,
# bare-metal-readiness plan). Canonical upstream confirmed directly
# (not guessed): the original Canonical/Jeremy Kerr tree is dead; the
# real, currently-maintained fork is James Bottomley's, at
# git.kernel.org/.../jejb/sbsigntools.git -- confirmed via that repo's
# own README ("The current maintained fork resides at:..."), its
# configure.ac maintainer contact, and independent corroboration from
# GNU Guix's own package definition sourcing the exact same URL/tag.
# v0.9.5 is upstream's newest real tag (confirmed via the live tag
# list), and upstream ships NO release tarball at all -- only git tags
# -- so pkg_source below fetches a real git-archive snapshot from
# cgit's own /snapshot/ endpoint instead (a real, byte-stable tarball
# of that exact tag's tree, the same shape cix.recipe's own
# self-hosted-gitea archive-download already relies on for the same
# reason).
#
# Second pkg_source entry: sbsigntools vendors CCAN (a small C utility
# library) as a git submodule (lib/ccan.git -> git.ozlabs.org/~ccan/
# ccan, confirmed via .gitmodules), pinned at commit b1f28e1 (confirmed
# by reading the real submodule gitlink in the sbsigntools tree at tag
# v0.9.5). A plain git-archive snapshot of a submodule reference isn't
# included in the parent repo's own snapshot, so it's fetched
# separately here. git.ozlabs.org's own gitweb snapshot endpoint (query-
# string-only URL, no clean trailing path segment) works but produces
# an unusable basename for this project's own /build/extra/<basename>
# convention -- fetched instead from rustyrussell/ccan on GitHub (ccan's
# own real author's mirror), same exact commit, confirmed byte-
# identical (matching sha256) against git.ozlabs.org's own snapshot,
# with a clean URL whose basename is the plain commit hash "b1f28e1".
#
pkg_name="sbsigntools"
pkg_version="0.9.5-10"
pkg_source="https://git.kernel.org/pub/scm/linux/kernel/git/jejb/sbsigntools.git/snapshot/sbsigntools-v0.9.5.tar.gz https://codeload.github.com/rustyrussell/ccan/tar.gz/b1f28e1"
pkg_sha256="88ffb0eead3687bed6b0c741e5f440d8f00978493794fa1d11a32bb6a316f7cf 79f709f16f6223c6d464fe17ea0dc4432ee67abaad575ed35b000a85b998f4f7"
pkg_artifact_sha256="9c4c66535c20d981d8984ce3a9baf2ecaa64bd2b9b82c4f8f6c701e1c0b4cdc5"
pkg_depends="gnu-efi libuuid binutils-dev openssl"

# ADR-0199/0209: the build environment is composed from exactly
# these and nothing else -- there is no fallback to inherit a missing
# tool from (#168). Revision bumped purely to carry this: a recipe
# version is immutable once published, so it could never reach a host
# that already has 0.9.5.
# Autotools regeneration from a git snapshot: aclocal/autoheader/autoconf/automake all run before configure, so perl/m4/autoconf/automake are real build inputs. tar unpacks the second declared source (ccan) in pkg_build. gnu-efi, libuuid and binutils-dev appear here as well as in pkg_depends because a composed build environment is built from pkg_build_depends ALONE -- their headers and libraries have to be present to compile against, not merely installed alongside the result.
# gzip: pkg_build() runs `tar xzf` on the ccan source, and the z flag
# makes tar spawn gzip -- so gzip is a build input even though this
# recipe never names it. -2 declared tar and not gzip, and failed with
#   tar (child): gzip: Cannot exec: No such file or directory
# Indirect tools are the recurring gap in these declarations: what a
# recipe writes is not the whole of what it runs.
# getopt: ccan's create-ccan-tree parses its own arguments with
#   getopt -o ab: --long copy-all,build-type: ...
# and nothing in this project shipped GNU getopt, so -4 failed with
#   create-ccan-tree: line 21: getopt: command not found
# Bash's builtin getopts is not a substitute -- it has no --long
# support, which is exactly what that call uses. Added as a real
# package (getopt 2.42.2-1, from the same util-linux source libuuid and
# libblkid already build) rather than patching the argument parsing out
# of third-party source to route around a missing tool.
# openssl: -7 got all the way through the ccan tree and into
# configure, then stopped at
#   checking for libcrypto >= 3.0.0... no
#   checking for libcrypto... no
#   configure: error: libcrypto (from the OpenSSL package) is required
# configure.ac finds libcrypto ONLY through PKG_CHECK_MODULES, so this
# is a question about a .pc file, not about headers or libcrypto.so --
# both of which openssl 3.0.20 already shipped correctly. That package
# deliberately deleted its pkgconfig directory, on a stated premise
# ("nothing here ... uses pkg-config") that this recipe is the
# counter-example to. Fixed in openssl 3.0.20-2, which installs its
# libraries where they actually belong and lets OpenSSL generate a
# truthful libcrypto.pc, rather than by teaching this recipe to work
# around a missing one.
#
# It appears in pkg_depends as well: sbsign links libcrypto, so the
# installed binary needs libcrypto.so.3 present at run time, not only
# at build time.
#
# The two checks that follow libcrypto in configure.ac were verified
# against the real package contents before this revision was built,
# rather than discovered one build at a time:
#   PKG_CHECK_MODULES(uuid, uuid)  -- libuuid 2.42.2 already ships
#     usr/lib/pkgconfig/uuid.pc, on pkgconf's real search path
#   the gnu-efi crt scan            -- gnu-efi 3.0.18-4 already ships
#     usr/lib/crt0-efi-x86_64.o, which is on configure.ac's own fixed
#     list of candidate paths, and usr/include/efi/efi.h to match its
#     -I/usr/include/efi
pkg_build_depends="bash coreutils make tcc linux-headers sed grep gawk binutils m4 perl autoconf automake tar gzip getopt findutils diffutils pkgconf gnu-efi libuuid binutils-dev openssl"
pkg_changelog="0.9.5-10: libc-dev retired; linux-headers declared for the kernel uapi headers glibc's own limits.h needs (#187)"

# autogen.sh is not run verbatim -- its own real steps are replicated
# here directly instead of patching the fetched script in place, since
# two of its steps (AUTHORS/ChangeLog generation) call `git log`
# against a live .git history this tarball-based fetch genuinely
# doesn't have (confirmed: no .git directory exists after extracting a
# cgit snapshot). Every other step -- the ccan-tree materialization,
# aclocal/autoheader/autoconf/automake -- is the exact same real
# sequence autogen.sh itself runs (confirmed by reading it directly),
# just with AUTHORS/ChangeLog replaced by minimal static files instead
# of a git-log call that would otherwise hard-fail the build. Neither
# file is read by the build itself (configure.ac/Makefile.am don't
# reference either) -- purely informational metadata a real release
# tarball would normally ship pre-generated.
# --std=gnu99: sbsigntool's own AM_CFLAGS spells the C-standard flag
# with TWO leading dashes. GCC accepts that as a synonym for -std=;
# TCC does not, and rejects it outright rather than ignoring it:
#   tcc: error: invalid option -- '--std=gnu99'
# -8 died there, on every source file in src/, immediately after
# configure had finally succeeded. Probed directly to establish the
# exact boundary rather than inferring it: `--std=gnu99` fails,
# `-std=gnu99` is accepted, and -Wall, -Wextra and -Werror are all
# accepted as-is. So one spelling is the whole incompatibility.
#
# Fixed with a thin CC wrapper -- the same "toolwrap" pattern
# chrony.recipe uses to strip a bare -pthread, and procps.recipe uses
# for its shebang gap. It rewrites --std=X to -std=X and passes
# everything else through untouched, so it changes a spelling and not a
# single compile semantic.
#
# The considered alternative was overriding AM_CFLAGS on the make
# command line. Rejected: a command-line variable beats every makefile
# assignment including the `AM_CFLAGS += -DOPENSSL_API_COMPAT=...` that
# src/Makefile.am adds under `if HAVE_OPENSSL3`, so the override would
# have to restate that define itself -- copying upstream's own
# conditional flag logic into this recipe as a second source of truth,
# silently wrong the moment upstream changes it. The wrapper leaves
# upstream's logic authoritative.
#
# -D__STDC_NO_VLA__=1 moves into the wrapper too, so the regex fix and
# the --std fix travel together on every invocation instead of being
# split between a CC string and a flag list.
pkg_build() {
	mkdir -p /build/toolwrap
	cat > /build/toolwrap/tcc-sbsign <<'WRAP'
#!/usr/bin/bash
args=()
for a in "$@"; do
	case "$a" in
	--std=*) args+=("${a#-}") ;;
	*)       args+=("$a") ;;
	esac
done
exec /usr/bin/tcc -D__STDC_NO_VLA__=1 "${args[@]}"
WRAP
	chmod +x /build/toolwrap/tcc-sbsign

	mkdir -p lib
	rm -rf lib/ccan.git
	tar xzf /build/extra/b1f28e1 -C lib
	mv lib/ccan-b1f28e1 lib/ccan.git

	# Invoked through bash explicitly rather than by its shebang.
	# ccan's create-ccan-tree starts "#!/bin/bash", and this project's
	# images have no /bin/bash: the bash package installs usr/bin/bash
	# and a bin/sh symlink, nothing else (confirmed from the package's
	# own file list). The shebang therefore fails at execve and bash
	# reports it as
	#   create-ccan-tree: cannot execute: required file not found
	# -- exit 127, which reads like a missing script rather than a
	# missing interpreter. Same class as this project's own
	# "/usr/bin/<tool>, never /bin/<tool>" rule; the difference is that
	# here the path is inside third-party source, so the call site is
	# what has to change.
	# CC=tcc exported into create-ccan-tree: the script runs ccan's own
	# make to build ccan_depends and tools/configurator, and ccan's
	# Makefile relies on make's BUILT-IN default of CC=cc. A composed
	# build environment has tcc and gcc but no "cc" at all, so -5 died
	# with
	#   make: cc: No such file or directory
	#   make: *** No rule to make target 'tools/configurator/configurator'
	#
	# The absence of a bare "cc" is deliberate and worth keeping: this
	# project has already shipped an sshd whose configure reported PAM
	# support and whose ELF carried no libpam, because a bare "cc"
	# silently resolved to a real GCC instead of TCC. Naming the
	# compiler here fixes the build without reintroducing that
	# ambiguity.
	# The define is baked into CC rather than passed as CFLAGS: ccan's
	# own Makefile assigns CFLAGS itself, which would override an
	# environment value, whereas a flag attached to the compiler
	# travels with every invocation.
	#
	# -D__STDC_NO_VLA__=1: glibc's <regex.h> declares regexec()'s
	# regmatch_t parameter with a C99 VLA-in-prototype size expression
	# referencing the NEXT parameter, and TCC rejects it --
	#   /usr/include/regex.h:682: error: '__nmatch' undeclared
	# The header has its own #ifndef __STDC_NO_VLA__ branch for a
	# compiler without VLA support, so saying so accurately steers it
	# onto a form TCC parses. Same gap xorriso 1.5.8.pl02-3 and
	# logstore.c both hit; see CLAUDE.md's environment notes.
	CC=/build/toolwrap/tcc-sbsign bash lib/ccan.git/tools/create-ccan-tree --build-type=automake lib/ccan \
		talloc read_write_all build_assert array_size endian

	printf 'Authors of sbsigntool:\n\n' > AUTHORS
	: > ChangeLog

	aclocal
	autoheader
	autoconf
	automake --add-missing -Wno-portability

	CC=/build/toolwrap/tcc-sbsign ./configure --prefix=/usr
	# SUBDIRS override: real, upstream SUBDIRS is "lib/ccan src docs
	# tests" -- docs needs help2man (not staged anywhere in this
	# project, and would only regenerate man pages pkg_install() below
	# already discards) and tests is irrelevant to actually installing
	# the tools. Confirmed real: the full unrestricted `make` genuinely
	# builds and links all 6 real binaries (sbsign/sbverify/sbattach/
	# sbvarsign/sbsiglist/sbkeysync) successfully before ever reaching
	# docs -- this override only skips the two subdirectories that were
	# never needed, not a workaround for a real build failure in
	# lib/ccan or src themselves.
	make -j"$(nproc)" SUBDIRS="lib/ccan src"
}

pkg_install() {
	make install SUBDIRS="lib/ccan src" DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man"
}
