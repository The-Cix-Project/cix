#
# openssl -- OpenSSL: the real `openssl` CLI, libssl.so.3/libcrypto.so.3
# runtime libraries, the -lssl/-lcrypto dev symlinks, headers, the
# libcrypto/libssl/openssl pkg-config files, and a real openssl.cnf --
# a genuine from-source build.
#
# This retires openssl-dev.recipe (deleted, not kept alongside this
# one -- One Source of Truth): that recipe's own header comment
# explicitly punted on a real build ("substantial work of its own... a
# bespoke Configure script, not autoconf") and instead copied this dev
# sandbox's own pre-built libssl.so.3/libcrypto.so.3 straight out of
# /lib/x86_64-linux-gnu. That call made sense before this project had
# a real gcc-based "dev" toolchain image to build OpenSSL in (gcc.recipe,
# task #479) -- now that one exists, copying dev-host binaries into a
# recipe's own output is exactly the kind of "atomic OS distribution"
# gap this project's own maxims rule out, so this recipe does the real
# build instead. Also the real fix behind mkbootroot.c's own
# PKI_OPENSSL_BIN staging -- that file still ships a raw dev-host
# /usr/bin/openssl copy today; wiring this recipe's own output into
# a shared host-tools image (task #693) is what actually closes that.
#
# Source is the OpenSSL project's own GitHub Releases asset --
# checksum verified against two independent sources (openssl.org's
# own /source/ tree and this GitHub release) -- byte-identical, same
# sha256. Pointed directly at GitHub rather than openssl.org's own
# "/source/" URL (which this same file used until this line, and
# which is itself now just a 301 redirect to this exact GitHub URL,
# confirmed directly): `cixd`'s own host-side fetch (PKG_CURL_BIN,
# daemon/include/pkg.h -- a fixed /usr/bin/curl on the daemon's own
# root, separate from this recipe's own build output) failed fetching
# through that redirect hop on a real installed box, exit 1
# ("unsupported protocol"), while fetching this direct URL (still
# itself `-L`-followed through GitHub's own further redirect to a
# time-limited signed blob URL, which is why this is the stable
# `releases/download/...` URL and not that ephemeral final one)
# succeeds -- a real, working-source fix, not a preference swap.
#
# --- What changed in -2, and why -------------------------------------
#
# 3.0.20 shipped NO pkg-config files. Its pkg_install() ended with a
# blanket `rm -rf "$PKG_DESTDIR/usr/lib64"` and justified it in a
# comment that said, in as many words, "nothing here links OpenSSL
# statically or uses pkg-config". The second half of that sentence
# stopped being true the moment a real consumer arrived:
# sbsigntools' configure.ac finds libcrypto ONLY through
# PKG_CHECK_MODULES, so against this package it failed with
#   checking for libcrypto >= 3.0.0... no
#   checking for libcrypto... no
#   configure: error: libcrypto (from the OpenSSL package) is required
# even though the headers and libcrypto.so were both present and
# correct. A .pc file is the standard, and only, way an autotools
# consumer asks that question -- so shipping one is the fix, not
# teaching each consumer to work around its absence.
#
# The fix is NOT to keep the generated .pc and patch its libdir line
# afterwards. 3.0.20 built with the Configure default LIBDIR ("lib64",
# confirmed by reading the generated Makefile) and then moved the four
# .so files to lib/x86_64-linux-gnu/ by hand -- so a .pc kept from that
# build would have said `libdir=${exec_prefix}/lib64`, naming a
# directory the recipe itself had just deleted. Rewriting that line
# with sed would work, and would also be a second, hand-maintained
# source of truth about where these libraries live.
#
# Configure takes the answer directly instead: `--libdir` accepts an
# ABSOLUTE path, and OpenSSL's own unix-Makefile.tmpl branches on
# exactly that --
#     if [ -n "$(LIBDIR)" ]; then echo 'libdir=$${exec_prefix}/$(LIBDIR)'
#     else                        echo 'libdir=$(libdir)'
# -- so an absolute --libdir leaves LIBDIR empty and emits the real
# path verbatim. Verified by generating the Makefile both ways and
# reading it: the default gives `LIBDIR=lib64`, and
# `--libdir=/lib/x86_64-linux-gnu` gives `LIBDIR=` plus
# `libdir=/lib/x86_64-linux-gnu`. The libraries are therefore INSTALLED
# where this project actually keeps them, rather than installed
# elsewhere and moved, and OpenSSL generates a truthful .pc with no
# post-processing at all. The hand-written `mv` of the four .so files
# is gone with it.
#
# The .pc files still have to move: they are installed to
# $(libdir)/pkgconfig, and pkgconf's real compiled-in search path here
# is /usr/lib/pkgconfig:/usr/share/pkgconfig -- confirmed directly by
# running `pkg-config --variable pc_path pkg-config` in a real
# container on the installed host, not inferred from its ./configure
# line. Moving a .pc is safe in a way rewriting one is not: every path
# inside it is already absolute, so the file says the same thing from
# either directory.
#
# Libs.private is `-ldl -pthread` (confirmed: CNF_EX_LIBS in the
# generated Makefile). pkg-config only emits it for --static, and
# nothing here links OpenSSL statically, so it is inert today. It is
# worth knowing it is there: TCC does not merely ignore `-pthread`, it
# silently drops the source-file argument (see CLAUDE.md), so a future
# TCC-built consumer that ever asks for --static libcrypto would fail
# in a way that looks nothing like its cause.
#
pkg_name="openssl"
pkg_version="3.0.20-2"
pkg_source="https://github.com/openssl/openssl/releases/download/openssl-3.0.20/openssl-3.0.20.tar.gz"
pkg_sha256="c80a01dfc70ece4dc21168932c37739042d404d46ccc81a5986dd75314ecda6f"
pkg_artifact_sha256="ac36c6adb96d4aca8e9b637a7dac14d29dc101ef41caafe84e0af51305380a0d"

# No pkg_artifact_sha256. 3.0.20's approved exactly the bytes 3.0.20
# produced, and this revision changes both the install layout and the
# file list -- carrying that line forward would approve a byte sequence
# this recipe cannot produce. A new one is only ever written after a
# real Cix host has built and published these bytes.

# perl is deliberately NOT here any more. 3.0.20 carried
# pkg_depends="perl" and said why: "stages a real perl into this same
# build container first -- Configure hard-requires it." That is a
# build-time need, and since ADR-0199 pkg_build_depends is what
# expresses it -- naming it in both makes pkg_depends a stale second
# statement of the same fact and drags a full perl into every image
# that installs openssl. The installed CLI itself needs only the
# libraries this package ships and libc: c_rehash, the one perl
# consumer OpenSSL installs, is dropped below.
pkg_depends=""

# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback to inherit a missing tool
# from (#168).
#
# Read off the real generated Makefile rather than assumed from what
# a build of this size usually needs:
#   bash       pkg_build()/pkg_install() run under it, and every make
#              recipe line here is a shell command
#   coreutils  RM=`rm -f`, RMDIR=rmdir, ECHO=echo, plus the cp/chmod/
#              mv/ln/mkdir/cat the install rules use, and `nproc` below
#   make       the build itself
#   perl       PERL=/usr/bin/perl -- Configure IS perl, and $(PERL)
#              appears in 68 further rules (mkdir-p.pl, the asm
#              generators, configdata.pm)
#   gcc        CC=$(CROSS_COMPILE)gcc, OpenSSL's own linux-x86_64
#              target. See the Tier-3 note below.
#   binutils   AR=ar and RANLIB=ranlib build the static archives, and
#              gcc shells out to as/ld for the perl-generated .s files
#   libc-dev   headers
#   diffutils  `cmp` -- 2149 invocations, one per object: every
#              per-object dependency rule compares the freshly written
#              .d.tmp against the previous .d with cmp. Easy to omit by
#              analogy with other autotools recipes (grub's declaration
#              does not need it) and it would have failed the build.
#
# NOT declared, having checked where they actually appear: sed occurs
# only in generate_crypto_objects, grep/find/xargs only in the FIPS
# module target and `clean`. None is on the path a plain `make` takes,
# and this build enables no FIPS provider.
#
# Sufficiency is enforced by the build itself. Minimality is review,
# not enforcement (ADR-0199).
pkg_build_depends="bash coreutils make perl gcc binutils libc-dev diffutils"

# gcc, not tcc, and deliberately: openssl is one of the small, explicit
# Tier-3 exceptions to this project's TCC-by-default policy (with
# kernel, gnu-efi, python and grub -- ADR-0211), for its own build-image
# reasons. Nothing about this revision revisits that.
#
# ./Configure is OpenSSL's own bespoke, Perl-driven build script -- not
# autotools; confirmed real via a local build in this sandbox, which
# also confirmed no build-time "skip docs" flag exists in this release
# -- install_sw below is what actually avoids the doc tree, not a
# Configure option. Invoked as "perl ./Configure ..." rather than
# "./Configure" directly: the script's own shebang is
# "#!/usr/bin/env perl" (confirmed by reading it), which needs
# /usr/bin/env on PATH -- real on an image that also has coreutils, but
# not guaranteed on a lean composed environment (confirmed the hard
# way: cix-hosttools has perl but no coreutils/env, and execve() on the
# un-runnable script fell back to bash interpreting Configure's own
# Perl source as shell, "use: command not found"). Calling perl
# directly needs nothing beyond what pkg_build_depends already
# guarantees. "linux-x86_64" is the real, exact Configure target this
# platform is (confirmed via a real `uname -m` plus OpenSSL's own
# Configurations/10-main.conf entry for it), not a generic/auto-detect
# target. --openssldir=/usr/lib/ssl matches the real path
# mkbootroot.c's own openssl.cnf staging already uses.
pkg_build() {
	perl ./Configure --prefix=/usr --openssldir=/usr/lib/ssl \
		--libdir=/lib/x86_64-linux-gnu linux-x86_64
	make -j"$(nproc)"
}

# install_sw (OpenSSL's own real, documented "software only" target --
# binaries/libraries/headers/pkg-config, no docs, confirmed via a real
# local build+install in this sandbox) plus install_ssldirs (the real
# openssl.cnf/ct_log_list.cnf/certs+private+misc directory layout --
# this project's own PKI subsystem needs a real openssl.cnf present at
# exactly usr/lib/ssl/openssl.cnf, confirmed via `pki ca bootstrap`
# failing without one). Confirmed live: 3.0.20's build output, which
# this revision changes only in layout, generates a real working
# self-signed cert via `openssl req -x509`.
#
# install_sw depends on install_dev, which is what installs both the
# .pc files and the static archives (confirmed by reading
# Configurations/unix-Makefile.tmpl: "install_sw: install_dev
# install_engines install_modules install_runtime"). The .pc files are
# the point of this revision and are kept; the rest below is dropped
# for the same reasons 3.0.20 dropped it, just named individually now
# that a blanket `rm -rf` of the whole library directory would take the
# real libraries with it:
#   *.a          nothing here links OpenSSL statically
#   c_rehash     a perl wrapper around a symlink-farm convention
#                nothing in this project uses -- and the only reason
#                the installed package would need perl at runtime
#   engines-3    the legacy provider/engine plugins (e.g. padlock).
#   ossl-modules Real, but daemon/src/pki.c only ever needs the default
#                provider, which is built into libcrypto.so.3 itself,
#                not a dynamically-loaded plugin.
#
# The .pc files move from $(libdir)/pkgconfig to usr/lib/pkgconfig
# because that is where pkgconf actually looks; see the header note.
# Both steps fail loudly if their input is missing rather than
# silently producing a package without them -- shipping a quietly
# incomplete package is exactly the failure this revision exists to
# correct.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install_sw
	make DESTDIR="$PKG_DESTDIR" install_ssldirs

	rm -f "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.a
	rm -f "$PKG_DESTDIR/usr/bin/c_rehash"
	rm -rf "$PKG_DESTDIR/lib/x86_64-linux-gnu/engines-3" \
	       "$PKG_DESTDIR/lib/x86_64-linux-gnu/ossl-modules"

	test -f "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig/libcrypto.pc"
	mkdir -p "$PKG_DESTDIR/usr/lib/pkgconfig"
	mv "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"/*.pc \
	   "$PKG_DESTDIR/usr/lib/pkgconfig/"
	rmdir "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"

	# Assert the generated .pc names the directory the libraries were
	# actually installed into. This is the one claim the whole revision
	# rests on, and it is checked with `case` against cat's output
	# rather than grep so that verifying it costs no extra declared
	# build tool.
	case "$(cat "$PKG_DESTDIR/usr/lib/pkgconfig/libcrypto.pc")" in
	*"libdir=/lib/x86_64-linux-gnu"*) ;;
	*)
		echo "libcrypto.pc does not name /lib/x86_64-linux-gnu" >&2
		exit 1
		;;
	esac
}
