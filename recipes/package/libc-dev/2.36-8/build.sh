#
# libc-dev -- glibc's own development files: headers (stdio.h,
# stdlib.h, string.h, etc.) and the real C-runtime startup/teardown
# object files (crt1.o/crti.o/crtn.o and friends) every dynamically-
# linked ELF binary on this platform needs at link time. Found as a
# real, missing gap only by actually trying to compile a real C program
# with the newly-built gcc.recipe inside a real running container --
# every prior recipe in this project only ever produced a *finished*,
# already-linked binary (built inside the isolated sandbox, using the
# sandbox's own real glibc-dev), so nothing before this recipe ever
# needed glibc's own dev files present in a *target* image. gcc.recipe
# supplies its own crtbegin.o/crtend.o and libgcc -- those are GCC's
# own files, real and already staged there; crt1.o/crti.o/crtn.o and
# the libc headers below are glibc's, a genuinely separate package on
# every real distro (libc6-dev on Debian) and equally separate here.
#
# Building glibc itself from source is a real, giant undertaking of
# its own -- a multi-day bootstrap with its own kernel-header/build-
# system intricacies, squarely out of scope for this recipe set, the
# same "too complex to reasonably build here, stage the real
# pre-built host copy instead" call this project already made for the
# Go/Rust toolchains (test/test_image_fixture.c's own extras[],
# staged for the BUILD sandbox's own use). This recipe makes the
# equivalent real files available in a *target* image instead, which
# that toolchain-extras mechanism doesn't reach. pkg_source below is
# the real, genuine glibc 2.36 upstream release -- the exact version
# this build host's own already-installed libc6-dev is (confirmed via
# `dpkg -s libc6-dev`) -- fetched and checksum-verified against two
# independent GNU mirrors for real provenance, even though
# pkg_install() below copies the pre-built files directly from this
# build container's own toolchain-provided glibc-dev (not built from
# this tarball -- see pkg_build()'s own comment).
#
pkg_name="libc-dev"
pkg_version="2.36-8"
# mirrors.kernel.org, not ftp.gnu.org (issue #167): every TLS
# handshake to ftp.gnu.org fails from this network --
# "SSL_ERROR_SYSCALL" -- reproduced from two independent hosts over
# both IPv4 and IPv6, with DNS resolving and TCP 443 connecting. The
# bytes are identical and the checksum below is unchanged from -4,
# which is the whole point of the checksum living in git: it approves
# a byte sequence, not a hostname, so the mirror needs no new trust.
# Verified by downloading from this URL and checking it against that
# same sha256. 28 other recipes are still pinned to ftp.gnu.org and
# need the same treatment as a batch -- see #167.
pkg_source="https://mirrors.kernel.org/gnu/libc/glibc-2.36.tar.xz"
pkg_sha256="1c959fea240906226062cb4b1e7ebce71a9f0e3c0836c09e7e3423d434fcfe75"
pkg_artifact_sha256="d932ad2d06d050c612f4a64d1d5f3b0b18d208a19d274449e0c822b1ad953faf"
pkg_depends=""
# ADR-0199/0209: every tool this recipe actually reaches for, and
# nothing else -- there is no fallback environment to inherit from any
# more (#168). Derived by reading the two function bodies below rather
# than copied from another recipe: pkg_build() runs grep; pkg_install()
# runs mkdir and cp (coreutils) and one sed. echo/printf/test/for/if
# are bash builtins and need no package. Note "tar.h" in the header
# list below is a glibc header, not the tar tool -- a name-match is not
# a dependency.
# libc-dev is in its own build environment, and that is the whole
# point rather than an oversight. -6 declared only the four tools its
# function bodies run, which was a correct reading of those bodies and
# still could not build: the composed environment then contains no
# glibc headers at all, and both halves of this recipe read them.
# pkg_build()'s version assertion failed first, saying so exactly --
#   grep: /usr/include/features.h: No such file or directory
#   toolchain's own glibc-dev is not 2.36
# -- and pkg_install() would have found nothing to copy either.
#
# Before ADR-0199 this worked by accident: the shared sandbox carried
# the build host's entire /usr, so /usr/include was simply always
# there, belonging to no package and named by no recipe. That is the
# exact implicit-dependency class #168 exists to end, and ending it is
# what exposed this one.
#
# So the headers now come from a real, named, versioned package: the
# previously published libc-dev. Building version N+1 in an environment
# holding version N is ordinary toolchain bootstrapping, and it is
# strictly better than what it replaces -- the input is a checksummed
# artifact with a version, not whatever a long-lived sandbox had
# accumulated.
#
# It does NOT launder the input. These headers still originate from a
# distribution glibc via the original bootstrap; that is the last
# unclosed link in this project's self-hosting, tracked as its own
# work and closed only by a real glibc built from source. What this
# revision does change is that the wholesale `cp -a /usr/include/.`
# is gone: -6 already replaced it with 106 individually named glibc
# headers plus the kernel UAPI trees, so selecting them out of the
# predecessor's tree yields exactly the glibc set and drops the
# foreign headers (Erlang, valgrind, the host gcc's C++ tree) that got
# the kernel-builder image artifact withdrawn under #169.
#
# Deliberately NOT version-pinned. The `name@version` form exists
# (#127) and would be more reproducible, but the only libc-dev in the
# artifact cache is 2.36-3 -- 2.36-5, the newest installed, was never
# published -- so a pin would name bytes a fresh host cannot obtain.
# Unpinned resolves to the newest installed copy, which is what a
# bootstrap step should do.
pkg_build_depends="bash coreutils sed grep libc-dev"

# Deliberately not a real build -- see this recipe's own header
# comment for why (glibc's own build is its own giant undertaking, out
# of scope). This step exists only to fail loudly if the fetched
# source's own version doesn't match what pkg_install() is about to
# copy from the toolchain sandbox's real glibc-dev, keeping the two in
# sync rather than silently drifting apart.
pkg_build() {
	grep -Eq '#define[[:space:]]+__GLIBC_MINOR__[[:space:]]+36' /usr/include/features.h || {
		echo "toolchain's own glibc-dev is not 2.36 -- update pkg_version/pkg_source above to match" >&2
		exit 1
	}
}

# Real files copied directly from this build container's own
# toolchain-provided glibc-dev (confirmed present via the wholesale
# /usr/include, /usr/lib/x86_64-linux-gnu toolchain copy every other
# recipe's own build already relies on) -- not built from the fetched
# tarball above. crt1.o (the real _start/__libc_start_main entry
# point), crti.o/crtn.o (.init/.fini section prologue/epilogue,
# bracketing gcc's own crtbegin.o/crtend.o), Scrt1.o (the PIE variant,
# what gcc actually links by default -- confirmed via gcc.recipe's own
# `gcc -v` output using non-PIE crt1.o only because this recipe was
# tested with default flags; Scrt1.o staged too since PIE is this
# platform's own real default for anything not passing -no-pie), and
# gcrt1.o (profiling variant, real and small, kept for completeness)
# are all genuinely required at link time -- confirmed the hard way,
# `collect2: fatal error: cannot find 'ld'` then (after symlinking ld
# into gcc's own private prefix directory, a separate real finding --
# see gcc.recipe) `cannot find crt1.o: No such file or directory` on a
# real compile+link attempt inside a real running container. libc.so
# (a small real GNU ld script, not a symlink -- confirmed via `file`,
# `GROUP ( /lib/x86_64-linux-gnu/libc.so.6 ... )`-style, what `-lc`
# actually resolves against at link time) and libc.a (static libc,
# real and legitimate for anything statically linking) are kept too.
# The full real /usr/include tree (88MB, glibc's own headers plus
# every other dev package already installed on this build host --
# openssl, zlib, ncurses, sqlite3, etc.) is staged wholesale rather
# than hand-picking "just glibc's" headers out of it -- the same
# "too fragile to hand-curate perfectly, stage the real complete tree"
# reasoning already applied to gcc's own runtime library staging.
# Destination for the object files/libc.so/libc.a is /lib/x86_64-linux-gnu
# (this project's own real runtime-library path, confirmed via `ls -la`
# that /lib is a genuine directory here, not a /usr/lib symlink the way
# a real usrmerge'd host has it) -- NOT /usr/lib/x86_64-linux-gnu, which
# is where the build host itself keeps them but which gcc's own
# LIBRARY_PATH (confirmed via `gcc -v`) never actually searches on this
# platform; only /lib/x86_64-linux-gnu/ is in that list.
#
# ADR-0057: the crt objects (crt1.o/crti.o/crtn.o/Scrt1.o/gcrt1.o/
# Mcrt1.o) are ALSO staged a second time at /usr/lib/x86_64-linux-gnu --
# confirmed the hard way via a real cix.recipe hostbuild failing
# `tcc: error: file 'crt1.o' not found` even though the file genuinely
# existed at /lib/x86_64-linux-gnu, then root-caused against tinycc's own
# upstream source (tcc.h's CONFIG_TCC_CRTPREFIX macro, confirmed vanilla,
# not a Debian patch): TCC maintains a separate, single-path "crt:"
# search list independent of its broader "libraries:" list used for
# libc.a/libc.so/-l lookups, and that single path defaults to
# /usr/lib/x86_64-linux-gnu specifically -- a real, structural
# difference from gcc's own LIBRARY_PATH convention, not an oversight in
# either tool. libc.a/libc_nonshared.a/libpthread*.a are NOT duplicated
# there since TCC's broader "libraries:" list (confirmed via `tcc -vv`)
# already includes /lib/x86_64-linux-gnu -- only the crt objects need
# the second copy. libc.so is itself a real GNU ld script, not a symlink (confirmed via `cat`):
# `GROUP ( /lib/x86_64-linux-gnu/libc.so.6 /usr/lib/x86_64-linux-gnu/
# libc_nonshared.a AS_NEEDED (...) )` -- its first path already matches
# this project's own convention, but its second (libc_nonshared.a, a
# small real static archive with the handful of libc symbols that are
# never in the shared lib) points at /usr/lib/x86_64-linux-gnu, which
# doesn't exist on this project's own images. Rather than introduce a
# second, otherwise-unused directory convention just for one file, the
# script is rewritten here to reference /lib/x86_64-linux-gnu for both
# paths, matching where libc_nonshared.a is actually staged below.
pkg_install() {
	mkdir -p "$PKG_DESTDIR/usr/include" "$PKG_DESTDIR/lib/x86_64-linux-gnu" \
	   "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu"
	# -6 (issue #169): stage what a libc development environment IS,
	# by name. -5 and every version before it did
	#
	#     cp -a /usr/include/. "$PKG_DESTDIR/usr/include/"
	#
	# which is the build container's ENTIRE /usr/include -- and via the
	# bootstrap that directory is the build HOST's, so every image
	# installing libc-dev received another operating system's headers.
	# Measured in a real image built this way: valgrind/callgrind.h,
	# erl_driver.h and ei_connect.h (Erlang), and c++/12/* (the host
	# gcc's C++ headers) in an image declaring none of them and pinning
	# gcc 16.2.0. The Build Provenance Mandate forbids exactly this.
	#
	# The lists below are not guesses: they are what `dpkg -L libc6-dev`
	# and `dpkg -L linux-libc-dev` actually own. Everything else under
	# /usr/include belongs to some other package and has no business
	# here. Note especially x86_64-linux-gnu/, which is NOT copied
	# wholesale -- it also holds libstdc++-12, libffi, lua and openssl
	# headers, so its glibc/kernel entries are named individually.
	#
	# The one line immediately below this comment used to be the
	# outlier in a recipe that otherwise names every file it takes
	# (crt1.o, crti.o, crtn.o, Scrt1.o, libc.a ...). It now matches.
	GLIBC_HEADERS="aio.h aliases.h alloca.h ar.h argp.h argz.h assert.h 
	  byteswap.h complex.h cpio.h ctype.h dirent.h dlfcn.h elf.h endian.h 
	  envz.h err.h errno.h error.h execinfo.h fcntl.h features-time64.h 
	  features.h fenv.h fmtmsg.h fnmatch.h fstab.h fts.h ftw.h gconv.h getopt.h 
	  glob.h gnu-versions.h grp.h gshadow.h iconv.h ifaddrs.h inttypes.h 
	  langinfo.h lastlog.h libgen.h libintl.h limits.h link.h locale.h malloc.h 
	  math.h mcheck.h memory.h mntent.h monetary.h mqueue.h netdb.h nl_types.h 
	  nss.h obstack.h paths.h poll.h printf.h proc_service.h pthread.h pty.h 
	  pwd.h re_comp.h regex.h regexp.h resolv.h sched.h search.h semaphore.h 
	  setjmp.h sgtty.h shadow.h signal.h spawn.h stab.h stdc-predef.h stdint.h 
	  stdio.h stdio_ext.h stdlib.h string.h strings.h syscall.h sysexits.h 
	  syslog.h tar.h termio.h termios.h tgmath.h thread_db.h threads.h time.h 
	  ttyent.h uchar.h ucontext.h ulimit.h unistd.h utime.h utmp.h utmpx.h 
	  values.h wait.h wchar.h wctype.h wordexp.h"

	GLIBC_DIRS="arpa finclude net netash netatalk netax25 neteconet netinet
	  netipx netiucv netpacket netrom netrose nfs protocols rpc scsi"

	# sys/ is deliberately NOT in that list. On a multiarch layout
	# glibc's own sys/ lives under x86_64-linux-gnu/sys (staged below),
	# and top-level /usr/include/sys belongs to whatever else installed
	# headers there -- on the host this was written against, libcap-dev
	# (capability.h, psx_syscall.h). Copying it blindly re-introduced
	# foreign headers, which is how this line came to be tested rather
	# than assumed. It is still staged where it genuinely is glibc's,
	# decided by content and not by guessing the layout.
	if test -e /usr/include/sys/types.h; then
		cp -a /usr/include/sys "$PKG_DESTDIR/usr/include/"
	fi

	# Kernel UAPI headers (linux-libc-dev). Genuinely required to
	# compile against a Linux kernel; they are not glibc's, but they
	# are part of what "libc development environment" means here.
	KERNEL_DIRS="asm-generic linux misc mtd rdma sound video xen"

	# x86_64-linux-gnu/ entries owned by glibc or the kernel headers,
	# named one by one for the reason given above.
	MULTIARCH_ENTRIES="a.out.h asm bits fpu_control.h gnu ieee754.h sys"

	for h in $GLIBC_HEADERS; do
		cp -a "/usr/include/$h" "$PKG_DESTDIR/usr/include/" || {
			echo "libc-dev: /usr/include/$h missing -- this build container has no complete glibc-dev" >&2
			exit 1
		}
	done
	for d in $GLIBC_DIRS $KERNEL_DIRS; do
		test -d "/usr/include/$d" || continue
		cp -a "/usr/include/$d" "$PKG_DESTDIR/usr/include/"
	done
	mkdir -p "$PKG_DESTDIR/usr/include/x86_64-linux-gnu"
	for e in $MULTIARCH_ENTRIES; do
		test -e "/usr/include/x86_64-linux-gnu/$e" || continue
		cp -a "/usr/include/x86_64-linux-gnu/$e" "$PKG_DESTDIR/usr/include/x86_64-linux-gnu/"
	done

	# A gate, not a comment: prove no other operating system's headers
	# came along. These are the exact markers found in the contaminated
	# image that prompted #169, so a regression reproduces as a failed
	# build rather than as a shipped artifact.
	for foreign in valgrind erl_driver.h ei_connect.h c++ node openssl lua5.1 X11 python3.11 sys/capability.h; do
		if test -e "$PKG_DESTDIR/usr/include/$foreign"; then
			echo "libc-dev: foreign header tree '$foreign' staged -- see issue #169" >&2
			exit 1
		fi
	done
# -8: every SOURCE path above reads /lib/x86_64-linux-gnu, not
# /usr/lib/x86_64-linux-gnu. -7 read the latter and failed on the very
# first archive:
#   cp: cannot stat '/usr/lib/x86_64-linux-gnu/libc.a': No such file
#   cp: cannot stat '/usr/lib/x86_64-linux-gnu/libc_nonshared.a': ...
# The reason is this recipe's own staging layout, one revision earlier
# in the same bootstrap chain. It writes the crt objects to BOTH
# lib/x86_64-linux-gnu and usr/lib/x86_64-linux-gnu (deliberately --
# see the comment above about tcc's single hardcoded crt path), but the
# archives and libc.so to lib/x86_64-linux-gnu ONLY. So when the
# predecessor package IS the input, the crt reads resolve and every
# archive read does not.
#
# On the original Debian bootstrap host both spellings named the same
# file, because a usrmerge'd /lib is a symlink to /usr/lib -- so this
# was invisible for as long as the input was a distribution host
# rather than a package. Exactly the class CLAUDE.md already records
# for mkbootroot's staging paths: a /usr-prefixed path that "only ever
# resolved by accident on a rich dev sandbox".
#
# /lib/x86_64-linux-gnu is the correct spelling in BOTH environments --
# it is where this recipe stages, and on a usrmerge'd host it still
# resolves to the same file -- so this is a fix, not a trade.
	cp -a /lib/x86_64-linux-gnu/crt1.o /lib/x86_64-linux-gnu/crti.o \
	   /lib/x86_64-linux-gnu/crtn.o /lib/x86_64-linux-gnu/Scrt1.o \
	   /lib/x86_64-linux-gnu/gcrt1.o /lib/x86_64-linux-gnu/Mcrt1.o \
	   /lib/x86_64-linux-gnu/libc.a /lib/x86_64-linux-gnu/libc_nonshared.a \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"
	cp -a /lib/x86_64-linux-gnu/crt1.o /lib/x86_64-linux-gnu/crti.o \
	   /lib/x86_64-linux-gnu/crtn.o /lib/x86_64-linux-gnu/Scrt1.o \
	   /lib/x86_64-linux-gnu/gcrt1.o /lib/x86_64-linux-gnu/Mcrt1.o \
	   "$PKG_DESTDIR/usr/lib/x86_64-linux-gnu/"
	sed 's|/usr/lib/x86_64-linux-gnu/|/lib/x86_64-linux-gnu/|' /lib/x86_64-linux-gnu/libc.so \
	   > "$PKG_DESTDIR/lib/x86_64-linux-gnu/libc.so"
	# -lpthread compat archives: glibc >= 2.34 folded libpthread's
	# functions into libc.so.6 itself and ships no libpthread.so at
	# all anymore (confirmed on this exact host -- only the .a/
	# _nonshared.a static compat archives remain), but real-world
	# build systems (the Linux kernel's own scripts/Makefile.host
	# among them) still pass -lpthread explicitly. Confirmed missing
	# the hard way (ADR-0056): a real kernel hostbuild failed on
	# `cannot find -lpthread` the moment it built its first HOSTCC
	# tool linked with -lpthread (scripts/sorttable).
	cp -a /lib/x86_64-linux-gnu/libpthread.a \
	   /lib/x86_64-linux-gnu/libpthread_nonshared.a \
	   "$PKG_DESTDIR/lib/x86_64-linux-gnu/"

	# -ldl: the identical story one library over, and missed until a
	# build environment held only what it declared. glibc >= 2.34
	# folded libdl into libc.so.6 the same way it folded libpthread,
	# leaving an 8-byte placeholder archive and no libdl.so at all --
	# but real build systems still pass -ldl unconditionally. tcc's own
	# Makefile does (`tcc -o tcc tcc.o libtcc.a -lm -ldl`), so tcc could
	# not link itself: `tcc: error: library 'dl' not found`. Staged
	# exactly like libpthread.a above, for exactly the same reason.
	cp -a /lib/x86_64-linux-gnu/libdl.a "$PKG_DESTDIR/lib/x86_64-linux-gnu/"

	# libm.so: the same real, missing-until-actually-tried gap
	# libpthread.a/libc.so above already document, found the same way --
	# a real g++ link of a genuine C++ program (gcc/4.7.4-6's own
	# validation) failed with `cannot find -lm`, even though the real
	# runtime library it needs (libm.so.6) is already present via
	# pkg_seed_image_baseline()'s own global runtime set. `-lm` (and
	# libstdc++'s own internal use of libm) needs the bare, unversioned
	# *dev* symlink specifically, which nothing before this recipe's own
	# libc.so ever staged. glibc's own real
	# `/usr/lib/x86_64-linux-gnu/libm.so` here is a GNU ld script
	# (confirmed via `file`+`cat`, not a plain symlink):
	# `GROUP ( /lib/.../libm.so.6 AS_NEEDED ( /lib/.../libmvec.so.1 ) )`.
	#
	# -2's own straight path-rewrite of that real script hit a second,
	# real gap immediately: this project's own gcc-built ld (confirmed
	# with gcc/4.7.4's own bundled binutils-era ld, an older release)
	# does NOT treat AS_NEEDED inside a GROUP as genuinely optional the
	# way glibc's own upstream comment intends -- it errors outright,
	# `cannot find libmvec.so.1`, for a library this project never
	# stages (SIMD-vectorized math variants, a real but niche glibc
	# optimization, not architecturally required for `-lm` to work at
	# all). Written out as a plain, minimal GROUP referencing only the
	# real, always-present libm.so.6 -- not a blind rewrite of the
	# upstream script's own text -- since the AS_NEEDED clause has no
	# safe, portable way to express "genuinely optional" across every ld
	# this project's own toolchains might use.
	printf 'GROUP ( /lib/x86_64-linux-gnu/libm.so.6 )\n' \
	   > "$PKG_DESTDIR/lib/x86_64-linux-gnu/libm.so"
}
