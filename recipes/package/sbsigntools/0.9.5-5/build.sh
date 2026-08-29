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
pkg_version="0.9.5-5"
pkg_source="https://git.kernel.org/pub/scm/linux/kernel/git/jejb/sbsigntools.git/snapshot/sbsigntools-v0.9.5.tar.gz https://codeload.github.com/rustyrussell/ccan/tar.gz/b1f28e1"
pkg_sha256="88ffb0eead3687bed6b0c741e5f440d8f00978493794fa1d11a32bb6a316f7cf 79f709f16f6223c6d464fe17ea0dc4432ee67abaad575ed35b000a85b998f4f7"
pkg_depends="gnu-efi libuuid binutils-dev"

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
pkg_build_depends="bash coreutils make tcc libc-dev sed grep gawk binutils m4 perl autoconf automake tar gzip getopt findutils diffutils pkgconf gnu-efi libuuid binutils-dev"

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
pkg_build() {
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
	bash lib/ccan.git/tools/create-ccan-tree --build-type=automake lib/ccan \
		talloc read_write_all build_assert array_size endian

	printf 'Authors of sbsigntool:\n\n' > AUTHORS
	: > ChangeLog

	aclocal
	autoheader
	autoconf
	automake --add-missing -Wno-portability

	CC=tcc ./configure --prefix=/usr
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
