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

#endif /* SRCUPSTREAM_H */
