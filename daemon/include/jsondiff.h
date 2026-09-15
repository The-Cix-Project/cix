#ifndef JSONDIFF_H
#define JSONDIFF_H

#include <stddef.h>

struct json_value;

/*
 * A structural diff between two parsed JSON trees (ADR-0292).
 *
 * One implementation, used by both POST /v1/config/diff (which reports
 * the result) and POST /v1/config (which decides from it what to
 * apply), so the plan an operator is shown and the plan that runs can
 * never be two different computations.
 *
 * ARRAYS ARE THE WHOLE DIFFICULTY. Comparing two arrays by index is
 * correct only when nothing was inserted or removed; one insertion at
 * the front reports every later element as changed, which for a
 * 167-entry `packages` section is a diff nobody can read. So the
 * caller may declare an identity key -- the field (or comma-separated
 * fields) that names an element -- and elements are then matched by
 * that name rather than by position. The key comes from the
 * ConfigDocument schema, never from a guess here.
 *
 * Keyed matching needs the names to be unique on both sides. When they
 * are not, this falls back to index comparison for that array and says
 * so in `unkeyed_path` rather than reporting a silently wrong diff --
 * a duplicate name means the caller's declared key does not actually
 * identify an element, which is a fact worth surfacing.
 *
 * `from` and `to` point INTO the two trees passed in. Both must
 * outlive the result.
 */

#define JSONDIFF_PATH_MAX 256

enum jsondiff_op {
	JSONDIFF_ADD,     /* present in `supplied`, absent from `live` */
	JSONDIFF_REMOVE,  /* present in `live`, absent from `supplied` */
	JSONDIFF_REPLACE  /* present in both, different */
};

struct jsondiff_change {
	char path[JSONDIFF_PATH_MAX];
	enum jsondiff_op op;
	const struct json_value *from; /* NULL for ADD */
	const struct json_value *to;   /* NULL for REMOVE */
};

struct jsondiff {
	struct jsondiff_change *changes;
	int count;
	int cap;
	/*
	 * The cap exists because a request body is attacker-shaped input:
	 * a deeply nested document with thousands of differences would
	 * otherwise decide how much memory this allocates. Hitting it is
	 * reported, never silently swallowed -- a truncated diff that
	 * claimed to be complete would be the worst possible answer for a
	 * caller about to apply it.
	 */
	int truncated;
	/* "" unless a declared key was not unique; names the array. */
	char unkeyed_path[JSONDIFF_PATH_MAX];
};

/*
 * Compares `supplied` against `live`. `array_key` names the identity
 * field(s) of the TOP-LEVEL array, comma-separated for a composite
 * key; NULL or "" compares it by index. Nested arrays are always
 * compared by index -- no schema declares their keys, and inventing
 * one here would be this file guessing about a subsystem's shape.
 *
 * Returns 0 on success, -1 on allocation failure. A zero `count` means
 * the two trees are identical.
 */
int jsondiff_compute(const struct json_value *live, const struct json_value *supplied,
                     const char *array_key, struct jsondiff *out);

void jsondiff_free(struct jsondiff *d);

/* "add" / "remove" / "replace" -- the wire spelling. */
const char *jsondiff_op_name(enum jsondiff_op op);

/* Deep equality, the same comparison the diff itself uses -- exported
 * so a caller checking one field against its live value asks the same
 * question the plan did, rather than a second implementation of it. */
int jsondiff_equal(const struct json_value *a, const struct json_value *b);

#endif /* JSONDIFF_H */
