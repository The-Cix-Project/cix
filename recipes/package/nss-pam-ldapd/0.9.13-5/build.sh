#
# nss-pam-ldapd -- NSS and PAM lookups against a real LDAP directory,
# via a small privileged daemon (nslcd) that libnss_ldap.so.2/
# pam_ldap.so both talk to over a local unix socket rather than
# embedding real LDAP credentials/connections in every process that
# calls getpwnam()/pam_authenticate(). This is task #832's own actual
# target, closing out ADR-0144's container-side real-LDAP chain
# (openldap-client -> linux-pam -> this): a container running this
# plus the two earlier recipes can resolve real Cix LDAP accounts
# via ordinary getpwnam()/getgrnam()/PAM, the mechanism a future
# PAM-enabled openssh rebuild (a later ADR-0144 part) will actually
# use for real SSH login -- replacing the file-rendering SSH-target
# mechanism this whole ADR set out to retire in the first place.
#
# Chosen over the classic PADL nss_ldap/pam_ldap modules (the
# historical reference implementation, and the literal names task
# #832 uses) because PADL's own upstream has been unmaintained since
# ~2013 with no current source distribution point; nss-pam-ldapd is
# Arthur de Jong's actively-maintained modern replacement, a drop-in
# functional equivalent (same nsswitch.conf/pam.d integration shape,
# same real LDAP-backed lookups) with the added, genuinely better
# security posture of centralizing LDAP access in one small daemon
# rather than linking libldap into every process on the box.
#
# Source is the project's own canonical download site. No published
# independent checksum exists for this release either (same real
# limitation openldap-client.recipe/linux-pam.recipe already note for
# their own sources) -- a single-source checksum.
#
pkg_name="nss-pam-ldapd"
pkg_version="0.9.13-5"
pkg_changelog="0.9.13-5: install to /usr/lib rather than the multiarch directory (#184 stage 2), together with linux-pam 1.6.1-6. The pam-seclib-dir must name the same directory libpam has compiled into its own DEFAULT_MODULE_PATH, so these two move in one change or PAM looks where this module is not. The artifact approval is dropped because those bytes came from the previous revision. 0.9.13-4: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 0.9.13-3: declare build tools so this can be rebuilt (#206); rebuilt against the TCC-clean linux-pam 1.6.1-4 and openldap-client 2.6.14-3 (#113)"
pkg_source="https://arthurdejong.org/nss-pam-ldapd/nss-pam-ldapd-0.9.13.tar.gz"
pkg_sha256="e01784e17cb533bb66bd0601e205e785263445c3c2df7a6f90232ab4131c716d"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/nss-pam-ldapd-0.9.13-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_depends="openldap-client linux-pam"

# pkg_build_depends added (#206). Derived, not guessed: the baseline the
# already-declaring recipes converge on for an autotools package, plus
# findutils (the sibling openldap build proved autotools shells out to
# `find`), plus the three libraries this recipe's own configure line
# names -- LIBS="-llber -lssl -lcrypto" makes openldap-client and
# openssl build-time needs, and linux-pam supplies the PAM headers the
# pam_ldap module compiles against.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils openldap-client linux-pam openssl"

# -2: real, confirmed-needed revision found while actually wiring this
# up for real SSH login (ADR-0144's own openssh-with-PAM part, not a
# new gap in this recipe's own build) -- nslcd's own compiled-in
# defaults are NSLCD_PIDFILE="/var/run/nslcd/nslcd.pid" and
# NSLCD_SOCKET="/var/run/nslcd/socket", the exact same "/var/run
# doesn't exist, only /run does" gap this project has already hit and
# documented for dnsmasq/chrony's own default pidfile paths. Both are
# real, first-class upstream configure knobs
# (--with-nslcd-pidfile=/--with-nslcd-socket=), not a source patch --
# repointed at /run/nslcd/... below.

# No new TCC-vs-upstream-source gap in this recipe -- every one of
# nss-pam-ldapd's own ~35 source files across compat/common/nss/pam/
# nslcd compiled clean under TCC on the first real attempt (confirmed
# via a local build in this sandbox, using openldap-client's and
# linux-pam's own already-built headers/libraries directly, before
# this recipe was ever registered). Two real issues did surface, both
# consequences of choices already made in this ADR-0144 chain's
# earlier recipes, not new TCC gaps of their own:
#
# 1. configure's own AC_SEARCH_LIBS(ldap_search_ext, [ldap_r ldap])
#    probe failed to link against openldap-client's real, deliberately
#    static-only libldap.a/liblber.a (see that recipe's own comment
#    for why this project builds OpenLDAP static rather than shared):
#    `-lldap` alone pulls in libldap.a's own real, unresolved
#    references to liblber AND to every OpenSSL TLS symbol it was
#    built against (`--with-tls=openssl`) -- a shared libldap.so would
#    absorb these transitively via its own DT_NEEDED, but a static
#    archive needs every dependency spelled out explicitly by whoever
#    links against it. Fixed by seeding LIBS="-llber -lssl -lcrypto"
#    before configure runs, so its own link-test candidates
#    ("-lldap $LIBS", etc.) actually succeed.
#
# 2. `tcc: error: unsupported linker option '-h,libnss_ldap.so.2'` --
#    nss/Makefile.am (via configure.ac) hardcodes Solaris-flavored
#    `-Wl,-h,...` SONAME syntax plus a GNU-ld `--version-script`
#    (real, upstream Linux-specific LDFLAGS, not gated behind any
#    capability probe the way OpenLDAP's own `--enable-versioning=no`
#    knob is) for both nss_ldap.so and pam_ldap.so -- TCC supports
#    neither: SONAME must be spelled `-Wl,-soname,...` (the syntax TCC
#    actually implements), and symbol versioning isn't implemented at
#    all, the identical gap `openldap-client.recipe` already
#    documents. Neither is functionally needed for a module a process
#    only ever dlopen()s by its own literal filename (nss_ldap.so.2)
#    or that glibc/PAM never inspects DT_SONAME on at all -- dropping
#    them loses nothing real. Fixed with a targeted `sed -i` on the
#    two *generated* Makefiles (after `./configure`, not a
#    `configure.ac`/autoreconf-level patch -- simpler, and this
#    recipe never runs autoreconf) rather than a source patch, since
#    the LDFLAGS in question are Makefile-level substitutions, not
#    C source.
pkg_build() {
	CPPFLAGS="-D__STDC_NO_VLA__=1" LDFLAGS="-L/usr/lib" \
	LIBS="-llber -lssl -lcrypto" ac_cv_func_pthread_kill_other_threads_np=no \
	CC=tcc ./configure --prefix=/usr --sysconfdir=/etc --libdir=/usr/lib \
	            --with-pam-seclib-dir=/usr/lib/security \
	            --with-nslcd-pidfile=/run/nslcd/nslcd.pid \
	            --with-nslcd-socket=/run/nslcd/socket

	sed -i 's/-Wl,-h,\$(NSS_LDAP_SONAME) -Wl,--version-script,exports.map/-Wl,-soname,$(NSS_LDAP_SONAME)/' \
		nss/Makefile pam/Makefile
	sed -i 's/-Wl,--version-script,\$(srcdir)\/pam_ldap.map//' nss/Makefile pam/Makefile

	make -C compat
	make -C common
	make -C nss
	make -C pam
	make -C nslcd
}

# Only the three real runtime pieces this recipe actually needs are
# installed -- never pynslcd (a pure-Python reimplementation of nslcd
# this project has no use for, real C nslcd above is what's built),
# utils/man/tests. libnss_ldap.so.2/pam_ldap.so land under this
# project's own real multiarch/PAM-module paths (via --libdir=/
# --with-pam-seclib-dir= above, the same /usr/lib[/
# security] convention linux-pam.recipe already established and
# confirmed live) rather than upstream's own /usr/lib/[/lib/security]
# defaults, which this project's images (no ld.so.cache/ldconfig
# step) would never actually resolve. nslcd itself (the daemon) is an
# ordinary /usr/sbin binary. Deliberately NOT staged here: a real
# /etc/nslcd.conf (base DN, server URI, bind credentials) -- that's a
# per-deployment concern, the same "Cix owns the durable config,
# a container's own files entry stages the concrete value" posture
# every other real service config in this project (dnsmasq, glauth)
# already has, not something a package recipe should bake in.
pkg_install() {
	make -C nss DESTDIR="$PKG_DESTDIR" install
	make -C pam DESTDIR="$PKG_DESTDIR" install
	make -C nslcd DESTDIR="$PKG_DESTDIR" install
}
