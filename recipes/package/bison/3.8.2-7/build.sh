#
# bison -- GNU Bison, the parser generator (yacc-compatible). Needed by
# iproute2.recipe's own bison-driven grammar (already staged as a
# toolchain-only build dependency, /usr/share/bison in
# test/test_image_fixture.c's extras[]) and now, with this recipe,
# actually installable as a real runtime package into a target image
# for its own dev-toolchain use.
#
# Source is GNU's own canonical ftp.gnu.org release, unchanged from
# 3.8.2, checksum verified against two independent mirrors
# (ftp.gnu.org and mirrors.kernel.org) -- byte-identical, same sha256.
#
pkg_name="bison"
pkg_version="3.8.2-7"
pkg_source="https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.gz"
pkg_sha256="06c9e13bdf7eb24d4ceb6b59205a4f67c2c7e7213119644430fe82fbd14a0abb"

# Prebuilt artifact for THIS exact version (ADR-0122 package tier).
# With it set, an install fetches <artifact base_url>/bison-3.8.2-2.tar.gz
# and verifies it against this checksum instead of building from
# source; without it the artifact tier is skipped entirely.
# The checksum lives here, in git, because the artifact server is
# never a trust boundary -- it serves bytes, this line approves them.
pkg_artifact_sha256="af823589ec2e4cd70fda4d3996f03de1130524e5af4afe93f2c58786e51d8c8e"
pkg_depends="m4"
#
# Build tools derived rather than guessed: the baseline the declaring
# recipes converge on, plus what this recipe's own pkg_build() invokes
# and the libraries it already declares. See
# docs/guides/writing-recipes.md.
#
pkg_build_depends="tcc make linux-headers bash coreutils sed grep gawk binutils findutils diffutils m4"
pkg_changelog="3.8.2-7: rebuilt against tcc 0.9.28rc (ADR-0223). The 2017 0.9.27 release could give two simultaneously-live locals the same stack slot (#216), a fault that corrupts values silently wherever the aliased pair is only read and written, so every binary it produced is suspect rather than merely the ones that failed. No source change: the revision exists to make the rebuild real, because an image version is a hash of the package manifest (ADR-0155) and a same-version reinstall is deduped and discarded. 3.8.2-6: works around the gnulib/glibc-2.44 posix_spawn_file_actions_addchdir clash, same as m4. 3.8.2-3: declares its build tools so it can be rebuilt through the ordinary install path (#206)"

# 3.8.2's own bare `CC=tcc ./configure` failed the same, already
# root-caused way m4.recipe/sed/coreutils/grep all document: TCC's C99
# static-inline conformance gap makes gnulib emit full, globally-
# visible definitions (the gl_list_*/gl_oset_* generic container
# helpers, xnrealloc/xsum*/xmax, the xtime_*/timespec_* helpers)
# instead of file-local ones, producing dozens of "defined twice"
# errors in lib/libbison.a. Same fix:
# `_GL_EXTERN_INLINE_STDHEADER_BUG=1` forces gnulib's own designed-in
# `static _GL_UNUSED` fallback.
#
# The accompanying `tcc: error: undefined symbol '__dso_handle'` is the
# same real, environment-specific bare-tcc-link CRT gap sysklogd/m4/
# sed/coreutils/grep all already document -- fixed the same proven
# way: a `weak` stub object passed as a bare object-file path in LIBS.
pkg_build() {
	echo 'void *__dso_handle __attribute__((weak)) = (void *)0;' > dso_stub.c
	tcc -c dso_stub.c -o dso_stub.o

	#
	# glibc 2.44 exports posix_spawn_file_actions_addchdir_np as a real
	# symbol but provides the POSIX-2024 name only as a header-level asm
	# alias. autoconf's link probe deliberately declares the function
	# itself rather than including the header, so it links a bare name
	# that does not exist and concludes the function is absent; gnulib
	# then redeclares it on top of glibc's own declaration:
	#
	#   ./lib/spawn.h:1445: error: incompatible types for redefinition of
	#                       'posix_spawn_file_actions_addchdir'
	#
	# Corrected through autoconf's own cache variables, which is
	# truthful: the function IS available to any translation unit that
	# includes the header, the only legitimate way C code calls it. That
	# lands on gnulib's strictly safer REPLACE=1 branch. Same bug as m4;
	# full trail in recipes/package/m4/1.4.20-2/build.sh.
	#
	# Exported ABOVE the configure invocation, not inserted into it: the
	# CC=... line below ends in a backslash and ./configure is its
	# continuation, so anything placed between them breaks the
	# continuation and configure then finds no compiler at all.
	#
	export ac_cv_func_posix_spawn_file_actions_addchdir=yes
	export ac_cv_func_posix_spawn_file_actions_addfchdir=yes

	CC=tcc CFLAGS="-D_GL_EXTERN_INLINE_STDHEADER_BUG=1" \
	    ./configure --prefix=/usr LIBS="$(pwd)/dso_stub.o"
	make -j"$(nproc)" MAKEINFO=true
}

# A real, load-bearing runtime dependency, not just a build-time one:
# bison hardcodes the absolute path to the m4 binary it was configured
# against (confirmed via `strings` on the built binary --
# "/usr/bin/m4" is compiled in) and shells out to it at parser-
# generation time, not just while building bison itself -- so
# pkg_depends="m4" above is required, not optional, or a target image
# with bison but no m4 would fail at first real use, not at install
# time. Confirmed via ldd that bison itself links against nothing but
# libc; yacc is bison's own compatibility wrapper script, not a
# separate binary.
#
# usr/share/bison/ is ALSO a real runtime dependency, not docs -- it
# holds the m4sugar/skeleton files bison's own grammar generation
# reads at every real invocation, not just build-time output. The
# first version of this recipe wiped the whole of usr/share
# (man/info/locale, genuinely doc-only) and took usr/share/bison down
# with it by accident -- caught empirically the first time something
# actually tried to run bison from an image built with this recipe
# (a real kernel hostbuild, ADR-0056), not by inspection. Only the
# genuinely doc-only subdirectories are trimmed now.
pkg_install() {
	make install DESTDIR="$PKG_DESTDIR" MAKEINFO=true
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/info" \
	       "$PKG_DESTDIR/usr/share/locale" "$PKG_DESTDIR/usr/lib"/*.a
}
