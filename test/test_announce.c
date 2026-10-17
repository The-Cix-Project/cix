/*
 * test_announce -- a state change the daemon makes by itself is
 * announced, by construction (ADR-0339 point 8, #599).
 *
 * A change made through the API is announced from one place,
 * dispatch(). A change the daemon makes on its own -- a container
 * exiting, a build reaching its next stage, a migration finishing --
 * has no API call in front of it, so it is announced where the state is
 * assigned. That only holds while every assignment goes through the
 * code that announces, and a new one written directly would leave a
 * page silently stale. So this reads the source and refuses one.
 *
 * Text, not syntax, in the test_naming pattern: an assignment is a line
 * outside a comment that assigns `state` or `running` (`x->state = `,
 * not `==`). The allowed lines are the announcing setters' own bodies,
 * each named below with the reason it is allowed.
 */
#include <stdio.h>
#include <string.h>

static int g_failures;

/* The line with leading whitespace removed. */
static const char *trimmed(const char *line)
{
	while (*line == ' ' || *line == '\t')
		line++;
	return line;
}

static int is_comment(const char *t)
{
	return t[0] == '*' || strncmp(t, "/*", 2) == 0 || strncmp(t, "//", 2) == 0;
}

/* Whether the line assigns `field` through -> or . (not a comparison). */
static int assigns(const char *t, const char *field)
{
	char arrow[32], dot[32];
	const char *p;
	size_t n;

	snprintf(arrow, sizeof(arrow), "->%s =", field);
	snprintf(dot, sizeof(dot), ".%s =", field);
	p = strstr(t, arrow);
	n = strlen(arrow);
	if (p == NULL) {
		p = strstr(t, dot);
		n = strlen(dot);
	}
	return p != NULL && p[n] != '=';
}

static int chomp_eq(const char *line, const char *want)
{
	size_t n = strlen(want);

	return strncmp(line, want, n) == 0 && (line[n] == '\n' || line[n] == '\0');
}

/*
 * Every assignment of `field` in `path` must be one of `allowed`
 * (trimmed, exact). With `must_follow` set, each assignment must
 * instead be followed on the next line by that call.
 */
static void check_file(const char *path, const char *field, const char *const *allowed,
                       const char *must_follow)
{
	FILE *f = fopen(path, "r");
	char line[1024];
	int lineno = 0, pending = 0, pending_line = 0;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		g_failures++;
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *t = trimmed(line);
		size_t i;
		int ok = 0;

		lineno++;
		if (pending) {
			if (!chomp_eq(t, must_follow)) {
				fprintf(stderr,
				        "FAIL: %s:%d assigns %s without %s on the next line -- a state "
				        "change the daemon makes by itself must be announced (ADR-0339)\n",
				        path, pending_line, field, must_follow);
				g_failures++;
			}
			pending = 0;
		}
		if (is_comment(t) || !assigns(t, field))
			continue;
		if (must_follow != NULL) {
			pending = 1;
			pending_line = lineno;
			continue;
		}
		for (i = 0; allowed != NULL && allowed[i] != NULL; i++)
			if (chomp_eq(t, allowed[i]))
				ok = 1;
		if (!ok) {
			fprintf(stderr,
			        "FAIL: %s:%d assigns %s directly -- go through the announcing setter "
			        "(ADR-0339): %s",
			        path, lineno, field, t);
			g_failures++;
		}
	}
	fclose(f);
}

int main(void)
{
	/*
	 * pkg.c: a package job's state changes only in pkg_job_set_state(),
	 * and the recipe-commit job's only in recipe_commit_set_state().
	 * The third line copies a state into a listing's output struct; it
	 * changes nothing.
	 */
	static const char *const pkg_allowed[] = {
		"e->state = s;",
		"g_recipe_commit.state = s;",
		"out[n].state = e->state;",
		NULL,
	};
	/* The two storage-migration files: their migrate_job_set_state(). */
	static const char *const migrate_allowed[] = {
		"job->state = s;",
		NULL,
	};

	printf("test_announce\n");
	check_file("daemon/src/pkg.c", "state", pkg_allowed, NULL);
	check_file("daemon/src/storagemigrate.c", "state", migrate_allowed, NULL);
	check_file("daemon/src/containerstoragemigrate.c", "state", migrate_allowed, NULL);
	/* registry.c: a container's running state, announced on the spot. */
	check_file("daemon/src/registry.c", "running", NULL, "registry_announce();");
	if (g_failures != 0) {
		printf("FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("PASS\n");
	return 0;
}
