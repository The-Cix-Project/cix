#ifndef KERNELPOLICY_H
#define KERNELPOLICY_H

#include "json.h"

/*
 * Issue #65: which kernel line this box tracks.
 *
 * The platform used to hardcode that judgment call -- one pinned
 * version in the kernel recipe, and nothing anywhere expressing which
 * *line* it belongs to or how far behind that line it had drifted. A
 * cautious box and an aggressive one want different answers, and
 * neither is a property of the recipe: it is operator state, exactly
 * like the per-package policy #64 introduced (ADR-0188).
 *
 * The channels are kernel.org's own monikers, not names invented here,
 * and they are resolved from kernel.org's own machine-readable
 * releases.json -- no hand-maintained mirror of kernel.org's state
 * anywhere in this repo, which would be a second source of truth for a
 * fact that changes weekly without anyone here noticing.
 *
 * "pinned" is today's behaviour and stays the default: the version
 * lives where it always has, in the kernel recipe, and nothing here
 * proposes moving it.
 */

enum kernel_channel {
	KERNEL_CHANNEL_PINNED = 0,
	KERNEL_CHANNEL_LONGTERM,
	KERNEL_CHANNEL_STABLE,
	KERNEL_CHANNEL_MAINLINE
};

#define KERNEL_VERSION_MAX 64
#define KERNEL_SERIES_MAX 16
#define KERNEL_SOURCE_MAX 512
#define KERNEL_MAX_RELEASES 32

/*
 * What a channel currently points at, and enough context to judge it.
 *
 * "longterm" is the one channel a version alone cannot answer:
 * kernel.org lists six longterm lines at once (6.18, 6.12, 6.6, 6.1,
 * 5.15, 5.10). Resolving it to whichever is newest would silently
 * propose a cross-major jump to a box deliberately sitting on an older
 * longterm line -- so it resolves within the line the box is already
 * on, while still reporting the newest longterm line separately. Both
 * facts, neither hidden behind the other.
 */
struct kernel_resolution {
	int known;                            /* 0 until releases.json has been ingested */
	char version[KERNEL_VERSION_MAX];     /* what this channel points at now */
	char source_url[KERNEL_SOURCE_MAX];   /* kernel.org's own tarball URL for it */
	char series[KERNEL_SERIES_MAX];       /* the line that version belongs to */
	int series_maintained;                /* running series still listed as longterm */
	char newest_longterm[KERNEL_VERSION_MAX];
	char newest_longterm_series[KERNEL_SERIES_MAX];
};

int kernelpolicy_init(const char *path);
void kernelpolicy_repoint(const char *path);

enum kernel_channel kernelpolicy_channel(void);
int kernelpolicy_set_channel(enum kernel_channel channel);

const char *kernel_channel_name(enum kernel_channel channel);
int kernel_channel_from_name(const char *name, enum kernel_channel *out);

/*
 * "6.18.40-13" / "6.18.46" / "7.2" -> "6.18" / "6.18" / "7.2". The
 * series is what makes two versions comparable as the same line; a
 * recipe revision suffix is this platform's own and never part of it.
 * Returns 0 on success, -1 if s does not look like a kernel version.
 */
int kernel_version_series(const char *s, char *out, size_t out_size);

/*
 * Ingests a downloaded kernel.org releases.json. Replaces the cached
 * release list wholesale on success and records now as the fetch time;
 * a malformed file leaves the previous cache untouched, since a stale
 * true answer beats an empty one. Returns 0 on success, -1 otherwise.
 */
int kernelpolicy_ingest_releases(const char *path, long now);

long kernelpolicy_fetched_at(void);

/*
 * Every version this cache holds for `moniker`, written into `out` as
 * `max` slots of `stride` bytes each. Returns how many were written.
 *
 * ADR-0255's source catalogue needs the release LIST, not the single
 * answer kernelpolicy_resolve() picks: depth ("n-1" and friends) is
 * what chooses among lines there, where this module's own channel
 * resolution steers longterm by the running kernel's series instead.
 * Both are legitimate answers to different questions, so this exposes
 * the raw list rather than either module second-guessing the other.
 *
 * Note what kernel.org's releases.json actually contains: the newest
 * release of each line and nothing else. So "longterm" yields one entry
 * per maintained line (six, currently) while "stable" and "mainline"
 * yield exactly one -- a depth asking to go back WITHIN a line has
 * nothing to go back to, and that is a property of the feed, not a bug
 * in the caller.
 */
int kernelpolicy_channel_versions(const char *moniker, char *out, size_t stride, int max);

/*
 * Resolves channel against the cached list. running_series is the line
 * the box is on (from its running kernel) and steers longterm only.
 * Always fills *out; out->known is 0 when nothing has been ingested
 * yet, which is the honest answer rather than a guess.
 */
void kernelpolicy_resolve(enum kernel_channel channel, const char *running_series,
                           struct kernel_resolution *out);

#endif /* KERNELPOLICY_H */
