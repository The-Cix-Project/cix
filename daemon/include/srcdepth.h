#ifndef SRCDEPTH_H
#define SRCDEPTH_H

#include <stddef.h>

/*
 * ADR-0255: how far back in a channel a package sits.
 *
 * A channel names a stream; it does not say where in that stream to
 * stand. Depth does, and the grammar is read literally:
 *
 *   n-<lines>.<releases>
 *
 * "go back <lines> release lines, then <releases> releases within that
 * line". The `.<releases>` part is optional and defaults to 0, so `n-1`
 * is `n-1.0`.
 *
 * Against kernel.org's `stable` channel at the time of writing (7.2.3
 * newest, 7.1.13 newest of the previous line):
 *
 *   n       newest release in the channel        7.2.3
 *   n-0.1   same line, one release back          7.2.2
 *   n-1     previous line, newest release of it  7.1.13
 *   n-2     two lines back, newest of it         7.0.x
 *
 * `n-0.1` and `n-1` are spelled differently rather than being two
 * readings of one token because on that channel today they differ by an
 * entire release line. A platform that guessed between them would
 * silently walk a box across a major version boundary. ADR-0193 hit the
 * identical ambiguity with `longterm` -- six lines listed at once, where
 * "newest" means nothing on its own -- and resolved it the same way.
 *
 * A depth that cannot be satisfied is an error with a reason, never a
 * silent fallback to whatever exists. That is the whole point: a
 * package asked to sit one line back on a channel that has only one
 * line has not "resolved to the newest", it has failed to resolve, and
 * ADR-0255 requires it to say so and stop.
 */

enum srcdepth_error {
	SRCDEPTH_OK = 0,
	SRCDEPTH_ERR_SYNTAX,        /* not a depth expression at all */
	SRCDEPTH_ERR_NO_CANDIDATES, /* nothing to choose from */
	SRCDEPTH_ERR_NO_SUCH_LINE,  /* fewer release lines than <lines> asks for */
	SRCDEPTH_ERR_NO_SUCH_RELEASE /* that line has fewer releases than asked */
};

const char *srcdepth_strerror(enum srcdepth_error e);

/*
 * Parses a depth expression. Accepts "n", "n-<lines>" and
 * "n-<lines>.<releases>"; rejects everything else, including trailing
 * junk, a bare "-", and a third component.
 */
enum srcdepth_error srcdepth_parse(const char *s, int *out_lines, int *out_releases);

/*
 * An upstream version, split for ordering.
 *
 * Deliberately NOT pkg_version_compare()'s job, and not a second copy
 * of it. That function orders *recipe* versions -- dpkg-style natural
 * sort over arbitrary strings, because it must cope with "v1.4.0" and
 * "1.5.8.pl02" and this platform's own "-<seq>" suffixes. This orders
 * *upstream* versions, where the question is a different one: which
 * release LINE does this belong to, and where does it sit inside that
 * line. A natural sort cannot answer the first question at all.
 */
struct srcdepth_version {
	int major;
	int minor;
	int patch;
	int valid;
};

/*
 * "7.2.3" -> {7,2,3}. "7.2" -> {7,2,0}. A trailing "-<n>" recipe suffix
 * is this platform's own and is ignored, so an upstream version and the
 * recipe revision built from it land in the same line.
 */
struct srcdepth_version srcdepth_version_parse(const char *s);

/* <0, 0, >0 like strcmp, ordering by major then minor then patch. */
int srcdepth_version_cmp(const struct srcdepth_version *a, const struct srcdepth_version *b);

/*
 * Picks one version out of `candidates` according to `lines`/`releases`.
 *
 * Candidates may arrive in any order and may contain duplicates and
 * unparseable strings; the latter are ignored rather than failing the
 * whole resolution, because one malformed entry in an upstream feed
 * should not take a package offline.
 *
 * On success writes the chosen version to `out`. On failure writes a
 * human-readable reason to `err` naming what was asked for and what was
 * actually available -- ADR-0255 requires a resolution failure to say
 * why, not merely that.
 */
enum srcdepth_error srcdepth_resolve(const char *const *candidates, size_t count,
                                      int lines, int releases,
                                      char *out, size_t out_size,
                                      char *err, size_t err_size);

#endif /* SRCDEPTH_H */
