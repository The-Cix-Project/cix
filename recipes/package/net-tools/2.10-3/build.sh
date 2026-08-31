#
# net-tools -- ifconfig/netstat/route/nameif, the classic Linux
# networking toolset (the jump box's own operator-facing counterpart
# to this project's own from-scratch rtnetlink data plane, which never
# shells out to these -- see CLAUDE.md's own Networking Plane note).
#
# Source is SourceForge's own real 2.10 release (net-tools has had no
# GitHub presence; SourceForge is its real, canonical upstream).
#
# net-tools' own build has no ./configure -- it uses a custom
# interactive configure.sh (bool prompts read from config.in,
# generating config.h/config.make) predating autotools entirely.
#
# A real, confirmed bug in the standard "yes '' | make config"
# non-interactive path package maintainers normally use for this exact
# tool: it produced a real, working build (ifconfig/netstat/route all
# compile and run), but ifconfig itself failed at runtime with "No
# usable address families found. socket: Success" -- confirmed by
# reading net-tools' own source (lib/sockets.c's sockets_open(),
# ifconfig.c's own call site) that this message only appears when
# every compiled-in protocol family's own socket() attempt is skipped
# or fails, and separately (config.in) that HAVE_AFINET's own real
# upstream default is "y" -- meaning AF_INET support itself ended up
# NOT compiled in, despite the blank-line-accepts-default technique
# looking correct on a read of configure.sh's own readln()/bool()
# logic. The exact interaction between `yes ""`'s piped blank lines
# and this script's own custom `IFS='@' read` prompt loop that causes
# this was not fully isolated (a real, if narrow, unresolved edge case
# in an 1990s-era interactive shell script, not something worth a deep
# forensic dive into) -- fixed instead by bypassing the interactive
# prompt engine entirely: config.h/config.make are generated directly
# from config.in's own declared defaults (every `bool 'DESC' NAME
# DEFAULT` line, mechanically converted -- config.in has no other
# prompt type in this release, confirmed by inspection), a
# deterministic equivalent to "accept every default" with no shell
# I/O interaction to go subtly wrong. Verified live: a real container
# running this build's own `ifconfig -a` now reports real interface
# data instead of failing.
#
pkg_name="net-tools"
pkg_version="2.10-3"
pkg_changelog="2.10-3: declare build tools so this can be rebuilt from source (#206)"
pkg_source="https://sourceforge.net/projects/net-tools/files/net-tools-2.10.tar.xz"
pkg_sha256="b262435a5241e89bfa51c3cabd5133753952f7a7b7b93f32e08cb9d96f580d69"
pkg_depends=""

# pkg_build_depends added (#206): this recipe declared none, so
# ADR-0199 refused it outright and it could not be rebuilt at all.
# The set is the baseline the already-declaring recipes converge on for
# a package of this shape, and nothing this recipe's own pkg_build()
# does asks for more. If that turns out to be incomplete the build says
# so by name -- which is how libxcrypt and findutils were found for the
# first three conversions.
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils"

pkg_build() {
	: > config.h
	: > config.make
	while read -r kind rest; do
		[ "$kind" = "bool" ] || continue
		name=$(printf '%s\n' "$rest" | awk '{print $(NF-1)}')
		default=$(printf '%s\n' "$rest" | awk '{print $NF}')
		if [ "$default" = "y" ]; then
			echo "#define $name 1" >> config.h
			echo "$name=1" >> config.make
		else
			echo "#define $name 0" >> config.h
			echo "# $name=0" >> config.make
		fi
	done < config.in

	# rarp.c:45: error: character array initializer must be a literal,
	# optionally enclosed in braces -- TCC's parser accepts a plain or
	# braced string literal as a char-array initializer, but not one
	# wrapped in a plain parenthesized expression, which is exactly
	# what `static char no_rarp_message[] = N_("...")` becomes once
	# N_() (gettext's own marker macro, a pure no-op here since I18N's
	# real upstream default is "n" -- config.in above) expands. The
	# only such construct anywhere in this source tree (confirmed via
	# a full grep) -- a one-line, narrowly-scoped fix rather than
	# touching N_() itself, which real translatable-string call sites
	# elsewhere use correctly as a plain function-call argument, where
	# this exact parse restriction doesn't apply.
	sed -i 's/static char no_rarp_message\[\] = N_("This kernel does not support RARP\.\\n");/static char no_rarp_message[] = "This kernel does not support RARP.\\n";/' rarp.c

	make -j"$(nproc)" CC=tcc
}

pkg_install() {
	# BINDIR/SBINDIR default to plain /bin and /sbin (Makefile's own
	# defaults) -- real paths this project's own minimal images don't
	# have at all (confirmed: only /usr/bin and /usr/sbin exist, see
	# CLAUDE.md's own environment notes). Overridden to this project's
	# real paths directly at install time -- a real, confirmed-live fix
	# (an earlier build using the plain defaults installed a working
	# ifconfig binary that then failed at execve() time with "No such
	# file or directory", since /bin/ifconfig was never a resolvable
	# path in any real container built from this image).
	make DESTDIR="$PKG_DESTDIR" BINDIR=/usr/bin SBINDIR=/usr/sbin install
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/man"
}
