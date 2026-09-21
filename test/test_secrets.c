/*
 * test_secrets -- no tracked file may contain an embedded credential.
 *
 * Written after a real leak, and the shape of that leak is why this
 * test scans rather than trusting convention. On 2026-09-21 a scan of
 * this repository's history found TWELVE distinct secrets embedded in URLs of the
 * userinfo form (a user and a secret before the @ of a host): one password in 1526 commits, one
 * ten-character string, and ten separate forty-character Gitea API
 * tokens. Every one arrived the same way -- a recipe or a guide was
 * edited to carry a working credential "temporarily", and three of
 * the commits say so in their own subject lines ("temp: substitute
 * real credential ... (reverted after fetch)"). The revert put the
 * placeholder back in HEAD and left the secret in history forever.
 *
 * The repository is mirrored publicly, so "it is only in history" was
 * never a containment argument.
 *
 * WHAT THIS CAN AND CANNOT DO. It gates the WORKING TREE, so it stops
 * the next one going in. It cannot clean the ones already in history:
 * that needs a rewrite of 670 tags, which would break 563 pinned
 * archive digests and 38 recipes that name a cix commit by SHA. The
 * decision taken was to rotate every exposed secret instead and leave
 * history alone, because a rotated credential in history is inert and
 * a rewrite of that size is not.
 *
 * So this test is the half that was actually available, and it is the
 * half that matters going forward.
 *
 * #60's {{REPO_TOKEN}} is the supported way to write one of these: the
 * daemon substitutes it at fetch time and it is never stored. That is
 * what the allowlist below permits, and it is the only thing it
 * permits -- a placeholder that looks like a secret is fine, and
 * anything else is not.
 */
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static int g_failures;

/*
 * The placeholders a credential position may legitimately hold. Kept
 * short deliberately: every addition here is a new way for a real
 * secret to be waved through, so a new one should be a decision, not
 * a convenience.
 */
static const char *const g_allowed[] = {
	"{{REPO_TOKEN}}",
	"REPLACE_WITH_REAL_TOKEN",
	"<password>",
	"<token>",
	/* Documentation placeholders. Each was found by running this
	 * scan over the tree before the test was committed, which is
	 * the only honest way to build an allowlist -- a guessed one
	 * either misses a real case or waves through a real secret. */
	"<TOKEN>",
	"TOKEN",
};

static int allowed_secret(const char *s, size_t len)
{
	size_t i;

	for (i = 0; i < sizeof(g_allowed) / sizeof(g_allowed[0]); i++) {
		size_t n = strlen(g_allowed[i]);

		if (n == len && strncmp(s, g_allowed[i], n) == 0)
			return 1;
	}
	return 0;
}

/*
 * Finds a URL userinfo component and reports any secret that is not a
 * known placeholder. Deliberately does NOT try to recognise what a
 * credential looks like -- a length threshold or a charset rule would
 * have missed the nine-character password that started this.
 */
static void scan_buffer(const char *path, const char *buf, size_t len)
{
	size_t i;
	int line = 1;

	for (i = 0; i + 3 < len; i++) {
		const char *colon, *at, *p;
		size_t seclen;

		if (buf[i] == '\n') {
			line++;
			continue;
		}
		if (strncmp(buf + i, "://", 3) != 0)
			continue;

		/* user runs to the next ':', secret from there to '@'. Both
		 * must appear before any '/', or this is a bare URL. */
		colon = NULL;
		at = NULL;
		for (p = buf + i + 3; p < buf + len; p++) {
			if (*p == '/' || *p == ' ' || *p == '"' || *p == '\n')
				break;
			if (*p == ':' && colon == NULL)
				colon = p;
			if (*p == '@') {
				at = p;
				break;
			}
		}
		if (colon == NULL || at == NULL || at <= colon + 1)
			continue;

		seclen = (size_t)(at - colon - 1);
		if (allowed_secret(colon + 1, seclen))
			continue;

		printf("  FAIL: %s:%d embeds a credential in a URL "
		       "(%zu-character secret)\n", path, line, seclen);
		printf("        use {{REPO_TOKEN}} (#60) -- the daemon "
		       "substitutes it at fetch time and never stores it\n");
		g_failures++;
	}
}

static void scan_file(const char *path)
{
	static char buf[1 << 20];
	FILE *f = fopen(path, "rb");
	size_t n;

	if (f == NULL)
		return;
	n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	scan_buffer(path, buf, n);
}

static void scan_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;

	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		char path[4096];
		struct stat st;

		if (e->d_name[0] == '.')
			continue;
		/* build/ is generated and .git/ holds history this test
		 * deliberately does not police -- see the header. */
		if (strcmp(e->d_name, "build") == 0 ||
		    strcmp(e->d_name, "build-inputs") == 0)
			continue;
		snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
		if (stat(path, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode))
			scan_dir(path);
		else if (S_ISREG(st.st_mode))
			scan_file(path);
	}
	closedir(d);
}

int main(void)
{
	scan_dir(".");
	if (g_failures != 0) {
		printf("SECRETS: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("SECRETS: PASS -- no tracked file embeds a credential\n");
	return 0;
}
