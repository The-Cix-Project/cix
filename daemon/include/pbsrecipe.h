#ifndef CIX_PBSRECIPE_H
#define CIX_PBSRECIPE_H

#include <stddef.h>

/*
 * Reading a PBS recipe's identity (ADR-0305).
 *
 * A PBS recipe is a CPDL 0.1 document in a `build.cbs` file, and this
 * daemon does not parse CPDL -- `cbs explain --json` does, and this
 * module reads that JSON. A second CPDL parser in cixd would be a
 * parallel implementation of the language being adopted, and the two
 * would diverge on exactly the documents where the grammar is subtle.
 *
 * The split in this file is deliberate and is about what can be
 * TESTED. Everything below the exec boundary is pure: it takes JSON
 * text and answers questions about it, with no filesystem, no child
 * process and no package state. That matters because the daemon's own
 * package test (test_pkg) cannot run on a Cix host at all -- it needs
 * the ADR-0209 floor artifacts, a hand-fetched input that is not in
 * the source tarball (#485) -- so a mapping written inside pkg.c
 * would be gated by nothing. test_pbsrecipe drives these functions
 * from literal JSON strings and runs anywhere.
 *
 * The accessors are getters rather than one struct-filling call so
 * that no second copy of `struct pkg_recipe`'s shape exists here.
 * pkg.c owns that struct and maps onto it; this module owns the
 * document.
 */

/* Opaque handle over one parsed `cbs explain --json` document. */
struct pbs_explain;

/*
 * Parses explain-JSON. Returns NULL and fills err on malformed input
 * or on a document missing a field CPDL guarantees (name, version,
 * release), which is the signal that the JSON did not come from
 * `cbs explain --json` at all.
 */
struct pbs_explain *pbs_explain_parse(const char *text, size_t len, char *err, size_t err_size);

void pbs_explain_free(struct pbs_explain *ex);

/* The package name, exactly as the CPDL document declares it. */
const char *pbs_explain_name(const struct pbs_explain *ex);

/*
 * cixd's own version string, which is CPDL's `version` and `release`
 * joined as "<version>-<release>" (ADR-0305).
 *
 * The join is not a convention invented here: cixd publishes an
 * artifact as <name>-<version>-<arch>.tar.gz using this single fused
 * string, and the cache splits the trailing -N back off as the
 * release -- which is why `kernel 7.2.3-15` is stored there as
 * version 7.2.3, release 15. So the join reproduces every existing
 * name for every package whose version already carries a -N suffix.
 *
 * Returns 0 on success, -1 if the result would not fit.
 */
int pbs_explain_version(const struct pbs_explain *ex, char *out, size_t out_size);

/* How many sources the document declares (ADR-0036's positional list). */
int pbs_explain_source_count(const struct pbs_explain *ex);

/*
 * Source index's first URL and its sha256. CPDL allows a source to
 * carry several URLs (mirrors); cixd's own model is one URL per
 * positional source, so the first is taken and the rest are
 * deliberately ignored rather than silently concatenated.
 *
 * Returns 0 on success, -1 if index is out of range or either value
 * would not fit.
 */
int pbs_explain_source(const struct pbs_explain *ex, int index, char *url, size_t url_size,
                        char *sha256, size_t sha256_size);

/*
 * Space-joined values of one `requires { <role> { <kind> "..." } }`
 * list, in document order. Writes an empty string when the role or
 * kind is absent, which is not an error -- a package with no runtime
 * dependencies is ordinary.
 *
 * kind is the CPDL item keyword and is deliberately a parameter
 * rather than a fixed set: CPDL 0.1 puts no allow-list on it
 * (validate_requires checks only the item's VALUE), and dependency.c
 * hands it to an embedder verbatim. cixd asks for the keywords it
 * understands and ignores the rest, so a recipe using a keyword this
 * daemon does not know loses that declaration rather than failing --
 * see the note in pkg.c's mapping about why that is reported.
 *
 * Returns 0 on success, -1 if the joined list would not fit.
 */
int pbs_explain_requires(const struct pbs_explain *ex, const char *role, const char *kind,
                          char *out, size_t out_size);

/* "" when the document declares none. Never NULL. */
const char *pbs_explain_upstream(const struct pbs_explain *ex);
const char *pbs_explain_toolchain(const struct pbs_explain *ex);
const char *pbs_explain_toolchain_reason(const struct pbs_explain *ex);

/*
 * How many build capabilities the document declares -- a COUNT, not
 * the names, because that is all `cbs explain --json` reports
 * (cix-build-system#162). cixd cannot honour a capability it cannot
 * name, and granting one by count would be worse than not supporting
 * the field: a count of 1 does not say whether the recipe asked for
 * CAP_SYS_ADMIN or CAP_NET_ADMIN.
 *
 * So this exists to REFUSE, not to grant: a non-zero count makes the
 * publish fail with a message naming the upstream issue, rather than
 * building the package with the capability silently missing. Which is
 * the same rule ADR-0304 settled -- a declaration that is read by
 * nothing is worse than one that is absent.
 */
int pbs_explain_capability_count(const struct pbs_explain *ex);

/* How many phases the document declares (1..5). */
int pbs_explain_phase_count(const struct pbs_explain *ex);

#endif /* CIX_PBSRECIPE_H */
