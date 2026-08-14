#
# linux-pam -- real Linux-PAM: libpam.so/libpam_misc.so, their headers,
# and a real, working baseline module set (pam_unix, pam_permit,
# pam_deny, pam_env, pam_limits, pam_rootok, pam_warn). Part of
# ADR-0144's container-side real-LDAP work (task #832) -- the
# authentication framework a future pam_ldap module (this task's next
# part) plugs into, and that an openssh rebuild (a later ADR-0144
# part) will link against to make `sshd` actually consult PAM instead
# of nothing at all (`openssh.recipe`'s own current build has zero PAM
# support, a deliberate v1 scope boundary at the time it was written).
#
# Source is Linux-PAM's own canonical GitHub Releases asset. No
# published independent checksum exists for this release (no
# .sha256SUMS file, only a PGP .asc signature not independently
# cross-checked here) -- a single-source checksum, same real
# limitation openldap-client.recipe already notes for its own source.
#
# Pinned to the LAST autotools-based release, not the newest overall:
# confirmed directly (GitHub API content listing) that v1.6.1 still
# ships a real configure.ac/configure and no meson.build, while v1.7.0
# switched to meson-only. Meson's only supported Linux backend is
# ninja, and ninja is written in C++ -- this project's toolchain is
# TCC, a C-only compiler with no C++ support at all, so any
# meson-based release is categorically unbuildable here regardless of
# whether meson/ninja themselves could somehow be staged. Matches this
# project's own established precedent for exactly this situation
# (iputils.recipe's own choice of the last pre-meson release, for the
# identical reason).
#
pkg_name="linux-pam"
pkg_version="1.6.1"
pkg_source="https://github.com/linux-pam/linux-pam/releases/download/v1.6.1/Linux-PAM-1.6.1.tar.xz"
pkg_sha256="f8923c740159052d719dbfc2a2f81942d68dd34fcaf61c706a02c9b80feeef8e"
pkg_depends=""

# One real, confirmed TCC-vs-upstream-source gap, found via a local
# build in this sandbox before this recipe was ever registered:
# `pam_end.c:45: error: invalid array size`. Traced to `libpam/
# include/pam_cc_compat.h`'s own `PAM_IS_SAME_TYPE()` macro, gated by
# `#if PAM_GNUC_PREREQ(3, 0)` -- true only when `__GNUC__`/
# `__GNUC_MINOR__` are both defined, which TCC (confirmed via `tcc -E
# -dM`) never does, so this macro falls back to a stub that always
# evaluates to `0` regardless of the real argument types. That stub
# is what breaks: `pam_end.c`'s own `pam_overwrite_string(pamh->
# authtok)` expands to a `PAM_MUST_NOT_BE_ARRAY()` compile-time check
# built on this same macro, and the always-`0` fallback makes that
# check unconditionally fail with `sizeof(int[-1])`, a hard error --
# for ANY expression, not just the real array-vs-pointer mismatches
# this machinery exists to catch. This is a real upstream portability
# gap in Linux-PAM's own header, not TCC-specific in principle (any
# compiler lacking `__builtin_types_compatible_p`, gated the same way,
# would hit this identical failure) -- confirmed real distros packaging
# Linux-PAM only ever build it with GCC/Clang, both of which implement
# the builtin, so this gap has never surfaced there. TCC *does*
# correctly implement `__builtin_types_compatible_p` itself (confirmed
# directly with a minimal standalone test: `0` for an array-vs-pointer
# mismatch, `1` for two matching pointer types) -- the failure is
# purely in how `PAM_GNUC_PREREQ` gates access to it, not in the
# builtin's own TCC behavior. Fixed with a one-line, narrowly-scoped
# patch: extend that single `#if` to also match `defined(__TINYC__)`
# (TCC's own always-defined self-identifying macro), routing TCC
# through the real `__builtin_types_compatible_p` path instead of the
# broken always-`0` stub. Deliberately NOT `-D__GNUC__=N` via
# CPPFLAGS, which was tried first and rejected: that approach also
# changes how glibc's OWN system headers parse (`__GNUC__`-gated
# branches expecting full GCC C support -- `_Float128`/`_Float64x`
# extended-float types, `__REDIRECT`, etc. -- that TCC doesn't
# implement), breaking `<sys/syslog.h>`'s own include chain with a
# real, different parse error (`bits/floatn.h:75: ';' expected (got
# "float")`) the moment it was tried. The one-line source patch below
# is scoped to exactly the one macro that needed it, with zero
# blast radius on anything else this recipe (or a future one) builds.
pkg_build() {
	sed -i 's/^#if PAM_GNUC_PREREQ(3, 0)$/#if PAM_GNUC_PREREQ(3, 0) || defined(__TINYC__)/' \
		libpam/include/pam_cc_compat.h

	CPPFLAGS="-D__STDC_NO_VLA__=1" ac_cv_func_pthread_kill_other_threads_np=no \
	CC=tcc ./configure --prefix=/usr --libdir=/lib/x86_64-linux-gnu \
	            --disable-nls --disable-doc --disable-selinux --disable-audit \
	            --disable-systemd --disable-econf --disable-nis --disable-db \
	            --disable-lckpwdf

	make -C libpam_internal
	make -C libpam
	make -C libpam_misc
	make -C modules/pam_unix
	make -C modules/pam_permit
	make -C modules/pam_deny
	make -C modules/pam_env
	make -C modules/pam_limits
	make -C modules/pam_rootok
	make -C modules/pam_warn
	make -C conf
}

# Only the pieces actually built above are installed (never the doc/
# tests/xtests/examples/po trees, and no module beyond the seven real,
# useful baseline ones this recipe deliberately built -- pam_ldap is
# its own future recipe, not added here). Everything lands under this
# project's own real multiarch runtime path, /lib/x86_64-linux-gnu --
# both the libraries (--libdir= above) AND the modules: confirmed
# directly (configure.ac + libpam/Makefile.am) that upstream computes
# SECUREDIR as $(libdir)/security by default and bakes that same value
# into libpam.so's own DEFAULT_MODULE_PATH constant, so overriding
# libdir alone correctly relocates both together with no separate
# --enable-securedir= needed -- confirmed live too, not just read from
# source: the deployed libpam.so.0's own compiled-in string literally
# reads "/lib/x86_64-linux-gnu/security/" (checked via a real
# container, fetching the installed .so back out through GET .../files
# and grepping it), matching exactly where the modules themselves
# landed. This project's images have no ld.so.cache/ldconfig step, so
# putting anything at PAM's own upstream /lib64 default (untouched by
# --libdir) would have left it unresolvable by the dynamic linker --
# real reason to override, not a cosmetic path preference. *.la
# (libtool metadata) and pkgconfig/*.pc are dropped, matching every
# other recipe in this set.
pkg_install() {
	make -C libpam_internal DESTDIR="$PKG_DESTDIR" install
	make -C libpam DESTDIR="$PKG_DESTDIR" install
	make -C libpam_misc DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_unix DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_permit DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_deny DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_env DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_limits DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_rootok DESTDIR="$PKG_DESTDIR" install
	make -C modules/pam_warn DESTDIR="$PKG_DESTDIR" install
	make -C conf DESTDIR="$PKG_DESTDIR" install

	rm -f "$PKG_DESTDIR/lib/x86_64-linux-gnu"/*.la "$PKG_DESTDIR/lib64/security"/*.la
	rm -rf "$PKG_DESTDIR/lib/x86_64-linux-gnu/pkgconfig"
}
