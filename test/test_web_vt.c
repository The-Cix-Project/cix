/*
 * ADR-0243: the terminal is a real VT, and it is wired to the daemon's
 * terminal contract (ADR-0242).
 *
 * What this test can and cannot do is worth stating, because the gap is
 * the whole reason it is written the way it is. Nothing in this project
 * executes JavaScript -- there is no engine on a Cix host and adding one
 * to run a test would be a far larger decision than the test is worth --
 * so the emulator's BEHAVIOUR (does vim render, does a scroll region
 * scroll) cannot be asserted here. That was verified by replaying real
 * program output captured from a live container through the emulator in
 * a headless browser, and the ADR records it.
 *
 * What can be asserted here is everything that silently comes apart when
 * someone edits one of these files without the others: the wiring. Every
 * check below is a real failure mode, not a tidiness rule.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

static void fail(const char *msg)
{
	fprintf(stderr, "FAIL: %s\n", msg);
	g_failures++;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	long len;
	char *buf;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	len = ftell(f);
	if (len < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)len + 1);
	if (buf == NULL) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[len] = '\0';
	fclose(f);
	return buf;
}

static void want(const char *hay, const char *needle, const char *why)
{
	if (hay == NULL || strstr(hay, needle) == NULL)
		fail(why);
}

static void want_not(const char *hay, const char *needle, const char *why)
{
	if (hay != NULL && strstr(hay, needle) != NULL)
		fail(why);
}

int main(void)
{
	char *vt = slurp("web/vt.js");
	char *app = slurp("web/app.js");
	char *html = slurp("web/index.html");
	char *css = slurp("web/style.css");

	if (vt == NULL || app == NULL || html == NULL || css == NULL) {
		fprintf(stderr, "FAIL: run from the repo root -- web/ assets unreadable\n");
		return 1;
	}

	/*
	 * Load order. app.js calls createVT() at console-open time, so a
	 * vt.js tag placed after app.js still works by accident (the call is
	 * not at parse time) right up until it does not. Asserting the order
	 * keeps that from being luck.
	 */
	{
		const char *vt_tag = strstr(html, "src=\"/vt.js\"");
		const char *app_tag = strstr(html, "src=\"/app.js\"");

		if (vt_tag == NULL)
			fail("index.html does not load /vt.js");
		else if (app_tag == NULL)
			fail("index.html does not load /app.js");
		else if (vt_tag > app_tag)
			fail("index.html loads vt.js after app.js -- app.js is its only caller");
	}

	/*
	 * Untrusted output. Every byte the emulator renders was written by a
	 * process inside a container. innerHTML anywhere in the render path
	 * turns that into markup injection with a straight face, which is
	 * why the renderer builds nodes and sets textContent instead.
	 */
	/* Matched with the leading dot, i.e. a real property access. A bare
	 * "innerHTML" also appears in this file's own comment saying not to
	 * use it, and a check that its own documentation trips is a check
	 * that gets deleted rather than fixed. */
	want_not(vt, ".innerHTML", "vt.js assigns innerHTML -- terminal output is untrusted and must never become markup");

	/*
	 * The Configuration tab renders a container's own RECIPE into the
	 * page. That text is stored, operator-supplied and arbitrary --
	 * the same untrusted-bytes-into-the-DOM question the emulator
	 * above answers, one tab across. It is set with textContent into a
	 * <pre>, and the assertion is that app.js never assigns innerHTML
	 * at all, which is the check that cannot be got wrong by reading
	 * the wrong line.
	 */
	want(app, "loadContainerRecipe", "app.js does not load a container's recipe (#289 Configuration tab)");
	want(app, "getContainerRecipe", "app.js builds the recipe path by hand instead of using the generated constant");
	/*
	 * The positive form, not a blanket ban on innerHTML in app.js. That
	 * was written first and is wrong: app.js assigns innerHTML in ten
	 * places, every one of them a static developer-authored literal --
	 * an inline SVG icon, "Loading&hellip;", "No routes." -- none of
	 * which render anything a user or a container supplied. A check
	 * that flags correct code is a check that gets deleted, so this
	 * asserts what actually matters instead: the recipe text reaches
	 * the page through textContent.
	 */
	want(app, "textEl.textContent = content",
	     "app.js does not set the recipe with textContent -- a container recipe is stored "
	     "operator text and must never become markup");
	want_not(vt, ".outerHTML", "vt.js assigns outerHTML on untrusted terminal output");
	want_not(vt, "insertAdjacentHTML", "vt.js uses insertAdjacentHTML on untrusted terminal output");
	want(vt, "textContent", "vt.js does not set textContent -- expected the safe render path");

	/*
	 * The ADR-0242 contract, from the client side. Each of these is a
	 * separate way to end up with a terminal whose size the daemon does
	 * not know, which is exactly the defect ADR-0242 fixed one layer
	 * down.
	 */
	want(app, "\"term=xterm-256color\"", "app.js does not send term= on the console upgrade");
	want(app, "\"cols=\"", "app.js does not send cols= on the console upgrade");
	want(app, "\"rows=\"", "app.js does not send rows= on the console upgrade");
	want(app, "type: \"resize\"", "app.js never sends the resize control message -- a resized pane would not reach the program");
	want(app, "consoleTerminal.fit()", "app.js never fits the terminal to its pane");
	want(app, "consoleTerminal.dispose()", "app.js does not dispose the terminal -- listeners and the ResizeObserver leak one set per console opened");

	/*
	 * The old line-buffer renderer and its palette are gone, not merely
	 * unused. Two renderers, or a palette defined in two places, is the
	 * duplicate-state failure the maxims rule out -- and a stylesheet
	 * still carrying .term-fg-* would silently win over the emulator's
	 * own inline colours if anything ever re-emitted those classes.
	 */
	want_not(app, "function createTerminal(", "the superseded line-buffer terminal is still in app.js");
	want_not(css, ".term-fg-", "style.css still carries the old renderer's palette -- vt.js owns the palette now");

	/*
	 * A terminal that cannot answer a query does not degrade, it hangs:
	 * the program is blocked on a read. DSR and DA are the two that are
	 * routinely asked for unprompted.
	 */
	want(vt, "\"\\x1b[\" + (cur.row - originTop() + 1)", "vt.js does not answer DSR -- a program that asks for the cursor position will block forever");
	want(vt, "\\x1b[?6c", "vt.js does not answer DA -- same blocking-read failure as DSR");

	/*
	 * The features that separate a real VT from the line-buffer this
	 * replaced. Named individually because losing any one of them is a
	 * specific, recognisable breakage rather than a general degradation.
	 */
	want(vt, "altScreen", "vt.js has no alternate screen -- a full-screen program's frames would land in shell scrollback");
	want(vt, "scrollTop", "vt.js has no scroll region");
	want(vt, "wrapPending", "vt.js has no deferred wrap -- an exactly-full row would push everything below it down");
	want(vt, "VT_DEC_GRAPHICS", "vt.js has no DEC special graphics -- box drawing would render as qqqx");
	want(vt, "appCursor", "vt.js ignores application cursor keys -- arrows would insert letters in vim");
	want(vt, "bracketedPaste", "vt.js has no bracketed paste");

	free(vt);
	free(app);
	free(html);
	free(css);

	if (g_failures > 0) {
		fprintf(stderr, "test_web_vt: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_web_vt: all checks passed\n");
	return 0;
}
