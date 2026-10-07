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
#define APIGEN_MAX_PERMS 64
#define APIGEN_PERM_MAX 48
#define APIGEN_MAX_COMP_SCHEMAS 512
#define APIGEN_MAX_REQUIRED 24
#define APIGEN_MAX_ARRAY_PROPS 8

struct api_op {
	char method[12];   /* uppercased: GET, POST, ... */
	char path[APIGEN_PATH_MAX];   /* spec path, no /v1 prefix */
	char op_id[APIGEN_ID_MAX];
	char expose[APIGEN_EXPOSE_MAX]; /* raw list contents, "" when absent */
	int rest_param;
	/* ADR-0317 (#539): the one permission this operation requires,
	 * from x-cix-permission; "" until read, and a build failure if
	 * it is still "" once the spec has been read. */
	char permission[APIGEN_PERM_MAX];
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
	/*
	 * The shape of this operation's 200 JSON response (#575).
	 *
	 * ADR-0218 made a route the spec does not declare unroutable, and
	 * nothing did the same for a response BODY: a handler could stop
	 * sending a field the spec marks `required`, or rename it, and
	 * every gate stayed green. That is cix#574 -- the source
	 * catalogue moved from a `state` string plus four counts to a
	 * `stage`/`status` pair plus four differently-named counts, the
	 * spec kept declaring the old shape as required, and the result
	 * was a blank verdict on 172 rows.
	 *
	 * resp_schema is a components/schemas key; resp_required holds an
	 * INLINE schema's own required list. One or the other, never
	 * both, and both empty for the forms this tool deliberately does
	 * not read (a bare array, allOf, a string). That silence is
	 * reported as a COUNT by the test that consumes this, so
	 * under-coverage is a visible number rather than a quiet pass --
	 * the route table must be complete because a missing route is
	 * unroutable, but a shape it cannot read simply contributes no
	 * assertion.
	 */
	/*
	 * The property NAMES an inline 200 schema declares, as ",a,b,c,"
	 * so a membership test is one strstr().
	 *
	 * Only for the self-consistency check below: a schema whose
	 * `required` names a property it does not declare is a contract
	 * that contradicts itself, and the spec had exactly one --
	 * `listStorage` promised `disks` while declaring `storage`, which
	 * is what the daemon has always sent. Found by sweeping all 82
	 * gateable GETs against 192.168.15.95 before this gate had ever
	 * been compiled; nothing else in 339 operations or 144 component
	 * schemas has it, so refusing is affordable and permanent.
	 */
	char resp_props[1024];
	char resp_schema[APIGEN_ID_MAX];
	char resp_required[APIGEN_MAX_REQUIRED][APIGEN_QNAME_MAX];
	int n_resp_required;
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

/*
 * ADR-0317 (#539): the closed permission vocabulary, read from the
 * spec's own top-level `x-cix-permissions:` list -- the contract
 * declares both the words and which operation needs which, so there
 * is no second copy of the list anywhere to drift from it.
 *
 * A word is `public`, `authenticated`, or `<area>:<verb>` in lowercase
 * letters and hyphens. An empty list, a malformed word or a duplicate
 * is a build failure: every operation's annotation is checked against
 * this, so a sloppy vocabulary would make that check meaningless.
 */
static char g_perms[APIGEN_MAX_PERMS][APIGEN_PERM_MAX];
static int g_perm_count;

static int perm_word_is_valid(const char *w)
{
	const char *colon = strchr(w, ':');
	const char *p;

	if (strcmp(w, "public") == 0 || strcmp(w, "authenticated") == 0)
		return 1;
	if (colon == NULL || colon == w || colon[1] == '\0' || strchr(colon + 1, ':') != NULL)
		return 0;
	for (p = w; *p != '\0'; p++) {
		if (p == colon)
			continue;
		if (!((*p >= 'a' && *p <= 'z') || *p == '-'))
			return 0;
	}
	return 1;
}

/*
 * components/schemas, reduced to the one question #575 asks: which keys
 * does a response of this shape PROMISE?
 *
 * Its own pass, for the same reason load_component_params() has one --
 * components: sits after paths: and the paths reader stops at the first
 * column-0 key that follows.
 *
 * Reads a narrow, named subset and is silent about the rest, which is a
 * different posture from the route reader directly above and worth
 * saying why. A route this tool fails to read is UNROUTABLE, so
 * refusing an unrecognised construct is the only safe answer. A schema
 * it fails to read costs an assertion, not a route -- and the schema
 * grammar really does carry allOf, oneOf, nested objects and
 * discriminators that this tool has no business modelling. So it takes
 * `required: [...]` and array properties, leaves everything else, and
 * the consuming test reports how many operations it could gate. An
 * honest number beats a tool pretending to understand OpenAPI.
 */
struct comp_schema {
	char comp[APIGEN_ID_MAX];
	char required[APIGEN_MAX_REQUIRED][APIGEN_QNAME_MAX];
	int n_required;
	int line;                  /* where the schema key is, for a refusal */
	/* Its property names as ",a,b,c," -- see api_op.resp_props. */
	char props[2048];
	/*
	 * A property that is an array whose items name another schema, and
	 * that schema's key -- ONE level deep.
	 *
	 * One level because that is where the half of cix#574 that an
	 * operator actually saw lived: `SourceCatalogue.packages[]` holds
	 * `SourceCatalogueEntry`, whose `stage`/`status` were the blank
	 * columns. A general recursive walk would need the rest of the
	 * grammar this pass just declined to model.
	 */
	struct {
		char prop[APIGEN_QNAME_MAX];
		char items[APIGEN_ID_MAX];
	} arr[APIGEN_MAX_ARRAY_PROPS];
	int n_arr;
};

static struct comp_schema g_comp_schemas[APIGEN_MAX_COMP_SCHEMAS];
static int g_comp_schema_count;

/* The last path segment of a "#/components/schemas/Name" reference. */
static void ref_tail(const char *text, char *out, size_t out_size)
{
	const char *q = strchr(text, '#');
	size_t n = 0;

	out[0] = '\0';
	if (q == NULL)
		return;
	q = strrchr(q, '/');
	if (q == NULL)
		return;
	q++;
	while (*q != '\0' && *q != '"' && *q != '\'' && *q != ' ' && *q != '\r' && *q != '\n' &&
	       n + 1 < out_size)
		out[n++] = *q++;
	out[n] = '\0';
}

/*
 * `required: [a, b, c]` into out[], returning how many landed.
 *
 * The flow form is the only one the spec uses -- 131 of them, and zero
 * block lists (measured 2026-10-07). A block list would read as zero
 * entries here, which costs assertions rather than correctness, and is
 * why the consuming test prints its coverage.
 */
static int parse_inline_list(const char *v, char out[][APIGEN_QNAME_MAX], int max)
{
	const char *c = v;
	int n = 0;

	while (*c == ' ')
		c++;
	if (*c != '[')
		return 0;
	c++;
	while (*c != '\0' && *c != ']' && n < max) {
		size_t k = 0;

		while (*c == ' ' || *c == ',')
			c++;
		while (*c != '\0' && *c != ',' && *c != ']' && *c != ' ' &&
		       k + 1 < (size_t)APIGEN_QNAME_MAX)
			out[n][k++] = *c++;
		out[n][k] = '\0';
		if (k > 0)
			n++;
		while (*c != '\0' && *c != ',' && *c != ']')
			c++;
	}
	return n;
}

static void load_component_schemas(const char *spec)
{
	FILE *f = fopen(spec, "r");
	char line[4096];
	int in_components = 0, in_schemas = 0, cur = -1;
	int lineno = 0;
	int in_props = 0;
	char prop[APIGEN_QNAME_MAX] = "";
	int prop_is_array = 0;

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char key[APIGEN_PATH_MAX];
		int ind;

		lineno++;
		if (is_ignorable(line))
			continue;
		ind = indent_of(line);
		if (ind == 0) {
			in_components = key_at(line, 0, key, sizeof(key)) &&
			                strcmp(key, "components") == 0;
			in_schemas = 0;
			cur = -1;
			in_props = 0;
			continue;
		}
		if (!in_components)
			continue;
		if (ind == 2) {
			in_schemas = key_at(line, 2, key, sizeof(key)) &&
			             strcmp(key, "schemas") == 0;
			cur = -1;
			in_props = 0;
			continue;
		}
		if (!in_schemas)
			continue;
		if (ind == 4) {
			in_props = 0;
			prop[0] = '\0';
			if (!key_at(line, 4, key, sizeof(key))) {
				cur = -1;
				continue;
			}
			if (g_comp_schema_count >= APIGEN_MAX_COMP_SCHEMAS)
				die_at(spec, 0, "more than %d component schemas",
				       APIGEN_MAX_COMP_SCHEMAS);
			cur = g_comp_schema_count++;
			memset(&g_comp_schemas[cur], 0, sizeof(g_comp_schemas[cur]));
			snprintf(g_comp_schemas[cur].comp, sizeof(g_comp_schemas[cur].comp), "%s", key);
			g_comp_schemas[cur].line = lineno;
			continue;
		}
		if (cur < 0)
			continue;
		if (ind == 6 && key_at(line, 6, key, sizeof(key))) {
			in_props = strcmp(key, "properties") == 0;
			prop[0] = '\0';
			if (strcmp(key, "required") == 0) {
				char v[1024];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				g_comp_schemas[cur].n_required =
				    parse_inline_list(v, g_comp_schemas[cur].required,
				                      APIGEN_MAX_REQUIRED);
			}
			continue;
		}
		if (!in_props)
			continue;
		if (ind == 8) {
			prop_is_array = 0;
			if (!key_at(line, 8, prop, sizeof(prop)))
				prop[0] = '\0';
			else {
				size_t used = strlen(g_comp_schemas[cur].props);

				if (used + strlen(prop) + 2 >= sizeof(g_comp_schemas[cur].props))
					die_at(spec, 0,
					       "schema %s declares more properties than this tool can "
					       "record; raise props rather than let the self-consistency "
					       "check go unsound",
					       g_comp_schemas[cur].comp);
				snprintf(g_comp_schemas[cur].props + used,
				         sizeof(g_comp_schemas[cur].props) - used, "%s,", prop);
			}
			continue;
		}
		if (prop[0] == '\0')
			continue;
		if (ind == 10 && key_at(line, 10, key, sizeof(key)) && strcmp(key, "type") == 0) {
			char v[32];

			snprintf(v, sizeof(v), "%s", value_of(line));
			strip_eol(v);
			prop_is_array = strcmp(v, "array") == 0;
			continue;
		}
		/* items: at 10, its $ref: at 12 -- the only nesting this pass
		 * follows, and only for a property it has just seen typed as
		 * an array. */
		if (ind == 12 && prop_is_array && key_at(line, 12, key, sizeof(key)) &&
		    strcmp(key, "$ref") == 0) {
			char items[APIGEN_ID_MAX];
			int a;

			ref_tail(value_of(line), items, sizeof(items));
			if (items[0] == '\0')
				continue;
			if (g_comp_schemas[cur].n_arr >= APIGEN_MAX_ARRAY_PROPS)
				continue;
			a = g_comp_schemas[cur].n_arr++;
			snprintf(g_comp_schemas[cur].arr[a].prop,
			         sizeof(g_comp_schemas[cur].arr[a].prop), "%s", prop);
			snprintf(g_comp_schemas[cur].arr[a].items,
			         sizeof(g_comp_schemas[cur].arr[a].items), "%s", items);
			prop_is_array = 0;
		}
	}
	fclose(f);
}

static const struct comp_schema *comp_schema_find(const char *comp)
{
	int i;

	for (i = 0; i < g_comp_schema_count; i++)
		if (strcmp(g_comp_schemas[i].comp, comp) == 0)
			return &g_comp_schemas[i];
	return NULL;
}

static int perm_is_known(const char *w)
{
	int i;

	for (i = 0; i < g_perm_count; i++) {
		if (strcmp(g_perms[i], w) == 0)
			return 1;
	}
	return 0;
}

static void load_permission_vocabulary(const char *spec)
{
	FILE *f = fopen(spec, "r");
	char line[4096];
	int lineno = 0;
	int in_vocab = 0;
	int seen = 0;

	if (f == NULL)
		return;
	while (fgets(line, sizeof(line), f) != NULL) {
		char key[APIGEN_PATH_MAX];
		char w[APIGEN_PERM_MAX];
		const char *text;
		int ind;

		lineno++;
		if (is_ignorable(line))
			continue;
		ind = indent_of(line);
		if (ind == 0) {
			in_vocab = key_at(line, 0, key, sizeof(key)) &&
			           strcmp(key, "x-cix-permissions") == 0;
			seen |= in_vocab;
			continue;
		}
		if (!in_vocab)
			continue;
		text = line + ind;
		if (ind != 2 || text[0] != '-')
			die_at(spec, lineno, "x-cix-permissions holds a list of words, one \"- word\" per line");
		text++;
		while (*text == ' ')
			text++;
		snprintf(w, sizeof(w), "%s", text);
		strip_eol(w);
		if (!perm_word_is_valid(w))
			die_at(spec, lineno,
			       "\"%s\" is not a permission -- a permission is public, authenticated, "
			       "or <area>:<verb> in lowercase letters and hyphens (ADR-0317)",
			       w);
		if (perm_is_known(w))
			die_at(spec, lineno, "permission \"%s\" is listed twice", w);
		if (g_perm_count >= APIGEN_MAX_PERMS)
			die_at(spec, lineno, "more than %d permissions", APIGEN_MAX_PERMS);
		snprintf(g_perms[g_perm_count++], APIGEN_PERM_MAX, "%s", w);
	}
	fclose(f);
	if (!seen || g_perm_count == 0) {
		fprintf(stderr,
		        "apigen: %s declares no x-cix-permissions vocabulary -- every operation's "
		        "x-cix-permission is checked against it (ADR-0317)\n",
		        spec);
		exit(1);
	}
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

/* Where the response-shape reader is: inside an operation's `responses:`,
 * and inside its `"200":` (#575). */
static int g_in_responses;
static int g_in_200;
/* Inside an inline 200 schema's own `properties:` (#575). */
static int g_in_resp_props;

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
			fprintf(o, "}, op_%s, \"%s\", %d, q_%s, %d, \"%s\" },\n", g_ops[i].op_id,
			        g_ops[i].op_id, g_ops[i].rest_param, g_ops[i].op_id,
			        g_ops[i].n_query, g_ops[i].permission);
		else
			fprintf(o, "}, op_%s, \"%s\", %d, NULL, 0, \"%s\" },\n", g_ops[i].op_id,
			        g_ops[i].op_id, g_ops[i].rest_param, g_ops[i].permission);
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
 * Emits build/generated/api_shapes.h: the keys each operation's 200
 * response PROMISES, as a table something can walk (#575).
 *
 * A table and not a #define per operation, which is what emit_cli()
 * writes, because the consumer is a loop: a test that calls every
 * parameterless GET and compares the keys that came back against the
 * keys the contract declares required. A header of 339 #defines cannot
 * be iterated, and a hand-written list of endpoints to check is the
 * second source of truth this whole generator exists to remove.
 *
 * `required` is NULL when the contract declares none and when the
 * schema is a form this tool deliberately does not read. Those are
 * different facts to a reader and the same fact to the gate -- nothing
 * to assert -- so they are not distinguished here. What matters is that
 * the consumer reports how many operations it DID gate, which is the
 * number that goes stale visibly if the spec drifts towards shapes this
 * cannot see.
 */
static void emit_shapes(const char *out_path, const char *spec)
{
	FILE *o = fopen(out_path, "w");
	int i, j, k;

	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o,
	        "/*\n"
	        " * GENERATED by tools/apigen.c from %s -- DO NOT EDIT.\n"
	        " *\n"
	        " * One entry per API operation: its path, method, how many path\n"
	        " * parameters it takes, the keys its 200 response declares required,\n"
	        " * and the same for the items of every array property that declares\n"
	        " * any (#575). Regenerated on every build and emitted under build/,\n"
	        " * so it can never be committed (ADR-0218).\n"
	        " */\n"
	        "#ifndef CIX_GENERATED_API_SHAPES_H\n"
	        "#define CIX_GENERATED_API_SHAPES_H\n\n"
	        "struct cix_api_shape_array {\n"
	        "\tconst char *prop;                 /* NULL terminates the list */\n"
	        "\tconst char *const *required;      /* NULL-terminated */\n"
	        "};\n\n"
	        "struct cix_api_shape {\n"
	        "\tconst char *op_id;\n"
	        "\tconst char *path;    /* with %%s per path parameter */\n"
	        "\tconst char *path_raw; /* as the contract spells it, {name} and all */\n"
	        "\tconst char *method;\n"
	        "\tint n_params;\n"
	        "\tconst char *const *required;             /* NULL-terminated, or NULL */\n"
	        "\tconst struct cix_api_shape_array *arrays; /* prop==NULL terminated, or NULL */\n"
	        "};\n\n",
	        spec);
	/*
	 * Named arrays first: a C initializer cannot hold an anonymous one
	 * with static storage duration.
	 */
	for (i = 0; i < g_op_count; i++) {
		const struct comp_schema *cs = NULL;
		int n_req, n_emitted = 0;

		if (g_ops[i].resp_schema[0] != '\0')
			cs = comp_schema_find(g_ops[i].resp_schema);
		n_req = cs != NULL ? cs->n_required : g_ops[i].n_resp_required;
		if (n_req > 0) {
			fprintf(o, "static const char *const shape_req_%s[] = { ", g_ops[i].op_id);
			for (j = 0; j < n_req; j++)
				fprintf(o, "\"%s\", ",
				        cs != NULL ? cs->required[j] : g_ops[i].resp_required[j]);
			fprintf(o, "NULL };\n");
		}
		if (cs == NULL)
			continue;
		/*
		 * EVERY array property whose items declare keys, not the first
		 * one. `Pipeline` carries both `packages[]` and `edges[]`, and
		 * which of the two a gate happens to pick is not a question
		 * that should have an answer -- picking one would need
		 * defending and would leave the other able to drift.
		 */
		for (j = 0; j < cs->n_arr; j++) {
			const struct comp_schema *it = comp_schema_find(cs->arr[j].items);

			if (it == NULL || it->n_required == 0)
				continue;
			fprintf(o, "static const char *const shape_item_%s_%d[] = { ",
			        g_ops[i].op_id, n_emitted);
			for (k = 0; k < it->n_required; k++)
				fprintf(o, "\"%s\", ", it->required[k]);
			fprintf(o, "NULL };\n");
			n_emitted++;
		}
		if (n_emitted == 0)
			continue;
		fprintf(o, "static const struct cix_api_shape_array shape_arrays_%s[] = {",
		        g_ops[i].op_id);
		n_emitted = 0;
		for (j = 0; j < cs->n_arr; j++) {
			const struct comp_schema *it = comp_schema_find(cs->arr[j].items);

			if (it == NULL || it->n_required == 0)
				continue;
			fprintf(o, " { \"%s\", shape_item_%s_%d },", cs->arr[j].prop, g_ops[i].op_id,
			        n_emitted);
			n_emitted++;
		}
		fprintf(o, " { NULL, NULL } };\n");
	}
	fprintf(o, "\nstatic const struct cix_api_shape cix_api_shapes[] = {\n");
	for (i = 0; i < g_op_count; i++) {
		const struct comp_schema *cs = NULL;
		const char *c;
		int have_req = 0, have_arrays = 0, n_params = 0;
		int in_param = 0;

		if (g_ops[i].resp_schema[0] != '\0')
			cs = comp_schema_find(g_ops[i].resp_schema);
		have_req = cs != NULL ? cs->n_required > 0 : g_ops[i].n_resp_required > 0;
		if (cs != NULL) {
			for (j = 0; j < cs->n_arr; j++) {
				const struct comp_schema *it = comp_schema_find(cs->arr[j].items);

				if (it != NULL && it->n_required > 0) {
					have_arrays = 1;
					break;
				}
			}
		}
		for (c = g_ops[i].path; *c != '\0'; c++) {
			if (*c == '{')
				in_param = 1;
			else if (*c == '}' && in_param) {
				in_param = 0;
				n_params++;
			}
		}
		fprintf(o, "\t{ \"%s\", \"/v1", g_ops[i].op_id);
		in_param = 0;
		for (c = g_ops[i].path; *c != '\0'; c++) {
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
		fprintf(o, "\", \"/v1%s\", \"%s\", %d, ", g_ops[i].path, g_ops[i].method, n_params);
		if (have_req)
			fprintf(o, "shape_req_%s, ", g_ops[i].op_id);
		else
			fputs("NULL, ", o);
		if (have_arrays)
			fprintf(o, "shape_arrays_%s", g_ops[i].op_id);
		else
			fputs("NULL", o);
		fputs(" },\n", o);
	}
	fprintf(o, "};\n\n#endif /* CIX_GENERATED_API_SHAPES_H */\n");
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
		/* ADR-0317 (#544): the permission this operation requires, so
		 * the dashboard can tell what a session may do without a second
		 * copy of the policy in JS (sessionMay() in web/app.js). */
		fprintf(o, "\t%s_PERMISSION: \"%s\",\n", g_ops[i].op_id, g_ops[i].permission);
		/* #548: the path template, so the dashboard can tell which
		 * operation a request is (its pre-check in apiRequest()). */
		fprintf(o, "\t%s_PATH: \"/v1%s\",\n", g_ops[i].op_id, g_ops[i].path);
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
/*
 * Strips a trailing newline and, if the value is quoted, the quotes --
 * so `x-cix-config-key: "image,name"` and `x-cix-config-key: name`
 * both yield exactly the field list, and a comma inside a key cannot
 * be mistaken for YAML structure.
 */
static void scalar_value(const char *line, char *out, size_t out_size)
{
	size_t n;

	snprintf(out, out_size, "%s", value_of(line));
	strip_eol(out);
	n = strlen(out);
	if (n >= 2 && out[0] == '"' && out[n - 1] == '"') {
		memmove(out, out + 1, n - 2);
		out[n - 2] = '\0';
	}
}

/*
 * The ConfigDocument schema is the single source of the configuration
 * vocabulary (ADR-0206) and, since ADR-0292, of how each section may
 * be written. Each section declares:
 *
 *   x-cix-config-kind   object | array
 *   x-cix-config-key    arrays only -- the identity field(s)
 *   x-cix-config-apply  replace | reconcile | manual
 *   x-cix-config-state  optional -- the members that are OBSERVED
 *
 * The first three are REQUIRED, and anything else under a section is a
 * hard error. x-cix-config-state is optional because "this section
 * reports nothing back" is a real and common answer, and writing an
 * empty string for it everywhere would be noise rather than a
 * decision. A section whose write semantics are unstated would otherwise
 * default to something, and a default here is a decision nobody made
 * about how an operator's document is allowed to change a live host.
 */
/*
 * ADR-0317 (#540): the permission vocabulary for the daemon, so that
 * "every permission" and "is this a permission" have one source there
 * too -- the list apigen already checked every operation against. A
 * header of its own rather than a section of the route table, because
 * the route table references every handler and hostauth.c must not
 * need them to know the words.
 */
static void emit_permissions(const char *out_path)
{
	FILE *o = fopen(out_path, "w");
	int i;

	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o, "/* Generated by tools/apigen.c from docs/api/openapi.yaml's "
	           "x-cix-permissions (ADR-0317). Do not edit. */\n");
	fprintf(o, "#ifndef CIX_PERMISSIONS_H\n#define CIX_PERMISSIONS_H\n\n");
	fprintf(o, "#define CIX_PERMISSION_COUNT %d\n\n", g_perm_count);
	fprintf(o, "static const char *const cix_permissions[CIX_PERMISSION_COUNT] = {\n");
	for (i = 0; i < g_perm_count; i++)
		fprintf(o, "\t\"%s\",\n", g_perms[i]);
	fprintf(o, "};\n\n#endif\n");
	fclose(o);
}

static void emit_config_sections(const char *out_path, const char *spec)
{
	FILE *f = fopen(spec, "r");
	FILE *o;
	char line[4096];
	char names[256][APIGEN_ID_MAX];
	char kinds[256][16];
	char keys[256][128];
	char modes[256][16];
	char states[256][320];
	int has_key[256];
	int count = 0;
	int lineno = 0;
	int in_doc = 0, in_props = 0;
	int replace_count = 0;
	int reconcile_count = 0;
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
		if (ind > 10)
			continue; /* a folded description's continuation lines */
		if (ind == 10) {
			if (count == 0)
				die_at(spec, lineno, "a section attribute before any section");
			if (!key_at(line, 10, key, sizeof(key)))
				die_at(spec, lineno, "expected a section attribute here, got: %.60s",
				       line + ind);
			if (strcmp(key, "description") == 0)
				continue;
			if (strcmp(key, "x-cix-config-kind") == 0) {
				scalar_value(line, kinds[count - 1], sizeof(kinds[0]));
				continue;
			}
			if (strcmp(key, "x-cix-config-key") == 0) {
				scalar_value(line, keys[count - 1], sizeof(keys[0]));
				has_key[count - 1] = 1;
				continue;
			}
			if (strcmp(key, "x-cix-config-apply") == 0) {
				scalar_value(line, modes[count - 1], sizeof(modes[0]));
				continue;
			}
			if (strcmp(key, "x-cix-config-state") == 0) {
				scalar_value(line, states[count - 1], sizeof(states[0]));
				continue;
			}
			die_at(spec, lineno,
			       "unrecognised attribute \"%s\" under config section \"%s\"", key,
			       names[count - 1]);
		}
		if (ind != 8)
			continue;
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
		kinds[count][0] = '\0';
		keys[count][0] = '\0';
		modes[count][0] = '\0';
		states[count][0] = '\0';
		has_key[count] = 0;
		count++;
	}
	fclose(f);

	if (!in_doc)
		die_at(spec, 0, "no ConfigDocument schema found -- ADR-0206 requires it to "
		                "exist, since it is the source of the config vocabulary");
	if (count == 0)
		die_at(spec, 0, "ConfigDocument has no properties -- a config document with "
		                "no sections would be a silently empty view");

	for (i = 0; i < count; i++) {
		int is_array = strcmp(kinds[i], "array") == 0;

		if (!is_array && strcmp(kinds[i], "object") != 0)
			die_at(spec, 0,
			       "config section \"%s\": x-cix-config-kind must be \"object\" or "
			       "\"array\" (got \"%s\")", names[i], kinds[i]);
		if (strcmp(modes[i], "replace") != 0 && strcmp(modes[i], "reconcile") != 0 &&
		    strcmp(modes[i], "manual") != 0)
			die_at(spec, 0,
			       "config section \"%s\": x-cix-config-apply must be \"replace\", "
			       "\"reconcile\" or \"manual\" (got \"%s\")", names[i], modes[i]);
		if (is_array && !has_key[i])
			die_at(spec, 0,
			       "config section \"%s\" is an array and must declare "
			       "x-cix-config-key -- \"\" to compare it by position, which is "
			       "right only for an ordered list of scalars", names[i]);
		if (!is_array && has_key[i])
			die_at(spec, 0,
			       "config section \"%s\" is an object, so x-cix-config-key means "
			       "nothing for it", names[i]);
		if (strcmp(modes[i], "replace") == 0)
			replace_count++;
		if (strcmp(modes[i], "reconcile") == 0)
			reconcile_count++;
	}

	o = fopen(out_path, "w");
	if (o == NULL) {
		fprintf(stderr, "apigen: cannot write %s\n", out_path);
		exit(1);
	}
	fprintf(o, "/* Generated by tools/apigen.c from %s -- do not edit. */\n", spec);
	fprintf(o, "#ifndef CIX_GENERATED_CONFIG_SECTIONS_H\n");
	fprintf(o, "#define CIX_GENERATED_CONFIG_SECTIONS_H\n\n");
	fprintf(o, "/* Section order is the contract: a section never depends on one\n");
	fprintf(o, " * below it. The last field is the section's OBSERVED members --\n");
	fprintf(o, " * what the host reports rather than what it was told. See the\n");
	fprintf(o, " * ConfigDocument schema for why both are declared there. */\n");
	fprintf(o, "#define CIX_CONFIG_SECTIONS(X) \\\n");
	for (i = 0; i < count; i++)
		fprintf(o, "\tX(%s, CONFIG_KIND_%s, \"%s\", CONFIG_APPLY_%s, \"%s\")%s\n", names[i],
		        strcmp(kinds[i], "array") == 0 ? "ARRAY" : "OBJECT", keys[i],
		        strcmp(modes[i], "replace") == 0     ? "REPLACE"
		        : strcmp(modes[i], "reconcile") == 0 ? "RECONCILE"
		                                             : "MANUAL",
		        states[i],
		        i + 1 < count ? " \\" : "");
	fprintf(o, "\n/* The sections one setter can replace outright. Expanded on its\n");
	fprintf(o, " * own so the apply functions are declared, defined and tabulated\n");
	fprintf(o, " * from this list and nothing else -- a section declared\n");
	fprintf(o, " * \"replace\" with no apply function does not link, and an apply\n");
	fprintf(o, " * function for a section that is not is an unused-function error. */\n");
	fprintf(o, "#define CIX_CONFIG_SECTIONS_REPLACE(X) \\\n");
	for (i = 0; i < count; i++) {
		if (strcmp(modes[i], "replace") != 0)
			continue;
		replace_count--;
		fprintf(o, "\tX(%s)%s\n", names[i], replace_count > 0 ? " \\" : "");
	}
	fprintf(o, "\n/* The sections applied element by element. Same guard as above:\n");
	fprintf(o, " * the element operations are declared and tabulated from this\n");
	fprintf(o, " * list alone. */\n");
	fprintf(o, "#define CIX_CONFIG_SECTIONS_RECONCILE(X) \\\n");
	for (i = 0; i < count; i++) {
		if (strcmp(modes[i], "reconcile") != 0)
			continue;
		reconcile_count--;
		fprintf(o, "\tX(%s)%s\n", names[i], reconcile_count > 0 ? " \\" : "");
	}
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
	const char *emit_permissions_path = NULL;
	const char *emit_shapes_path = NULL;

	if (argc < 2) {
		fprintf(stderr, "usage: apigen <openapi.yaml> "
		                "[--list | --emit-routes <out.h> | --emit-cli <out.h> | "
		                "--emit-web <out.js> | --emit-config-sections <out.h> | "
		                "--emit-permissions <out.h> | --emit-shapes <out.h>]\n");
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
		else if (strcmp(argv[i], "--emit-permissions") == 0 && i + 1 < argc)
			emit_permissions_path = argv[++i];
		else if (strcmp(argv[i], "--emit-shapes") == 0 && i + 1 < argc)
			emit_shapes_path = argv[++i];
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
	/* #575: a response schema named by $ref must be known before the
	 * paths are read, for the same reason -- components: comes after. */
	load_component_schemas(spec);
	/* ADR-0317: the words every x-cix-permission is checked against. */
	load_permission_vocabulary(spec);

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
			g_in_responses = 0;
			g_in_200 = 0;
			g_in_resp_props = 0;
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
			/* #575: the response reader resets here for the same reason
			 * g_in_params does -- a new operation must not inherit where
			 * the previous one left off, even where the grammar makes it
			 * harmless today. */
			g_in_responses = 0;
			g_in_200 = 0;
			g_in_resp_props = 0;
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
			/*
			 * #575: the same for the response-shape reader. An
			 * operation-level key that is not `responses` ends it, so a
			 * `required` belonging to some later section cannot be read
			 * as a response's.
			 */
			g_in_responses = strcmp(key, "responses") == 0;
			g_in_resp_props = 0;
			if (!g_in_responses)
				g_in_200 = 0;
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
			} else if (strcmp(key, "x-cix-permission") == 0) {
				char v[APIGEN_PERM_MAX];

				snprintf(v, sizeof(v), "%s", value_of(line));
				strip_eol(v);
				if (g_ops[cur_op].permission[0] != '\0')
					die_at(spec, lineno,
					       "%s %s declares x-cix-permission twice -- one permission per "
					       "operation (ADR-0317)",
					       g_ops[cur_op].method, g_ops[cur_op].path);
				if (!perm_is_known(v))
					die_at(spec, lineno,
					       "%s %s requires \"%s\", which is not in x-cix-permissions -- "
					       "the vocabulary is closed (ADR-0317)",
					       g_ops[cur_op].method, g_ops[cur_op].path, v);
				snprintf(g_ops[cur_op].permission, sizeof(g_ops[cur_op].permission), "%s", v);
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
		 * The 200 response's schema (#575).
		 *
		 * Two forms, and they are the only two the spec uses for an
		 * object response -- measured 2026-10-07: 159 operations name
		 * a components/schemas entry with `$ref` at indent 16, and 69
		 * declare an inline `type: object`, of which 42 carry their
		 * own `required`. A bare array (2), a string (3), and anything
		 * reached through allOf nest deeper than this reads, so they
		 * record no shape -- see api_op's own comment for why silence
		 * is the right answer here and a refusal is the right answer
		 * for a route.
		 */
		if (g_in_responses && cur_op >= 0) {
			if (ind == 8) {
				g_in_200 = key_at(line, 8, key, sizeof(key)) &&
				           strcmp(key, "\"200\"") == 0;
				continue;
			}
			if (g_in_200 && ind == 16 && key_at(line, 16, key, sizeof(key))) {
				if (strcmp(key, "$ref") == 0) {
					ref_tail(value_of(line), g_ops[cur_op].resp_schema,
					         sizeof(g_ops[cur_op].resp_schema));
				} else if (strcmp(key, "required") == 0) {
					char v[1024];

					snprintf(v, sizeof(v), "%s", value_of(line));
					strip_eol(v);
					g_ops[cur_op].n_resp_required =
					    parse_inline_list(v, g_ops[cur_op].resp_required,
					                      APIGEN_MAX_REQUIRED);
				}
				g_in_resp_props = strcmp(key, "properties") == 0;
				continue;
			}
			/* An inline schema's own property names, at 18. */
			if (g_in_200 && g_in_resp_props && ind == 18 &&
			    key_at(line, 18, key, sizeof(key))) {
				size_t used = strlen(g_ops[cur_op].resp_props);

				if (used + strlen(key) + 2 >= sizeof(g_ops[cur_op].resp_props))
					die_at(spec, lineno,
					       "operation \"%s\" declares more inline response properties "
					       "than this tool can record; raise resp_props rather than let "
					       "the self-consistency check below go unsound",
					       g_ops[cur_op].op_id);
				snprintf(g_ops[cur_op].resp_props + used,
				         sizeof(g_ops[cur_op].resp_props) - used, "%s,", key);
				continue;
			}
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
	/*
	 * #575: a schema whose `required` names a property it does not
	 * declare is a contract that contradicts itself, and nothing could
	 * ever satisfy it.
	 *
	 * A build failure rather than a warning, by the same argument every
	 * other refusal in this tool makes: the spec is the authority, and
	 * an authority that disagrees with itself is worse than one that is
	 * merely incomplete. There is no legitimate reason to write one.
	 *
	 * The spec had exactly one, found by sweeping all 82 gateable GETs
	 * against 192.168.15.95 before `test_apishape` had ever been
	 * compiled: `listStorage` declared `required: [disks]` with a
	 * `properties` block naming `storage`, which is what the daemon has
	 * always sent. Fixed in the same change. 144 component schemas and
	 * the other 338 operations were clean, so this check costs nothing
	 * and closes the hole permanently -- and it needs no daemon, which
	 * makes it the cheaper half of #575's two gates.
	 */
	for (i = 0; i < g_op_count; i++) {
		int q;

		for (q = 0; q < g_ops[i].n_resp_required; q++) {
			char needle[APIGEN_QNAME_MAX + 2];

			snprintf(needle, sizeof(needle), "%s,", g_ops[i].resp_required[q]);
			if (strstr(g_ops[i].resp_props, needle) != NULL)
				continue;
			die_at(spec, g_ops[i].line,
			       "%s declares its 200 response requires \"%s\", but that schema's own "
			       "properties do not include it -- nothing could satisfy this, so it is "
			       "a contract that contradicts itself",
			       g_ops[i].op_id, g_ops[i].resp_required[q]);
		}
	}
	for (i = 0; i < g_comp_schema_count; i++) {
		int q;

		for (q = 0; q < g_comp_schemas[i].n_required; q++) {
			char needle[APIGEN_QNAME_MAX + 2];

			snprintf(needle, sizeof(needle), "%s,", g_comp_schemas[i].required[q]);
			if (strstr(g_comp_schemas[i].props, needle) != NULL)
				continue;
			die_at(spec, g_comp_schemas[i].line,
			       "schema %s requires \"%s\", but does not declare it as a property -- "
			       "nothing could satisfy this, so it is a contract that contradicts "
			       "itself",
			       g_comp_schemas[i].comp, g_comp_schemas[i].required[q]);
		}
	}
	/*
	 * ADR-0317 (#539): every operation states the permission it
	 * requires. A default would make "no stated policy" representable
	 * again, so a missing one is a build failure, not a fallback --
	 * the owner's decision of 2026-09-29.
	 */
	for (i = 0; i < g_op_count; i++) {
		if (g_ops[i].permission[0] == '\0')
			die_at(spec, g_ops[i].line,
			       "%s %s has no x-cix-permission -- every operation must state the "
			       "permission it requires (ADR-0317)",
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
	if (emit_shapes_path != NULL)
		emit_shapes(emit_shapes_path, spec);
	if (emit_web_path != NULL)
		emit_web(emit_web_path, spec);
	if (emit_permissions_path != NULL)
		emit_permissions(emit_permissions_path);
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
