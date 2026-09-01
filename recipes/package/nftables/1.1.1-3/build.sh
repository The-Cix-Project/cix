#
# nftables -- the nft CLI and libnftables.
#
# A DIFFERENT UPSTREAM PROJECT FROM libnftnl, despite the names. libnftnl
# is the low-level netlink library that builds and parses the messages;
# this is the high-level rule syntax and the tool an operator actually
# types at. Packaging one does not give you the other, which is worth
# saying because the naming invites the opposite assumption.
#
# Source is netfilter.org's own release archive. Checksum computed
# directly from the downloaded bytes.
#
# ============================================================
# DECLARED CAPABILITY LOSS: no interactive CLI (ADR-0222)
# ============================================================
#
# REMOVED:  nft -i, the interactive readline-style shell.
#
# KEPT:     everything else -- one-shot commands (nft add rule ...),
#           script files (nft -f), the whole rule syntax, and
#           libnftables for programmatic use. The interactive prompt is
#           a convenience wrapper around the same parser.
#
# WHY:      configure requires one of libreadline, libedit or linenoise
#           and errors out if it cannot find the one it was asked for;
#           the default is editline. None of the three is packaged, and
#           readline would pull in ncurses as well. --without-cli is
#           upstream's own supported answer rather than something
#           forced on it.
#
# This is reversible in the ordinary way: package one of the three line
# editors and rebuild with --with-cli=readline. It is recorded here
# because an operator who types 'nft -i' and gets nothing deserves to
# find the reason in the package rather than by reading configure.
#
pkg_name="nftables"
pkg_version="1.1.1-3"
pkg_source="https://www.netfilter.org/projects/nftables/files/nftables-1.1.1.tar.xz"
pkg_sha256="6358830f3a64f31e39b0ad421d7dadcd240b72343ded48d8ef13b8faf204865a"
pkg_artifact_sha256="b6c00b939044e026474e9ae8313453cf11bb8f25246dcf2520530ea5bf629da2"
pkg_depends="libmnl libnftnl gmp"
#
# bison and flex are real requirements, not extras: nftables' rule
# syntax IS a grammar (AC_PROG_YACC / AC_PROG_LEX), and the parser is
# generated at build time.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils pkgconf bison flex libmnl libnftnl gmp"
pkg_changelog="1.1.1-3: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 1.1.1-2: the 1.1.1 build succeeded and its own assertion failed -- it checked the nft BINARY for libnftnl, but nft links libnftables, and libnftables is what links libnftnl and libmnl. Asserts the right artifact. Unblocked by tcc@0.9.27-12 (#211). 1.1.1: first packaging -- the nft CLI, which no amount of libnftnl provides on its own. Built --without-cli: the interactive shell needs libreadline/libedit/linenoise and none is packaged; every non-interactive use is unaffected (#207, ADR-0222)"

pkg_build() {
	export PKG_CONFIG_PATH="/usr/lib/pkgconfig:/lib/x86_64-linux-gnu/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig"

	echo "=== dependencies visible to pkg-config ==="
	for m in libmnl libnftnl; do
		if pkg-config --exists "$m" 2>/dev/null; then
			echo "  present  $m $(pkg-config --modversion "$m")"
		else
			echo "  ABSENT   $m"
		fi
	done

	# configure requires these as hard errors, so fail here with a clear
	# message rather than inside PKG_CHECK_MODULES.
	pkg-config --atleast-version=1.0.4 libmnl || {
		echo "libmnl >= 1.0.4 required" >&2
		exit 1
	}
	pkg-config --atleast-version=1.2.8 libnftnl || {
		echo "libnftnl >= 1.2.8 required" >&2
		exit 1
	}

	CC=tcc ./configure --prefix=/usr --disable-static --without-cli

	#
	# TCC cannot honour --version-script; Makefile.am passes one for
	# libnftables.map. Stripped from the GENERATED makefiles so
	# upstream's tree stays unmodified. See
	# docs/guides/writing-recipes.md for why the character class is
	# [^[:space:]] and not [^ ] -- the latter eats a TAB and the
	# line-continuation backslash after it.
	#
	before=$(find . -name Makefile -exec cat {} + | grep -c '\\$')
	find . -name Makefile -exec sed -i 's/-Wl,--version-script[=,][^[:space:]]*//g' {} +
	if find . -name Makefile -exec grep -l -- '--version-script' {} + | grep -q .; then
		echo "version-script flag survived the strip" >&2
		exit 1
	fi
	after=$(find . -name Makefile -exec cat {} + | grep -c '\\$')
	if [ "$before" != "$after" ]; then
		echo "strip destroyed line continuations: $before -> $after" >&2
		exit 1
	fi

	make -j"$(nproc)"
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"

	rm -rf "$PKG_DESTDIR/usr/share/man"
	rm -f "$PKG_DESTDIR"/usr/lib/*.la

	echo "=== what shipped ==="
	find "$PKG_DESTDIR" -name 'nft' -o -name 'libnftables*' -o -name '*.pc' \
	    | sed "s|$PKG_DESTDIR||" | sort

	#
	# The binary is the point of the package. Assert it rather than
	# trust make's exit status (#113).
	#
	nft_bin=""
	for cand in "$PKG_DESTDIR/usr/sbin/nft" "$PKG_DESTDIR/sbin/nft" "$PKG_DESTDIR/usr/bin/nft"; do
		if [ -x "$cand" ]; then
			nft_bin="$cand"
			break
		fi
	done
	if [ -z "$nft_bin" ]; then
		echo "nft binary not found" >&2
		exit 1
	fi
	echo "nft installed at: ${nft_bin#$PKG_DESTDIR}"

	ls "$PKG_DESTDIR"/usr/lib/libnftables.so.* >/dev/null 2>&1 || {
		echo "no shared libnftables was built" >&2
		exit 1
	}

	echo "=== nft DT_NEEDED ==="
	readelf -d "$nft_bin" | grep NEEDED

	#
	# Assert the right artifact. nft is a thin front end over
	# libnftables: it links libnftables and libgmp directly, and the
	# NETLINK stack (libnftnl, libmnl) is linked by libnftables, not by
	# the binary. The 1.1.1 revision asserted libnftnl against nft
	# itself and failed after an otherwise complete, correct build --
	# the same mistake as checking keepalived's DT_NEEDED for a library
	# it dlopens.
	#
	echo "=== libnftables DT_NEEDED ==="
	nft_lib=$(ls "$PKG_DESTDIR"/usr/lib/libnftables.so.*.* 2>/dev/null | head -1)
	readelf -d "$nft_lib" | grep NEEDED

	for lib in libgmp; do
		readelf -d "$nft_bin" | grep -q "NEEDED.*$lib" || {
			echo "nft is not linked against $lib" >&2
			exit 1
		}
	done
	readelf -d "$nft_bin" | grep -q 'NEEDED.*libnftables' || {
		echo "nft is not linked against libnftables" >&2
		exit 1
	}
	for lib in libnftnl libmnl; do
		readelf -d "$nft_lib" | grep -q "NEEDED.*$lib" || {
			echo "libnftables is not linked against $lib" >&2
			exit 1
		}
	done
}
