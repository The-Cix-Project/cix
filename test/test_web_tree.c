/*
 * The dashboard's navigation, and the one failure it kept having.
 *
 * The tree listed 56 destinations that resolved to 18 views, so two
 * thirds of its leaves were tabs on somebody else's page -- and the
 * tree and the tab bars were two hand-maintained lists that nothing
 * forced to agree. They didn't: `recipes` and `cat-recipes` were the
 * same thing spelled twice, and a tree leaf could route to a page whose
 * tab bar had no such tab, which renders as "the page ignored my
 * click".
 *
 * This asserts the wiring that a person cannot hold in their head:
 *
 *  1. Every tree hash routes somewhere (CATEGORY_VIEWS knows it).
 *  2. Every route target is a real <section> in index.html.
 *  3. Every tree hash pointing at a TABBED page has a tab of the same
 *     name on that page -- the check that catches `recipes` against
 *     `cat-recipes` before anyone clicks it.
 *
 * It is deliberately a text scan of both files rather than anything
 * that runs them: the failure being caught is two files disagreeing,
 * which is visible without a browser and worth catching in a build
 * rather than in a screenshot.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *fmt, const char *a, const char *b)
{
	fprintf(stderr, "FAIL: ");
	fprintf(stderr, fmt, a, b);
	fprintf(stderr, "\n");
	g_failures++;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long n;
	char *buf;

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot open %s\n", path);
		exit(1);
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n + 1);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "FAIL: cannot read %s\n", path);
		exit(1);
	}
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* The view id CATEGORY_VIEWS maps `hash` to, or NULL. */
static int route_target(const char *app, const char *hash, char *out, size_t out_size)
{
	const char *tbl = strstr(app, "const CATEGORY_VIEWS = {");
	const char *end;
	const char *p;
	char needle[128];

	if (tbl == NULL)
		return -1;
	end = strstr(tbl, "\n};");
	snprintf(needle, sizeof(needle), "\"%s\": \"view-", hash);
	p = strstr(tbl, needle);
	if (p == NULL || (end != NULL && p > end)) {
		/* unquoted key form: `containers: "view-containers",` */
		snprintf(needle, sizeof(needle), "\n\t%s: \"view-", hash);
		p = strstr(tbl, needle);
		if (p == NULL || (end != NULL && p > end))
			return -1;
	}
	p = strstr(p, "\"view-") + 1;
	{
		size_t i = 0;

		while (p[i] != '"' && i + 1 < out_size) {
			out[i] = p[i];
			i++;
		}
		out[i] = '\0';
	}
	return 0;
}

/* Does index.html's <section id="view"> carry a tab bar, and if so does
 * it have a button for `tab`? */
static int section_has_tab(const char *html, const char *view, const char *tab, int *tabbed)
{
	char open[128], needle[160];
	const char *s, *e;

	*tabbed = 0;
	snprintf(open, sizeof(open), "<section class=\"view\" id=\"%s\"", view);
	s = strstr(html, open);
	if (s == NULL)
		return -1; /* no such section at all */
	e = strstr(s + 1, "<section class=\"view\"");
	if (strstr(s, "class=\"tab-bar\"") == NULL ||
	    (e != NULL && strstr(s, "class=\"tab-bar\"") > e))
		return 1; /* untabbed page: nothing to match */
	*tabbed = 1;
	snprintf(needle, sizeof(needle), "data-tab=\"%s\"", tab);
	{
		const char *hit = strstr(s, needle);

		if (hit != NULL && (e == NULL || hit < e))
			return 1;
	}
	return 0;
}

int main(void)
{
	char *app = slurp("web/app.js");
	char *html = slurp("web/index.html");
	const char *p = app;
	const char *tree = strstr(app, "const topLevel = [");
	int checked = 0, tabbed_checked = 0;
	char parent_view[64] = "";
	char parent_label[64] = "";
	int parent_is_group = 0;

	printf("test_web_tree\n");
	if (tree == NULL) {
		fprintf(stderr, "FAIL: no topLevel tree found in web/app.js\n");
		return 1;
	}

	for (p = tree; (p = strstr(p, "hash: \"")) != NULL; p++) {
		char hash[96];
		char view[64];
		size_t i = 0;
		int tabbed = 0;
		int r;
		int is_child = 0;

		/*
		 * A child entry is indented deeper than a top-level page. Its
		 * route must land on its OWN parent's page -- a child that
		 * navigates somewhere else is precisely the lie this tree was
		 * rebuilt to remove, and the first version of this test missed
		 * one (Volumes, listed under Storage, routed to a page of its
		 * own).
		 */
		{
			const char *ls = p;

			while (ls > app && ls[-1] != '\n')
				ls--;
			is_child = strncmp(ls, "\t\t\t\t{", 5) == 0;
		}
		/*
		 * A page node is itself selectable and lands on its first tab,
		 * so a child repeating its parent's label is that same
		 * destination listed twice -- "Networks > Networks". Caught
		 * here because it reads as a mistake to everyone who sees it
		 * and as nothing at all to whoever added it.
		 */
		{
			const char *lb = p;
			char label[64];
			size_t li = 0;

			while (lb > tree && strncmp(lb, "label: \"", 8) != 0)
				lb--;
			lb += strlen("label: \"");
			while (lb[li] != '"' && li + 1 < sizeof(label)) {
				label[li] = lb[li];
				li++;
			}
			label[li] = '\0';
			if (is_child) {
				if (strcmp(label, parent_label) == 0)
					fail("tree child \"%s\" repeats its parent's own label -- the parent is "
					     "already selectable and lands there%s",
					     label, "");
			} else {
				snprintf(parent_label, sizeof(parent_label), "%s", label);
			}
		}
		p += strlen("hash: \"");
		while (p[i] != '"' && i + 1 < sizeof(hash)) {
			hash[i] = p[i];
			i++;
		}
		hash[i] = '\0';
		/* Per-item leaves ("containers/<name>") are built from live
		 * data and route through DETAIL_VIEWS, not CATEGORY_VIEWS. */
		if (strchr(hash, '/') != NULL || strstr(hash, "encodeURI") != NULL)
			continue; /* a live item, built from live data */
		checked++;

		if (route_target(app, hash, view, sizeof(view)) != 0) {
			fail("tree hash \"%s\" is not in CATEGORY_VIEWS -- clicking it goes nowhere%s",
			     hash, "");
			continue;
		}
		r = section_has_tab(html, view, hash, &tabbed);
		if (r < 0) {
			fail("tree hash \"%s\" routes to %s, which is not a section in index.html", hash,
			     view);
			continue;
		}
		if (is_child && !parent_is_group) {
			/*
			 * Tabs live on the page; a PAGE's tree children are the
			 * live things it contains, which are always "<page>/<name>"
			 * routes. A plain hash here would be a tab listed in the
			 * tree as well as on the page -- the duplication this whole
			 * rework removed.
			 */
			fail("tree child \"%s\" under page %s is a tab, not a live item -- tabs belong on "
			     "the page, not in the tree",
			     hash, parent_view);
		} else if (!is_child) {
			const char *ls = p;
			const char *blk;

			snprintf(parent_view, sizeof(parent_view), "%s", view);
			/* A group node says so explicitly. Its children are pages
			 * of their own and are not expected to share its view. */
			while (ls > app && strncmp(ls, "\n\t\t{", 4) != 0)
				ls--;
			blk = strstr(ls, "hash:");
			parent_is_group = blk != NULL && strstr(ls, "group: true") != NULL &&
			                  strstr(ls, "group: true") < blk;
		}
		if (tabbed) {
			tabbed_checked++;
			if (r == 0)
				fail("tree hash \"%s\" routes to tabbed page %s, which has no tab of that "
				     "name -- the click would land on the page and change nothing",
				     hash, view);
		}
	}

	/*
	 * Every view id app.js reaches for must be a real section.
	 *
	 * This is not hypothetical: selectCatalogueTab() kept asking for
	 * "view-recipes" after that section became "view-pipeline". It
	 * returns silently on a null lookup, so the tab never switched and
	 * four pages sat on "Loading" forever -- a dead string that no
	 * compiler and no route check would ever notice.
	 */
	for (p = app; (p = strstr(p, "getElementById(\"view-")) != NULL; p++) {
		char vid[64];
		char open[128];
		size_t i = 0;

		p += strlen("getElementById(\"");
		while (p[i] != '"' && i + 1 < sizeof(vid)) {
			vid[i] = p[i];
			i++;
		}
		vid[i] = '\0';
		snprintf(open, sizeof(open), "<section class=\"view\" id=\"%s\"", vid);
		if (strstr(html, open) == NULL)
			fail("app.js asks for element \"%s\", which is not a section in index.html -- "
			     "the lookup returns null and whatever it guarded silently does nothing%s",
			     vid, "");
	}

	if (checked == 0) {
		fprintf(stderr, "FAIL: parsed no tree hashes at all -- this test is not testing\n");
		return 1;
	}
	if (g_failures > 0) {
		printf("test_web_tree: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("  %d tree destinations checked, %d of them against a tab bar\n", checked,
	        tabbed_checked);
	printf("test_web_tree: all checks passed\n");
	return 0;
}
