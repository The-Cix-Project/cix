/*
 * ADR-0255: the source-policy axis -- which upstream release a package
 * builds -- and its interaction with the discovery kind.
 *
 * The property worth guarding is that the platform-wide default is a
 * PREFERENCE and not a mandate. A global "stable" is right for the
 * packages it fits and simply does not apply to the ones it does not,
 * and the difference between those two outcomes must be a specific
 * sentence naming the package rather than an empty resolution.
 *
 * NOTE ON COVERAGE: the "kind with no channels" branch (glibc's shape)
 * is exercised in test_srcupstream against a kind declared there. It
 * cannot be reached through srcpolicy_effective_for_kind() yet, because
 * that resolves a kind by name from the registry and kernel.org is the
 * only registered kind. That is a real gap and is recorded here rather
 * than papered over with a fake registration.
 */
#include "srcpolicy.h"
#include "srcdepth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
static char g_path[256];

static void bad(const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "  FAIL: ");
	fprintf(stderr, fmt, a, b);
	fprintf(stderr, "\n");
	g_failures++;
}

static void expect_effective(const char *name, const char *kind, int want_ok,
                              const char *want_channel, const char *must_mention)
{
	struct srcpolicy p;
	char err[256];
	int rc = srcpolicy_effective_for_kind(name, kind, &p, err, sizeof(err));

	if (want_ok && rc != 0) {
		bad("%s should resolve, refused: %s", name, err);
		return;
	}
	if (!want_ok) {
		if (rc == 0) {
			bad("%s should NOT resolve for kind %s", name, kind ? kind : "(none)");
			return;
		}
		if (err[0] == '\0') {
			bad("%s refused with no reason%s", name, "");
			return;
		}
		if (must_mention != NULL && strstr(err, must_mention) == NULL) {
			bad("%s reason did not mention \"%s\"", err, must_mention);
			return;
		}
		printf("  %-22s refused: %s\n", name, err);
		return;
	}
	if (want_channel != NULL && strcmp(p.channel, want_channel) != 0) {
		bad("%s channel is \"%s\"", name, p.channel);
		return;
	}
	printf("  %-22s -> channel=\"%s\" depth=%s%s\n", name, p.channel, p.depth,
	        p.explicit_entry ? "" : " (inherited)");
}

int main(void)
{
	char err[256];
	struct srcpolicy p;

	snprintf(g_path, sizeof(g_path), "/tmp/cix_srcpolicy_test_%d.json", (int)getpid());
	unlink(g_path);
	printf("test_srcpolicy\n");

	if (srcpolicy_init(g_path) != 0) {
		fprintf(stderr, "  FAIL: init\n");
		return 1;
	}

	/* An unconfigured platform: no channel, depth n. */
	srcpolicy_default_get(&p);
	if (p.channel[0] != '\0' || strcmp(p.depth, "n") != 0) {
		bad("fresh default is channel=\"%s\" depth=%s", p.channel, p.depth);
	} else {
		printf("  fresh default          -> channel=\"\" depth=n\n");
	}

	/*
	 * With no default channel, a kind that HAS channels cannot roll --
	 * there is no defensible pick between mainline and longterm.
	 */
	expect_effective("kernel", "kernel.org", 0, NULL, "choose one");

	/* THE DEFAULT ACTS: setting it globally makes the package roll. */
	if (srcpolicy_default_set("stable", "n", err, sizeof(err)) != 0)
		bad("default_set: %s%s", err, "");
	expect_effective("kernel", "kernel.org", 1, "stable", NULL);

	/*
	 * A default naming a channel this kind does not publish is dropped
	 * rather than obeyed, and the package then reports what IT needs.
	 * The message must be actionable for this package, not a complaint
	 * about a global setting that is fine for other packages.
	 */
	if (srcpolicy_default_set("lts", "n", err, sizeof(err)) != 0)
		bad("default_set lts: %s%s", err, "");
	expect_effective("kernel", "kernel.org", 0, NULL, "choose one");

	/* A per-package entry overrides the default. */
	if (srcpolicy_set("kernel", "kernel.org", "longterm", "n-1", err, sizeof(err)) != 0)
		bad("set kernel: %s%s", err, "");
	expect_effective("kernel", "kernel.org", 1, "longterm", NULL);
	srcpolicy_get("kernel", &p);
	if (!p.explicit_entry || strcmp(p.depth, "n-1") != 0)
		bad("override not recorded: depth=%s explicit=%s", p.depth, p.explicit_entry ? "y" : "n");

	/* Invalid channel refused AT SET TIME, with the valid list. */
	if (srcpolicy_set("kernel", "kernel.org", "lts", "n", err, sizeof(err)) == 0)
		bad("an invented channel was accepted%s%s", "", "");
	else if (strstr(err, "longterm") == NULL)
		bad("set-time refusal did not list channels: %s%s", err, "");
	else
		printf("  set invented channel   refused: %s\n", err);

	/* Invalid depth refused, naming the grammar. */
	if (srcpolicy_set("kernel", "kernel.org", "stable", "n-1.2.3", err, sizeof(err)) == 0)
		bad("bad depth accepted%s%s", "", "");
	else
		printf("  set bad depth          refused: %s\n", err);

	/* A package with no declared upstream cannot roll, and says so. */
	expect_effective("bash", NULL, 0, NULL, "pinned");
	if (srcpolicy_set("bash", NULL, "stable", "n", err, sizeof(err)) == 0)
		bad("channel accepted for a package with no upstream%s%s", "", "");
	else
		printf("  channel w/o upstream   refused: %s\n", err);

	/* An unknown kind is named, not silently treated as unrollable. */
	expect_effective("kernel", "sourceforge", 0, NULL, "sourceforge");

	/* Persistence: the override must survive a reload. */
	if (srcpolicy_init(g_path) != 0)
		bad("re-init failed%s%s", "", "");
	srcpolicy_get("kernel", &p);
	if (!p.explicit_entry || strcmp(p.channel, "longterm") != 0 || strcmp(p.depth, "n-1") != 0)
		bad("after reload: channel=%s depth=%s", p.channel, p.depth);
	else
		printf("  survives reload        -> longterm / n-1\n");
	srcpolicy_default_get(&p);
	if (strcmp(p.channel, "lts") != 0)
		bad("default channel lost across reload: \"%s\"%s", p.channel, "");

	/* clear() returns the package to inheriting the default. */
	if (srcpolicy_clear("kernel") != 0)
		bad("clear failed%s%s", "", "");
	srcpolicy_get("kernel", &p);
	if (p.explicit_entry)
		bad("clear left an explicit entry%s%s", "", "");
	else
		printf("  clear                  -> inherits again\n");

	unlink(g_path);
	if (g_failures != 0) {
		fprintf(stderr, "test_srcpolicy: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_srcpolicy: PASS\n");
	return 0;
}
