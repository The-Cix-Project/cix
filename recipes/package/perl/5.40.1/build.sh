#
# perl -- Perl 5. Needed as a real runtime by autoconf/automake (both
# already hardcode #!/usr/bin/perl into their generated tools -- see
# autoconf.recipe/automake.recipe's own pkg_depends="... perl ..."),
# and useful as its own real scripting language for anything built in
# a "dev" image.
#
# Source is CPAN's own canonical distribution point, checksum verified
# against a second, independent CPAN mirror (cpan.metacpan.org's own
# authors/id path) -- byte-identical, same sha256. (5.40.1, not 5.40.0:
# an initial guess at .0 was corrected after failing to find a second
# independent source to cross-check it against; .1 is the real current
# stable patch release and has one.)
#
pkg_name="perl"
pkg_version="5.40.1"
pkg_source="https://www.cpan.org/src/5.0/perl-5.40.1.tar.gz"
pkg_sha256="02f8c45bb379ed0c3de7514fad48c714fd46be8f0b536bfd5320050165a1ee26"
pkg_depends=""

# Perl's own Configure (not autotools -- a hand-rolled, interactive-by-
# default script, -des makes it non-interactive with defaults) --
# confirmed to work directly inside the isolated build container.
# -Dusethreads matches this build host's own perl (confirmed via
# `perl -V:usethreads` on the host), so anything that assumes a
# thread-capable perl still works the same way here. -Dcc=tcc is
# Perl's own documented Configure flag for overriding the compiler
# (not a bare CC= environment guess -- Configure's own interactive-
# default-skipping logic under -des doesn't reliably honor environment
# overrides the way autotools' ./configure does) -- added as part of
# task #845's audit of every recipe missing an explicit tcc pin.
#
# Real, first-ever TCC rebuild verification of this recipe (issue #25's
# own hostapd work needed a working perl -> openssl chain and hit this
# live) found one genuine, precisely-isolated gap: perl.h's own
# PERL_DIAG_STR_(x) macro expands to a PARENTHESIZED string-literal
# concatenation, `("" x "")`, used in dquote.c (and elsewhere) as a
# `char[]` array initializer. GCC/Clang accept a parenthesized string-
# literal expression there; TCC's stricter array-initializer parser
# does not ("character array initializer must be a literal, optionally
# enclosed in braces") -- confirmed via two minimal, isolated probes:
# `char x[] = ("" "s" "");` fails, `char x[] = "" "s" "";` (identical,
# parens removed) succeeds. The parens are not semantically load-
# bearing here (a string-literal-concatenation is already a single,
# complete primary expression in every context this macro is actually
# used, function-argument or array-initializer) -- stripping them is
# correct, not a hack. Applied as a targeted sed against the extracted
# source rather than a maintained patch file, matching this project's
# own established convention for a single-line third-party compat fix
# (see m4/1.4.19's own gnulib _GL_EXTERN_INLINE_STDHEADER_BUG fix,
# squashfs-tools' -Dlinux=1 fix -- same class of narrow, well-justified
# TCC gap, not upstream perl being wrong).
#
# A second, separate gap found immediately after the first: perl's own
# hints/linux.sh sets ccdlflags to a dynamic-symbol-export flag spelled
# `-E`/`-Wl,-E` (the traditional ld shorthand for --export-dynamic),
# which TCC's own linker frontend doesn't recognize at all ("tcc: error:
# unsupported linker option '-E'"). This one is genuinely load-bearing,
# not a spurious flag to strip -- confirmed the hard way: stripping it
# outright let the link succeed, but then every dynamically-loaded XS
# module failed at runtime ("Cwd.so: undefined symbol:
# Perl_croak_nocontext"), since the main perl binary's own symbols
# were never exported for its own .so modules to resolve against. TCC
# does support the underlying capability, just under its own spelling
# (`tcc --help`: "-rdynamic export all global symbols to dynamic
# linker") -- fixed with a thin cc wrapper translating -E/-Wl,-E to
# -rdynamic (matching chrony.recipe's own toolwrap pattern) rather than
# a bare -Dcc=tcc. Verified end-to-end, not just "link succeeded":
# `./perl -Ilib -e 'use Cwd; print Cwd::getcwd()'` and `use POSIX;
# POSIX::floor(3.7)` both load their real, dynamically-linked .so and
# run correctly.
pkg_build() {
	sed -i 's/#define PERL_DIAG_STR_(x)[[:space:]]*("" x "")/#define PERL_DIAG_STR_(x) "" x ""/' perl.h
	grep -q '#define PERL_DIAG_STR_(x) "" x ""' perl.h || exit 1

	mkdir -p /build/toolwrap
	cat > /build/toolwrap/tcc-perl <<'WRAP'
#!/usr/bin/bash
args=()
for a in "$@"; do
	case "$a" in
		-E|-Wl,-E) args+=("-rdynamic") ;;
		*) args+=("$a") ;;
	esac
done
exec tcc "${args[@]}"
WRAP
	chmod +x /build/toolwrap/tcc-perl

	./Configure -des -Dcc=/build/toolwrap/tcc-perl -Dprefix=/usr -Dusethreads
	make -j"$(nproc)"
}

# Confirmed via ldd against the real perl binary and every one of its
# core XS (compiled) modules under lib/perl5: everything links only
# against libm, libcrypt.so.1 (crypt()/password hashing, a real,
# separate glibc-family library, not part of libc itself), and libc --
# libcrypt is staged the same SONAME-symlink-plus-real-target pattern
# every other recipe's own runtime libs already use. usr/lib/perl5
# (the real standard library -- core modules, not documentation) is
# kept in full; it's what makes perl actually usable, the same
# "load-bearing runtime data" reasoning autoconf.recipe's own
# usr/share/autoconf staging already established. Man pages
# (usr/share/man, ~20MB, real pod-to-man output for every core module)
# are dropped -- no image in this project's own set ships man
# infrastructure for anything else either.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man"
	mkdir -p "$PKG_DESTDIR/lib/x86_64-linux-gnu"
	cp -a /lib/x86_64-linux-gnu/libcrypt.so.1 /lib/x86_64-linux-gnu/libcrypt.so.1.1.0 \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
}
