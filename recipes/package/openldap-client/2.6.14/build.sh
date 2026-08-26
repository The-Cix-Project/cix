#
# openldap-client -- real OpenLDAP client-side software: libldap.a/
# liblber.a (static), their headers, and the standard ldapsearch/
# ldapmodify/ldapdelete/ldapmodrdn/ldappasswd/ldapwhoami/ldapvc/
# ldapcompare/ldapexop/ldapurl/ldapadd tools. Part of ADR-0144's
# container-side real-LDAP work (task #832): the library this
# recipe's own headers/archives exist for is nss_ldap/pam_ldap
# (separate recipes, both statically link against libldap.a/
# liblber.a here rather than depending on a shared libldap.so --
# see this file's own "static, not shared" note below for why).
# slapd/lloadd (the server side) and every backend are explicitly
# disabled -- this project's own directory server is glauth
# (pkg/recipes/glauth), never slapd; only the client side is needed
# here.
#
# Source is the OpenLDAP Foundation's own canonical download site.
# No published .sha256/.asc-independent checksum file exists there
# (only a PGP .asc signature, not independently cross-checked here),
# so this is a single-source checksum -- noted plainly rather than
# claimed as independently verified, unlike e.g. openssl.recipe's own
# two-source check (a second mirror, mirror.symas.com, was tried and
# came back empty for this exact release at the time this recipe was
# written).
#
pkg_name="openldap-client"
pkg_version="2.6.14"
pkg_source="https://www.openldap.org/software/download/OpenLDAP/openldap-release/openldap-2.6.14.tgz"
pkg_sha256="806dcd21d366428187fba3278da773d5930f774852c9e92517f950d585f19107"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/openldap-client-2.6.14.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="e4dff14fcc1bd965b56b1a7aeac0cf76d2aab73a9a5408bf181b162ed68ebcc2"
pkg_depends="openssl libuuid"

# Three real, distinct TCC-vs-this-source gaps found and fixed while
# developing this recipe (confirmed via a real local build in this
# sandbox before ever touching the package registry):
#
# 1. "POSIX regex.h required" -- the exact same TCC/glibc <regex.h>
#    VLA-in-prototype parse failure documented in CLAUDE.md's own
#    environment notes (daemon/src/logstore.c hit this first).
#    CPPFLAGS="-D__STDC_NO_VLA__=1" steers configure's own regex.h
#    detection test onto the same non-VLA branch that fix already
#    uses -- no header rewrite, this is a config-time-only flag.
#
# 2. "LinuxThreads header/library mismatch" -- a real TCC linker
#    leniency bug, not a real environment fact: configure's own
#    AC_CHECK_FUNC probe for pthread_kill_other_threads_np() (a
#    symbol that only ever existed in the old, pre-NPTL LinuxThreads
#    implementation, and has no business existing on this glibc at
#    all) reports a false "yes" -- tcc's linker accepts the probe
#    program's undefined reference to that symbol at link time
#    instead of failing the way a real linker (correctly) does,
#    which then sends configure down its legacy-LinuxThreads
#    consistency-check path, which correctly fails because this
#    really is modern NPTL, not LinuxThreads. Overridden directly via
#    the standard autoconf cache-variable mechanism:
#    ac_cv_func_pthread_kill_other_threads_np=no forces the correct
#    answer without patching the generated configure script itself.
#
# 3. "unsupported linker option '--version-script=...'" -- tcc's own
#    linker has no support for GNU ld's symbol-versioning linker
#    script mechanism at all (confirmed: configure's own $LD --help
#    probe for this capability, `grep gnu-version-script`/`grep
#    version-script`, is what wrongly reports "yes" here too -- tcc's
#    --help text apparently mentions the flag name without actually
#    implementing it). OpenLDAP has a real, first-class configure
#    knob for this exact situation: --enable-versioning=no.
#
# Separately, --enable-static=yes --enable-shared=no (STATIC
# libraries only, not this project's usual "build a real shared
# .so.N, matching openssl.recipe's own precedent" default) is a
# deliberate choice, not a fallback from something better: building
# liblber.so/libldap.so themselves links cleanly under tcc, but
# linking anything else *against* them (this recipe's own client
# tools, and this whole ADR-0144 part's actual end consumers,
# nss_ldap.so/pam_ldap.so) does not -- tcc's linker fails to resolve
# a shared library's own transitive shared-library dependency (here,
# libldap.so's own DT_NEEDED on liblber.so.2) purely at link time,
# even though the exact file it's looking for is really present and
# -rpath/-L would resolve it at runtime just fine under glibc's own
# ld.so. Confirmed directly: building shared succeeds for the
# libraries themselves, then fails linking ldapsearch against them
# with "referenced dll 'liblber.so.2' not found" -- a real, narrow
# tcc linker gap in multi-hop shared-library resolution, not a
# configuration mistake. Static linking sidesteps it entirely and is
# the right shape for this recipe's own real consumers anyway: a
# small, dlopen()'d NSS/PAM module wants its own LDAP client code
# self-contained, not a separate runtime .so dependency every
# process loading that module would then also need resolvable.
pkg_build() {
	CPPFLAGS="-D__STDC_NO_VLA__=1" ac_cv_func_pthread_kill_other_threads_np=no \
	CC=tcc ./configure --prefix=/usr \
	            --disable-slapd --disable-backends --without-systemd \
	            --enable-static=yes --enable-shared=no --enable-versioning=no \
	            --with-tls=openssl
	make depend
	make -C include
	make -C libraries
	make -C clients
}

# Only include/libraries/clients are installed (never servers/tests/
# doc -- slapd is disabled above and this project builds no man
# pages anywhere else either). ldap.conf lands under usr/etc/openldap
# by this build's own --prefix=/usr default (no --sysconfdir given);
# moved to the real /etc convention every other recipe's own runtime
# config uses (openssh.recipe's --sysconfdir=/etc/ssh, glauth's own
# /etc/glauth) rather than adding a second configure variable to
# track. *.la (libtool's own metadata, no libtool-aware consumer
# anywhere in this project) and pkgconfig/*.pc (nothing here uses
# pkg-config, same reasoning openssl.recipe's own install step
# already documents) are dropped.
pkg_install() {
	make -C include DESTDIR="$PKG_DESTDIR" install
	make -C libraries DESTDIR="$PKG_DESTDIR" install
	make -C clients DESTDIR="$PKG_DESTDIR" install
	mkdir -p "$PKG_DESTDIR/etc"
	mv "$PKG_DESTDIR/usr/etc/openldap" "$PKG_DESTDIR/etc/openldap"
	rmdir "$PKG_DESTDIR/usr/etc"
	rm -f "$PKG_DESTDIR/usr/lib"/*.la
	rm -rf "$PKG_DESTDIR/usr/lib/pkgconfig"
}
