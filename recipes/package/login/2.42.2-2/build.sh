#
# -2: the verification gate looked for the binary in the wrong place,
# and -1's account of --with-pam was wrong. Both found by building it.
#
# util-linux's automake is NON-RECURSIVE: every program links in the
# top-level build directory, so the built binary is ./login, not
# login-utils/login. The gate ran readelf against a path that does not
# exist, readelf failed, grep found no NEEDED line, and the build was
# failed with "PAM did not link" -- against a binary that was linked
# perfectly. Confirmed by pulling the real binary out of the preserved
# build container (keep_on_failure, ADR-0175) and reading its dynamic
# section directly: libpam.so.0, libpam_misc.so.0, libc.so.6.
#
# A gate that fails when its own path is wrong is not a strict gate,
# it is a broken one, and this one reported a false diagnosis with
# total confidence. So it now fails loudly on a missing binary rather
# than letting an absent file read as an absent dependency.
#
# --with-pam is NOT recognised by util-linux 2.42.2. Its own configure
# says so on the first line of every build here:
#
#   configure: WARNING: unrecognized options: --with-pam
#
# PAM is auto-detected from the headers instead, which is exactly the
# accident-of-the-environment this recipe's own comment warns about
# for selinux/audit/systemd -- and it is why the real blocker was in
# linux-pam rather than here. security/pam_misc.h includes
# security/pam_client.h from libpamc, which linux-pam did not build
# until 1.6.1-7, so the header would not compile, configure quietly
# concluded PAM was unavailable, and it refused to build login at all.
# The flag is dropped rather than kept as decoration.
#
#
# login -- util-linux's login(1), the program that prompts for a
# username and password and starts that user's shell. Asked for
# alongside htop and btop for the jump box, where it is what turns a
# console session into an authenticated one rather than a shell that is
# already root.
#
# Built standalone from the util-linux tree via that project's own real,
# documented `--disable-all-programs --enable-login` convention -- the
# identical mechanism recipes/package/libuuid already uses against this
# same tarball, which is why the source and checksum here match it
# exactly. Two packages from one upstream source is deliberate:
# util-linux is a collection of independent programs behind one build
# system, and packaging the whole thing to obtain one binary would put
# forty unrelated setuid-capable tools into every image that wanted a
# login prompt.
#
pkg_name="login"
pkg_version="2.42.2-2"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
pkg_artifact_sha256="3c980dee64f5406172e962262083979f0ae41692de1458720ee13ce50ddc283d"
#
# linux-pam at runtime, not merely at build time: login dlopens
# nothing -- it links libpam and libpam_misc directly and
# authenticates entirely through them. On this platform that is the whole
# point, since PAM is what reaches this project's own LDAP through
# nss-pam-ldapd (ADR-0144/ADR-0145); a login built without PAM would
# authenticate against a local /etc/shadow that these images do not have.
#
pkg_depends="linux-pam"
#
# ADR-0199/0209: the build environment is composed from exactly these
# and nothing else -- there is no fallback environment to inherit a
# missing tool from (#168). This list is libuuid's own, which builds the
# same tarball with the same configure, plus linux-pam for the headers
# and libraries PAM detection and linking need -- 1.6.1-7 or later,
# since earlier revisions ship a security/pam_misc.h that cannot
# compile.
#
# Sufficiency is enforced by the build itself. Minimality is review, not
# enforcement (ADR-0199).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf linux-pam"
pkg_changelog="2.42.2-2: fix the verification gate, which looked for login-utils/login while util-linux's non-recursive automake links the binary at the top level, so readelf failed on a missing path and the build was failed for a PAM dependency that was in fact present. Drops --with-pam, which util-linux 2.42.2 does not recognise and warns about; PAM is auto-detected, and the real blocker was linux-pam not building libpamc (fixed in 1.6.1-7) so that security/pam_misc.h could not compile. 2.42.2-1: first packaging. login(1) alone from the util-linux tree, built --with-pam so it authenticates through PAM and therefore through this platform's own LDAP, rather than against a local shadow file these images do not carry."

pkg_build() {
	#
	# --disable-all-programs then --enable-login: util-linux's own
	# supported way to build one program from the tree. Confirmed
	# against this exact tarball's configure.ac, the same way
	# libuuid.recipe confirmed its own.
	#
	# PAM is auto-detected, not requested: util-linux 2.42.2 has no
	# --with-pam and warns that it is unrecognised. What decides it is
	# whether security/pam_misc.h compiles, which is why linux-pam
	# 1.6.1-7 (building libpamc, so that header's own first include
	# resolves) is the real dependency here. The readelf check below
	# is therefore the ONLY thing standing between this and a login
	# that silently authenticates against an /etc/shadow these images
	# do not carry.
	#
	# --without-selinux/--without-audit/--without-systemd: none of the
	# three exists as a recipe here, and util-linux's configure enables
	# each one silently whenever it happens to find the headers lying
	# around in a build container. Disabling them explicitly makes that
	# a decision rather than an accident of the environment -- the same
	# trap htop.recipe documents for its own auto-detected features.
	#
	CC=tcc ./configure --prefix=/usr \
	    --disable-all-programs --enable-login \
	    --without-selinux --without-audit --without-systemd
	make -j"$(nproc)"

	#
	# Prove PAM actually got linked in. configure reporting "PAM
	# support: yes" is not sufficient evidence -- openssh.recipe hit
	# exactly this (ADR-0144, task #838): configure said yes, -lpam was
	# on the link line, and the binary carried no DT_NEEDED for it
	# because the compiler in the shared build sandbox was not the one
	# the recipe thought it was. The only trustworthy check is the
	# built ELF's own dynamic section.
	#
	# The binary's absence and its dependencies' absence are different
	# failures and are reported as different failures. Conflating them
	# is what -1 did.
	if [ ! -f login ]; then
		echo "login: no binary at ./login after make -- the build did not produce it" >&2
		exit 1
	fi
	if ! readelf -d login | grep -q 'NEEDED.*libpam'; then
		echo "login: built without a real libpam dependency -- PAM did not link:" >&2
		readelf -d login | grep NEEDED >&2 || true
		exit 1
	fi
	echo "  confirmed: login links libpam"
}

#
# Just the binary and its man page. `make install` on a
# --disable-all-programs tree still stages util-linux's shared
# translation catalogues and pkg-config files, which belong to the
# libraries this build did not produce.
#
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/locale" "$PKG_DESTDIR/usr/lib" \
	       "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/usr/share/bash-completion"
}
