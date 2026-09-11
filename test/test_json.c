/*
 * test_json -- the \uXXXX escape, decoded to UTF-8 (#386).
 *
 * This parser used to decode \uXXXX to a single byte and reject
 * anything above 0xFF, which failed the WHOLE surrounding parse: one
 * escape anywhere in a body meant `400 invalid JSON body` for valid
 * RFC 8259 JSON. That mattered because ensure_ascii=True is the
 * DEFAULT for Python's json.dumps, so a client written the obvious way
 * -- in the language this project's own probe and deploy scripts use
 * -- produced bodies the API refused, naming the wrong thing.
 *
 * The two halves this pins down are the two that can regress quietly.
 * Below 0x80 the new encoder must produce the IDENTICAL single byte
 * the old one did, because that is the entire range this file's own
 * writer emits (\u00XX for control characters) -- getting that wrong
 * would break capture_output round-trips rather than throw an error.
 * And a surrogate pair must reassemble into one codepoint rather than
 * two replacement characters, which likewise fails as wrong bytes and
 * not as a refusal.
 *
 * A pure parse, no syscalls, so this can sit in SELFTESTS and run on
 * the box (#224).
 */
#include <stdio.h>
#include <string.h>

#include "json.h"

static int g_fail;

static void expect_str(const char *what, const char *body, const char *key, const char *want)
{
	struct json_value *root = json_parse(body, strlen(body));
	const struct json_value *v;
	const char *got;

	if (root == NULL) {
		fprintf(stderr, "FAIL: %s: json_parse() rejected %s\n", what, body);
		g_fail = 1;
		return;
	}
	v = json_object_get(root, key);
	got = json_as_string(v);
	if (got == NULL) {
		fprintf(stderr, "FAIL: %s: no string at key \"%s\"\n", what, key);
		g_fail = 1;
		json_free(root);
		return;
	}
	if (strcmp(got, want) != 0) {
		size_t i;

		fprintf(stderr, "FAIL: %s: got", what);
		for (i = 0; got[i] != '\0'; i++)
			fprintf(stderr, " %02x", (unsigned char)got[i]);
		fprintf(stderr, ", want");
		for (i = 0; want[i] != '\0'; i++)
			fprintf(stderr, " %02x", (unsigned char)want[i]);
		fprintf(stderr, "\n");
		g_fail = 1;
	}
	json_free(root);
}

static void expect_reject(const char *what, const char *body)
{
	struct json_value *root = json_parse(body, strlen(body));

	if (root != NULL) {
		fprintf(stderr, "FAIL: %s: json_parse() ACCEPTED %s\n", what, body);
		g_fail = 1;
		json_free(root);
	}
}

int main(void)
{
	/*
	 * ASCII and the control-character range: the old single-byte
	 * decode and the new UTF-8 encode must agree here exactly. This
	 * is the compatibility half.
	 */
	expect_str("ascii escape", "{\"s\":\"\\u0041\"}", "s", "A");
	expect_str("control char", "{\"s\":\"a\\u001bb\"}", "s", "a\033b");
	expect_str("nul-adjacent low byte", "{\"s\":\"\\u0001\"}", "s", "\001");

	/* Two bytes: Latin-1 supplement, the range that used to decode wrong. */
	expect_str("two-byte", "{\"s\":\"caf\\u00e9\"}", "s", "caf\xc3\xa9");

	/* Three bytes: box drawing, the character that actually filed #386. */
	expect_str("three-byte", "{\"s\":\"\\u2500\"}", "s", "\xe2\x94\x80");

	/* Four bytes, via a surrogate pair: U+1F600. */
	expect_str("surrogate pair", "{\"s\":\"\\ud83d\\ude00\"}", "s", "\xf0\x9f\x98\x80");

	/* Escapes and raw UTF-8 must reach the same bytes. */
	expect_str("raw utf-8 unchanged", "{\"s\":\"caf\xc3\xa9\"}", "s", "caf\xc3\xa9");

	/* The rest of the escape set still works alongside it. */
	expect_str("mixed escapes", "{\"s\":\"a\\n\\u00e9\\t\\\"b\\\"\"}", "s", "a\n\xc3\xa9\t\"b\"");

	/* Malformed surrogates are a parse failure, not a silent replacement. */
	expect_reject("lone high surrogate", "{\"s\":\"\\ud83d\"}");
	expect_reject("lone low surrogate", "{\"s\":\"\\ude00\"}");
	expect_reject("high surrogate then plain", "{\"s\":\"\\ud83dA\"}");
	expect_reject("high surrogate then non-low", "{\"s\":\"\\ud83d\\u0041\"}");
	expect_reject("short escape", "{\"s\":\"\\u41\"}");
	expect_reject("non-hex escape", "{\"s\":\"\\u00zz\"}");

	if (g_fail)
		return 1;
	printf("test_json: \\uXXXX decodes to UTF-8, surrogate pairs pair, bad ones refused\n");
	return 0;
}
