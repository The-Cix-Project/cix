/*
 * ADR-0255: channels come from the discovery kind, never from free text.
 *
 * The property under test is that a channel an upstream does not
 * publish is refused AT CONFIGURATION TIME with the valid list, rather
 * than accepted and later surfacing as an empty resolution. Those two
 * failures look nothing alike to an operator: one says "glibc has no
 * channels", the other says "nothing found", and only the first tells
 * them what they did.
 */
#include "srcupstream.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

static void expect(enum srcupstream_error got, enum srcupstream_error want, const char *what,
                    const char *err, const char *must_mention)
{
	if (got != want) {
		fprintf(stderr, "  FAIL: %s -- got %s, want %s\n", what, srcupstream_strerror(got),
		        srcupstream_strerror(want));
		g_failures++;
		return;
	}
	if (want != SRCUPSTREAM_OK) {
		if (err == NULL || err[0] == '\0') {
			fprintf(stderr, "  FAIL: %s gave no reason\n", what);
			g_failures++;
			return;
		}
		if (must_mention != NULL && strstr(err, must_mention) == NULL) {
			fprintf(stderr, "  FAIL: %s reason \"%s\" did not mention \"%s\"\n", what, err,
			        must_mention);
			g_failures++;
			return;
		}
		printf("  %-34s refused: %s\n", what, err);
		return;
	}
	printf("  %-34s ok\n", what);
}

/*
 * A kind with no channels -- glibc's shape. Declared here rather than
 * registered as a product kind, because discovery for it is not
 * implemented and registering a kind that cannot enumerate anything
 * would be a placeholder pretending to be a capability.
 */
static const struct srcupstream_kind LINEAR = { "test.linear", NULL, "one linear sequence" };

int main(void)
{
	char err[256];

	printf("test_srcupstream\n");

	/* kernel.org: channels exist and are kernel.org's own words. */
	expect(srcupstream_check("kernel.org", "stable", err, sizeof(err)), SRCUPSTREAM_OK,
	       "kernel.org + stable", err, NULL);
	expect(srcupstream_check("kernel.org", "longterm", err, sizeof(err)), SRCUPSTREAM_OK,
	       "kernel.org + longterm", err, NULL);
	expect(srcupstream_check("kernel.org", "mainline", err, sizeof(err)), SRCUPSTREAM_OK,
	       "kernel.org + mainline", err, NULL);

	/*
	 * The free-text failure this exists to prevent. "lts" is a word
	 * kernel.org has never used; accepting it would configure a box to
	 * track a channel that does not exist.
	 */
	expect(srcupstream_check("kernel.org", "lts", err, sizeof(err)), SRCUPSTREAM_ERR_BAD_CHANNEL,
	       "kernel.org + lts (invented)", err, "longterm");

	/* A kind with channels demands one; there is no defensible default. */
	expect(srcupstream_check("kernel.org", NULL, err, sizeof(err)),
	       SRCUPSTREAM_ERR_CHANNEL_REQUIRED, "kernel.org + no channel", err, "stable");
	expect(srcupstream_check("kernel.org", "", err, sizeof(err)),
	       SRCUPSTREAM_ERR_CHANNEL_REQUIRED, "kernel.org + empty channel", err, "stable");

	/* An unknown kind names the ones that exist. */
	expect(srcupstream_check("github", "stable", err, sizeof(err)), SRCUPSTREAM_ERR_NO_SUCH_KIND,
	       "unknown kind", err, "kernel.org");

	/* glibc's shape: no channels, so absence is correct and any channel is an error. */
	expect(srcupstream_check_channel(&LINEAR, NULL, err, sizeof(err)), SRCUPSTREAM_OK,
	       "linear + no channel", err, NULL);
	expect(srcupstream_check_channel(&LINEAR, "stable", err, sizeof(err)),
	       SRCUPSTREAM_ERR_NO_CHANNELS, "linear + a channel", err, "single release sequence");

	/* The registry is enumerable, so an API can show the choices. */
	{
		size_t i, n = srcupstream_count();
		int saw_korg = 0, saw_gitea = 0;

		if (n == 0) {
			fprintf(stderr, "  FAIL: registry is empty\n");
			g_failures++;
		}
		for (i = 0; i < n; i++) {
			const struct srcupstream_kind *k = srcupstream_at(i);

			if (k == NULL || k->name == NULL || k->summary == NULL) {
				fprintf(stderr, "  FAIL: kind %zu incomplete\n", i);
				g_failures++;
				continue;
			}
			if (strcmp(k->name, "gitea-tags") == 0 && k->channels == NULL)
				saw_gitea = 1;
			if (strcmp(k->name, "kernel.org") == 0)
				saw_korg = 1;
		}
		if (!saw_gitea) {
			fprintf(stderr, "  FAIL: gitea-tags not registered as a channel-less kind\n");
			g_failures++;
		}
		if (!saw_korg) {
			fprintf(stderr, "  FAIL: kernel.org not registered\n");
			g_failures++;
		}
		if (srcupstream_at(n) != NULL) {
			fprintf(stderr, "  FAIL: index past the end returned a kind\n");
			g_failures++;
		}
		printf("  registry enumerable (%zu kind(s))    ok\n", n);
	}

	/* ADR-0323: a template becomes a url the way cbs expands it. */
	{
		char out[256];
		struct {
			const char *tmpl, *version, *want;
		} cases[] = {
			{ "https://u:{{REPO_TOKEN}}@h/api/v1/repos/o/r/archive/v{version}.tar.gz", "1.1",
			  "https://u:{{REPO_TOKEN}}@h/api/v1/repos/o/r/archive/v1.1.tar.gz" },
			{ "x/{major}/v{version}", "7.2.8", "x/7/v7.2.8" },
			/* rung 2: one signed list per release directory */
			{ "https://cdn.kernel.org/pub/linux/kernel/v{major}.x/sha256sums.asc", "7.2.8",
			  "https://cdn.kernel.org/pub/linux/kernel/v7.x/sha256sums.asc" },
		};
		size_t i;

		for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
			if (srcupstream_expand(cases[i].tmpl, cases[i].version, out, sizeof(out)) != 0 ||
			    strcmp(out, cases[i].want) != 0) {
				fprintf(stderr, "  FAIL: expand %s -> %s\n", cases[i].tmpl, out);
				g_failures++;
			}
		if (srcupstream_expand("{version}{version}", "123456", out, 8) == 0) {
			fprintf(stderr, "  FAIL: expand truncated instead of refusing\n");
			g_failures++;
		}
		printf("  templates expand as cbs does        ok\n");
	}

	if (g_failures != 0) {
		fprintf(stderr, "test_srcupstream: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_srcupstream: PASS\n");
	return 0;
}
