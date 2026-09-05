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
#define APIGEN_MAX_QUERY 12
#define APIGEN_QNAME_MAX 48
#define APIGEN_MAX_COMP_PARAMS 128

struct api_op {
	char method[12];   /* uppercased: GET, POST, ... */
	char path[APIGEN_PATH_MAX];   /* spec path, no /v1 prefix */
	char op_id[APIGEN_ID_MAX];
	char expose[APIGEN_EXPOSE_MAX]; /* raw list contents, "" when absent */
	int rest_param;
	int line;
	/*
	 * The query parameters this operation DECLARES (#282).
	 *
	 * Emitted into the route table so the dispatcher can refuse one it
	 * was never given, rather than accepting it and running as though
	 * it had not been sent. That silence destroyed a package in an
	 * image the caller never named: DELETE /v1/pkg/htop?image=jumpbox
	 * deleted htop from the DEFAULT image and answered 204, because
	 * the router strips the query string to match segments and nothing
	 * afterwards ever looked at it again.
	 *
	 * The spec is already the authority on which routes exist
	 * (ADR-0218); this extends the identical authority to the
	 * parameters those routes accept, so an undeclared selector cannot
	 * be silently dropped anywhere in the API rather than only here.
	 */
	char query[APIGEN_MAX_QUERY][APIGEN_QNAME_MAX];
	int n_query;
};

/*
 * components/parameters entries, so a "- $ref:" in an operation's
 * parameter list can be resolved to the real parameter's name and
 * location. Read in their own pass because components: sits AFTER
 * paths: in the spec and the paths reader deliberately stops at the
 * first column-0 key that follows.
 */
struct comp_param {
	char comp[APIGEN_ID_MAX];      /* the components/parameters key */
	char name[APIGEN_QNAME_MAX];   /* its own "name:" */
	int is_query;                  /* its "in:" is query */
};

static struct comp_param g_comp_params[APIGEN_MAX_COMP_PARAMS];
static int g_comp_param_count;

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


/*
 * Emits build/generated/api_routes.h: forward declarations of every
 * op_<operationId> function plus the route table itself, in the shape
 * daemon/include/apiroute.h defines.
 *
 * The table and the op declarations are `static`, so this header is
 * included by exactly one translation unit (daemon/src/main.c). That
 * is a deliberate strengthening of ADR-0218's "link error" property
 * into a COMPILE error: a static function that is declared and
 * referenced (by the table initializer) but never defined fails the
 * build of main.c itself, naming the missing op -- no link step
 * needed, no way to satisfy it from anywhere else.
 *
 * A {param} segment becomes NULL in segs[]; op functions receive the
 * extracted values via ctx->p[0]/p[1] in spec order.
 */
/*
 * Reads components/parameters into g_comp_params (#282).
 *
 * Deliberately its own scan of the file rather than a branch inside the
 * paths reader: that reader breaks out at the first column-0 key after
 * paths:, which is exactly where components: begins, and widening it to
 * stay resident would make it responsible for a second grammar. Two
 * small passes over a file this tool already reads once are cheaper
 * than one reader that has to know where it is.
 */
static void load_component_params(const char *spec)
{
	FILE *f = fopen(spec, "r");
	char line[4096];
	int in_components = 0, in_params = 0, cur = -1;

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char key[APIGEN_PATH_MAX];
		int ind;

		if (is_ignorable(line))
			continue;
		ind = indent_of(line);
		if (ind == 0) {
			in_components = key_at(line, 0, key, sizeof(key)) &&
			                strcmp(key, "components") == 0;
			in_params = 0;
			cur = -1;
			continue;
		}
		if (!in_components)
			continue;
		if (ind == 2) {
			in_params = key_at(line, 2, key, sizeof(key)) &&
			            strcmp(key, "parameters") == 0;
			cur = -1;
			continue;
		}
		if (!in_params)
			continue;
		if (ind == 4) {
			if (!key_at(line, 4, key, sizeof(key)))
				continue;
			if (g_comp_param_count >= APIGEN_MAX_COMP_PARAMS) {
				cur = -1;
				continue;
			}
			cur = g_comp_param_count++;
			memset(&g_comp_params[cur], 0, sizeof(g_comp_params[cur]));
			snprintf(g_comp_params[cur].comp, sizeof(g_comp_params[cur].comp), "%s", key);
			continue;
		}
		if (ind == 6 && cur >= 0 && key_at(line, 6, key, sizeof(key))) {
			char v[APIGEN_QNAME_MAX];

			snprintf(v, sizeof(v), "%s", value_of(line));
			strip_eol(v);
			if (strcmp(key, "name") == 0)
				snprintf(g_comp_params[cur].name, sizeof(g_comp_params[cur].name), "%s", v);
			else if (strcmp(key, "in") == 0 && strcmp(v, "query") == 0)
				g_comp_params[cur].is_query = 1;
		}
	}
	fclose(f);
}

/* Adds one declared query parameter to an operation, ignoring repeats. */
static void op_add_query(int op, const char *name, const char *spec, int lineno)
{
	int i;

	if (op < 0 || name == NULL || name[0] == '\0')
		return;
	for (i = 0; i < g_ops[op].n_query; i++)
		if (strcmp(g_ops[op].query[i], name) == 0)
			return;
	if (g_ops[op].n_query >= APIGEN_MAX_QUERY)
		die_at(spec, lineno, "more than %d query parameters on one operation",
		       APIGEN_MAX_QUERY);
	snprintf(g_ops[op].query[g_ops[op].n_query], APIGEN_QNAME_MAX, "%s", name);
	g_ops[op].n_query++;
}

/*
 * One entry of an operation's "parameters:" list, accumulated across
 * the lines that describe it and committed when the next entry starts
 * or the list ends -- "name:" and "in:" may appear in either order, so
 * neither can be acted on alone.
 */
static char g_item_name[APIGEN_QNAME_MAX];
static int g_item_is_query;
static int g_in_params;

static void param_item_flush(int op, const char *spec, int lineno)
{
	if (g_item_is_query)
		op_add_query(op, g_item_name, spec, lineno);
	g_item_name[0] = '\0';
	g_item_is_query = 0;
}

/*
 * "- $ref: \"#/components/parameters/Foo\"" -- takes the component key
 * and, if that component is a query parameter, declares its real name.
 */
static void param_ref_resolve(int op, const char *text, const char *spec, int lineno)
{
	const char *q = strchr(text, '#');
	char comp[APIGEN_ID_MAX];
	size_t n = 0;
	int i;

	if (q == NULL)
		return;
	q = strrchr(q, '/');
	if (q == NULL)
		return;
	q++;
	while (*q != '\0' && *q != '"' && *q != '\'' && *q != ' ' && *q != '\r' && *q != '\n' &&
	       n + 1 < sizeof(comp))
		comp[n++] = *q++;
	comp[n] = '\0';
	for (i = 0; i < g_comp_param_count; i++)
		if (strcmp(g_comp_params[i].comp, comp) == 0) {
			if (g_comp_params[i].is_query)
				op_add_query(op, g_comp_params[i].name, spec, lineno);
			return;
		}
}

static void emit_routes(const char *out_path, const char *spec)
{
	FILE *o = fopen(out_path, "w");
	int i;

	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o,
	        "/*\n"
	        " * GENERATED by tools/apigen.c from %s -- DO NOT EDIT.\n"
	        " *\n"
	        " * Regenerated on every build; lives under build/ so it can never\n"
	        " * be committed (ADR-0218). To change a route, change the spec.\n"
	        " */\n\n",
	        spec);
	for (i = 0; i < g_op_count; i++)
		fprintf(o, "static void op_%s(const struct api_ctx *ctx);\n", g_ops[i].op_id);
	/*
	 * One array per operation that declares query parameters (#282).
	 * The dispatcher refuses any query parameter absent from its
	 * operation's array, so the spec decides what the API accepts
	 * rather than each handler deciding what it happens to read.
	 */
	fprintf(o, "\n");
	for (i = 0; i < g_op_count; i++) {
		int q;

		if (g_ops[i].n_query == 0)
			continue;
		fprintf(o, "static const char *const q_%s[] = { ", g_ops[i].op_id);
		for (q = 0; q < g_ops[i].n_query; q++)
			fprintf(o, "\"%s\", ", g_ops[i].query[q]);
		fprintf(o, "};\n");
	}
	fprintf(o, "\nstatic const struct api_route g_api_routes[] = {\n");
	for (i = 0; i < g_op_count; i++) {
		char full[APIGEN_PATH_MAX + 8];
		char *seg, *save = NULL;
		int n_segs = 0;

		fprintf(o, "\t{ \"%s\", ", g_ops[i].method);
		snprintf(full, sizeof(full), "v1%s", g_ops[i].path);
		/* count first (n_segs precedes segs in the struct) */
		{
			char tmp[APIGEN_PATH_MAX + 8];
			snprintf(tmp, sizeof(tmp), "%s", full);
			for (seg = strtok_r(tmp, "/", &save); seg != NULL;
			     seg = strtok_r(NULL, "/", &save))
				n_segs++;
		}
		if (n_segs > 8) {
			fprintf(stderr, "apigen: %s has %d segments, apiroute.h caps at 8\n",
			        g_ops[i].path, n_segs);
			exit(1);
		}
		fprintf(o, "%d, { ", n_segs);
		save = NULL;
		for (seg = strtok_r(full, "/", &save); seg != NULL;
		     seg = strtok_r(NULL, "/", &save)) {
			size_t sl = strlen(seg);

			if (seg[0] == '{' && sl > 1 && seg[sl - 1] == '}')
				fprintf(o, "NULL, ");
			else
				fprintf(o, "\"%s\", ", seg);
		}
		if (g_ops[i].n_query > 0)
			fprintf(o, "}, op_%s, \"%s\", %d, q_%s, %d },\n", g_ops[i].op_id,
			        g_ops[i].op_id, g_ops[i].rest_param, g_ops[i].op_id,
			        g_ops[i].n_query);
		else
			fprintf(o, "}, op_%s, \"%s\", %d, NULL, 0 },\n", g_ops[i].op_id,
			        g_ops[i].op_id, g_ops[i].rest_param);
	}
	fprintf(o, "};\n");
	fclose(o);
}

/*
 * Emits build/generated/cix_api.h: one path constant per operation, for
 * the CLI (ADR-0218 layer 1).
 *
 * The point is not convenience, it is that a channel cannot invent,
 * misspell or drift a path. A hand-typed "/v1/containters/%s" compiles
 * and fails at runtime; CIX_API_getContainer either exists or the
 * build stops. Every path parameter becomes a %s, in spec order, so
 * the existing snprintf() call sites keep their shape -- this is a
 * substitution of the string, not a new calling convention, which is
 * what makes converting ~250 sites a mechanical, compiler-checked
 * edit rather than a rewrite.
 *
 * The method is emitted alongside because it is half of the route's
 * identity: a call site that names the operation should not be free to
 * pick a different verb than the contract declares.
 */
static void emit_cli(const char *out_path, const char *spec)
{
	FILE *o = fopen(out_path, "w");
	int i;

	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o,
	        "/*\n"
	        " * GENERATED by tools/apigen.c from %s -- DO NOT EDIT.\n"
	        " *\n"
	        " * One entry per API operation. Regenerated on every build and\n"
	        " * emitted under build/, so it can never be committed (ADR-0218).\n"
	        " * To change a path, change the spec.\n"
	        " *\n"
	        " * CIX_API_<operationId>        the path, with %%s per parameter\n"
	        " * CIX_API_<operationId>_METHOD the verb the contract declares\n"
	        " */\n"
	        "#ifndef CIX_GENERATED_API_H\n"
	        "#define CIX_GENERATED_API_H\n\n",
	        spec);
	for (i = 0; i < g_op_count; i++) {
		const char *c = g_ops[i].path;
		int in_param = 0;

		fprintf(o, "#define CIX_API_%s \"/v1", g_ops[i].op_id);
		for (; *c != '\0'; c++) {
			if (*c == '{') {
				in_param = 1;
				fputs("%s", o);
				continue;
			}
			if (*c == '}') {
				in_param = 0;
				continue;
			}
			if (!in_param)
				fputc(*c, o);
		}
		fprintf(o, "\"\n#define CIX_API_%s_METHOD \"%s\"\n", g_ops[i].op_id,
		        g_ops[i].method);
	}
	fprintf(o, "\n#endif /* CIX_GENERATED_API_H */\n");
	fclose(o);
}

/*
 * Emits the dashboard's API map (ADR-0218 layer 1, web side).
 *
 * A plain script defining one global, loaded before app.js, because
 * that is what the dashboard already is -- no module system, no build
 * step of its own. Each operation becomes a function returning its
 * path, so a parameterised call reads CIX_API.getContainer(name)
 * instead of "/v1/containers/" + encodeURIComponent(name).
 *
 * The parameters are encodeURIComponent()'d here rather than at ~226
 * call sites. That is not tidiness: the old hand-written calls did it
 * inconsistently, and a container name is operator-supplied text.
 *
 * Unlike the C header this cannot live only under build/ -- the
 * dashboard is SERVED, so the file has to reach the web root. It is
 * still never committed: the cix recipe copies it out of the build
 * tree at install time, alongside web/*.
 */
static void emit_web(const char *out_path, const char *spec)
{
	FILE *o = fopen(out_path, "w");
	int i;

	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o,
	        "/*\n"
	        " * GENERATED by tools/apigen.c from %s -- DO NOT EDIT.\n"
	        " *\n"
	        " * Regenerated on every build and never committed (ADR-0218);\n"
	        " * the cix recipe copies it into the web root at install time.\n"
	        " * To change a path, change the spec.\n"
	        " */\n"
	        "const CIX_API = {\n",
	        spec);
	for (i = 0; i < g_op_count; i++) {
		const char *c;
		int np = 0, seen = 0;

		for (c = g_ops[i].path; *c != '\0'; c++)
			if (*c == '{')
				np++;
		fprintf(o, "\t%s: (", g_ops[i].op_id);
		for (c = NULL, seen = 0; seen < np; seen++)
			fprintf(o, "%sp%d", seen ? ", " : "", seen);
		fprintf(o, ") => `/v1");
		seen = 0;
		for (c = g_ops[i].path; *c != '\0'; c++) {
			if (*c == '{') {
				fprintf(o, "${encodeURIComponent(p%d)}", seen++);
				while (*c != '\0' && *c != '}')
					c++;
				continue;
			}
			fputc(*c, o);
		}
		fprintf(o, "`,\n");
		fprintf(o, "\t%s_METHOD: \"%s\",\n", g_ops[i].op_id, g_ops[i].method);
	}
	fprintf(o, "};\n");
	fclose(o);
}

/*
 * The config section vocabulary, generated from the ConfigDocument
 * schema (ADR-0206).
 *
 * ADR-0206's fifth point is the owner's hard requirement on the whole
 * design: the schema generates the vocabulary, and nothing hand-
 * maintains a second copy of it. Agreement between two hand-maintained
 * lists is exactly what drifts, and every drift this project has
 * suffered has that shape.
 *
 * So this emits an X-macro of the section names, IN SCHEMA ORDER,
 * which daemon/src/config.c expands to build its renderer table. A
 * section in the schema with no renderer fails to compile; a renderer
 * with no schema entry has nothing to expand from and is equally a
 * build error. That is the enforcement the ADR asks for -- adding a
 * subsystem and forgetting fails the build rather than shipping a
 * quietly partial document.
 *
 * Its own pass over the file, because main()'s parser deliberately
 * stops at the first column-0 key after paths: and components: is
 * exactly that. Same posture as the rest of this tool: anything
 * unrecognised inside the block is a hard error naming the line.
 */
static void emit_config_sections(const char *out_path, const char *spec)
{
	FILE *f = fopen(spec, "r");
	FILE *o;
	char line[4096];
	char names[256][APIGEN_ID_MAX];
	int count = 0;
	int lineno = 0;
	int in_doc = 0, in_props = 0;
	int i;

	if (f == NULL) {
		fprintf(stderr, "apigen: cannot reopen %s\n", spec);
		exit(1);
	}
	while (fgets(line, sizeof(line), f) != NULL) {
		char key[APIGEN_PATH_MAX];
		int ind;

		lineno++;
		if (is_ignorable(line))
			continue;
		ind = indent_of(line);
		if (!in_doc) {
			if (ind == 4 && key_at(line, 4, key, sizeof(key)) &&
			    strcmp(key, "ConfigDocument") == 0)
				in_doc = 1;
			continue;
		}
		/* The schema ends where the next same-level key begins. */
		if (ind <= 4)
			break;
		if (!in_props) {
			if (ind == 6 && key_at(line, 6, key, sizeof(key)) &&
			    strcmp(key, "properties") == 0)
				in_props = 1;
			continue;
		}
		if (ind != 8)
			continue; /* the description under each section */
		if (!key_at(line, 8, key, sizeof(key)))
			die_at(spec, lineno,
			       "expected a config section name here, got: %.60s", line + ind);
		for (i = 0; i < count; i++) {
			if (strcmp(names[i], key) == 0)
				die_at(spec, lineno,
				       "config section \"%s\" is listed twice -- the section "
				       "vocabulary must be a set", key);
		}
		if (count >= (int)(sizeof(names) / sizeof(names[0])))
			die_at(spec, lineno, "too many config sections");
		snprintf(names[count], sizeof(names[count]), "%s", key);
		count++;
	}
	fclose(f);

	if (!in_doc)
		die_at(spec, 0, "no ConfigDocument schema found -- ADR-0206 requires it to "
		                "exist, since it is the source of the config vocabulary");
	if (count == 0)
		die_at(spec, 0, "ConfigDocument has no properties -- a config document with "
		                "no sections would be a silently empty view");

	o = fopen(out_path, "w");
	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o, "/* Generated by tools/apigen.c from %s -- do not edit. */\n", spec);
	fprintf(o, "#ifndef CIX_GENERATED_CONFIG_SECTIONS_H\n");
	fprintf(o, "#define CIX_GENERATED_CONFIG_SECTIONS_H\n\n");
	fprintf(o, "/* Section order is the contract: a section never depends on one\n");
	fprintf(o, " * below it. See the ConfigDocument schema for why. */\n");
	fprintf(o, "#define CIX_CONFIG_SECTIONS(X) \\\n");
	for (i = 0; i < count; i++)
		fprintf(o, "\tX(%s)%s\n", names[i], i + 1 < count ? " \\" : "");
	fprintf(o, "\n#define CIX_CONFIG_SECTION_COUNT %d\n\n", count);
	fprintf(o, "#endif\n");
	fclose(o);
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
	const char *emit_path = NULL;
	const char *emit_cli_path = NULL;
	const char *emit_web_path = NULL;
	const char *emit_config_path = NULL;

	if (argc < 2) {
		fprintf(stderr, "usage: apigen <openapi.yaml> "
		                "[--list | --emit-routes <out.h> | --emit-cli <out.h> | "
		                "--emit-web <out.js> | --emit-config-sections <out.h>]\n");
		return 2;
	}
	spec = argv[1];
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0)
			list = 1;
		else if (strcmp(argv[i], "--emit-routes") == 0 && i + 1 < argc)
			emit_path = argv[++i];
		else if (strcmp(argv[i], "--emit-cli") == 0 && i + 1 < argc)
			emit_cli_path = argv[++i];
		else if (strcmp(argv[i], "--emit-web") == 0 && i + 1 < argc)
			emit_web_path = argv[++i];
		else if (strcmp(argv[i], "--emit-config-sections") == 0 && i + 1 < argc)
			emit_config_path = argv[++i];
		else {
			fprintf(stderr, "apigen: unknown option '%s'\n", argv[i]);
			return 2;
		}
	}

	/* Parameters referenced by $ref must be known before the paths are
	 * read, and components: comes after paths: in the file (#282). */
	load_component_params(spec);

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
			param_item_flush(cur_op, spec, lineno);
			g_in_params = 0;
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
			param_item_flush(cur_op, spec, lineno);
			g_in_params = 0;
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
			/* Any operation-level key ends the parameter list that
			 * may have preceded it (#282). */
			param_item_flush(cur_op, spec, lineno);
			g_in_params = strcmp(key, "parameters") == 0;
			if (strcmp(key, "operationId") == 0) {
				char v[APIGEN_ID_MAX];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				if (v[0] == '\0')
					die_at(spec, lineno, "empty operationId");
				snprintf(g_ops[cur_op].op_id, sizeof(g_ops[cur_op].op_id), "%s", v);
			} else if (strcmp(key, "x-cix-rest-param") == 0) {
				char v[16];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				if (strcmp(v, "true") != 0)
					die_at(spec, lineno,
					       "x-cix-rest-param only takes the value true -- omit it "
					       "entirely for an ordinary single-segment parameter");
				g_ops[cur_op].rest_param = 1;
			} else if (strcmp(key, "x-cix-expose") == 0) {
				char v[APIGEN_EXPOSE_MAX];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				/* Whitespace-free, so a consumer can treat the value as
				 * one token: "[cli, web]" and "[cli,web]" mean the same
				 * thing and must not read differently downstream. */
				{
					char *w = v;
					char *r = v;

					while (*r != '\0') {
						if (*r != ' ' && *r != '\t')
							*w++ = *r;
						r++;
					}
					*w = '\0';
				}
				snprintf(g_ops[cur_op].expose, sizeof(g_ops[cur_op].expose), "%s", v);
			}
			continue;
		}

		/*
		 * The parameter list itself (#282): entries at 8, their own
		 * keys at 10. Only the declared NAMES are wanted here --
		 * schema, description and required are the contract's
		 * business and not this tool's.
		 */
		if (g_in_params && cur_op >= 0 && (ind == 8 || ind == 10)) {
			const char *text = line + ind;

			if (ind == 8) {
				param_item_flush(cur_op, spec, lineno);
				if (text[0] != '-')
					continue;
				text++;
				while (*text == ' ')
					text++;
				if (strncmp(text, "$ref:", 5) == 0) {
					param_ref_resolve(cur_op, text, spec, lineno);
					continue;
				}
			}
			{
				char k[APIGEN_QNAME_MAX];
				const char *colon = strchr(text, ':');
				size_t kl;

				if (colon == NULL)
					continue;
				kl = (size_t)(colon - text);
				if (kl == 0 || kl + 1 >= sizeof(k))
					continue;
				memcpy(k, text, kl);
				k[kl] = '\0';
				{
					char v[APIGEN_QNAME_MAX];

					snprintf(v, sizeof(v), "%s", value_of(colon));
					strip_eol(v);
					if (strcmp(k, "name") == 0)
						snprintf(g_item_name, sizeof(g_item_name), "%s", v);
					else if (strcmp(k, "in") == 0 && strcmp(v, "query") == 0)
						g_item_is_query = 1;
				}
			}
			continue;
		}
	}
	param_item_flush(cur_op, spec, lineno);
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

	if (emit_path != NULL)
		emit_routes(emit_path, spec);
	if (emit_cli_path != NULL)
		emit_cli(emit_cli_path, spec);
	if (emit_web_path != NULL)
		emit_web(emit_web_path, spec);
	if (emit_config_path != NULL)
		emit_config_sections(emit_config_path, spec);
	if (list) {
		for (i = 0; i < g_op_count; i++)
			printf("%s /v1%s %s %s\n", g_ops[i].method, g_ops[i].path, g_ops[i].op_id,
			       g_ops[i].expose[0] != '\0' ? g_ops[i].expose : "-");
	} else {
		printf("%d\n", g_op_count);
	}
	return 0;
}
