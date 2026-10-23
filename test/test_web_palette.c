/*
 * The dashboard's data-series palette, measured (#601).
 *
 * A series colour says which SERIES a line or segment is, and must be
 * told apart from its neighbour by everyone who reads the chart. That is
 * a computed property, not a matter of taste, and until this gate it was
 * asserted: --series-2 was Nickel, the dark theme's --muted, and
 * --series-3 was Copper, the brand accent.
 *
 * For each theme this reads the series slots and the chart surface out
 * of web/style.css, and checks the categorical rules of the dataviz
 * method, the same arithmetic as its validate_palette.js:
 *
 *  1. Lightness: OKLCH L within the theme's band (light 0.43-0.77, dark
 *     0.48-0.67).
 *  2. Chroma: OKLCH C >= 0.10, or the hue reads as grey.
 *  3. Colour-vision deficiency: adjacent slots >= 8 apart (OKLab delta E
 *     x100) under protanopia and deuteranopia, simulated with Machado,
 *     Oliveira and Fernandes 2009 at severity 1.0, which the threshold is
 *     calibrated to.
 *  4. Normal vision: adjacent slots >= 15 apart, unsimulated.
 *  5. Contrast: each slot >= 3:1 (WCAG) against --bg, the canvas the
 *     charts draw on (.stats-card canvas).
 *
 * And two of this platform's own:
 *
 *  6. A series is never a reserved colour. --text, --muted, --accent,
 *     --ok, --paused and --error each mean something on every page, so a
 *     slot must sit >= 8 from each of them (this gate's choice of floor:
 *     the CVD target above, the distance the method calls telling two
 *     colours apart).
 *  7. Each theme is written twice -- the prefers-color-scheme block and
 *     the data-theme block the header's toggle sets -- and the two copies
 *     must resolve to the same colours.
 *
 * It prints every number, so a palette change is chosen from this
 * output rather than by eye. A text scan of the stylesheet, like
 * test_web_tree: nothing here renders anything.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BAND_LIGHT_LO 0.43
#define BAND_LIGHT_HI 0.77
#define BAND_DARK_LO 0.48
#define BAND_DARK_HI 0.67
#define CHROMA_FLOOR 0.10
#define CVD_TARGET 8.0
#define NORMAL_FLOOR 15.0
#define CONTRAST_MIN 3.0
#define RESERVED_MIN 8.0
#define MAX_SLOTS 8

static int g_failures;

static void fail(const char *fmt, const char *a, double v)
{
	fprintf(stderr, "FAIL: ");
	fprintf(stderr, fmt, a, v);
	fprintf(stderr, "\n");
	g_failures++;
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "r");
	char *buf;
	long n;

	if (f == NULL)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return NULL;
	}
	buf = malloc((size_t)n + 1);
	if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		fclose(f);
		return NULL;
	}
	buf[n] = '\0';
	fclose(f);
	return buf;
}

/* A block of custom properties: from the '{' after where to the first
 * '}' after that. The token blocks hold no nested braces. */
struct block {
	const char *start;
	const char *end;
};

static int block_at(const char *css, const char *marker, struct block *b)
{
	const char *m = strstr(css, marker);
	const char *open;

	if (m == NULL)
		return -1;
	open = strchr(m, '{');
	if (open == NULL)
		return -1;
	b->start = open + 1;
	b->end = strchr(b->start, '}');
	return b->end == NULL ? -1 : 0;
}

/* The block whose body contains marker: back to its '{', on to its '}'. */
static int block_around(const char *css, const char *marker, struct block *b)
{
	const char *m = strstr(css, marker);
	const char *p;

	if (m == NULL)
		return -1;
	for (p = m; p > css && *p != '{'; p--)
		;
	if (*p != '{')
		return -1;
	b->start = p + 1;
	b->end = strchr(m, '}');
	return b->end == NULL ? -1 : 0;
}

/* The value of --name in b, trimmed, into out. 0, or -1 when absent. A
 * declaration starts a line, so a mention inside a comment or another
 * value does not count. */
static int prop(const struct block *b, const char *name, char *out, size_t outsz)
{
	char want[64];
	const char *p = b->start;
	size_t wl;

	snprintf(want, sizeof(want), "--%s:", name);
	wl = strlen(want);
	while (p < b->end) {
		const char *line = p;
		const char *nl = memchr(p, '\n', (size_t)(b->end - p));
		const char *eol = nl != NULL ? nl : b->end;

		while (line < eol && (*line == ' ' || *line == '\t'))
			line++;
		if ((size_t)(eol - line) > wl && strncmp(line, want, wl) == 0) {
			const char *v = line + wl;
			const char *semi = memchr(v, ';', (size_t)(eol - v));
			size_t n;

			if (semi == NULL)
				return -1;
			while (v < semi && (*v == ' ' || *v == '\t'))
				v++;
			n = (size_t)(semi - v);
			while (n > 0 && (v[n - 1] == ' ' || v[n - 1] == '\t'))
				n--;
			if (n + 1 > outsz)
				return -1;
			memcpy(out, v, n);
			out[n] = '\0';
			return 0;
		}
		p = eol + 1;
	}
	return -1;
}

static int parse_hex(const char *s, double rgb[3])
{
	unsigned v;
	int i;

	if (s[0] != '#' || strlen(s) != 7)
		return -1;
	for (i = 1; i < 7; i++) {
		char c = s[i];

		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
			return -1;
	}
	v = (unsigned)strtoul(s + 1, NULL, 16);
	rgb[0] = ((v >> 16) & 0xff) / 255.0;
	rgb[1] = ((v >> 8) & 0xff) / 255.0;
	rgb[2] = (v & 0xff) / 255.0;
	return 0;
}

/* A token's colour in a theme: its value in the theme block, through any
 * var() to the brand block, into hex. 0, or -1 with why on stderr. */
static int colour(const struct block *theme, const struct block *brand, const char *name,
                  char hex[8])
{
	char v[128];
	char ref[64];
	int depth;

	snprintf(ref, sizeof(ref), "%s", name);
	for (depth = 0; depth < 4; depth++) {
		if (prop(theme, ref, v, sizeof(v)) != 0 && prop(brand, ref, v, sizeof(v)) != 0) {
			fprintf(stderr, "FAIL: --%s is not defined for this theme\n", ref);
			g_failures++;
			return -1;
		}
		if (strncmp(v, "var(--", 6) == 0) {
			const char *close = strchr(v + 6, ')');
			size_t n = close != NULL ? (size_t)(close - (v + 6)) : 0;

			if (close == NULL || n == 0 || n >= sizeof(ref)) {
				fprintf(stderr, "FAIL: --%s = %s cannot be resolved\n", ref, v);
				g_failures++;
				return -1;
			}
			memcpy(ref, v + 6, n);
			ref[n] = '\0';
			continue;
		}
		if (strlen(v) != 7 || v[0] != '#') {
			fprintf(stderr, "FAIL: --%s = %s is not a #rrggbb colour\n", ref, v);
			g_failures++;
			return -1;
		}
		memcpy(hex, v, 8);
		return 0;
	}
	fprintf(stderr, "FAIL: --%s resolves through too many var()s\n", name);
	g_failures++;
	return -1;
}

/* ---- colour arithmetic: validate_palette.js, line for line ---- */

static double s2lin(double c)
{
	return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

static void lin(const char *hex, double out[3])
{
	double rgb[3];
	int i;

	parse_hex(hex, rgb);
	for (i = 0; i < 3; i++)
		out[i] = s2lin(rgb[i]);
}

static double rel_lum(const char *hex)
{
	double l[3];

	lin(hex, l);
	return 0.2126 * l[0] + 0.7152 * l[1] + 0.0722 * l[2];
}

static double contrast(const char *a, const char *b)
{
	double x = rel_lum(a), y = rel_lum(b);
	double hi = x > y ? x : y, lo = x > y ? y : x;

	return (hi + 0.05) / (lo + 0.05);
}

static void oklab_from_lin(const double c[3], double out[3])
{
	double l = cbrt(0.4122214708 * c[0] + 0.5363325363 * c[1] + 0.0514459929 * c[2]);
	double m = cbrt(0.2119034982 * c[0] + 0.6806995451 * c[1] + 0.1073969566 * c[2]);
	double s = cbrt(0.0883024619 * c[0] + 0.2817188376 * c[1] + 0.6299787005 * c[2]);

	out[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
	out[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
	out[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
}

enum cvd { CVD_NONE, CVD_PROTAN, CVD_DEUTAN, CVD_TRITAN };

static const double g_machado[3][3][3] = {
	{ { 0.152286, 1.052583, -0.204868 },
	  { 0.114503, 0.786281, 0.099216 },
	  { -0.003882, -0.048116, 1.051998 } },
	{ { 0.367322, 0.860646, -0.227968 },
	  { 0.280085, 0.672501, 0.047413 },
	  { -0.011820, 0.042940, 0.968881 } },
	{ { 1.255528, -0.076749, -0.178779 },
	  { -0.078411, 0.930809, 0.147602 },
	  { 0.004733, 0.691367, 0.303900 } },
};

static double clamp01(double v)
{
	return v < 0 ? 0 : (v > 1 ? 1 : v);
}

static void oklab_seen(const char *hex, enum cvd kind, double out[3])
{
	double c[3], s[3];
	int i;

	lin(hex, c);
	if (kind != CVD_NONE) {
		const double (*m)[3] = g_machado[kind - CVD_PROTAN];

		for (i = 0; i < 3; i++)
			s[i] = clamp01(m[i][0] * c[0] + m[i][1] * c[1] + m[i][2] * c[2]);
		memcpy(c, s, sizeof(c));
	}
	oklab_from_lin(c, out);
}

static double delta_e(const char *a, const char *b, enum cvd kind)
{
	double x[3], y[3];

	oklab_seen(a, kind, x);
	oklab_seen(b, kind, y);
	return 100.0 * sqrt((x[0] - y[0]) * (x[0] - y[0]) + (x[1] - y[1]) * (x[1] - y[1]) +
	                    (x[2] - y[2]) * (x[2] - y[2]));
}

/* ---- the checks ---- */

struct theme {
	const char *name;
	double band_lo, band_hi;
	struct block a, b; /* the two copies of the theme */
	const char *a_what, *b_what;
};

static const char *const g_reserved[] = { "text", "muted", "accent", "ok", "paused", "error" };

static int read_slots(const struct block *t, const struct block *brand, char slots[][8])
{
	int n = 0;

	while (n < MAX_SLOTS) {
		char name[32], v[128];

		snprintf(name, sizeof(name), "series-%d", n + 1);
		if (prop(t, name, v, sizeof(v)) != 0)
			break;
		if (colour(t, brand, name, slots[n]) != 0)
			return -1;
		n++;
	}
	return n;
}

static void check_theme(const struct theme *t, const struct block *brand)
{
	char slots[MAX_SLOTS][8], other[MAX_SLOTS][8];
	char bg[8], bg_other[8];
	int n, n_other, i, k;

	n = read_slots(&t->a, brand, slots);
	n_other = read_slots(&t->b, brand, other);
	if (n < 0 || n_other < 0 || colour(&t->a, brand, "bg", bg) != 0 ||
	    colour(&t->b, brand, "bg", bg_other) != 0)
		return;
	printf("%s theme: %d series slots, chart surface --bg %s\n", t->name, n, bg);
	if (n == 0) {
		fail("the %s theme defines no series slot (%.0f found)", t->name, 0);
		return;
	}

	/* 7. the two copies agree */
	if (n != n_other)
		fail("the %s theme's two blocks define different numbers of slots (%.0f apart)",
		     t->name, (double)(n - n_other));
	for (i = 0; i < n && i < n_other; i++) {
		if (strcmp(slots[i], other[i]) != 0) {
			fprintf(stderr, "FAIL: %s theme --series-%d is %s in %s and %s in %s\n", t->name,
			        i + 1, slots[i], t->a_what, other[i], t->b_what);
			g_failures++;
		}
	}
	if (strcmp(bg, bg_other) != 0) {
		fprintf(stderr, "FAIL: %s theme --bg is %s in %s and %s in %s\n", t->name, bg, t->a_what,
		        bg_other, t->b_what);
		g_failures++;
	}

	for (i = 0; i < n; i++) {
		double lab[3], c, cr;

		oklab_seen(slots[i], CVD_NONE, lab);
		c = sqrt(lab[1] * lab[1] + lab[2] * lab[2]);
		cr = contrast(slots[i], bg);
		printf("  --series-%d %s  L %.3f  C %.3f  contrast %.2f:1\n", i + 1, slots[i], lab[0], c,
		       cr);
		/* 1, 2, 5 */
		if (lab[0] < t->band_lo || lab[0] > t->band_hi)
			fail("%s: OKLCH L %.3f is outside the theme's lightness band", slots[i], lab[0]);
		if (c < CHROMA_FLOOR)
			fail("%s: OKLCH C %.3f is under the chroma floor, so it reads as grey", slots[i], c);
		if (cr < CONTRAST_MIN)
			fail("%s: contrast %.2f:1 against --bg is under 3:1", slots[i], cr);
		/* 6 */
		for (k = 0; k < (int)(sizeof(g_reserved) / sizeof(g_reserved[0])); k++) {
			char r[8];
			double d;

			if (colour(&t->a, brand, g_reserved[k], r) != 0)
				continue;
			d = delta_e(slots[i], r, CVD_NONE);
			if (d < RESERVED_MIN) {
				fprintf(stderr,
				        "FAIL: %s theme --series-%d %s is %.1f from --%s %s; a series is "
				        "never a reserved colour\n",
				        t->name, i + 1, slots[i], d, g_reserved[k], r);
				g_failures++;
			}
		}
	}
	/* 3, 4: adjacent pairs, as a line or a stack draws them */
	for (i = 0; i + 1 < n; i++) {
		double dn = delta_e(slots[i], slots[i + 1], CVD_NONE);
		double dp = delta_e(slots[i], slots[i + 1], CVD_PROTAN);
		double dd = delta_e(slots[i], slots[i + 1], CVD_DEUTAN);
		double dt = delta_e(slots[i], slots[i + 1], CVD_TRITAN);

		printf("  slots %d-%d  normal %.1f  protan %.1f  deutan %.1f  tritan %.1f\n", i + 1,
		       i + 2, dn, dp, dd, dt);
		if (dn < NORMAL_FLOOR)
			fail("%s: adjacent slots %.1f apart under normal vision, under 15", t->name, dn);
		if (dp < CVD_TARGET || dd < CVD_TARGET)
			fail("%s: adjacent slots %.1f apart under protanopia or deuteranopia, under 8",
			     t->name, dp < dd ? dp : dd);
	}
}

int main(void)
{
	char *css = slurp("web/style.css");
	struct block brand;
	struct theme themes[2];
	int i;

	if (css == NULL) {
		fprintf(stderr, "FAIL: cannot read web/style.css\n");
		return 1;
	}
	themes[0].name = "light";
	themes[0].band_lo = BAND_LIGHT_LO;
	themes[0].band_hi = BAND_LIGHT_HI;
	themes[0].a_what = "the :root block";
	themes[0].b_what = ":root[data-theme=\"light\"]";
	themes[1].name = "dark";
	themes[1].band_lo = BAND_DARK_LO;
	themes[1].band_hi = BAND_DARK_HI;
	themes[1].a_what = "the prefers-color-scheme: dark block";
	themes[1].b_what = ":root[data-theme=\"dark\"]";
	if (block_around(css, "--carbon:", &brand) != 0 ||
	    block_around(css, "color-scheme: light;", &themes[0].a) != 0 ||
	    block_at(css, ":root[data-theme=\"light\"]", &themes[0].b) != 0 ||
	    block_around(css, "color-scheme: dark;", &themes[1].a) != 0 ||
	    block_at(css, ":root[data-theme=\"dark\"]", &themes[1].b) != 0) {
		fprintf(stderr, "FAIL: web/style.css no longer has the brand block and the four theme "
		                "blocks this gate reads\n");
		free(css);
		return 1;
	}
	for (i = 0; i < 2; i++)
		check_theme(&themes[i], &brand);
	free(css);
	if (g_failures != 0) {
		fprintf(stderr, "test_web_palette: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_web_palette: PASS\n");
	return 0;
}
