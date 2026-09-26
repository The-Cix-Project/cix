#ifndef CIX_CBSRECIPE_H
#define CIX_CBSRECIPE_H

#include <stddef.h>

/*
 * Reading a CBS recipe's identity (ADR-0305).
 *
 * A CBS recipe is a CPDL 0.1 document in a `build.cbs` file, and this
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
 * would be gated by nothing. test_cbsrecipe drives these functions
 * from literal JSON strings and runs anywhere.
 *
 * The accessors are getters rather than one struct-filling call so
 * that no second copy of `struct pkg_recipe`'s shape exists here.
 * pkg.c owns that struct and maps onto it; this module owns the
 * document.
 */

/* Opaque handle over one parsed `cbs explain --json` document. */
struct cbs_explain;

/*
 * Parses explain-JSON. Returns NULL and fills err on malformed input
 * or on a document missing a field CPDL guarantees (name, version,
 * release), which is the signal that the JSON did not come from
 * `cbs explain --json` at all.
 */
struct cbs_explain *cbs_explain_parse(const char *text, size_t len, char *err, size_t err_size);

void cbs_explain_free(struct cbs_explain *ex);

/* The package name, exactly as the CPDL document declares it. */
const char *cbs_explain_name(const struct cbs_explain *ex);

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
int cbs_explain_version(const struct cbs_explain *ex, char *out, size_t out_size);

/* How many sources the document declares (ADR-0036's positional list). */
int cbs_explain_source_count(const struct cbs_explain *ex);

/*
 * Source index's FIRST URL and its sha256.
 *
 * A source may declare several urls, which CPDL defines as ordered
 * mirrors for one source identity sharing this one checksum. This
 * accessor returns the first; cbs_explain_source_url() below reaches
 * the rest, and parse_cbs_recipe() records them so the fetch can fall
 * back through the list.
 *
 * The comment here used to say the rest were "deliberately ignored",
 * and that was a real defect rather than a design note: a recipe could
 * declare three mirrors, validate, publish, and fetch only ever tried
 * one. freetype@2.13.3-7 did exactly that and failed naming only
 * download.savannah.gnu.org while two working mirrors sat unused
 * (#507).
 *
 * Returns 0 on success, -1 if index is out of range or either value
 * would not fit.
 */
int cbs_explain_source(const struct cbs_explain *ex, int index, char *url, size_t url_size,
                        char *sha256, size_t sha256_size);

/*
 * How many urls source `index` declares. 0 for an absent source, so a
 * caller may loop without checking the index separately.
 */
int cbs_explain_source_url_count(const struct cbs_explain *ex, int index);

/*
 * Source `index`'s url at `url_index`, in document order -- which is
 * mirror precedence order. url_index 0 is the same string
 * cbs_explain_source() returns.
 *
 * Returns 0 on success, -1 if either index is out of range or the
 * value would not fit.
 */
int cbs_explain_source_url(const struct cbs_explain *ex, int index, int url_index, char *url,
                            size_t url_size);

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
int cbs_explain_requires(const struct cbs_explain *ex, const char *role, const char *kind,
                          char *out, size_t out_size);

/*
 * The artifact format the recipe declares (ADR-0307): "cixpkg" or
 * "tar.gz", CPDL's only two legal values, exactly one of which every
 * package must declare (cix-build-system `src/validate.c:698`).
 *
 * "" means the ENGINE did not report it, not that the recipe declared
 * nothing -- a document derived by a cbs older than v0.1.26 has no
 * `format` key at all (cix-build-system#173). Telling those two apart
 * is not possible from here and does not need to be: ADR-0307 clause 7
 * re-derives a document whose engine has changed, so a "" reaching a
 * caller means the sweep has not run or the engine is older than the
 * daemon requires, and both are refusals rather than defaults.
 */
const char *cbs_explain_format(const struct cbs_explain *ex);

/* "" when the document declares none. Never NULL. */
const char *cbs_explain_upstream(const struct cbs_explain *ex);
const char *cbs_explain_toolchain(const struct cbs_explain *ex);
const char *cbs_explain_toolchain_reason(const struct cbs_explain *ex);

/*
 * The build capabilities the document declares, space-separated, in the
 * same shape `pkg_build_caps=` gives the shell path -- so both formats
 * reach pkg_build_container_spec()'s one caps argument and neither owns
 * a second spelling of the same list.
 *
 * `cbs explain --json` reported a capability COUNT until
 * cix-build-system#162, which is why this accessor once existed only to
 * REFUSE a publish (a count of 1 does not say whether the recipe asked
 * for CAP_SYS_ADMIN or CAP_NET_ADMIN, and granting by count would be
 * worse than not supporting the field). That is fixed upstream and
 * verified here: cbs 0.1.25-6 is built from cix-build-system main at
 * 170dc744d, and its own `cli-contract-test.sh` -- which asserts
 * `"capabilities":["CAP_ONE","CAP_TWO"]` out of explain --json -- passes
 * in a Cix build container on 192.168.15.95.
 *
 * Returns:
 *   0   names written to out (an empty string when none are declared)
 *  -1   bad argument, or the names do not fit
 *   1   the document reports a NUMBER, not an array -- an older cbs
 *
 * The 1 is not a compatibility path and must never become one. cixd
 * execs whichever `cbs` is installed, so an older one silently turns a
 * declared CAP_SYS_ADMIN into no capability at all -- the build then
 * fails somewhere unrelated, or worse, succeeds having done less than
 * the recipe asked. The caller refuses the publish and says to upgrade
 * cbs. Guessing is the one thing that is not allowed here.
 */
int cbs_explain_capabilities(const struct cbs_explain *ex, char *out, size_t out_size);

/*
 * One value out of the recipe's opaque `metadata { }` block
 * (cix-build-system#161), or "" written to out when the key is absent.
 *
 * CBS assigns these keys no meaning; the platform does. Two matter, and
 * they are the two that kept a CBS recipe from expressing everything a
 * shell recipe can:
 *
 *   artifact_sha256   the platform's approval of one published byte
 *                     sequence. Without a home for it, a converted
 *                     recipe rebuilds from source on every install on
 *                     every host, forever -- 88 of 148 current recipes
 *                     carry one.
 *   changelog         the revision's own note on why it exists.
 *
 * Deliberately a getter per key rather than a whole-block accessor: the
 * daemon has exactly two keys it understands, and enumerating the rest
 * would invite reading a key nothing acts on, which is the state
 * ADR-0304 settled against.
 *
 * Returns 0 on success (including "absent"), -1 on a bad argument or a
 * value that does not fit.
 */
int cbs_explain_metadata(const struct cbs_explain *ex, const char *key, char *out,
                          size_t out_size);

/* How many phases the document declares (1..5). */
int cbs_explain_phase_count(const struct cbs_explain *ex);

#endif /* CIX_CBSRECIPE_H */
