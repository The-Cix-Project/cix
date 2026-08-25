/*
 * Issue #65 (ADR-0193): unit test for the kernel release-channel
 * policy. Links kernelpolicy.c directly against throwaway state files
 * -- no daemon subprocess and no network, which matters: the one thing
 * this module must never do is depend on kernel.org being reachable to
 * answer what it already knows.
 *
 * Asserts the properties the ADR names: pinned is the default and
 * proposes nothing; a channel survives a reload; longterm resolves
 * within the line the box is already on rather than jumping to the
 * newest one; a file that names no usable release keeps the previous
 * answer rather than replacing a true one with an empty one.
 */
#include "kernelpolicy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int ok = 1;

#define CHECK(cond, msg)                                                                          \
	do {                                                                                       \
		if (!(cond)) {                                                                      \
			fprintf(stderr, "FAIL: %s\n", msg);                                        \
			ok = 0;                                                                     \
		}                                                                                  \
	} while (0)

static void write_file(const char *path, const char *content)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		perror("fopen");
		exit(1);
	}
	fputs(content, f);
	fclose(f);
}

/* A trimmed but structurally real kernel.org releases.json: six
 * longterm lines at once is the actual shape, and it is precisely what
 * makes "longterm" ambiguous without a rule. */
static const char *RELEASES =
    "{\"latest_stable\":{\"version\":\"7.2\"},\"releases\":["
    "{\"moniker\":\"mainline\",\"version\":\"7.2\","
    "\"source\":\"https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.2.tar.xz\"},"
    "{\"moniker\":\"stable\",\"version\":\"7.1.10\","
    "\"source\":\"https://cdn.kernel.org/pub/linux/kernel/v7.x/linux-7.1.10.tar.xz\"},"
    "{\"moniker\":\"longterm\",\"version\":\"6.18.46\","
    "\"source\":\"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.18.46.tar.xz\"},"
    "{\"moniker\":\"longterm\",\"version\":\"6.12.105\","
    "\"source\":\"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.12.105.tar.xz\"},"
    "{\"moniker\":\"longterm\",\"version\":\"6.6.153\","
    "\"source\":\"https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.6.153.tar.xz\"},"
    "{\"moniker\":\"linux-next\",\"version\":\"next-20260821\",\"source\":\"\"}"
    "]}";

int main(void)
{
	char state[] = "/tmp/cix_test_kpol_XXXXXX";
	char releases[] = "/tmp/cix_test_krel_XXXXXX";
	char state_path[256];
	char releases_path[256];
	struct kernel_resolution res;
	char series[KERNEL_SERIES_MAX];

	if (mkdtemp(state) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	if (mkdtemp(releases) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(state_path, sizeof(state_path), "%s/kernel_policy.json", state);
	snprintf(releases_path, sizeof(releases_path), "%s/releases.json", releases);

	/* 1. Series extraction: the line is the first two components, and
	 * this platform's own recipe revision is never part of it. */
	CHECK(kernel_version_series("6.18.40-13", series, sizeof(series)) == 0 &&
	          strcmp(series, "6.18") == 0,
	      "a recipe revision leaks into the kernel line");
	CHECK(kernel_version_series("7.2", series, sizeof(series)) == 0 && strcmp(series, "7.2") == 0,
	      "a two-component mainline version has no line");
	CHECK(kernel_version_series("7", series, sizeof(series)) != 0,
	      "a bare number was accepted as a kernel line");
	CHECK(kernel_version_series("not-a-version", series, sizeof(series)) != 0,
	      "a non-version was accepted as a kernel line");

	/* 2. Pinned is the default, and it proposes nothing. */
	CHECK(kernelpolicy_init(state_path) == 0, "kernelpolicy_init on a fresh dir failed");
	CHECK(kernelpolicy_channel() == KERNEL_CHANNEL_PINNED, "the default channel is not pinned");
	kernelpolicy_resolve(KERNEL_CHANNEL_PINNED, "6.18", &res);
	CHECK(res.version[0] == '\0', "pinned proposed a version");

	/* 3. Nothing ingested yet is its own answer, not a guess. */
	kernelpolicy_resolve(KERNEL_CHANNEL_LONGTERM, "6.18", &res);
	CHECK(!res.known && res.version[0] == '\0',
	      "a channel resolved to something before any release data existed");

	/* 4. A malformed file leaves the (empty) cache alone rather than
	 * claiming success. */
	write_file(releases_path, "this is not json");
	CHECK(kernelpolicy_ingest_releases(releases_path, 1000) != 0,
	      "malformed release data was accepted");
	CHECK(kernelpolicy_fetched_at() == 0, "a rejected ingest recorded a fetch time");

	/* 5. Real data. */
	write_file(releases_path, RELEASES);
	CHECK(kernelpolicy_ingest_releases(releases_path, 1234) == 0, "real release data was rejected");
	CHECK(kernelpolicy_fetched_at() == 1234, "the fetch time was not recorded");

	/* 6. longterm stays on the line the box is already on -- the whole
	 * point of the rule, since six longterm lines are listed at once. */
	kernelpolicy_resolve(KERNEL_CHANNEL_LONGTERM, "6.12", &res);
	CHECK(res.known && strcmp(res.version, "6.12.105") == 0,
	      "longterm jumped off the line this box is on");
	CHECK(res.series_maintained, "6.12 was reported as no longer a longterm line");
	CHECK(strcmp(res.newest_longterm, "6.18.46") == 0,
	      "the newest longterm line was not reported alongside");

	/* 7. A line kernel.org no longer lists falls back to the newest,
	 * and says that is what happened. */
	kernelpolicy_resolve(KERNEL_CHANNEL_LONGTERM, "5.4", &res);
	CHECK(res.known && strcmp(res.version, "6.18.46") == 0,
	      "an unmaintained line did not fall back to the newest longterm");
	CHECK(!res.series_maintained, "an unlisted line was reported as maintained");

	/* 8. stable and mainline are unambiguous -- one entry each. */
	kernelpolicy_resolve(KERNEL_CHANNEL_STABLE, "6.18", &res);
	CHECK(strcmp(res.version, "7.1.10") == 0, "stable resolved wrongly");
	kernelpolicy_resolve(KERNEL_CHANNEL_MAINLINE, "6.18", &res);
	CHECK(strcmp(res.version, "7.2") == 0, "mainline resolved wrongly");
	CHECK(strstr(res.source_url, "linux-7.2.tar.xz") != NULL,
	      "the resolved source URL is not kernel.org's own");

	/* 9. linux-next has no tarball and no channel here; it must not be
	 * reachable as one. */
	kernelpolicy_resolve(KERNEL_CHANNEL_MAINLINE, "next-20260821", &res);
	CHECK(strcmp(res.version, "7.2") == 0, "a linux-next entry leaked into a channel");

	/* 10. Once real data is cached, a later malformed fetch keeps it --
	 * a stale true answer beats an empty fresh one. */
	write_file(releases_path, "{\"releases\":[]}");
	CHECK(kernelpolicy_ingest_releases(releases_path, 9999) != 0,
	      "release data naming nothing usable was accepted");
	kernelpolicy_resolve(KERNEL_CHANNEL_STABLE, "6.18", &res);
	CHECK(strcmp(res.version, "7.1.10") == 0, "a rejected ingest destroyed the cached answer");
	CHECK(kernelpolicy_fetched_at() == 1234, "a rejected ingest overwrote the fetch time");

	/* 11. The channel survives a reload -- it is operator state. */
	CHECK(kernelpolicy_set_channel(KERNEL_CHANNEL_MAINLINE) == 0, "set_channel failed");
	CHECK(kernelpolicy_init(state_path) == 0, "reload failed");
	CHECK(kernelpolicy_channel() == KERNEL_CHANNEL_MAINLINE, "the channel did not survive a reload");

	/* 12. Name round-tripping, since these are kernel.org's own words
	 * and inventing a spelling here would be inventing a channel. */
	{
		enum kernel_channel c;

		CHECK(kernel_channel_from_name("longterm", &c) == 0 && c == KERNEL_CHANNEL_LONGTERM,
		      "longterm did not round-trip");
		CHECK(kernel_channel_from_name("edge", &c) != 0, "an invented channel name was accepted");
		CHECK(kernel_channel_from_name(NULL, &c) != 0, "a NULL channel name was accepted");
		CHECK(strcmp(kernel_channel_name(KERNEL_CHANNEL_STABLE), "stable") == 0,
		      "stable did not render as kernel.org spells it");
	}

	unlink(state_path);
	unlink(releases_path);
	rmdir(state);
	rmdir(releases);

	printf("KERNEL POLICY RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
