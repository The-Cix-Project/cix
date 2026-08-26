#
# openssl -- OpenSSL: the real `openssl` CLI, libssl.so.3/libcrypto.so.3
# runtime libraries, the -lssl/-lcrypto dev symlinks, headers, and a
# real openssl.cnf -- a genuine from-source build.
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
pkg_name="openssl"
pkg_version="3.0.20"
pkg_source="https://github.com/openssl/openssl/releases/download/openssl-3.0.20/openssl-3.0.20.tar.gz"
pkg_sha256="c80a01dfc70ece4dc21168932c37739042d404d46ccc81a5986dd75314ecda6f"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/openssl-3.0.20.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="c6bbc96a41823972560fdfd9833eb78136b4fece7368d70962dcc4d504603b7b"
pkg_depends="perl"

# ./Configure (OpenSSL's own bespoke, Perl-driven build script -- not
# autotools; confirmed real via a local build in this sandbox, which
# also confirmed no build-time "skip docs" flag exists in this release
# -- install_sw below is what actually avoids the doc tree, not a
# Configure option). pkg_depends="perl" above stages a real perl into
# this same build container first -- Configure hard-requires it.
# Invoked as "perl ./Configure ..." rather than "./Configure" directly:
# the script's own shebang is "#!/usr/bin/env perl" (confirmed by
# reading it), which needs /usr/bin/env on PATH -- real on an image
# that also has coreutils, but not guaranteed on a lean image built
# only from this recipe set's own pkg_depends chain (confirmed the
# hard way: cix-hosttools has perl but no coreutils/env, and
# execve() on the un-runnable script fell back to bash interpreting
# Configure's own Perl source as shell, "use: command not found").
# Calling perl directly needs nothing beyond what pkg_depends="perl"
# already guarantees, so it's the real fix, not a version of "add
# coreutils as a dependency too" that would only be needed for this
# one indirection. "linux-x86_64" is the real, exact Configure target
# this platform is (confirmed via a real `uname -m` plus OpenSSL's own
# Configurations/10-main.conf entry for it), not a generic/auto-detect
# target. --openssldir=/usr/lib/ssl matches the real path
# mkbootroot.c's own openssl.cnf staging already uses.
pkg_build() {
	perl ./Configure --prefix=/usr --openssldir=/usr/lib/ssl linux-x86_64
	make -j"$(nproc)"
}

# install_sw (OpenSSL's own real, documented "software only" target --
# binaries/libraries/headers, no docs, confirmed via a real local
# build+install in this sandbox) plus install_ssldirs (the real
# openssl.cnf/ct_log_list.cnf/certs+private+misc directory layout --
# this project's own PKI subsystem needs a real openssl.cnf present at
# exactly usr/lib/ssl/openssl.cnf, confirmed via `pki ca bootstrap`
# failing without one). Confirmed live: this exact recipe's build
# output generates a real, working self-signed cert via `openssl req
# -x509` before being written here.
#
# install_sw's own real default lands libraries under usr/lib64 (this
# Configure target's own default LIBDIR, confirmed via a real
# DESTDIR install) -- moved to lib/x86_64-linux-gnu/, this project's
# own real runtime-library path convention every other recipe already
# uses. .a static archives, pkgconfig/*.pc, and c_rehash (a perl
# wrapper around a symlink-farm convention nothing in this project
# uses) are dropped -- nothing here links OpenSSL statically or uses
# pkg-config. engines-3/ossl-modules (the legacy provider/engine
# plugins, e.g. padlock) are also dropped: real, but this project's
# own PKI subsystem (daemon/src/pki.c) only ever needs the default
# provider, already built into libcrypto.so.3 itself, not a
# dynamically-loaded plugin.
pkg_install() {
	make DESTDIR="$PKG_DESTDIR" install_sw
	make DESTDIR="$PKG_DESTDIR" install_ssldirs
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	mv "$PKG_DESTDIR/usr/lib64/libssl.so.3" "$PKG_DESTDIR/usr/lib64/libssl.so" \
	   "$PKG_DESTDIR/usr/lib64/libcrypto.so.3" "$PKG_DESTDIR/usr/lib64/libcrypto.so" \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	rm -rf "$PKG_DESTDIR/usr/lib64" "$PKG_DESTDIR/usr/bin/c_rehash"
}
