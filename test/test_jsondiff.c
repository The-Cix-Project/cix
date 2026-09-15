/*
 * test_jsondiff -- the diff engine behind POST /v1/config (ADR-0292).
 *
 * The whole value of that endpoint is that the plan an operator is
 * shown is the plan that runs, so a wrong diff is not a cosmetic bug:
 * it is a host changed in a way nobody agreed to, or a change refused
 * for a difference that is not there.
 *
 * What this pins down is the array behaviour, because that is where a
 * diff can be wrong while looking right. Matching elements by position
 * reports an insertion at the front as "everything after it changed",
 * which for a 167-entry section is a diff nobody can read and nobody
 * would trust. Matching them by a declared identity key fixes that --
 * and introduces its own failure, since a key that does not uniquely
 * name an element would pair up the wrong ones, authoritatively. So
 * the fallback to positional comparison, and the fact that it is
 * REPORTED rather than silent, are tested as hard as the happy path.
 *
 * Pure computation, no syscalls, so it sits in SELFTESTS and runs on
 * the box (#224) rather than only where the tree happens to build.
 */
#include <stdio.h>
#include <string.h>

#include "json.h"
#include "jsondiff.h"

static int g_fail;

static void fail(const char *what, const char *why)
{
	fprintf(stderr, "FAIL: %s: %s\n", what, why);
	g_fail = 1;
}

static struct json_value *parse(const char *text)
{
	struct json_value *v = json_parse(text, strlen(text));

	if (v == NULL)
		fail("fixture", text);
	return v;
}

/*
 * Every expectation is written as the exact set of "path op" pairs the
 * diff must produce -- order-independent, so a change to how the walk
 * is ordered does not fail a test about what it found.
 */
static void expect_ignoring(const char *what, const char *live_text, const char *sup_text,
                            const char *key, const char *ignore, const char *const *want,
                            int want_n)
{
	struct json_value *live = parse(live_text);
	struct json_value *sup = parse(sup_text);
	struct jsondiff d;
	int i, j;

	if (live == NULL || sup == NULL)
		return;
	if (jsondiff_compute(live, sup, key, ignore, &d) != 0) {
		fail(what, "jsondiff_compute() failed");
		json_free(live);
		json_free(sup);
		return;
	}
	if (d.count != want_n) {
		fprintf(stderr, "FAIL: %s: expected %d changes, got %d:\n", what, want_n, d.count);
		for (i = 0; i < d.count; i++)
			fprintf(stderr, "        %s %s\n", d.changes[i].path,
			        jsondiff_op_name(d.changes[i].op));
		g_fail = 1;
	} else {
		for (i = 0; i < want_n; i++) {
			int found = 0;

			for (j = 0; j < d.count; j++) {
				char got[320];

				snprintf(got, sizeof(got), "%s %s", d.changes[j].path,
				         jsondiff_op_name(d.changes[j].op));
				if (strcmp(got, want[i]) == 0) {
					found = 1;
					break;
				}
			}
			if (!found) {
				fprintf(stderr, "FAIL: %s: expected change \"%s\", not produced\n",
				        what, want[i]);
				for (j = 0; j < d.count; j++)
					fprintf(stderr, "        got: %s %s\n", d.changes[j].path,
					        jsondiff_op_name(d.changes[j].op));
				g_fail = 1;
			}
		}
	}
	jsondiff_free(&d);
	json_free(live);
	json_free(sup);
}

static void expect(const char *what, const char *live_text, const char *sup_text, const char *key,
                   const char *const *want, int want_n)
{
	expect_ignoring(what, live_text, sup_text, key, "", want, want_n);
}

static void expect_fallback(const char *what, const char *live_text, const char *sup_text,
                            const char *key, int want_fallback)
{
	struct json_value *live = parse(live_text);
	struct json_value *sup = parse(sup_text);
	struct jsondiff d;

	if (live == NULL || sup == NULL)
		return;
	if (jsondiff_compute(live, sup, key, "", &d) != 0) {
		fail(what, "jsondiff_compute() failed");
		json_free(live);
		json_free(sup);
		return;
	}
	if ((d.unkeyed_path[0] != '\0') != want_fallback)
		fail(what, want_fallback ? "expected a reported fallback to positional comparison"
		                         : "fell back to positional comparison unexpectedly");
	jsondiff_free(&d);
	json_free(live);
	json_free(sup);
}

int main(void)
{
	{
		static const char *const want[] = { "nameservers[1] replace" };

		expect("one element of a scalar list", "{\"nameservers\":[\"1.1.1.1\",\"8.8.8.8\"]}",
		       "{\"nameservers\":[\"1.1.1.1\",\"9.9.9.9\"]}", "", want, 1);
	}
	{
		static const char *const want[] = { "enabled replace", "compressor replace" };

		expect("two fields of an object",
		       "{\"enabled\":true,\"max_pool_percent\":20,\"compressor\":\"lzo\"}",
		       "{\"enabled\":false,\"max_pool_percent\":20,\"compressor\":\"zstd\"}", "",
		       want, 2);
	}
	{
		static const char *const want[] = { "ref remove", "branch add" };

		expect("a renamed field is a removal and an addition",
		       "{\"url\":\"u\",\"ref\":\"master\"}", "{\"url\":\"u\",\"branch\":\"master\"}",
		       "", want, 2);
	}
	expect("an identical document has no changes",
	       "{\"a\":1,\"b\":[{\"name\":\"x\",\"v\":2}],\"c\":{\"d\":null}}",
	       "{\"a\":1,\"b\":[{\"name\":\"x\",\"v\":2}],\"c\":{\"d\":null}}", "", NULL, 0);
	{
		static const char *const want[] = { "[b].image replace" };

		/*
		 * The case positional comparison gets wrong: an element added
		 * at the FRONT shifts every later one. Keyed, this is one
		 * addition and one real change; positionally it would be
		 * three replacements and an addition.
		 */
		expect("a keyed array reports only what actually differs",
		       "[{\"name\":\"b\",\"image\":\"old\"},{\"name\":\"c\",\"image\":\"c\"}]",
		       "[{\"name\":\"b\",\"image\":\"new\"},{\"name\":\"c\",\"image\":\"c\"}]",
		       "name", want, 1);
	}
	{
		static const char *const want[] = { "[a] add", "[c] remove" };

		expect("a keyed array names an added and a removed element",
		       "[{\"name\":\"b\"},{\"name\":\"c\"}]", "[{\"name\":\"a\"},{\"name\":\"b\"}]",
		       "name", want, 2);
	}
	{
		static const char *const want[] = { "[base,tcc].version replace" };

		expect("a composite key names an element by both fields",
		       "[{\"image\":\"base\",\"name\":\"tcc\",\"version\":\"1\"},"
		       "{\"image\":\"dev\",\"name\":\"tcc\",\"version\":\"1\"}]",
		       "[{\"image\":\"base\",\"name\":\"tcc\",\"version\":\"2\"},"
		       "{\"image\":\"dev\",\"name\":\"tcc\",\"version\":\"1\"}]", "image,name", want,
		       1);
	}
	{
		/*
		 * The same two documents the composite key handles cleanly,
		 * compared under a key that does NOT identify an element.
		 * Pairing them by name would compare base/tcc against dev/tcc
		 * and report a difference that is not there, so this must
		 * fall back -- and say so.
		 */
		expect_fallback("a duplicate key falls back to position",
		                "[{\"image\":\"base\",\"name\":\"tcc\"},{\"image\":\"dev\",\"name\":"
		                "\"tcc\"}]",
		                "[{\"image\":\"base\",\"name\":\"tcc\"},{\"image\":\"dev\",\"name\":"
		                "\"tcc\"}]",
		                "name", 1);
		expect_fallback("a unique key does not fall back",
		                "[{\"image\":\"base\",\"name\":\"tcc\"}]",
		                "[{\"image\":\"base\",\"name\":\"tcc\"}]", "image,name", 0);
		expect_fallback("an element missing the key field falls back",
		                "[{\"name\":\"a\"},{\"other\":\"b\"}]",
		                "[{\"name\":\"a\"},{\"other\":\"b\"}]", "name", 1);
	}
	{
		static const char *const want[] = { " replace" };

		/*
		 * A difference at the ROOT has an empty path, and that is the
		 * layer boundary: this engine knows nothing about
		 * configuration, so naming the root "(section)" is
		 * api_config.c's job when it renders the change. Asserted here
		 * because the two layers agreeing about it is what makes a
		 * root-level difference readable at all -- and they did not
		 * agree the first time this ran.
		 */
		expect("a difference at the root has an empty path", "{\"a\":1}", "[1]", "", want, 1);
	}
	{
		static const char *const want[] = { "[x].nets[1] add" };

		/* A nested array has no declared key, so it is positional --
		 * which is correct, and is why only the TOP level takes one. */
		expect("a nested array is compared by position",
		       "[{\"name\":\"x\",\"nets\":[\"a\"]}]",
		       "[{\"name\":\"x\",\"nets\":[\"a\",\"b\"]}]", "name", want, 1);
	}
	{
		static const char *const want[] = { "n replace" };

		expect("numbers compare by value, not by spelling", "{\"n\":1}", "{\"n\":2}", "",
		       want, 1);
		expect("the same number written differently is not a change", "{\"n\":1}",
		       "{\"n\":1.0}", "", NULL, 0);
	}
	{
		/*
		 * An observed member is left out of the comparison entirely
		 * (ADR-0292/#471). Without this, a document fetched before
		 * something else moved differs in a field nobody can set --
		 * and since a section that cannot be applied refuses the
		 * whole request, one container restarting made a whole
		 * document unusable.
		 */
		expect_ignoring("an observed member is not a difference",
		                "{\"enabled\":true,\"kernel\":{\"enabled\":true}}",
		                "{\"enabled\":true,\"kernel\":{\"enabled\":false}}", "",
		                "kernel", NULL, 0);
		expect_ignoring("an observed member may be absent entirely",
		                "{\"enabled\":true,\"pid\":41}", "{\"enabled\":true}", "", "pid",
		                NULL, 0);
	}
	{
		static const char *const want[] = { "enabled replace" };

		expect_ignoring("ignoring one member does not hide the others",
		                "{\"enabled\":true,\"pid\":41}", "{\"enabled\":false,\"pid\":9}",
		                "", "pid", want, 1);
	}
	{
		/* Matched at any depth: a service's own `state` and `pid` are
		 * nested two levels inside a container element, and naming
		 * them once has to cover both. */
		expect_ignoring("an observed member is ignored at any depth",
		                "[{\"name\":\"web\",\"services\":[{\"name\":\"sshd\",\"state\":"
		                "\"running\",\"pid\":7}]}]",
		                "[{\"name\":\"web\",\"services\":[{\"name\":\"sshd\",\"state\":"
		                "\"exited\",\"pid\":9}]}]",
		                "name", "state,pid", NULL, 0);
	}
	{
		static const char *const want[] = { "[web].services[0].cmd replace" };

		expect_ignoring("and the declaration does not swallow its siblings",
		                "[{\"name\":\"web\",\"services\":[{\"name\":\"sshd\",\"state\":"
		                "\"running\",\"cmd\":\"a\"}]}]",
		                "[{\"name\":\"web\",\"services\":[{\"name\":\"sshd\",\"state\":"
		                "\"exited\",\"cmd\":\"b\"}]}]",
		                "name", "state", want, 1);
	}
	{
		/* A prefix is not a match: "pid" must not silence "pids_max". */
		static const char *const want[] = { "pids_max replace" };

		expect_ignoring("a listed name matches whole members only",
		                "{\"pid\":1,\"pids_max\":10}", "{\"pid\":2,\"pids_max\":20}", "",
		                "pid", want, 1);
	}
	if (g_fail) {
		fprintf(stderr, "test_jsondiff: FAILED\n");
		return 1;
	}
	printf("test_jsondiff: all checks passed\n");
	return 0;
}
