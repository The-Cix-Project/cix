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
 *   5. Every flag literal in main.c is somewhere in the table.
 *   6. Every subcommand the source routes (strcmp(sub, "...")) is
 *      somewhere in the table.
 *   7. Every top-level command in the table has a line in the help text
 *      (print_usage()). Measured 2026-09-23: 14 routed commands --
 *      volume, deployment, dhcp, factory-reset and ten more -- could be
 *      run and completed but did not appear in `cixctl help` at all.
 *
 * 5 and 6 are the reverse direction, and it used to be missing. The
 * comment that stood here argued it could not be done -- "a flag literal
 * cannot be attributed to its command by scanning alone" -- and called
 * the gap "a missing convenience". Measured on 2026-09-12, the gap was
 * 33 flags: 239 distinct flag literals in main.c against 207 in the
 * table, so --level=, --mail=, --since=, --ssh-key=, every one of
 * `ldap config set`'s nine TLS flags and every one of `cixctl logs`'s
 * six were accepted by a parser and offered by nothing. `pki export`
 * and `pki import` were absent as whole subcommands. The owner's report
 * was "it does not do second or third parameter completion", which is
 * exactly what that looks like from the outside -- not a convenience.
 *
 * The attribution problem was real and is sidestepped rather than
 * solved: these two checks ask whether a flag or subcommand appears
 * ANYWHERE in the table, not whether it hangs off the right node. So a
 * flag listed under the wrong command still passes. That is a weaker
 * guarantee than checks 1-4 give, and it is the strongest one available
 * without a real C parser -- 85 of the table's 274 paths have no
 * matching cmd_* function at all, because those commands switch on
 * their subcommand inline in one function, so a naive name-based
 * attribution would invent failures rather than find them.
 *
 * Comments are stripped before scanning. Without that, prose describing
 * a flag counts as the flag existing -- and cli/src/main.c really does
 * discuss "--name" in a comment about completion, which is precisely
 * the false negative that would let a genuinely missing flag through.
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

/*
 * Blanks out comments in place, preserving length and newlines so any
 * offset arithmetic and line counting stay valid. Without this, a flag
 * merely DISCUSSED in a comment reads as a flag that exists -- see the
 * header comment for the real case that caused.
 */
static void strip_comments(char *src)
{
	size_t i = 0;

	while (src[i] != '\0') {
		if (src[i] == '"' || src[i] == '\'') {
			char quote = src[i++];

			while (src[i] != '\0' && src[i] != quote) {
				if (src[i] == '\\' && src[i + 1] != '\0')
					i++;
				i++;
			}
			if (src[i] != '\0')
				i++;
			continue;
		}
		if (src[i] == '/' && src[i + 1] == '*') {
			src[i] = ' ';
			src[i + 1] = ' ';
			i += 2;
			while (src[i] != '\0' && !(src[i] == '*' && src[i + 1] == '/')) {
				if (src[i] != '\n')
					src[i] = ' ';
				i++;
			}
			if (src[i] != '\0') {
				src[i] = ' ';
				src[i + 1] = ' ';
				i += 2;
			}
			continue;
		}
		if (src[i] == '/' && src[i + 1] == '/') {
			while (src[i] != '\0' && src[i] != '\n')
				src[i++] = ' ';
			continue;
		}
		i++;
	}
}

/* Recursive membership tests over the whole table -- "does this appear
 * anywhere", deliberately not "does it hang off the right node"; see the
 * header comment for why that is the available guarantee. */
static int level_has_flag(const struct cli_node *level, const char *flag)
{
	int i, j;

	for (i = 0; level != NULL && level[i].name != NULL; i++) {
		for (j = 0; level[i].flags != NULL && level[i].flags[j] != NULL; j++) {
			if (strcmp(level[i].flags[j], flag) == 0)
				return 1;
		}
		if (level_has_flag(level[i].subs, flag))
			return 1;
	}
	return 0;
}

static int level_has_sub(const struct cli_node *level, const char *name)
{
	int i;

	for (i = 0; level != NULL && level[i].name != NULL; i++) {
		if (strcmp(level[i].name, name) == 0)
			return 1;
		if (level_has_sub(level[i].subs, name))
			return 1;
	}
	return 0;
}

/*
 * Flags the table legitimately does not carry, each because it is not a
 * command's flag at all. Kept as an explicit list rather than a rule, so
 * adding one is a visible line in a diff and has to be argued for.
 */
static const char *const g_flag_exempt[] = {
	"--json",  /* global: parsed in main() before dispatch, valid with every command,
	             * so hanging it off any single node would be a lie and hanging it off
	             * all of them would drown every completion */
	NULL
};

static int flag_is_exempt(const char *flag)
{
	int i;

	for (i = 0; g_flag_exempt[i] != NULL; i++) {
		if (strcmp(g_flag_exempt[i], flag) == 0)
			return 1;
	}
	return 0;
}

/*
 * Every flag literal the parser reads must be offered by something.
 *
 * The token shape is validated rather than assumed: "--" then at least
 * one [a-z0-9-], an optional "=", and then the closing quote and
 * nothing else. That precision is load-bearing. main.c is full of usage
 * banners that also begin with a quote and two dashes --
 * "--gateway=A.B.C.D --interface=IFNAME] [--wait]\n" is a real one --
 * and a scanner that took everything up to the next quote reported 19
 * of those as missing flags. A guard that cries wolf gets switched off,
 * so it matches only what is unambiguously a flag literal and leaves
 * prose alone.
 */
static void scan_flag_literals(void)
{
	size_t off = 0;

	for (;;) {
		const char *at = strstr(g_src + off, "\"--");
		const char *c;
		char tok[160];
		size_t len;

		if (at == NULL)
			return;
		off = (size_t)(at - g_src) + 3;

		c = at + 3;
		while ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-')
			c++;
		if (c == at + 3)
			continue; /* "--" with no name: prose, not a flag */
		if (*c == '=')
			c++;
		if (*c != '"')
			continue; /* something follows the flag inside the same string: prose */

		len = (size_t)(c - (at + 1)); /* from the first '-' up to the quote */
		if (len == 0 || len >= sizeof(tok))
			continue;
		memcpy(tok, at + 1, len);
		tok[len] = '\0';
		if (flag_is_exempt(tok))
			continue;
		if (!level_has_flag(CLI_TREE, tok))
			fail("cli/src/main.c reads flag %s, which cmdtree.h offers nowhere -- "
			     "it can be typed but never completed",
			     tok);
	}
}

/* And every subcommand the source routes must be in the table. */
static void scan_routed_subcommands(void)
{
	static const char *const open = "strcmp(sub, \"";
	size_t off = 0;
	size_t openlen = strlen(open);

	for (;;) {
		const char *at = strstr(g_src + off, open);
		const char *start, *end;
		char tok[160];
		size_t len;

		if (at == NULL)
			return;
		start = at + openlen;
		off = (size_t)(start - g_src);
		end = strchr(start, '"');
		if (end == NULL)
			continue;
		len = (size_t)(end - start);
		if (len == 0 || len >= sizeof(tok))
			continue;
		memcpy(tok, start, len);
		tok[len] = '\0';
		if (!level_has_sub(CLI_TREE, tok))
			fail("cli/src/main.c routes subcommand '%s', which appears nowhere in "
			     "cmdtree.h -- it could be run but never completed",
			     tok);
	}
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
	strip_comments(g_src);

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

	/*
	 * 5 and 6: the reverse direction, which used to be missing
	 * entirely. Worth 33 flags and two whole subcommands when it was
	 * first switched on -- see the header comment.
	 */
	scan_flag_literals();
	scan_routed_subcommands();

	/*
	 * 7: every top-level command has a help line. A help line is a
	 * string literal inside print_usage() that begins with two spaces
	 * and the command name, followed by a space or the end of the line.
	 */
	{
		const char *begin = strstr(g_src, "static void print_usage(FILE *out)");
		const char *finish = begin != NULL ? strstr(begin, "\n}\n") : NULL;

		if (begin == NULL || finish == NULL) {
			fail("cannot find print_usage() in %s", src_path);
		} else {
			for (i = 0; CLI_TREE[i].name != NULL; i++) {
				char with_space[160], at_eol[160];
				const char *hit_space, *hit_eol;

				snprintf(with_space, sizeof(with_space), "\"  %s ", CLI_TREE[i].name);
				snprintf(at_eol, sizeof(at_eol), "\"  %s\\n", CLI_TREE[i].name);
				hit_space = strstr(begin, with_space);
				hit_eol = strstr(begin, at_eol);
				if ((hit_space == NULL || hit_space > finish) &&
				    (hit_eol == NULL || hit_eol > finish))
					fail("top-level command '%s' has no line in print_usage() -- "
					     "it can be run but `cixctl help` never mentions it",
					     CLI_TREE[i].name);
			}
		}
	}

	/*
	 * 8: a flag matched by a hand-counted prefix length must be
	 * matched by the RIGHT one.
	 *
	 * `strncmp(argv[i], "--max-concurrent-jobs=", 23)` shipped with
	 * 23 against a 22-byte literal. Comparing one byte too many
	 * includes the literal's own NUL, so it matched only an argument
	 * that ended there -- the flag with no value -- and every real
	 * invocation fell through to "unknown option". The offset used to
	 * read the value was wrong the same way, so even a match would
	 * have dropped the first character of it.
	 *
	 * Nothing caught it. `cixctl help` advertised the flag, cmdtree.h
	 * offered it for completion, and checks 3 and 5 above were both
	 * satisfied -- because the LITERAL is present and correct in
	 * main.c, which is all they ask. The flag existed everywhere
	 * except in the one comparison that had to be right, and
	 * ADR-0157's operator control over build concurrency was
	 * unreachable from the CLI for as long as it had existed. Found
	 * by hand, trying to use it.
	 *
	 * Off-by-one in the other direction is just as bad and is caught
	 * by the same equality: a length one short makes `--memory-maxX=`
	 * match `--memory-max=`.
	 */
	for (off = 0; off < g_src_len;) {
		const char *at = strstr(g_src + off, "strncmp(argv[");
		const char *quote, *end, *comma;
		char flag[160];
		size_t len;
		long declared;

		if (at == NULL)
			break;
		off = (size_t)(at - g_src) + 1;
		quote = strchr(at, '"');
		if (quote == NULL)
			continue;
		end = strchr(quote + 1, '"');
		if (end == NULL)
			continue;
		len = (size_t)(end - (quote + 1));
		if (len == 0 || len >= sizeof(flag))
			continue;
		memcpy(flag, quote + 1, len);
		flag[len] = '\0';
		/* Only the prefix form has a length to get wrong. */
		if (flag[len - 1] != '=')
			continue;
		comma = strchr(end, ',');
		if (comma == NULL)
			continue;
		declared = strtol(comma + 1, NULL, 10);
		if (declared > 0 && (size_t)declared != len)
			fail("cli/src/main.c compares %zu bytes of \"%s\" against %ld -- the flag can "
			     "never match with a value, and the offset that reads the value is wrong "
			     "the same way",
			     len, flag, declared);
	}

	free(g_src);
	if (g_failures > 0) {
		fprintf(stderr, "test_clitree: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("  cli tree: %d top-level commands, consistent with the dispatcher\n", top);
	return 0;
}
