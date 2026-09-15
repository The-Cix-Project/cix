/*
 * Structural JSON diff (ADR-0292). See jsondiff.h for the contract.
 */
#include "jsondiff.h"

#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A request body decides how much work this does, so the change list
 * is bounded. 512 is well past any real configuration difference --
 * the whole running-config document of a 12-container host renders
 * 46 KB -- and far below what an adversarial body could ask for.
 */
#define JSONDIFF_MAX_CHANGES 512
#define JSONDIFF_KEY_TEXT_MAX 256

static int diff_walk(struct jsondiff *d, const char *base, const struct json_value *live,
                     const struct json_value *sup, const char *array_key);
static int element_name(const struct json_value *el, const char *array_key, char *out, size_t n);

/*
 * The ignored-member list, carried on the struct rather than threaded
 * through every recursion -- it is a property of the whole comparison,
 * not of a level within it.
 */
static const char *g_ignore;

int jsondiff_field_listed(const char *list, const char *name)
{
	const char *p = list;
	size_t n = strlen(name);

	if (list == NULL || list[0] == '\0')
		return 0;
	while (*p != '\0') {
		const char *start = p;
		size_t len;

		while (*p != '\0' && *p != ',')
			p++;
		len = (size_t)(p - start);
		if (len == n && strncmp(start, name, n) == 0)
			return 1;
		if (*p == ',')
			p++;
	}
	return 0;
}

static int values_equal(const struct json_value *a, const struct json_value *b)
{
	size_t i;

	if (a == NULL || b == NULL)
		return a == b;
	if (a->type != b->type)
		return 0;
	switch (a->type) {
	case JSON_NULL:
		return 1;
	case JSON_BOOL:
		return a->u.boolean == b->u.boolean;
	case JSON_NUMBER:
		return a->u.number == b->u.number;
	case JSON_STRING:
		return strcmp(a->u.string, b->u.string) == 0;
	case JSON_ARRAY:
		if (a->u.array.count != b->u.array.count)
			return 0;
		for (i = 0; i < a->u.array.count; i++) {
			if (!values_equal(a->u.array.items[i], b->u.array.items[i]))
				return 0;
		}
		return 1;
	case JSON_OBJECT:
		if (a->u.object.count != b->u.object.count)
			return 0;
		for (i = 0; i < a->u.object.count; i++) {
			const struct json_value *bv = json_object_get(b, a->u.object.keys[i]);

			if (bv == NULL || !values_equal(a->u.object.values[i], bv))
				return 0;
		}
		return 1;
	}
	return 0;
}

static int add_change(struct jsondiff *d, const char *path, enum jsondiff_op op,
                      const struct json_value *from, const struct json_value *to)
{
	struct jsondiff_change *c;

	if (d->truncated)
		return 0;
	if (d->count >= JSONDIFF_MAX_CHANGES) {
		d->truncated = 1;
		return 0;
	}
	if (d->count == d->cap) {
		int ncap = d->cap == 0 ? 16 : d->cap * 2;
		struct jsondiff_change *n = realloc(d->changes, (size_t)ncap * sizeof(*n));

		if (n == NULL)
			return -1;
		d->changes = n;
		d->cap = ncap;
	}
	c = &d->changes[d->count];
	snprintf(c->path, sizeof(c->path), "%s", path);
	c->op = op;
	c->from = from;
	c->to = to;
	d->count++;
	return 0;
}

static void path_field(char *out, size_t n, const char *base, const char *field)
{
	if (base[0] == '\0')
		snprintf(out, n, "%s", field);
	else
		snprintf(out, n, "%s.%s", base, field);
}

static void path_index(char *out, size_t n, const char *base, int i)
{
	snprintf(out, n, "%s[%d]", base, i);
}

static void path_named(char *out, size_t n, const char *base, const char *name)
{
	snprintf(out, n, "%s[%s]", base, name);
}

/*
 * The text an identity field contributes to an element's name. Only
 * scalars can identify anything, so an object or array field makes the
 * key unusable and the array falls back to index comparison.
 */
static int scalar_text(const struct json_value *v, char *out, size_t n)
{
	if (v == NULL)
		return -1;
	switch (v->type) {
	case JSON_STRING:
		snprintf(out, n, "%s", v->u.string);
		return 0;
	case JSON_NUMBER:
		if (v->u.number == (double)(long long)v->u.number)
			snprintf(out, n, "%lld", (long long)v->u.number);
		else
			snprintf(out, n, "%g", v->u.number);
		return 0;
	case JSON_BOOL:
		snprintf(out, n, "%s", v->u.boolean ? "true" : "false");
		return 0;
	case JSON_NULL:
		snprintf(out, n, "null");
		return 0;
	default:
		return -1;
	}
}

/*
 * Builds an element's name from the declared key. A composite key is
 * comma-separated in the schema and joined the same way here, so the
 * name that appears in a diff path is the one an operator would write.
 */
int jsondiff_element_name(const struct json_value *el, const char *array_key, char *out,
                          size_t out_size)
{
	return element_name(el, array_key, out, out_size);
}

static int element_name(const struct json_value *el, const char *array_key, char *out, size_t n)
{
	const char *p = array_key;
	size_t used = 0;

	if (el == NULL || el->type != JSON_OBJECT)
		return -1;
	out[0] = '\0';
	while (*p != '\0') {
		char field[64];
		char text[JSONDIFF_KEY_TEXT_MAX];
		size_t flen = 0;
		int written;

		while (*p != '\0' && *p != ',') {
			if (flen + 1 >= sizeof(field))
				return -1;
			field[flen++] = *p++;
		}
		field[flen] = '\0';
		if (*p == ',')
			p++;
		if (flen == 0)
			return -1;
		if (scalar_text(json_object_get(el, field), text, sizeof(text)) != 0)
			return -1;
		written = snprintf(out + used, n - used, "%s%s", used > 0 ? "," : "", text);
		if (written < 0 || (size_t)written >= n - used)
			return -1;
		used += (size_t)written;
	}
	return out[0] == '\0' ? -1 : 0;
}

/*
 * Every element must yield a name, and no two may yield the same one.
 * Otherwise the declared key does not identify an element in this
 * document and matching by it would pair up the wrong ones -- which is
 * worse than an index comparison, because it looks authoritative.
 *
 * 1 = usable, 0 = this document defeats the declared key, -1 = out of
 * memory. The three are distinct because the caller reports 0 to an
 * operator as a fact about their document, and reporting that for an
 * allocation failure would be a claim about their data that is not
 * true.
 */
static int names_usable(const struct json_value *arr, const char *array_key, char **names_out)
{
	size_t i, j;
	char *names;

	*names_out = NULL;
	if (arr->u.array.count == 0)
		return 1;
	names = calloc(arr->u.array.count, JSONDIFF_KEY_TEXT_MAX);
	if (names == NULL)
		return -1;
	for (i = 0; i < arr->u.array.count; i++) {
		if (element_name(arr->u.array.items[i], array_key,
		                 names + i * JSONDIFF_KEY_TEXT_MAX, JSONDIFF_KEY_TEXT_MAX) != 0) {
			free(names);
			return 0;
		}
		for (j = 0; j < i; j++) {
			if (strcmp(names + i * JSONDIFF_KEY_TEXT_MAX,
			           names + j * JSONDIFF_KEY_TEXT_MAX) == 0) {
				free(names);
				return 0;
			}
		}
	}
	*names_out = names;
	return 1;
}

static int diff_array_indexed(struct jsondiff *d, const char *base, const struct json_value *live,
                              const struct json_value *sup)
{
	size_t i;
	size_t shared = live->u.array.count < sup->u.array.count ? live->u.array.count
	                                                         : sup->u.array.count;
	char p[JSONDIFF_PATH_MAX];

	for (i = 0; i < shared; i++) {
		path_index(p, sizeof(p), base, (int)i);
		if (diff_walk(d, p, live->u.array.items[i], sup->u.array.items[i], NULL) != 0)
			return -1;
	}
	for (i = shared; i < live->u.array.count; i++) {
		path_index(p, sizeof(p), base, (int)i);
		if (add_change(d, p, JSONDIFF_REMOVE, live->u.array.items[i], NULL) != 0)
			return -1;
	}
	for (i = shared; i < sup->u.array.count; i++) {
		path_index(p, sizeof(p), base, (int)i);
		if (add_change(d, p, JSONDIFF_ADD, NULL, sup->u.array.items[i]) != 0)
			return -1;
	}
	return 0;
}

static int diff_array_keyed(struct jsondiff *d, const char *base, const struct json_value *live,
                            const struct json_value *sup, const char *live_names,
                            const char *sup_names)
{
	size_t i, j;
	char p[JSONDIFF_PATH_MAX];

	for (i = 0; i < live->u.array.count; i++) {
		const char *name = live_names + i * JSONDIFF_KEY_TEXT_MAX;
		int found = -1;

		for (j = 0; j < sup->u.array.count; j++) {
			if (strcmp(name, sup_names + j * JSONDIFF_KEY_TEXT_MAX) == 0) {
				found = (int)j;
				break;
			}
		}
		path_named(p, sizeof(p), base, name);
		if (found < 0) {
			if (add_change(d, p, JSONDIFF_REMOVE, live->u.array.items[i], NULL) != 0)
				return -1;
			continue;
		}
		if (diff_walk(d, p, live->u.array.items[i], sup->u.array.items[found], NULL) != 0)
			return -1;
	}
	for (j = 0; j < sup->u.array.count; j++) {
		const char *name = sup_names + j * JSONDIFF_KEY_TEXT_MAX;
		int found = 0;

		for (i = 0; i < live->u.array.count; i++) {
			if (strcmp(name, live_names + i * JSONDIFF_KEY_TEXT_MAX) == 0) {
				found = 1;
				break;
			}
		}
		if (found)
			continue;
		path_named(p, sizeof(p), base, name);
		if (add_change(d, p, JSONDIFF_ADD, NULL, sup->u.array.items[j]) != 0)
			return -1;
	}
	return 0;
}

static int diff_arrays(struct jsondiff *d, const char *base, const struct json_value *live,
                       const struct json_value *sup, const char *array_key)
{
	char *live_names = NULL;
	char *sup_names = NULL;
	int lu, su;
	int rc;

	if (array_key == NULL || array_key[0] == '\0')
		return diff_array_indexed(d, base, live, sup);
	lu = names_usable(live, array_key, &live_names);
	su = lu == 1 ? names_usable(sup, array_key, &sup_names) : 1;
	if (lu < 0 || su < 0) {
		free(live_names);
		free(sup_names);
		return -1;
	}
	if (lu == 0 || su == 0) {
		free(live_names);
		free(sup_names);
		if (d->unkeyed_path[0] == '\0')
			snprintf(d->unkeyed_path, sizeof(d->unkeyed_path), "%s",
			         base[0] == '\0' ? "(section)" : base);
		return diff_array_indexed(d, base, live, sup);
	}
	rc = diff_array_keyed(d, base, live, sup, live_names, sup_names);
	free(live_names);
	free(sup_names);
	return rc;
}

static int diff_objects(struct jsondiff *d, const char *base, const struct json_value *live,
                        const struct json_value *sup)
{
	size_t i;
	char p[JSONDIFF_PATH_MAX];

	for (i = 0; i < live->u.object.count; i++) {
		const struct json_value *sv;

		if (jsondiff_field_listed(g_ignore, live->u.object.keys[i]))
			continue;
		sv = json_object_get(sup, live->u.object.keys[i]);
		path_field(p, sizeof(p), base, live->u.object.keys[i]);
		if (sv == NULL) {
			if (add_change(d, p, JSONDIFF_REMOVE, live->u.object.values[i], NULL) != 0)
				return -1;
			continue;
		}
		if (diff_walk(d, p, live->u.object.values[i], sv, NULL) != 0)
			return -1;
	}
	for (i = 0; i < sup->u.object.count; i++) {
		if (json_object_get(live, sup->u.object.keys[i]) != NULL)
			continue;
		if (jsondiff_field_listed(g_ignore, sup->u.object.keys[i]))
			continue;
		path_field(p, sizeof(p), base, sup->u.object.keys[i]);
		if (add_change(d, p, JSONDIFF_ADD, NULL, sup->u.object.values[i]) != 0)
			return -1;
	}
	return 0;
}

static int diff_walk(struct jsondiff *d, const char *base, const struct json_value *live,
                     const struct json_value *sup, const char *array_key)
{
	if (d->truncated)
		return 0;
	if (live->type != sup->type)
		return add_change(d, base, JSONDIFF_REPLACE, live, sup);
	if (live->type == JSON_OBJECT)
		return diff_objects(d, base, live, sup);
	if (live->type == JSON_ARRAY)
		return diff_arrays(d, base, live, sup, array_key);
	if (values_equal(live, sup))
		return 0;
	return add_change(d, base, JSONDIFF_REPLACE, live, sup);
}

int jsondiff_compute(const struct json_value *live, const struct json_value *supplied,
                     const char *array_key, const char *ignore_fields, struct jsondiff *out)
{
	int rc;

	memset(out, 0, sizeof(*out));
	g_ignore = ignore_fields;
	rc = diff_walk(out, "", live, supplied, array_key);
	g_ignore = NULL;
	if (rc != 0) {
		jsondiff_free(out);
		return -1;
	}
	return 0;
}

void jsondiff_free(struct jsondiff *d)
{
	free(d->changes);
	memset(d, 0, sizeof(*d));
}

int jsondiff_equal_ignoring(const struct json_value *a, const struct json_value *b,
                            const char *ignore_fields)
{
	struct jsondiff d;
	int eq;

	if (jsondiff_compute(a, b, "", ignore_fields, &d) != 0)
		return -1;
	eq = d.count == 0;
	jsondiff_free(&d);
	return eq;
}

int jsondiff_equal(const struct json_value *a, const struct json_value *b)
{
	return values_equal(a, b);
}

const char *jsondiff_op_name(enum jsondiff_op op)
{
	switch (op) {
	case JSONDIFF_ADD:
		return "add";
	case JSONDIFF_REMOVE:
		return "remove";
	case JSONDIFF_REPLACE:
		return "replace";
	}
	return "replace";
}
