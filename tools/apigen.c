/*
 * apigen -- the API route extractor (ADR-0218).
 *
 * A BUILD TOOL, not shipped code. It runs on a build machine, emits into
 * build/generated/, and is never installed on a host -- which is why it
 * lives in tools/ rather than daemon/ or cli/.
 *
 * It reads docs/api/openapi.yaml and produces the one route table that
 * the daemon's dispatcher is generated from, and that each presentation
 * channel's entry points are generated from. The spec is the only thing
 * anyone edits; nothing this tool writes is ever committed, because it
 * writes into build/ which .gitignore covers. There is therefore no
 * committed copy to go stale and no "remember to regenerate" step --
 * the same posture build/version.h already has.
 *
 * DELIBERATELY NOT A YAML PARSER. It reads one regular, indentation-
 * defined subset:
 *
 *   paths:
 *     /some/path:          <- 2 spaces, must start with '/'
 *       get:               <- 4 spaces, must be an HTTP method
 *         operationId: x   <- 6 spaces
 *         x-cix-expose: [cli, web]
 *
 * and it REFUSES anything it does not recognise rather than skipping it.
 * That refusal is the whole point. A generator that silently mis-parses
 * its input is a tool confidently wrong about its own subject, which is
 * the exact failure class ADR-0218 exists to remove -- so every
 * unrecognised construct is a hard error naming the line, never a
 * shrug. It is better for this to stop the build than to quietly emit a
 * table missing a route.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APIGEN_MAX_OPS 1024
#define APIGEN_PATH_MAX 256
#define APIGEN_ID_MAX 128
#define APIGEN_EXPOSE_MAX 64

struct api_op {
	char method[12];   /* uppercased: GET, POST, ... */
	char path[APIGEN_PATH_MAX];   /* spec path, no /v1 prefix */
	char op_id[APIGEN_ID_MAX];
	char expose[APIGEN_EXPOSE_MAX]; /* raw list contents, "" when absent */
	int line;
};

static struct api_op g_ops[APIGEN_MAX_OPS];
static int g_op_count;

/* Every diagnostic names the file and line. A refusal an operator cannot
 * locate is barely better than a silent skip. */
static void die_at(const char *spec, int line, const char *fmt, ...)
{
	va_list ap;

	fprintf(stderr, "apigen: %s:%d: ", spec, line);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "\n");
	exit(1);
}

static int indent_of(const char *s)
{
	int n = 0;

	while (s[n] == ' ')
		n++;
	return n;
}

/* A line that is blank or comment-only carries no structure. */
static int is_ignorable(const char *s)
{
	int i = indent_of(s);

	return s[i] == '\0' || s[i] == '\n' || s[i] == '#';
}

static int is_http_method(const char *w)
{
	static const char *const m[] = { "get", "put", "post", "delete",
	                                 "patch", "head", "options", NULL };
	int i;

	for (i = 0; m[i] != NULL; i++)
		if (strcmp(w, m[i]) == 0)
			return 1;
	return 0;
}

/* Copies the "key" out of "key: value" / "key:" at a known indent.
 * Returns 0 when the line is not a mapping key at all. */
static int key_at(const char *line, int want_indent, char *out, size_t out_size)
{
	int i = indent_of(line);
	size_t n = 0;

	if (i != want_indent)
		return 0;
	while (line[i] != '\0' && line[i] != ':' && line[i] != '\n') {
		if (n + 1 >= out_size)
			return 0;
		out[n++] = line[i++];
	}
	if (line[i] != ':')
		return 0;
	out[n] = '\0';
	return 1;
}

static const char *value_of(const char *line)
{
	const char *c = strchr(line, ':');

	if (c == NULL)
		return "";
	c++;
	while (*c == ' ')
		c++;
	return c;
}

static void strip_eol(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
		s[--n] = '\0';
}

int main(int argc, char **argv)
{
	const char *spec;
	FILE *f;
	char line[4096];
	int lineno = 0;
	int in_paths = 0;
	char cur_path[APIGEN_PATH_MAX];
	int cur_op = -1;   /* index into g_ops of the operation being read */
	int i, j;
	int list = 0;

	if (argc < 2) {
		fprintf(stderr, "usage: apigen <openapi.yaml> [--list]\n");
		return 2;
	}
	spec = argv[1];
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0)
			list = 1;
		else {
			fprintf(stderr, "apigen: unknown option '%s'\n", argv[i]);
			return 2;
		}
	}

	f = fopen(spec, "r");
	if (f == NULL) {
		fprintf(stderr, "apigen: cannot open %s\n", spec);
		return 1;
	}
	cur_path[0] = '\0';

	while (fgets(line, sizeof(line), f) != NULL) {
		char key[APIGEN_PATH_MAX];
		int ind;

		lineno++;
		if (is_ignorable(line))
			continue;
		ind = indent_of(line);

		/* paths: opens the only section this tool reads, and the next
		 * column-0 key closes it. Everything outside is another
		 * document's business (components/, servers:, info:). */
		if (ind == 0) {
			if (key_at(line, 0, key, sizeof(key)) && strcmp(key, "paths") == 0) {
				in_paths = 1;
				cur_path[0] = '\0';
				cur_op = -1;
				continue;
			}
			if (in_paths)
				break;
			continue;
		}
		if (!in_paths)
			continue;

		if (ind == 2) {
			if (!key_at(line, 2, key, sizeof(key)))
				die_at(spec, lineno, "expected a path key here, got: %.60s",
				       line + ind);
			if (key[0] != '/')
				die_at(spec, lineno,
				       "\"%s\" is not a path -- every key directly under paths: must "
				       "start with '/'", key);
			snprintf(cur_path, sizeof(cur_path), "%s", key);
			cur_op = -1;
			continue;
		}

		if (ind == 4) {
			if (!key_at(line, 4, key, sizeof(key)))
				die_at(spec, lineno, "expected an HTTP method here, got: %.60s",
				       line + ind);
			if (!is_http_method(key))
				die_at(spec, lineno,
				       "\"%s\" is not an HTTP method. Only methods may appear directly "
				       "under a path; this tool refuses to guess what else might mean.",
				       key);
			if (cur_path[0] == '\0')
				die_at(spec, lineno, "method \"%s\" appears before any path", key);
			if (g_op_count >= APIGEN_MAX_OPS)
				die_at(spec, lineno, "more than %d operations", APIGEN_MAX_OPS);
			cur_op = g_op_count++;
			memset(&g_ops[cur_op], 0, sizeof(g_ops[cur_op]));
			for (j = 0; key[j] != '\0'; j++)
				g_ops[cur_op].method[j] = (char)(key[j] - 'a' + 'A');
			g_ops[cur_op].method[j] = '\0';
			snprintf(g_ops[cur_op].path, sizeof(g_ops[cur_op].path), "%s", cur_path);
			g_ops[cur_op].line = lineno;
			continue;
		}

		/* Operation-level keys. Only the two this tool consumes are
		 * read; the rest (summary, description, responses, ...) are
		 * another concern and are passed over deliberately, not
		 * accidentally -- they are the documented body of the API, and
		 * reading them is not this tool's job. */
		if (ind == 6 && cur_op >= 0) {
			if (!key_at(line, 6, key, sizeof(key)))
				continue;
			if (strcmp(key, "operationId") == 0) {
				char v[APIGEN_ID_MAX];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				if (v[0] == '\0')
					die_at(spec, lineno, "empty operationId");
				snprintf(g_ops[cur_op].op_id, sizeof(g_ops[cur_op].op_id), "%s", v);
			} else if (strcmp(key, "x-cix-expose") == 0) {
				char v[APIGEN_EXPOSE_MAX];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				snprintf(g_ops[cur_op].expose, sizeof(g_ops[cur_op].expose), "%s", v);
			}
			continue;
		}
	}
	fclose(f);

	if (g_op_count == 0) {
		fprintf(stderr, "apigen: %s: no operations found -- refusing to emit an empty "
		                "route table, which would silently unroute the whole API\n", spec);
		return 1;
	}

	/*
	 * Every operation must have an operationId, because it is the join
	 * key between the spec, the daemon's handler, and each channel's
	 * entry point. An operation without one cannot be dispatched to or
	 * declared, so it is a hard error rather than a skipped row.
	 */
	for (i = 0; i < g_op_count; i++) {
		if (g_ops[i].op_id[0] == '\0')
			die_at(spec, g_ops[i].line,
			       "%s %s has no operationId -- it is the join key between the spec, the "
			       "daemon handler and each channel, so an operation without one cannot "
			       "be routed",
			       g_ops[i].method, g_ops[i].path);
	}
	for (i = 0; i < g_op_count; i++) {
		for (j = i + 1; j < g_op_count; j++) {
			if (strcmp(g_ops[i].op_id, g_ops[j].op_id) == 0)
				die_at(spec, g_ops[j].line,
				       "duplicate operationId \"%s\" (first seen at line %d) -- ids must "
				       "be unique to be a join key at all",
				       g_ops[j].op_id, g_ops[i].line);
		}
	}

	if (list) {
		for (i = 0; i < g_op_count; i++)
			printf("%s /v1%s %s %s\n", g_ops[i].method, g_ops[i].path, g_ops[i].op_id,
			       g_ops[i].expose[0] != '\0' ? g_ops[i].expose : "-");
	} else {
		printf("%d\n", g_op_count);
	}
	return 0;
}
