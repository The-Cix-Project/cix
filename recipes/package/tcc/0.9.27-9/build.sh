#
# tcc -- the Tiny C Compiler, the exclusive toolchain this project's
# own daemon/CLI are built with (ADR-0001). An ordinary recipe (built
# inside the shared toolchain sandbox's own real gcc, not a hostbuild)
# -- installed onto a "cix-builder" image so cix.recipe (ADR-0057)
# has a real tcc to build cixd/cixctl with via hostbuild.
#
# Source is tinycc's own canonical download.savannah.nongnu.org
# release. Version matches this sandbox's own already-installed system
# tcc (0.9.27, confirmed via `tcc -v`) -- the same compiler already
# proven, this whole project long, to build this codebase cleanly.
#
# -9 adds __has_include support (see the patch below). Without it,
# util-linux 2.42's own include/c.h cannot be preprocessed at all, so
# libblkid -- and therefore btrfs-progs, and therefore mkfs.btrfs --
# could not be built by this compiler.
#
pkg_name="tcc"
pkg_version="0.9.27-9"
pkg_source="https://download.savannah.nongnu.org/releases/tinycc/tcc-0.9.27.tar.bz2"
pkg_sha256="de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"

# No pkg_artifact_sha256 for this version, deliberately: that line
# approves one specific byte sequence, and no -9 artifact has been
# built and published by a Cix host yet. Carrying -7's checksum
# forward would approve bytes that are not this build. It gets added
# once a real host has produced the artifact -- never from a tarball
# built anywhere else.
# Nothing at runtime: tcc links against libc alone, and its own
# libtcc1.a is part of this package.
pkg_depends=""
#
# Issue #109 / ADR-0199: the tools this package needs to BUILD,
# declared rather than inherited from whatever a shared sandbox
# happened to accumulate. The build container is composed from
# exactly these and nothing else, so this list is not
# documentation -- it is the environment.
#
# Each entry earns its place:
#   tcc       the compiler this recipe pins with --cc=tcc -- tcc builds
#             itself. Through -2 it did not: tcc's configure is hand
#             written and ignores a CC= environment variable entirely,
#             taking --cc= instead, so every tcc this project has ever
#             installed was compiled by whatever ambient gcc the shared
#             sandbox happened to carry. Visible only once the
#             environment held exactly what was declared: the build
#             printed `C compiler  gcc (0.0)` and then died with
#             `make: gcc: No such file or directory`
#   libc-dev  headers and libc to compile and link against
#   make      runs the generated Makefile
#   bash      pkg_build() runs under it, and configure is a shell script
#   coreutils nproc/rm/mkdir/cat/expr/ln, used throughout configure,
#             config.status and the Makefile
#   sed       configure rewrites its own output with sed constantly
#   grep      every feature test that greps a compiler or header
#   gawk      AC_PROG_AWK equivalents in the Makefile
#   binutils  the Makefile archives libtcc1.a with ar
#             (tcc -ar does the archiving itself, but configure
#             still probes for a real one)
#
# Sufficiency is enforced by the build itself. Minimality is
# review, not enforcement (ADR-0199).
pkg_build_depends="tcc make libc-dev bash coreutils sed grep gawk binutils"


# lib/bcheck.c (TCC's own optional bounds-checking runtime, only used
# by programs compiled with tcc's own -b flag -- not needed to build
# cixd/cixctl, which never pass it) unconditionally defines
# CONFIG_TCC_MALLOC_HOOKS on Linux and references glibc's
# __malloc_hook/__free_hook/etc. directly. Real, confirmed upstream/
# environment incompatibility, not a project bug: TCC 0.9.27 was
# released in 2017, years before glibc 2.34 (2021) removed those
# hooks' declarations from <malloc.h> entirely -- every real distro
# packaging tcc 0.9.27 against a modern glibc carries an equivalent
# patch. Confirmed empirically: the exact same source built clean
# before this patch on distro/gcc conditions predating glibc 2.34, and
# fails identically to this on any glibc >= 2.34 without it (this
# build host's own glibc is 2.36). Guards the existing
# CONFIG_TCC_MALLOC_HOOKS definition with the same glibc-version check
# real downstream packages use, mirroring the file's own existing
# BSD/Windows exclusion block for the identical reason (those
# platforms never had these hooks to begin with).
# TCC 0.9.27 implements neither the __atomic_* builtins nor the
# __ATOMIC_* memory-order macros that GCC and Clang have provided for
# over a decade. Real code uses them with no header and no library flag,
# precisely because they are builtins -- libcap's own internal spinlock
# is two lines of exactly that -- so any such package simply does not
# compile against this compiler.
#
# Supplying them from libtcc1.a is the same answer GCC gives with
# libgcc: the compiler ships its own runtime, and every TCC-linked
# binary already gets this archive. The alternative -- each affected
# recipe carrying its own copy -- would be the same code written N
# times, in the wrong place.
#
# XCHG with a memory operand asserts the LOCK signal implicitly on x86
# (Intel SDM Vol 2A, XCHG), so one xchgb is a full-barrier atomic
# read-modify-write: exactly the sequentially consistent semantics both
# builtins are specified to have, and what GCC emits for them. Verified
# by disassembly (`xchg %al,(%rcx)`) and by running the resulting
# spinlock, not by reading the manual alone.
#
# Known limitation, deliberately not papered over: TCC 0.9.27 has no
# mechanism for predeclaring a builtin's prototype (the tccdefs.h that
# would allow it arrived after this release), so a caller still gets
# "warning: implicit declaration". Harmless for an int-returning
# function, and a package built with -Werror would reject it -- but
# such a package does not build at all today, so this is strictly
# better, never a regression. Tracked rather than accepted silently.
pkg_build() {
	cat > lib/atomic.c <<'ATOMIC_EOF'
/*
 * Atomic builtins TCC 0.9.27 does not implement, supplied as real
 * functions in the compiler's own runtime library. See tcc.recipe for
 * the full reasoning.
 *
 * memorder is accepted and ignored because seq_cst is the strongest
 * ordering: answering every weaker request with a stronger guarantee
 * is always correct.
 */

int __atomic_test_and_set(volatile void *ptr, int memorder)
{
	unsigned char prev = 1;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(prev), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
	return prev != 0;
}

void __atomic_clear(volatile void *ptr, int memorder)
{
	unsigned char zero = 0;

	(void)memorder;
	__asm__ __volatile__("xchgb %0, %1"
	                     : "+q"(zero), "+m"(*(volatile unsigned char *)ptr)
	                     :
	                     : "memory");
}
ATOMIC_EOF

	# __dso_handle belongs in the compiler runtime for the same reason
	# the atomics do, and its absence has been paid for repeatedly:
	# sed, grep, coreutils, m4, bison, gawk and sysklogd each compile
	# and link their own one-line copy of this, because glibc's static
	# destructor bookkeeping expects a symbol that GCC supplies from
	# crtbegin.o and a bare TCC link has nothing to supply. Seven
	# identical stubs is the definition of solving a problem more than
	# once.
	#
	# Strong, not weak, and that is the whole trick. A weak definition
	# does not cause archive extraction -- confirmed directly here, and
	# the reason every one of those recipes passes a bare object path
	# rather than -l anything. A strong definition in an archive is
	# extracted only when nothing else already defines the symbol,
	# which is exactly the wanted behaviour: a program (or a GCC
	# crtbegin) carrying its own __dso_handle simply never pulls this
	# member in. Both paths verified: a program that needs it links,
	# and a program defining its own links with no conflict.
	printf 'void *__dso_handle = (void *)0;\n' > lib/dso_handle.c

	# Build both into libtcc1.a alongside the runtime pieces already
	# there. Purely additive: no existing object or symbol changes, so
	# this cannot alter how anything already built behaves.
	sed -i 's|^X86_64_O = libtcc1.o alloca86_64.o alloca86_64-bt.o$|X86_64_O = libtcc1.o alloca86_64.o alloca86_64-bt.o atomic.o dso_handle.o|' \
	    lib/Makefile
	grep -q 'atomic\.o dso_handle\.o' lib/Makefile || {
		echo "tcc: failed to add atomic.o/dso_handle.o to libtcc1.a" >&2
		exit 1
	}

	# The order constants, predefined by the compiler exactly as GCC
	# predefines them -- code says __ATOMIC_SEQ_CST without including
	# anything, so a header cannot supply these.
	sed -i 's|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);|    tcc_define_symbol(s, "__STDC_HOSTED__", NULL);\n    tcc_define_symbol(s, "__ATOMIC_RELAXED", "0");\n    tcc_define_symbol(s, "__ATOMIC_CONSUME", "1");\n    tcc_define_symbol(s, "__ATOMIC_ACQUIRE", "2");\n    tcc_define_symbol(s, "__ATOMIC_RELEASE", "3");\n    tcc_define_symbol(s, "__ATOMIC_ACQ_REL", "4");\n    tcc_define_symbol(s, "__ATOMIC_SEQ_CST", "5");|' \
	    libtcc.c
	grep -q '__ATOMIC_SEQ_CST' libtcc.c || { echo "tcc: failed to predefine the __ATOMIC_* macros" >&2; exit 1; }

	sed -i '/^#define HAVE_MEMALIGN$/a\
#if defined(__GLIBC__) \&\& (__GLIBC__ > 2 || (__GLIBC__ == 2 \&\& __GLIBC_MINOR__ >= 34))\
#undef CONFIG_TCC_MALLOC_HOOKS\
#endif' lib/bcheck.c
	# -6: do-while codegen fix. Upstream 0.9.27 drops the ENTIRE
	# while-condition of a do-while whose body's own fall-through end
	# is unreachable (ends in break/return/goto): TOK_BREAK sets
	# nocode_wanted, and TOK_DO then emits gexpr()/gvtst() -- the
	# condition -- while that flag is still set, restoring it only
	# afterwards. The condition is genuinely reachable via any
	# `continue` inside the body (their jumps chain onto b and gsym(b)
	# points them exactly there), so every such continue gets patched
	# to the loop EXIT instead, and cond() is never called.
	#
	# Found the hard way as issue #122: GNU tar 1.35's read_and() is
	# exactly this shape -- do { switch (read_header()) { case
	# HEADER_SUCCESS: ...; continue; ... } break; } while
	# (!all_names_found(...)) -- so our tar listed/extracted exactly
	# ONE archive member and exited 0, on every archive, while
	# CREATION (a different loop shape) was always correct. Confirmed
	# by a minimal reproducer: vanilla 0.9.27 runs it once and never
	# calls the condition; Debian's patched 0.9.27 and gcc both loop
	# three times. The fix: when any continue chained onto b, restore
	# code generation before emitting the condition. while/for loops
	# are unaffected (their condition/increment are emitted before the
	# body can poison nocode_wanted) -- verified by the same
	# reproducer battery, which now runs below as a permanent
	# build-time gate.
	sed -i '/skip(TOK_WHILE);/,/gsym(b);/ { /gsym(b);/ a\
        /* issue #122: the condition is reachable via continue even\
           when the body'"'"'s fall-through end is not -- re-enable code\
           generation here or the condition is silently dropped and\
           every continue exits the loop. */\
        if (b)\
            nocode_wanted = saved_nocode_wanted;
	}' tccgen.c
	# Guard on the unique inserted comment, not on a window after
	# skip(TOK_WHILE): -6 checked `grep -A7` for
	# `nocode_wanted = saved_nocode_wanted;`, but that line already
	# exists in unpatched source (the normal restore) AND the insert
	# pushes it past +7 -- so the check both false-passed on unpatched
	# source and false-FAILED on correctly-patched source, which is
	# exactly what aborted the -6 build after the sed had done its job.
	grep -q 'issue #122: the condition is reachable via continue' tccgen.c || {
		echo "tcc: do-while patch did not land -- re-check the sed against tccgen.c" >&2
		exit 1
	}

	# -9: __has_include, which upstream 0.9.27 does not implement at
	# all (nor any of the __has_* operators). In a preprocessor
	# conditional an unknown identifier evaluates to 0, so a real use
	#     #if __has_include(<stdcountof.h>)
	# became 0(<stdcountof.h>) -- a call through a non-function -- and
	# the whole translation unit died with "function pointer expected"
	# pointing at a blank line, which is about as unhelpful as a
	# compiler error gets.
	#
	# Not a theoretical gap: util-linux 2.42's own include/c.h uses
	# it, so libblkid could not be compiled by this compiler at all --
	# and libblkid is what btrfs-progs needs, which provides
	# mkfs.btrfs, which ADR-0207 makes the platform's own storage
	# depend on.
	#
	# Only __has_include is added, on purpose. Its siblings
	# (__has_attribute/__has_feature/__has_extension) need no compiler
	# support, because portable code can define its own fallback for
	# them -- util-linux does exactly that a few lines above the line
	# that broke, which is why the build got as far as it did.
	# __has_include is the one that cannot be faked: any fallback
	# would have to answer the very question it asks.
	#
	# Matched on the token TEXT rather than adding a TOK_ entry to
	# tcctok.h, whose ordering is load-bearing (token ids are
	# positional) -- a one-file change with no numbering impact is the
	# smaller, safer edit.
	#
	# (-8 was this same patch driven by python3, which does not exist
	# in a composed build environment -- only declared build tools do,
	# and this recipe rightly declares none. It failed on the box with
	# "python3: command not found" before compiling a line. Recipe
	# versions are immutable once published, so the fix is -9 rather
	# than an edit. sed and a heredoc are what -7's own do-while patch
	# uses, for exactly this reason.)
	cat > pp_has_include.inc <<'INC_EOF'
/*
 * __has_include(<header>) / __has_include("header"), for #if and #elif.
 *
 * Resolution walks the same search list in the same order as a real
 * #include -- absolute name, then the including file's own directory
 * for the "quoted" form, then -I paths, then system paths -- but only
 * tests openability: nothing is opened as a translation unit, pushed
 * on the include stack, or added to target deps. The header name
 * arrives as ordinary tokens ('<', name, '.', name, '>'), so it is
 * reassembled by concatenating token text up to the closing ')' --
 * the technique tcc's own "computed #include" branch already uses for
 * the identical problem.
 */
static int pp_has_include(void)
{
    TCCState *s1 = tcc_state;
    char buf[1024];
    char name[1024];
    int c, i, n, len, found = 0;

    next_nomacro();
    if (tok != '(')
        tcc_error("'__has_include' expects '('");
    buf[0] = '\0';
    for (;;) {
        next_nomacro();
        if (tok == ')' || tok == TOK_LINEFEED || tok == TOK_EOF)
            break;
        pstrcat(buf, sizeof(buf), get_tok_str(tok, &tokc));
    }
    if (tok != ')')
        tcc_error("'__has_include' expects ')'");

    len = strlen(buf);
    if (len < 2 || ((buf[0] != '"' || buf[len - 1] != '"') &&
                    (buf[0] != '<' || buf[len - 1] != '>')))
        tcc_error("'__has_include' expects \"FILENAME\" or <FILENAME>");
    c = buf[len - 1];
    memcpy(name, buf + 1, len - 2);
    name[len - 2] = '\0';

    n = 2 + s1->nb_include_paths + s1->nb_sysinclude_paths;
    for (i = 0; i < n && !found; ++i) {
        char buf1[sizeof ((BufferedFile *)0)->filename];
        const char *path;
        int fd;

        if (i == 0) {
            if (!IS_ABSPATH(name))
                continue;
            buf1[0] = 0;
        } else if (i == 1) {
            if (c != '"')
                continue;
            path = file->true_filename;
            pstrncpy(buf1, path, tcc_basename(path) - path);
        } else {
            int j = i - 2, k = j - s1->nb_include_paths;

            path = k < 0 ? s1->include_paths[j] : s1->sysinclude_paths[k];
            pstrcpy(buf1, sizeof(buf1), path);
            pstrcat(buf1, sizeof(buf1), "/");
        }
        pstrcat(buf1, sizeof(buf1), name);
        fd = open(buf1, O_RDONLY | O_BINARY);
        if (fd >= 0) {
            close(fd);
            found = 1;
        }
    }
    return found;
}

/* eval an expression for #if/#elif */
INC_EOF

	# Replace the anchor comment line with the function followed by
	# that same comment (the r-then-d idiom): the function lands
	# immediately before expr_preprocess(), and the comment still sits
	# on the function it actually describes.
	sed -i -e '/^\/\* eval an expression for #if\/#elif \*\/$/{r pp_has_include.inc' -e 'd}' tccpp.c

	# The call site, inserted ahead of the catch-all that turns every
	# remaining identifier into 0 -- which is precisely what swallowed
	# __has_include before.
	sed -i '/^        } else if (tok >= TOK_IDENT) {$/i\
        } else if (tok >= TOK_IDENT \&\&\
                   strcmp(get_tok_str(tok, \&tokc), "__has_include") == 0) {\
            c = pp_has_include();\
            tok = TOK_CINT;\
            tokc.i = c;' tccpp.c

	grep -q 'pp_has_include()' tccpp.c || {
		echo "tcc: __has_include patch did not land -- re-check the seds against tccpp.c" >&2
		exit 1
	}

	./configure --prefix=/usr --cc=tcc
	make -j"$(nproc)"

	# The __has_include gate: all three answers must be RIGHT, not
	# merely "it compiled". An unpatched tcc fails this file with
	# "function pointer expected" before any #error is even reached.
	cat > has_include_check.c <<'CHECK_EOF'
#if __has_include(<stdio.h>)
int present = 1;
#else
#error "__has_include said an existing system header is absent"
#endif
#if __has_include(<cix_definitely_not_a_real_header.h>)
#error "__has_include said a nonexistent header is present"
#endif
#if __has_include("cix_definitely_not_here_either.h")
#error "__has_include said a nonexistent quoted header is present"
#endif
int main(void) { return present ? 0 : 1; }
CHECK_EOF
	./tcc -B. has_include_check.c -o has_include_check
	./has_include_check || { echo "tcc: __has_include is present but wrong" >&2; exit 1; }

	# Prove the compiler that was just built actually provides them,
	# the way a caller uses them: no header, no library flag. A tcc
	# that silently lost this would otherwise only be discovered by
	# whatever package needed it next.
	cat > atomic_check.c <<'CHECK_EOF'
static char mutex;
int main(void)
{
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 1;                       /* was free, must report free */
	if (!__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 2;                       /* now held, must report held */
	__atomic_clear(&mutex, __ATOMIC_SEQ_CST);
	if (__atomic_test_and_set(&mutex, __ATOMIC_SEQ_CST))
		return 3;                       /* released, must report free */
	return 0;
}
CHECK_EOF
	./tcc -B. atomic_check.c -o atomic_check
	./atomic_check || { echo "tcc: __atomic_* runtime is present but wrong" >&2; exit 1; }

	# And that __dso_handle resolves out of the archive with no help
	# from the caller -- the whole point of moving it here.
	printf 'extern void *__dso_handle;\nint main(void){return __dso_handle != (void *)0;}\n' \
	    > dso_check.c
	./tcc -B. dso_check.c -o dso_check
	./dso_check || { echo "tcc: __dso_handle did not resolve from libtcc1.a" >&2; exit 1; }

	# The do-while regression gate (issue #122): the loop must run 3
	# times and the condition must be evaluated 3 times, through a
	# continue-inside-switch with the body's own end unreachable. An
	# unfixed tcc runs it once, never calls the condition, and exits 1.
	cat > dowhile_check.c <<'CHECK_EOF'
static int calls;
static int cond(void) { calls++; return calls >= 3; }
static int next_status(void) { static int n; return (n++ < 10) ? 1 : 2; }
int main(void)
{
	int status, iterations = 0;
	do {
		iterations++;
		status = next_status();
		switch (status) {
		case 0: break;
		case 1: continue;
		case 2: break;
		}
		break;
	} while (!cond());
	return (iterations == 3 && calls == 3) ? 0 : 1;
}
CHECK_EOF
	./tcc -B. dowhile_check.c -o dowhile_check
	./dowhile_check || { echo "tcc: do-while continue codegen is broken (issue #122)" >&2; exit 1; }
}

pkg_install() {
	make install DESTDIR="$PKG_DESTDIR"
	rm -rf "$PKG_DESTDIR/usr/share/man" "$PKG_DESTDIR/usr/share/doc"
}
