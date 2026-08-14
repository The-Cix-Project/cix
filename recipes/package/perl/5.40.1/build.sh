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
# thread-capable perl still works the same way here.
pkg_build() {
	./Configure -des -Dprefix=/usr -Dusethreads
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
