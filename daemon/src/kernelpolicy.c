#include "kernelpolicy.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct release_entry {
	char moniker[24];
	char version[KERNEL_VERSION_MAX];
	char source[KERNEL_SOURCE_MAX];
	char series[KERNEL_SERIES_MAX];
};

static enum kernel_channel g_channel = KERNEL_CHANNEL_PINNED;
static char g_state_path[512];
static struct release_entry g_releases[KERNEL_MAX_RELEASES];
static int g_release_count;
static long g_fetched_at;

const char *kernel_channel_name(enum kernel_channel channel)
{
	switch (channel) {
	case KERNEL_CHANNEL_LONGTERM:
		return "longterm";
	case KERNEL_CHANNEL_STABLE:
		return "stable";
	case KERNEL_CHANNEL_MAINLINE:
		return "mainline";
	case KERNEL_CHANNEL_PINNED:
	default:
		return "pinned";
	}
}

int kernel_channel_from_name(const char *name, enum kernel_channel *out)
{
	if (name == NULL)
		return -1;
	if (strcmp(name, "pinned") == 0)
		*out = KERNEL_CHANNEL_PINNED;
	else if (strcmp(name, "longterm") == 0)
		*out = KERNEL_CHANNEL_LONGTERM;
	else if (strcmp(name, "stable") == 0)
		*out = KERNEL_CHANNEL_STABLE;
	else if (strcmp(name, "mainline") == 0)
		*out = KERNEL_CHANNEL_MAINLINE;
	else
		return -1;
	return 0;
}

/*
 * The first two dot-separated numeric components. A kernel version is
 * always major.minor[.patch], and this platform's own recipe revision
 * ("-13") is appended after that -- never part of the line.
 */
int kernel_version_series(const char *s, char *out, size_t out_size)
{
	size_t i = 0;
	size_t dots = 0;

	if (s == NULL || out_size == 0)
		return -1;
	while (s[i] != '\0' && (s[i] == '.' || (s[i] >= '0' && s[i] <= '9'))) {
		if (s[i] == '.' && ++dots == 2)
			break;
		i++;
	}
	if (i == 0 || i >= out_size)
		return -1;
	/* One numeric component with no dot at all is not a kernel line. */
	if (dots == 0 && s[i] != '.')
		return -1;
	memcpy(out, s, i);
	out[i] = '\0';
	return 0;
}

static int save_state(void)
{
	char buf[128];
	int n = snprintf(buf, sizeof(buf), "{\"channel\":\"%s\"}\n", kernel_channel_name(g_channel));

	if (n < 0 || (size_t)n >= sizeof(buf) || g_state_path[0] == '\0')
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

int kernelpolicy_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *channel;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	g_channel = KERNEL_CHANNEL_PINNED;
	if (persist_read_file(path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return 0;
	channel = json_object_get(root, "channel");
	if (channel != NULL)
		(void)kernel_channel_from_name(json_as_string(channel), &g_channel);
	json_free(root);
	return 0;
}

void kernelpolicy_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

enum kernel_channel kernelpolicy_channel(void)
{
	return g_channel;
}

int kernelpolicy_set_channel(enum kernel_channel channel)
{
	g_channel = channel;
	return save_state();
}

long kernelpolicy_fetched_at(void)
{
	return g_fetched_at;
}

int kernelpolicy_ingest_releases(const char *path, long now)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;
	const struct json_value *releases;
	struct release_entry parsed[KERNEL_MAX_RELEASES];
	int count = 0;
	size_t i;

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL)
		return -1;
	releases = json_object_get(root, "releases");
	if (releases == NULL || releases->type != JSON_ARRAY) {
		json_free(root);
		return -1;
	}
	memset(parsed, 0, sizeof(parsed));
	for (i = 0; i < releases->u.array.count && count < KERNEL_MAX_RELEASES; i++) {
		const struct json_value *entry = releases->u.array.items[i];
		const char *moniker;
		const char *version;
		const char *source;
		char series[KERNEL_SERIES_MAX];

		if (entry == NULL || entry->type != JSON_OBJECT)
			continue;
		moniker = json_as_string(json_object_get(entry, "moniker"));
		version = json_as_string(json_object_get(entry, "version"));
		source = json_as_string(json_object_get(entry, "source"));
		/*
		 * linux-next and EOL entries carry monikers this platform has
		 * no channel for, and linux-next has no source tarball at all.
		 * Skipping them here keeps the cache to things a channel can
		 * actually resolve to.
		 */
		if (moniker == NULL || version == NULL || source == NULL || source[0] == '\0')
			continue;
		if (strcmp(moniker, "longterm") != 0 && strcmp(moniker, "stable") != 0 &&
		    strcmp(moniker, "mainline") != 0)
			continue;
		if (kernel_version_series(version, series, sizeof(series)) != 0)
			continue;
		snprintf(parsed[count].moniker, sizeof(parsed[count].moniker), "%s", moniker);
		snprintf(parsed[count].version, sizeof(parsed[count].version), "%s", version);
		snprintf(parsed[count].source, sizeof(parsed[count].source), "%s", source);
		snprintf(parsed[count].series, sizeof(parsed[count].series), "%s", series);
		count++;
	}
	json_free(root);
	/*
	 * A file that parsed but named no usable release is not an answer
	 * -- keep whatever was cached before rather than replacing a true
	 * stale list with an empty fresh one.
	 */
	if (count == 0)
		return -1;
	memcpy(g_releases, parsed, sizeof(g_releases));
	g_release_count = count;
	g_fetched_at = now;
	return 0;
}

/* kernel.org lists releases newest-first within each moniker, so the
 * first match is the one that moniker currently means. */
static const struct release_entry *find_moniker(const char *moniker)
{
	int i;

	for (i = 0; i < g_release_count; i++)
		if (strcmp(g_releases[i].moniker, moniker) == 0)
			return &g_releases[i];
	return NULL;
}

static const struct release_entry *find_longterm_series(const char *series)
{
	int i;

	if (series == NULL || series[0] == '\0')
		return NULL;
	for (i = 0; i < g_release_count; i++)
		if (strcmp(g_releases[i].moniker, "longterm") == 0 &&
		    strcmp(g_releases[i].series, series) == 0)
			return &g_releases[i];
	return NULL;
}

static void fill_from(struct kernel_resolution *out, const struct release_entry *e)
{
	out->known = 1;
	snprintf(out->version, sizeof(out->version), "%s", e->version);
	snprintf(out->source_url, sizeof(out->source_url), "%s", e->source);
	snprintf(out->series, sizeof(out->series), "%s", e->series);
}

void kernelpolicy_resolve(enum kernel_channel channel, const char *running_series,
                           struct kernel_resolution *out)
{
	const struct release_entry *newest_lt;
	const struct release_entry *hit = NULL;

	memset(out, 0, sizeof(*out));
	if (g_release_count == 0)
		return;

	newest_lt = find_moniker("longterm");
	if (newest_lt != NULL) {
		snprintf(out->newest_longterm, sizeof(out->newest_longterm), "%s", newest_lt->version);
		snprintf(out->newest_longterm_series, sizeof(out->newest_longterm_series), "%s",
		         newest_lt->series);
	}
	out->series_maintained = find_longterm_series(running_series) != NULL;

	switch (channel) {
	case KERNEL_CHANNEL_MAINLINE:
		hit = find_moniker("mainline");
		break;
	case KERNEL_CHANNEL_STABLE:
		hit = find_moniker("stable");
		break;
	case KERNEL_CHANNEL_LONGTERM:
		/* The line the box is already on, while it is still a
		 * longterm line; only once kernel.org stops listing it does
		 * this fall back to the newest, and the caller can see that
		 * happened from series_maintained. */
		hit = find_longterm_series(running_series);
		if (hit == NULL)
			hit = newest_lt;
		break;
	case KERNEL_CHANNEL_PINNED:
	default:
		/* Deliberately no resolution: the whole point of pinned is
		 * that nothing proposes a version. The context above is still
		 * reported, so an operator can see what they are declining. */
		out->known = 1;
		return;
	}
	if (hit != NULL)
		fill_from(out, hit);
	else
		out->known = 1;
}
