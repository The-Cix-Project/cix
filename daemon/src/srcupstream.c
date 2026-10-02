/*
 * See srcupstream.h for why channels belong to the kind rather than to
 * the operator.
 */
#include "srcupstream.h"

#include "kernelpolicy.h"
#include "srcgitea.h"

#include <stdio.h>
#include <string.h>

const char *srcupstream_strerror(enum srcupstream_error e)
{
	switch (e) {
	case SRCUPSTREAM_OK:
		return "ok";
	case SRCUPSTREAM_ERR_NO_SUCH_KIND:
		return "no such upstream kind";
	case SRCUPSTREAM_ERR_NO_CHANNELS:
		return "this upstream publishes a single release sequence";
	case SRCUPSTREAM_ERR_BAD_CHANNEL:
		return "not a channel this upstream publishes";
	case SRCUPSTREAM_ERR_CHANNEL_REQUIRED:
		return "this upstream publishes several channels and none was chosen";
	}
	return "unknown";
}

/*
 * kernel.org's own monikers, in the order releases.json lists them.
 * Not names invented here -- ADR-0193 made that choice and it holds:
 * an operator who reads kernel.org's front page should find the same
 * words here.
 */
static const char *const KORG_CHANNELS[] = { "mainline", "stable", "longterm", NULL };

/*
 * kernel.org's release list is already cached, parsed and persisted by
 * kernelpolicy (issue #65) -- this reads that one cache rather than
 * keeping a second copy of the same file. Two caches of releases.json
 * would answer differently the moment one was refreshed and the other
 * was not, and nothing would say which was right.
 */
static size_t kernelorg_candidates(const char *package, const char *channel,
                                    char out[][SRCUPSTREAM_VERSION_MAX], size_t max)
{
	int n;

	(void)package; /* one releases.json serves every kernel */
	if (channel == NULL || channel[0] == '\0')
		return 0;
	n = kernelpolicy_channel_versions(channel, out[0], SRCUPSTREAM_VERSION_MAX, (int)max);
	return n < 0 ? 0 : (size_t)n;
}

static long kernelorg_fetched_at(const char *package)
{
	(void)package;
	return kernelpolicy_fetched_at();
}

/* ADR-0323: one package's own Gitea tags, cached by srcgitea.c. One
 * linear sequence, so no channels: depth picks among the tags. */
static size_t giteatags_candidates(const char *package, const char *channel,
                                   char out[][SRCUPSTREAM_VERSION_MAX], size_t max)
{
	(void)channel;
	return srcgitea_candidates(package, out, max);
}

static long giteatags_fetched_at(const char *package)
{
	return srcgitea_fetched_at(package);
}

static int giteatags_note(const char *package, const char *version, struct srcupstream_note *out)
{
	struct srcgitea_note n;

	if (srcgitea_note(package, &n) != 0 || version == NULL || strcmp(n.version, version) != 0)
		return -1;
	snprintf(out->stage, sizeof(out->stage), "%s", n.stage);
	snprintf(out->status, sizeof(out->status), "%s", n.status);
	snprintf(out->reason, sizeof(out->reason), "%s", n.reason);
	return 0;
}

static const struct srcupstream_kind KINDS[] = {
	{ "kernel.org", KORG_CHANNELS,
	  "kernel.org releases.json; checksums from its signed sha256sums.asc",
	  kernelorg_candidates, kernelorg_fetched_at, NULL, NULL },
	{ "gitea-tags", NULL,
	  "the tags of the package's own Gitea repository, read from its upstream source template",
	  giteatags_candidates, giteatags_fetched_at, srcgitea_error, giteatags_note },
};

size_t srcupstream_candidates(const struct srcupstream_kind *kind, const char *package,
                               const char *channel,
                               char out[][SRCUPSTREAM_VERSION_MAX], size_t max)
{
	if (kind == NULL || kind->candidates == NULL || out == NULL || max == 0)
		return 0;
	return kind->candidates(package, channel, out, max);
}

long srcupstream_fetched_at(const struct srcupstream_kind *kind, const char *package)
{
	if (kind == NULL || kind->fetched_at == NULL)
		return 0;
	return kind->fetched_at(package);
}

void srcupstream_problem(const struct srcupstream_kind *kind, const char *package, char *out,
                         size_t out_size)
{
	if (out == NULL || out_size == 0)
		return;
	out[0] = '\0';
	if (kind != NULL && kind->problem != NULL && kind->problem(package, out, out_size) != 0)
		out[0] = '\0';
}

int srcupstream_note(const struct srcupstream_kind *kind, const char *package, const char *version,
                     struct srcupstream_note *out)
{
	if (kind == NULL || kind->note == NULL || out == NULL)
		return -1;
	memset(out, 0, sizeof(*out));
	return kind->note(package, version, out);
}

size_t srcupstream_count(void)
{
	return sizeof(KINDS) / sizeof(KINDS[0]);
}

const struct srcupstream_kind *srcupstream_at(size_t i)
{
	if (i >= srcupstream_count())
		return NULL;
	return &KINDS[i];
}

const struct srcupstream_kind *srcupstream_find(const char *name)
{
	size_t i;

	if (name == NULL || name[0] == '\0')
		return NULL;
	for (i = 0; i < srcupstream_count(); i++)
		if (strcmp(KINDS[i].name, name) == 0)
			return &KINDS[i];
	return NULL;
}

/* Appends "mainline, stable, longterm" to a buffer, for error text. */
static void join_channels(const struct srcupstream_kind *kind, char *buf, size_t size)
{
	size_t used = 0;
	size_t i;

	if (size == 0)
		return;
	buf[0] = '\0';
	if (kind->channels == NULL)
		return;
	for (i = 0; kind->channels[i] != NULL; i++) {
		int n = snprintf(buf + used, size - used, "%s%s", used > 0 ? ", " : "",
		                  kind->channels[i]);

		if (n < 0 || (size_t)n >= size - used)
			return;
		used += (size_t)n;
	}
}

enum srcupstream_error srcupstream_check_channel(const struct srcupstream_kind *kind,
                                                  const char *channel,
                                                  char *err, size_t err_size)
{
	int chosen;
	size_t i;
	char list[256];

	if (err != NULL && err_size > 0)
		err[0] = '\0';
	if (kind == NULL) {
		if (err != NULL)
			snprintf(err, err_size, "no upstream kind given");
		return SRCUPSTREAM_ERR_NO_SUCH_KIND;
	}
	chosen = (channel != NULL && channel[0] != '\0');

	if (kind->channels == NULL) {
		if (!chosen)
			return SRCUPSTREAM_OK;
		if (err != NULL)
			snprintf(err, err_size,
			          "upstream \"%s\" publishes a single release sequence, so there is no "
			          "channel to choose; got \"%s\"",
			          kind->name, channel);
		return SRCUPSTREAM_ERR_NO_CHANNELS;
	}

	join_channels(kind, list, sizeof(list));
	if (!chosen) {
		/*
		 * No default. There is no defensible way to pick between
		 * mainline and longterm on an operator's behalf -- they differ
		 * by years of support -- so the honest move is to refuse and
		 * say what the options are.
		 */
		if (err != NULL)
			snprintf(err, err_size, "upstream \"%s\" publishes channels (%s); choose one",
			          kind->name, list);
		return SRCUPSTREAM_ERR_CHANNEL_REQUIRED;
	}
	for (i = 0; kind->channels[i] != NULL; i++)
		if (strcmp(kind->channels[i], channel) == 0)
			return SRCUPSTREAM_OK;
	if (err != NULL)
		snprintf(err, err_size, "upstream \"%s\" does not publish a \"%s\" channel; it has %s",
		          kind->name, channel, list);
	return SRCUPSTREAM_ERR_BAD_CHANNEL;
}

enum srcupstream_error srcupstream_check(const char *kind_name, const char *channel,
                                          char *err, size_t err_size)
{
	const struct srcupstream_kind *k = srcupstream_find(kind_name);

	if (k == NULL) {
		size_t i;
		char list[256];
		size_t used = 0;

		list[0] = '\0';
		for (i = 0; i < srcupstream_count(); i++) {
			int n = snprintf(list + used, sizeof(list) - used, "%s%s", used > 0 ? ", " : "",
			                  KINDS[i].name);

			if (n < 0 || (size_t)n >= sizeof(list) - used)
				break;
			used += (size_t)n;
		}
		if (err != NULL && err_size > 0)
			snprintf(err, err_size, "no upstream kind \"%s\"; known kinds: %s",
			          kind_name != NULL ? kind_name : "", list);
		return SRCUPSTREAM_ERR_NO_SUCH_KIND;
	}
	return srcupstream_check_channel(k, channel, err, err_size);
}
