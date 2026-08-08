#ifndef JSON_H
#define JSON_H

#include <stddef.h>

/*
 * Minimal, generic JSON support -- not a full RFC 8259 implementation.
 * \uXXXX escapes are an explicit, documented scope boundary: neither
 * parsing nor writing supports them, since no field in this project's
 * API needs one. Every other basic escape (\" \\ \/ \n \t \r \b \f)
 * is handled on both sides.
 */

enum json_type {
	JSON_NULL,
	JSON_BOOL,
	JSON_NUMBER,
	JSON_STRING,
	JSON_ARRAY,
	JSON_OBJECT
};

struct json_value {
	enum json_type type;
	union {
		int boolean;
		double number;
		char *string;
		struct {
			struct json_value **items;
			size_t count;
		} array;
		struct {
			char **keys;
			struct json_value **values;
			size_t count;
		} object;
	} u;
};

/*
 * Parses text (len bytes, need not be NUL-terminated) into a tree.
 * Returns NULL on any malformed input; nothing is leaked on failure.
 * Caller owns a non-NULL result and must free it with json_free().
 */
struct json_value *json_parse(const char *text, size_t len);

void json_free(struct json_value *v);

/* NULL if obj isn't a JSON_OBJECT or key isn't present. */
const struct json_value *json_object_get(const struct json_value *obj, const char *key);

/* NULL/0 if v is NULL or not the expected type. */
const char *json_as_string(const struct json_value *v);
double json_as_number(const struct json_value *v);

/* --- Writer: builds JSON text into a dynamically-growing buffer --- */

#define JW_MAX_DEPTH 16

enum jw_container { JW_CONTAINER_OBJ, JW_CONTAINER_ARR };

struct json_writer {
	char *buf;
	size_t len;
	size_t cap;
	enum jw_container stack[JW_MAX_DEPTH];
	int has_item[JW_MAX_DEPTH];
	int depth;
};

void jw_init(struct json_writer *w);
void jw_free(struct json_writer *w);

void jw_obj_open(struct json_writer *w);
void jw_obj_close(struct json_writer *w);
void jw_arr_open(struct json_writer *w);
void jw_arr_close(struct json_writer *w);

/* Writes `"key":`. Must be called only while the innermost open
 * container is an object, immediately followed by exactly one
 * value-writing call (a scalar, or jw_obj_open/jw_arr_open). */
void jw_key(struct json_writer *w, const char *key);

void jw_str(struct json_writer *w, const char *s);
void jw_int(struct json_writer *w, long long v);
/* Fixed two-decimal-place formatting -- see jw_num()'s own comment in
 * json.c for why (matches /proc/loadavg's real precision, and every
 * value this project has ever needed to write with it is
 * non-negative). Not a general-purpose float writer. */
void jw_num(struct json_writer *w, double v);
void jw_bool(struct json_writer *w, int b);
void jw_null(struct json_writer *w);

#endif /* JSON_H */
