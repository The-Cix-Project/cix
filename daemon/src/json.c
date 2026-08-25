#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct parser {
	const char *p;
	const char *end;
};

static struct json_value *parse_value(struct parser *ps);

static struct json_value *alloc_value(enum json_type t)
{
	struct json_value *v = calloc(1, sizeof(*v));
	if (v != NULL)
		v->type = t;
	return v;
}

static void skip_ws(struct parser *ps)
{
	while (ps->p < ps->end &&
	       (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r'))
		ps->p++;
}

static char *parse_string_raw(struct parser *ps)
{
	size_t cap = 32, len = 0;
	char *buf;
	char *nb;

	if (ps->p >= ps->end || *ps->p != '"')
		return NULL;
	ps->p++;

	buf = malloc(cap);
	if (buf == NULL)
		return NULL;

	while (ps->p < ps->end && *ps->p != '"') {
		char out = *ps->p;

		if (out == '\\') {
			ps->p++;
			if (ps->p >= ps->end) {
				free(buf);
				return NULL;
			}
			switch (*ps->p) {
			case '"': out = '"'; break;
			case '\\': out = '\\'; break;
			case '/': out = '/'; break;
			case 'n': out = '\n'; break;
			case 't': out = '\t'; break;
			case 'r': out = '\r'; break;
			case 'b': out = '\b'; break;
			case 'f': out = '\f'; break;
			case 'u': {
				/*
				 * task #760: jw_escaped_string() (below, the writer
				 * half of this same file) has always emitted \u00XX
				 * for any control character < 0x20 -- the only
				 * \uXXXX shape this codebase's own writer ever
				 * produces, confirmed by inspection -- but this
				 * parser rejected every \u escape outright,
				 * silently failing the ENTIRE surrounding parse
				 * (json_parse() has no partial-success mode) the
				 * moment any string field contained one. Never hit
				 * before capture_output (ADR-0112) started
				 * capturing real stdout/stderr content: any
				 * program that colorizes its own terminal output
				 * (glauth's zerolog does) writes raw ANSI escape
				 * bytes (ESC = 0x1b) into what capture_output
				 * relays verbatim, which jw_escaped_string() then
				 * has to \u-escape to stay valid JSON at all --
				 * found live via `cixctl ps` silently returning
				 * nothing against a real box with LDAP containers
				 * running. json.h's own "no field needs \uXXXX"
				 * scope note was accurate when written, before
				 * this field existed; it no longer is. Scoped
				 * deliberately narrow, matching what the writer
				 * side actually needs (still not full RFC 8259):
				 * exactly 4 hex digits, decoded as a single byte
				 * (0x00-0xFF) -- no UTF-16 surrogate-pair handling,
				 * since nothing in this codebase's own writer ever
				 * emits or needs one.
				 */
				unsigned int cp = 0;
				int i;

				if (ps->end - ps->p < 5) {
					free(buf);
					return NULL;
				}
				for (i = 1; i <= 4; i++) {
					char h = ps->p[i];

					cp <<= 4;
					if (h >= '0' && h <= '9')
						cp |= (unsigned int)(h - '0');
					else if (h >= 'a' && h <= 'f')
						cp |= (unsigned int)(h - 'a' + 10);
					else if (h >= 'A' && h <= 'F')
						cp |= (unsigned int)(h - 'A' + 10);
					else {
						free(buf);
						return NULL;
					}
				}
				if (cp > 0xff) {
					free(buf);
					return NULL;
				}
				out = (char)cp;
				ps->p += 4;
				break;
			}
			default:
				free(buf);
				return NULL;
			}
		}

		if (len + 1 >= cap) {
			cap *= 2;
			nb = realloc(buf, cap);
			if (nb == NULL) {
				free(buf);
				return NULL;
			}
			buf = nb;
		}
		buf[len++] = out;
		ps->p++;
	}

	if (ps->p >= ps->end) {
		free(buf);
		return NULL;
	}
	ps->p++; /* closing quote */
	buf[len] = '\0';
	return buf;
}

static struct json_value *parse_number(struct parser *ps)
{
	const char *start = ps->p;
	char tmp[64];
	size_t n;
	struct json_value *v;

	if (ps->p < ps->end && *ps->p == '-')
		ps->p++;
	while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
		ps->p++;
	if (ps->p < ps->end && *ps->p == '.') {
		ps->p++;
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if (ps->p < ps->end && (*ps->p == 'e' || *ps->p == 'E')) {
		ps->p++;
		if (ps->p < ps->end && (*ps->p == '+' || *ps->p == '-'))
			ps->p++;
		while (ps->p < ps->end && *ps->p >= '0' && *ps->p <= '9')
			ps->p++;
	}
	if (ps->p == start)
		return NULL;

	n = (size_t)(ps->p - start);
	if (n >= sizeof(tmp))
		return NULL;
	memcpy(tmp, start, n);
	tmp[n] = '\0';

	v = alloc_value(JSON_NUMBER);
	if (v == NULL)
		return NULL;
	v->u.number = strtod(tmp, NULL);
	return v;
}

static struct json_value *parse_array(struct parser *ps)
{
	struct json_value *v = alloc_value(JSON_ARRAY);
	struct json_value *item;
	struct json_value **nitems;

	if (v == NULL)
		return NULL;
	ps->p++; /* [ */

	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == ']') {
		ps->p++;
		return v;
	}

	for (;;) {
		item = parse_value(ps);
		if (item == NULL) {
			json_free(v);
			return NULL;
		}

		nitems = realloc(v->u.array.items, (v->u.array.count + 1) * sizeof(*nitems));
		if (nitems == NULL) {
			json_free(item);
			json_free(v);
			return NULL;
		}
		v->u.array.items = nitems;
		v->u.array.items[v->u.array.count++] = item;

		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			skip_ws(ps);
			continue;
		}
		if (ps->p < ps->end && *ps->p == ']') {
			ps->p++;
			break;
		}
		json_free(v);
		return NULL;
	}
	return v;
}

static struct json_value *parse_object(struct parser *ps)
{
	struct json_value *v = alloc_value(JSON_OBJECT);
	char *key;
	struct json_value *val;
	char **nkeys;
	struct json_value **nvalues;

	if (v == NULL)
		return NULL;
	ps->p++; /* { */

	skip_ws(ps);
	if (ps->p < ps->end && *ps->p == '}') {
		ps->p++;
		return v;
	}

	for (;;) {
		skip_ws(ps);
		key = parse_string_raw(ps);
		if (key == NULL) {
			json_free(v);
			return NULL;
		}

		skip_ws(ps);
		if (ps->p >= ps->end || *ps->p != ':') {
			free(key);
			json_free(v);
			return NULL;
		}
		ps->p++;

		val = parse_value(ps);
		if (val == NULL) {
			free(key);
			json_free(v);
			return NULL;
		}

		nkeys = realloc(v->u.object.keys, (v->u.object.count + 1) * sizeof(*nkeys));
		if (nkeys == NULL) {
			free(key);
			json_free(val);
			json_free(v);
			return NULL;
		}
		v->u.object.keys = nkeys;

		nvalues = realloc(v->u.object.values, (v->u.object.count + 1) * sizeof(*nvalues));
		if (nvalues == NULL) {
			free(key);
			json_free(val);
			json_free(v);
			return NULL;
		}
		v->u.object.values = nvalues;

		v->u.object.keys[v->u.object.count] = key;
		v->u.object.values[v->u.object.count] = val;
		v->u.object.count++;

		skip_ws(ps);
		if (ps->p < ps->end && *ps->p == ',') {
			ps->p++;
			continue;
		}
		if (ps->p < ps->end && *ps->p == '}') {
			ps->p++;
			break;
		}
		json_free(v);
		return NULL;
	}
	return v;
}

static struct json_value *parse_value(struct parser *ps)
{
	char c;
	char *s;
	struct json_value *v;

	skip_ws(ps);
	if (ps->p >= ps->end)
		return NULL;
	c = *ps->p;

	if (c == '{')
		return parse_object(ps);
	if (c == '[')
		return parse_array(ps);
	if (c == '"') {
		s = parse_string_raw(ps);
		if (s == NULL)
			return NULL;
		v = alloc_value(JSON_STRING);
		if (v == NULL) {
			free(s);
			return NULL;
		}
		v->u.string = s;
		return v;
	}
	if (c == 't' && (size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "true", 4) == 0) {
		ps->p += 4;
		v = alloc_value(JSON_BOOL);
		if (v != NULL)
			v->u.boolean = 1;
		return v;
	}
	if (c == 'f' && (size_t)(ps->end - ps->p) >= 5 && memcmp(ps->p, "false", 5) == 0) {
		ps->p += 5;
		v = alloc_value(JSON_BOOL);
		if (v != NULL)
			v->u.boolean = 0;
		return v;
	}
	if (c == 'n' && (size_t)(ps->end - ps->p) >= 4 && memcmp(ps->p, "null", 4) == 0) {
		ps->p += 4;
		return alloc_value(JSON_NULL);
	}
	if (c == '-' || (c >= '0' && c <= '9'))
		return parse_number(ps);

	return NULL;
}

struct json_value *json_parse(const char *text, size_t len)
{
	struct parser ps;
	struct json_value *v;

	ps.p = text;
	ps.end = text + len;

	v = parse_value(&ps);
	if (v == NULL)
		return NULL;

	skip_ws(&ps);
	if (ps.p != ps.end) {
		json_free(v);
		return NULL;
	}
	return v;
}

void json_free(struct json_value *v)
{
	size_t i;

	if (v == NULL)
		return;

	switch (v->type) {
	case JSON_STRING:
		free(v->u.string);
		break;
	case JSON_ARRAY:
		for (i = 0; i < v->u.array.count; i++)
			json_free(v->u.array.items[i]);
		free(v->u.array.items);
		break;
	case JSON_OBJECT:
		for (i = 0; i < v->u.object.count; i++) {
			free(v->u.object.keys[i]);
			json_free(v->u.object.values[i]);
		}
		free(v->u.object.keys);
		free(v->u.object.values);
		break;
	default:
		break;
	}
	free(v);
}

const struct json_value *json_object_get(const struct json_value *obj, const char *key)
{
	size_t i;

	if (obj == NULL || obj->type != JSON_OBJECT)
		return NULL;
	for (i = 0; i < obj->u.object.count; i++) {
		if (strcmp(obj->u.object.keys[i], key) == 0)
			return obj->u.object.values[i];
	}
	return NULL;
}

const char *json_as_string(const struct json_value *v)
{
	if (v == NULL || v->type != JSON_STRING)
		return NULL;
	return v->u.string;
}

double json_as_number(const struct json_value *v)
{
	if (v == NULL || v->type != JSON_NUMBER)
		return 0;
	return v->u.number;
}

/* --- Writer --- */

void jw_init(struct json_writer *w)
{
	w->buf = NULL;
	w->len = 0;
	w->cap = 0;
	w->depth = 0;
}

void jw_free(struct json_writer *w)
{
	free(w->buf);
	w->buf = NULL;
	w->len = 0;
	w->cap = 0;
}

static void jw_ensure(struct json_writer *w, size_t extra)
{
	size_t ncap;
	char *nb;

	if (w->len + extra + 1 <= w->cap)
		return;

	ncap = w->cap != 0 ? w->cap * 2 : 128;
	while (ncap < w->len + extra + 1)
		ncap *= 2;

	nb = realloc(w->buf, ncap);
	if (nb == NULL) {
		/*
		 * Response bodies here are small and bounded (a handful of
		 * container records at most); an allocation failure at this
		 * size means the system is out of memory, not a bad
		 * request. There's no sane partial response to fall back
		 * to, so fail loudly rather than emit truncated JSON.
		 */
		abort();
	}
	w->buf = nb;
	w->cap = ncap;
}

static void jw_raw(struct json_writer *w, const char *s, size_t n)
{
	jw_ensure(w, n);
	memcpy(w->buf + w->len, s, n);
	w->len += n;
}

static void jw_raw_str(struct json_writer *w, const char *s)
{
	jw_raw(w, s, strlen(s));
}

/*
 * Shared by jw_escaped_string() (a full quoted value) and the exported
 * jw_raw_escaped_content() below (content only, no surrounding quotes
 * -- for splicing escaped text into the middle of a JSON string
 * literal that's already open, e.g. container-recipe secret
 * substitution, daemon/src/pkg.c's container_recipe_substitute_
 * secrets()) -- one escaping ruleset, never two drifting copies.
 */
static void jw_escape_content(struct json_writer *w, const char *s)
{
	char buf[8];

	for (; *s != '\0'; s++) {
		char c = *s;

		switch (c) {
		case '"': jw_raw(w, "\\\"", 2); break;
		case '\\': jw_raw(w, "\\\\", 2); break;
		case '\n': jw_raw(w, "\\n", 2); break;
		case '\t': jw_raw(w, "\\t", 2); break;
		case '\r': jw_raw(w, "\\r", 2); break;
		case '\b': jw_raw(w, "\\b", 2); break;
		case '\f': jw_raw(w, "\\f", 2); break;
		default:
			if ((unsigned char)c < 0x20) {
				snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
				jw_raw_str(w, buf);
			} else {
				jw_raw(w, &c, 1);
			}
		}
	}
}

static void jw_escaped_string(struct json_writer *w, const char *s)
{
	jw_raw(w, "\"", 1);
	jw_escape_content(w, s);
	jw_raw(w, "\"", 1);
}

/*
 * Public wrapper for jw_escape_content() -- appends s's escaped
 * CONTENT directly onto w's own growable buffer, no surrounding
 * quotes, no value-prefix/comma bookkeeping (unlike jw_str()). Callers
 * outside json.c that need to splice escaped text into the middle of
 * an already-open JSON string literal (rather than write a complete,
 * standalone value) use this instead of hand-rolling a second escaper.
 */
void jw_raw_escaped_content(struct json_writer *w, const char *s)
{
	jw_escape_content(w, s);
}

/*
 * Public wrapper for jw_raw() -- appends n bytes of s verbatim, no
 * escaping, no value-prefix/comma bookkeeping. Paired with
 * jw_raw_escaped_content() above by the same external callers: a
 * placeholder-substitution pass copies the JSON text BETWEEN tokens
 * verbatim (this function) and the substituted value's escaped
 * content AT each token (that one).
 */
void jw_raw_text(struct json_writer *w, const char *s, size_t n)
{
	jw_raw(w, s, n);
}

static void jw_value_prefix(struct json_writer *w)
{
	int d;

	if (w->depth == 0)
		return;
	d = w->depth - 1;
	if (w->stack[d] == JW_CONTAINER_ARR) {
		if (w->has_item[d])
			jw_raw(w, ",", 1);
		w->has_item[d] = 1;
	}
	/* JW_CONTAINER_OBJ: jw_key() already handled the comma and mark. */
}

void jw_obj_open(struct json_writer *w)
{
	jw_value_prefix(w);
	jw_raw(w, "{", 1);
	w->stack[w->depth] = JW_CONTAINER_OBJ;
	w->has_item[w->depth] = 0;
	w->depth++;
}

void jw_obj_close(struct json_writer *w)
{
	w->depth--;
	jw_raw(w, "}", 1);
}

void jw_arr_open(struct json_writer *w)
{
	jw_value_prefix(w);
	jw_raw(w, "[", 1);
	w->stack[w->depth] = JW_CONTAINER_ARR;
	w->has_item[w->depth] = 0;
	w->depth++;
}

void jw_arr_close(struct json_writer *w)
{
	w->depth--;
	jw_raw(w, "]", 1);
}

void jw_key(struct json_writer *w, const char *key)
{
	int d = w->depth - 1;

	if (w->has_item[d])
		jw_raw(w, ",", 1);
	w->has_item[d] = 1;
	jw_escaped_string(w, key);
	jw_raw(w, ":", 1);
}

void jw_str(struct json_writer *w, const char *s)
{
	jw_value_prefix(w);
	jw_escaped_string(w, s);
}

void jw_int(struct json_writer *w, long long v)
{
	char buf[32];

	jw_value_prefix(w);
	snprintf(buf, sizeof(buf), "%lld", v);
	jw_raw_str(w, buf);
}

/*
 * Every numeric field this daemon has ever written before this one
 * (container/host stats' own counters, sizes, timestamps) is a whole
 * number -- jw_int() alone sufficed. Host load average (GET
 * /system/stats, /proc/loadavg) is the first genuinely fractional
 * value ever needed here. "%.2f" matches /proc/loadavg's own real
 * precision (two decimal places) and every real value in that file is
 * always non-negative, so no sign/exponent handling is needed.
 */
void jw_num(struct json_writer *w, double v)
{
	char buf[32];

	jw_value_prefix(w);
	snprintf(buf, sizeof(buf), "%.2f", v);
	jw_raw_str(w, buf);
}

void jw_bool(struct json_writer *w, int b)
{
	jw_value_prefix(w);
	jw_raw_str(w, b ? "true" : "false");
}

void jw_null(struct json_writer *w)
{
	jw_value_prefix(w);
	jw_raw_str(w, "null");
}

/*
 * Writes an already-parsed value back out as JSON.
 *
 * The writer was build-only until now: everything it produced was
 * composed field by field from live state, so nothing ever needed to
 * round-trip a parsed tree. Editing one field of a stored request body
 * does (issue #92 -- attaching a volume to an existing container
 * rewrites that container's persisted body and must preserve every
 * other field of it exactly). The alternative was splicing the new
 * field into the stored text by hand, which is the kind of thing that
 * works until a value contains a brace.
 *
 * Numbers are the one lossy spot: the parser stores every number as a
 * double, so an integer is written back via jw_int() when it is exactly
 * integral (which every integer field in this API's bodies is) and via
 * jw_num()'s two decimal places otherwise. Nothing in a container
 * request body is a non-integral number, so this is exact in practice;
 * it is called out because it would not be for arbitrary JSON.
 */
void jw_value(struct json_writer *w, const struct json_value *v)
{
	size_t i;

	if (v == NULL) {
		jw_null(w);
		return;
	}
	switch (v->type) {
	case JSON_NULL:
		jw_null(w);
		break;
	case JSON_BOOL:
		jw_bool(w, v->u.boolean);
		break;
	case JSON_NUMBER:
		if (v->u.number == (double)(long long)v->u.number)
			jw_int(w, (long long)v->u.number);
		else
			jw_num(w, v->u.number);
		break;
	case JSON_STRING:
		jw_str(w, v->u.string);
		break;
	case JSON_ARRAY:
		jw_arr_open(w);
		for (i = 0; i < v->u.array.count; i++)
			jw_value(w, v->u.array.items[i]);
		jw_arr_close(w);
		break;
	case JSON_OBJECT:
		jw_obj_open(w);
		for (i = 0; i < v->u.object.count; i++) {
			jw_key(w, v->u.object.keys[i]);
			jw_value(w, v->u.object.values[i]);
		}
		jw_obj_close(w);
		break;
	}
}
