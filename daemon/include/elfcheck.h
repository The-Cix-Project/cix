#ifndef ELFCHECK_H
#define ELFCHECK_H

#include <stddef.h>

/*
 * Issue #176: catching a compiler builtin that was never implemented.
 *
 * TCC does not implement every GCC builtin, and for the ones it lacks
 * it does not fail -- it emits the builtin as an ordinary undefined
 * external symbol. The result compiles, links, installs and publishes
 * without a single error, and is broken: `libblkid.so` shipped with an
 * undefined `__builtin_clz` in its dynamic symbol table, and the
 * failure only surfaced much later when something tried to link
 * against it.
 *
 * This is the same shape as the TCC do-while miscompile that truncated
 * every tar listing (#122): a clean exit and a wrong artifact. The
 * difference is that this one is mechanically detectable, so it is
 * checked rather than trusted.
 *
 * No compiler ever DEFINES a symbol named `__builtin_*` -- a builtin is
 * expanded inline by definition. So an undefined one in a finished
 * binary always means the compiler quietly failed to expand it. That
 * makes this check exact rather than heuristic: there is no legitimate
 * reason for such a symbol to exist, so a hit is never a false alarm.
 */

/*
 * Looks for an undefined `__builtin_*` symbol in an ELF file's dynamic
 * symbol table.
 *
 * Returns 1 and fills out_sym when one is found, 0 when the file is
 * clean OR is not an ELF file at all (the overwhelmingly common case
 * for the files a package installs -- scripts, data, symlinks), and
 * -1 only when the file could not be read.
 *
 * A non-ELF file is deliberately 0 rather than an error: this runs over
 * every file of every install, so "not my concern" and "unreadable"
 * must not be the same answer.
 */
#define ELFCHECK_SONAME_MAX 128

/*
 * The shared libraries a binary declares in its own .dynamic section.
 * Returns how many were written, or -1 if the file could not be read as
 * an ELF64 object. Sonames, not paths -- where a library lives is a
 * property of the system, not of the binary, so resolving them is the
 * caller's job.
 *
 * Added for #224: test_dns carried a hardcoded twenty-library closure
 * for DEBIAN's dnsmasq, so it could not run on a Cix host, whose own
 * dnsmasq needs two. A list kept beside a binary is the hand-maintained
 * pair that drifts; the binary already states its own answer.
 */
int elfcheck_needed_libs(const char *path, char out[][ELFCHECK_SONAME_MAX], int max);

/*
 * Issue #389: the most DT_NEEDED entries one object is expected to
 * carry. Sixty-four is far above anything this platform produces (the
 * largest measured here needs single digits); an object exceeding it
 * has its remaining entries unread, which can only make this check
 * miss a violation, never invent one.
 */
#define ELFCHECK_MAX_NEEDED 64

/*
 * Whether a filename is the sort of name a DT_NEEDED entry holds --
 * `libssl.so.3`, `libfoo.so`, `libbar.so.1.2.3`, but not `parse.sock.c`.
 *
 * Public so that a caller assembling a provided-soname list from a
 * package's recorded file list applies the SAME rule this file's own
 * tree walk does. Two copies of that rule that disagreed would make
 * the gate refuse an install for a library it had itself decided not
 * to count.
 */
int elfcheck_is_soname(const char *name);

/*
 * Issue #389: a package that links a shared library it never declares.
 *
 * `cmake@4.4.3-2` declared `pkg_depends=""` while building against
 * openssl, so it linked `DT_NEEDED libssl.so.3` and was installed
 * recording no runtime dependency. It worked in the one image that
 * happened to have openssl installed for other reasons, and the
 * failure surfaced an hour later in an unrelated package -- fastfetch,
 * whose own recipe was correct -- as a 148-byte build log naming
 * neither cmake nor the missing declaration:
 *
 *     cmake: error while loading shared libraries: libssl.so.3
 *
 * The question asked here is about DECLARATION, not presence. Checking
 * whether the library exists in the target image would have passed
 * `cmake@4.4.3-2`, because openssl was sitting right there -- and the
 * lie would still have been recorded, to be told again by the next
 * environment composed from it.
 *
 * Walks the tree at `root` and reports the first DT_NEEDED soname that
 * is provided by neither the tree itself nor `provided`. A tree
 * satisfies its own internal libraries: a package whose binary links
 * its own `.so` declares nothing and is right not to.
 *
 * Returns 1 and fills out_file (relative to root) and out_soname on a
 * hit, 0 when every needed soname is accounted for, and -1 when the
 * question could not be answered -- an unreadable tree or an
 * allocation failure. A caller must never read -1 as "no violation":
 * this check refuses on evidence, never on ignorance.
 */
int elfcheck_undeclared_links(const char *root, const char provided[][ELFCHECK_SONAME_MAX],
                               int provided_count, char *out_file, size_t out_file_size,
                               char *out_soname, size_t out_soname_size);

int elfcheck_undefined_builtin(const char *path, char *out_sym, size_t sym_size);

/*
 * Issue #113: whether this binary was produced by GCC.
 *
 * GCC stamps its identity into an ELF `.comment` section
 * ("GCC: (Debian 12.2.0-14) 12.2.0"); TCC emits no `.comment` at all.
 * So a GCC marker in a binary from a recipe pinning `CC=tcc` is proof
 * the build did not go the way the recipe says it did.
 *
 * That matters because the failure is silent by construction. A
 * configure script that PROBES for a capability rather than requiring
 * it does not fail when the probe fails -- zlib quietly built a static
 * library instead of a shared one, installed cleanly, and the next
 * thing to link against it died. Where the ambient compiler was really
 * GCC, the probe instead PASSED and produced a GCC artifact from a
 * TCC-pinned recipe. Neither shows up as a build error.
 *
 * ONLY ANSWERS FOR SHARED LIBRARIES. Cix's own glibc ships
 * crt1.o/Scrt1.o carrying "GCC: (GNU) 16.2.0" -- correctly, since
 * glibc is built with Cix's GCC -- so every EXECUTABLE that TCC links
 * inherits that marker whatever compiled the package's own code. The
 * marker is therefore true of every executable on the platform and
 * distinguishes nothing. A shared library links crti.o/crtn.o (no
 * marker) and not crt1.o, so a marker there does come from compiled
 * code.
 *
 * Returns 1 when a GCC marker is present, 0 when the file is a shared
 * object with no such marker or is not ELF at all,
 * ELFCHECK_GCC_INCONCLUSIVE for an executable, -1 when unreadable.
 *
 * "Cannot tell" is a distinct answer from "no" on purpose: collapsing
 * them is exactly how an earlier version of this check produced a
 * confident, wrong conclusion that seven current recipes were
 * GCC-built, on the strength of their executables.
 *
 * Deliberately narrow even for a library: it answers "did GCC touch
 * this", not "was the whole thing built by GCC". A TCC link against
 * one GCC-built object carries the marker too.
 */
#define ELFCHECK_GCC_INCONCLUSIVE 2
int elfcheck_built_by_gcc(const char *path, char *out_version, size_t version_size);

#endif /* ELFCHECK_H */
