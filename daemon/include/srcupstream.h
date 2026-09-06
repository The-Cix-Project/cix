#ifndef SRCUPSTREAM_H
#define SRCUPSTREAM_H

#include <stddef.h>

/*
 * ADR-0255: how a package discovers what upstream has published.
 *
 * A recipe declares `pkg_upstream="<kind>"`. The kind knows two things
 * an operator should never have to restate: how to enumerate that
 * project's releases, and which channels -- if any -- that project
 * publishes in parallel.
 *
 * CHANNELS BELONG TO THE KIND, NOT TO THE OPERATOR.
 *
 * kernel.org publishes mainline, stable and longterm simultaneously, so
 * "which stream" is a real question there and its answers are
 * kernel.org's own monikers. glibc publishes one linear sequence, so
 * the question does not arise and a channel is simply absent. An
 * operator chooses *among what the upstream actually offers*; they do
 * not invent the list. A free-text channel would let a box be
 * configured to track "lts" on a project that has never used the word,
 * and the failure would surface as an empty resolution rather than as
 * the configuration error it is.
 *
 * Depth (see srcdepth.h) is the orthogonal question and applies to
 * every kind, channels or not.
 *
 * This registry is deliberately small. kernel.org is the first kind and
 * is ONE KIND AMONG SEVERAL TO COME, not the model -- generalising its
 * releases.json into a universal shape would be inventing a standard
 * that upstreams have not agreed to. A project offering neither a
 * machine-readable release list nor signed checksums cannot declare a
 * kind, and is therefore pinned, which ADR-0255 treats as a permanent
 * first-class answer rather than a gap.
 */

enum srcupstream_error {
	SRCUPSTREAM_OK = 0,
	SRCUPSTREAM_ERR_NO_SUCH_KIND,
	SRCUPSTREAM_ERR_NO_CHANNELS,   /* kind publishes one stream; a channel is meaningless */
	SRCUPSTREAM_ERR_BAD_CHANNEL,   /* not one this kind publishes */
	SRCUPSTREAM_ERR_CHANNEL_REQUIRED /* kind has channels and none was chosen */
};

const char *srcupstream_strerror(enum srcupstream_error e);

#define SRCUPSTREAM_VERSION_MAX 64

/*
 * How many releases one kind may offer the resolver at once. A feed
 * that lists more than this is not truncated silently -- see
 * srcupstream_candidates(), which reports the count it wrote so a
 * caller can tell a full buffer from a short list.
 */
#define SRCUPSTREAM_MAX_CANDIDATES 32

struct srcupstream_kind {
	const char *name;
	/*
	 * NULL-terminated, or NULL when the project publishes a single
	 * linear sequence. Empty is not representable on purpose: "has no
	 * channels" and "has a channel list that happens to be empty" would
	 * be the same state, and only one of them is ever true.
	 */
	const char *const *channels;
	const char *summary;
	/*
	 * THE KIND OWNS ENUMERATION. This is the second half of the
	 * contract stated at the top of this header: a kind knows how to
	 * enumerate its project's releases, so the resolver asks the kind
	 * rather than growing a switch on kind names that would have to be
	 * edited in a second place every time a kind is added.
	 *
	 * Writes at most `max` versions into `out` (slots of
	 * SRCUPSTREAM_VERSION_MAX bytes) and returns how many. Order does
	 * not matter -- srcdepth.h orders them, and it is the only thing
	 * that should, since ordering upstream versions is exactly the
	 * problem it exists to solve.
	 */
	size_t (*candidates)(const char *channel,
	                      char out[][SRCUPSTREAM_VERSION_MAX], size_t max);
	/*
	 * When this kind's release data was last fetched; 0 means never.
	 *
	 * Kept distinct from "the channel has no releases" on purpose. A
	 * box that has simply never fetched, and a channel that genuinely
	 * publishes nothing, both produce zero candidates -- and they need
	 * opposite responses from an operator, so the resolver must be able
	 * to tell them apart rather than reporting one generic emptiness.
	 */
	long (*fetched_at)(void);
};

size_t srcupstream_count(void);
const struct srcupstream_kind *srcupstream_at(size_t i);
const struct srcupstream_kind *srcupstream_find(const char *name);

/*
 * Is `channel` one this kind publishes?
 *
 * `channel` may be NULL or "" meaning "not chosen", which is correct
 * for a kind with no channels and an error for one that has them --
 * there is no sensible default among mainline, stable and longterm, and
 * picking one here would be this code making an operator's decision.
 *
 * On failure writes a reason to `err` that LISTS the valid channels,
 * because the whole point of taking them from the kind is that the
 * answer is knowable and should therefore be shown.
 */
enum srcupstream_error srcupstream_check_channel(const struct srcupstream_kind *kind,
                                                  const char *channel,
                                                  char *err, size_t err_size);

/* Same, resolving the kind by name first. */
enum srcupstream_error srcupstream_check(const char *kind_name, const char *channel,
                                          char *err, size_t err_size);

/*
 * The releases `kind` currently publishes in `channel`, from whatever
 * that kind's own cache holds. Never fetches: fetching is a network
 * operation on the daemon's own reactor and belongs to the refresh
 * path, not to a read. A stale answer is reported as stale (see
 * srcupstream_fetched_at) rather than silently refreshed under a
 * caller who asked a question, which is what makes a catalogue read
 * cheap enough to serve on demand.
 */
size_t srcupstream_candidates(const struct srcupstream_kind *kind, const char *channel,
                               char out[][SRCUPSTREAM_VERSION_MAX], size_t max);

long srcupstream_fetched_at(const struct srcupstream_kind *kind);

#endif /* SRCUPSTREAM_H */
