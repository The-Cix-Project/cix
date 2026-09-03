/*
 * The CLI command tree must agree with the CLI (#151).
 *
 * cli/src/cmdtree.h describes what commands, subcommands and flags
 * exist, so that completion can reach the end of a command. That table
 * is data, and dispatch_command() is a hand-written strcmp() chain, so
 * without something checking them against each other the table is a
 * second model of the same thing -- and a completion that offers a flag
 * the parser rejects is worse than no completion.
 *
 * This is that check. It re-derives the surface from cli/src/main.c
 * itself and fails the build when the two disagree, which is the same
 * approach test_docindex and test_apigen already take: the guard reads
 * the real source rather than trusting a number someone maintained.
 *
 * What it verifies:
 *
 *   1. Every command the dispatcher routes is in the table.
 *   2. Every command in the table is routed by the dispatcher.
 *   3. Every flag the table offers exists as a literal in main.c, so a
 *      completion can never suggest a flag no parser reads.
 *   4. Every subcommand name in the table exists as a literal too.
 *
 * What it deliberately does NOT verify: that every flag in main.c is in
 * the table. A flag literal cannot be attributed to its command by
 * scanning alone -- main.c has one function per subcommand, not one per
 * command -- so that direction would need a real C parser. The
 * consequence of the gap is a flag that exists and is not offered, which
 * is a missing convenience; the direction that is checked is the one
 * whose failure actively misleads.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "cmdtree.h"

static char *g_src;
static size_t g_src_len;
static int g_failures;

static void fail(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	fputs("  FAIL: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
	g_failures++;
}

static char *slurp(const char *path, size_t *out_len)
{
	FILE *f = fopen(path, "rb");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n < 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fclose(f);
		free(buf);
		return NULL;
	}
	fclose(f);
	buf[n] = '\0';
	*out_len = (size_t)n;
	return buf;
}

/* True if the exact quoted literal "word" appears in main.c. */
static int literal_present(const char *word)
{
	char needle[160];

	if (snprintf(needle, sizeof(needle), "\"%s\"", word) >= (int)sizeof(needle))
		return 1; /* too long to check rather than a failure */
	return strstr(g_src, needle) != NULL;
}

/* True if the dispatcher routes this top-level command name. */
static int dispatcher_routes(const char *name)
{
	char needle[160];

	if (snprintf(needle, sizeof(needle), "strcmp(cmd, \"%s\")", name) >= (int)sizeof(needle))
		return 0;
	return strstr(g_src, needle) != NULL;
}

static void check_node(const struct cli_node *n, const char *path)
{
	int i;

	for (i = 0; n->flags != NULL && n->flags[i] != NULL; i++) {
		if (!literal_present(n->flags[i]))
			fail("%s offers flag %s, which appears nowhere in cli/src/main.c -- "
			     "completion would suggest a flag no parser reads",
			     path, n->flags[i]);
	}
	for (i = 0; n->subs != NULL && n->subs[i].name != NULL; i++) {
		char child[256];

		if (!literal_present(n->subs[i].name))
			fail("%s names subcommand '%s', which appears nowhere in cli/src/main.c",
			     path, n->subs[i].name);
		snprintf(child, sizeof(child), "%s %s", path, n->subs[i].name);
		check_node(&n->subs[i], child);
	}
}

int main(void)
{
	const char *src_path = "cli/src/main.c";
	size_t off;
	int i, top = 0;

	g_src = slurp(src_path, &g_src_len);
	if (g_src == NULL) {
		fprintf(stderr, "test_clitree: cannot read %s -- run from the repository root\n",
		        src_path);
		return 1;
	}

	/* 1 + 3 + 4: everything the table claims must exist in the source. */
	for (i = 0; CLI_TREE[i].name != NULL; i++) {
		top++;
		if (!dispatcher_routes(CLI_TREE[i].name))
			fail("table has top-level command '%s', which dispatch_command() does not route",
			     CLI_TREE[i].name);
		check_node(&CLI_TREE[i], CLI_TREE[i].name);
	}

	/*
	 * 2: and everything the dispatcher routes must be in the table.
	 *
	 * Walks every `strcmp(cmd, "..."` in main.c, which is exactly the
	 * dispatcher's own routing form. This is the direction that caught
	 * the original defect: the hand-kept shell list had drifted to 59
	 * entries against 62 routed commands, so three commands could be
	 * run and never completed.
	 */
	for (off = 0; off + 14 < g_src_len;) {
		const char *at = strstr(g_src + off, "strcmp(cmd, \"");
		const char *start, *end;
		char name[128];
		size_t len;
		int found = 0;

		if (at == NULL)
			break;
		start = at + strlen("strcmp(cmd, \"");
		end = strchr(start, '"');
		off = (size_t)(start - g_src);
		if (end == NULL)
			continue;
		len = (size_t)(end - start);
		if (len == 0 || len >= sizeof(name))
			continue;
		memcpy(name, start, len);
		name[len] = '\0';
		for (i = 0; CLI_TREE[i].name != NULL; i++) {
			if (strcmp(CLI_TREE[i].name, name) == 0) {
				found = 1;
				break;
			}
		}
		/* The client-local commands are handled in main() before the
		 * dispatcher and have no endpoint, so they are legitimately
		 * absent from the tree. */
		if (!found && strcmp(name, "pager") != 0 && strcmp(name, "__complete") != 0)
			fail("dispatch_command() routes '%s', which is missing from cmdtree.h -- "
			     "it could be run but never completed",
			     name);
	}

	free(g_src);
	if (g_failures > 0) {
		fprintf(stderr, "test_clitree: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("  cli tree: %d top-level commands, consistent with the dispatcher\n", top);
	return 0;
}
