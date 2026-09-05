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
pkg_version="2.42.2-1"
pkg_source="https://www.kernel.org/pub/linux/utils/util-linux/v2.42/util-linux-2.42.2.tar.xz"
pkg_sha256="03a05d3adf9602ef128f2da05b84b3205ce60c351e5737c0370f74000679ce8a"
#
# linux-pam at runtime, not merely at build time: login is built
# --with-pam below and dlopens nothing -- it links libpam directly and
# authenticates entirely through it. On this platform that is the whole
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
# and library that --with-pam links against.
#
# Sufficiency is enforced by the build itself. Minimality is review, not
# enforcement (ADR-0199).
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf linux-pam"
pkg_changelog="2.42.2-1: first packaging. login(1) alone from the util-linux tree, built --with-pam so it authenticates through PAM and therefore through this platform's own LDAP, rather than against a local shadow file these images do not carry."

pkg_build() {
	#
	# --disable-all-programs then --enable-login: util-linux's own
	# supported way to build one program from the tree. Confirmed
	# against this exact tarball's configure.ac, the same way
	# libuuid.recipe confirmed its own.
	#
	# --with-pam is the load-bearing flag. Without it util-linux builds
	# a login that reads /etc/shadow directly, which on these images is
	# either absent or holds only the minimal root entry a container is
	# seeded with -- so every real account would fail to authenticate
	# with no indication that PAM was simply not compiled in.
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
	    --with-pam \
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
	if ! readelf -d login-utils/login 2>/dev/null | grep -q 'NEEDED.*libpam'; then
		echo "login: built without a real libpam dependency -- PAM did not link:" >&2
		readelf -d login-utils/login 2>&1 | grep NEEDED >&2 || true
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
