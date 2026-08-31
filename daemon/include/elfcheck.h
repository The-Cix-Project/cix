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
 * Returns 1 when a GCC marker is present, 0 when the file is ELF with
 * no such marker or is not ELF at all, -1 when it cannot be read.
 *
 * Deliberately narrow: this answers "did GCC touch this", not "was the
 * whole thing built by GCC". A TCC link against one GCC-built object
 * carries the marker too -- which is still worth knowing for a recipe
 * that claims to be pure TCC.
 */
int elfcheck_built_by_gcc(const char *path, char *out_version, size_t version_size);

#endif /* ELFCHECK_H */
