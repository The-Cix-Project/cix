#
# dnsmasq -- the real DNS/DHCP server this platform's own DNS
# subsystem has always assumed exists (daemon/src/dns.c's own doc
# comment: "actual resolution done by a real DNS server (dnsmasq)
# running as a normal containerized workload -- not hand-rolled").
# Same recipe contract as bash.recipe -- see that file's own header
# comment for the metadata-scanner-vs-sourced-shell-script split.
#
# Source is Debian's own ".orig.tar.xz" for their dnsmasq package --
# the exact, unmodified upstream release tarball, same convention
# every other recipe in this set already uses. Version pinned to 2.90
# specifically to match this sandbox's own already-installed dnsmasq
# (confirmed via `dnsmasq --version`) -- the exact binary
# test/test_dns.c's own fixture has already proven end-to-end
# (--addn-hosts + SIGHUP reload, real `dig` resolution) against this
# project's own dns_server_register() mechanism, so this recipe
# reproduces a binary already known to work, not an unproven new one.
#
pkg_name="dnsmasq"
pkg_version="2.90-3"
pkg_source="https://deb.debian.org/debian/pool/main/d/dnsmasq/dnsmasq_2.90.orig.tar.xz"
pkg_sha256="8e50309bd837bfec9649a812e066c09b6988b73d749b7d293c06c57d46a109e4"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/dnsmasq-2.90.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="842419ceca12ac0c4776eef49518f7744022e7213c0fc5989dd3f38750829561"
pkg_depends=""
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils"
pkg_changelog="2.90-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 2.90-2: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# Plain Makefile, no ./configure step at all (confirmed directly --
# this release's own top-level Makefile has no configure target).
# Every optional integration (DBus, IDN, Lua scripting, conntrack,
# nftables, nettle/DNSSEC) is gated behind bld/pkg-wrapper, which only
# ever invokes pkg-config for a feature whose HAVE_* macro is already
# set in config.h/COPTS -- none is, by default, so pkg-config is never
# even called (this toolchain has none of those -dev packages, and
# none are needed). Confirmed via a real build + `ldd`: the resulting
# binary links only libc.so.6 + ld-linux, nothing else -- exactly the
# minimal, self-contained footprint this platform's own images already
# provide as their baseline (pkg_seed_image_baseline()), no extra
# library staging needed in pkg_install() below at all. PREFIX=/usr
# matches every other recipe in this set (dnsmasq's own Makefile
# defaults to /usr/local, which would land in the wrong place once
# merged into the shared base image).
pkg_build() {
	make -j"$(nproc)" CC=tcc PREFIX=/usr
}

# `ldd` against the real build above confirms zero runtime library
# dependencies beyond libc (already part of every image's own
# baseline) -- the entire install is one binary, no man page (this is
# a container image, not a dev environment, the same boundary every
# other recipe in this set already keeps).
#
# Also writes a minimal /etc/passwd + /etc/group: dnsmasq's own -u/-g
# flags resolve the given name via NSS (getpwnam/getgrnam) even when
# it's already running as that user, so "-u root -g root" still needs
# a real root entry in both files -- confirmed the hard way
# (test/test_dns.c's own write_minimal_passwd_group(), the exact same
# content reproduced here). No other recipe in this set needs either
# file, and pkg_seed_image_baseline() deliberately doesn't provide
# them for every image (dev nodes only) -- this is dnsmasq's own
# runtime requirement, so it belongs in dnsmasq's own pkg_install(),
# not a new baseline-seeding step for every image regardless of
# whether it runs dnsmasq.
pkg_install() {
	dir="$PKG_DESTDIR/usr/sbin"
	mkdir -p "$dir"
	cp src/dnsmasq "$dir/"

	mkdir -p "$PKG_DESTDIR/etc"
	printf 'root:x:0:0:root:/root:/bin/sh\n' >"$PKG_DESTDIR/etc/passwd"
	printf 'root:x:0:\n' >"$PKG_DESTDIR/etc/group"
}
