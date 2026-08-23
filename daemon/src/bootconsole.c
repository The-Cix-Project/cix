#include "bootconsole.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct bootconsole_config g_config;
static char g_state_path[512];
/*
 * Whether bootconsole_init() has run. A renderer that quietly produces
 * an empty console list because nobody initialised it is how a machine
 * ends up booting with nothing on its console -- which is precisely how
 * this flag came to exist, after an A/B update wrote a loader entry
 * with no console= at all and the boot test timed out waiting for
 * output that could not exist. Every reader defaults itself first.
 */
static int g_loaded;

/*
 * A console name is a device under /dev plus optional comma-separated
 * options: letters, digits, comma, and nothing else. No spaces (they
 * would silently become a second parameter), no slashes (the kernel
 * wants `ttyS0`, not `/dev/ttyS0`), no quotes or newlines (they would
 * break the loader entry itself).
 */
static int console_name_is_valid(const char *s)
{
	size_t i;

	if (s == NULL || s[0] == '\0' || strlen(s) >= BOOTCONSOLE_CONSOLE_MAX)
		return 0;
	for (i = 0; s[i] != '\0'; i++) {
		char c = s[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == ',')
			continue;
		return 0;
	}
	return 1;
}

/*
 * Extra parameters are ordinary kernel arguments -- `video=1024x768`,
 * `nomodeset`, `fbcon=map:1`. Spaces separate them; everything a shell
 * or the loader-entry format would treat specially is refused. `=`,
 * `.`, `:`, `,`, `-`, `_` and `/` cover every real parameter shape
 * (including module.parameter=value and paths) without allowing a
 * newline to end the options line early or a quote to unbalance it.
 */
static int extra_is_valid(const char *s)
{
	size_t i;

	if (s == NULL)
		return 1;
	if (strlen(s) >= BOOTCONSOLE_EXTRA_MAX)
		return 0;
	for (i = 0; s[i] != '\0'; i++) {
		char c = s[i];

		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
		    c == '=' || c == '.' || c == ':' || c == ',' || c == '-' || c == '_' || c == '/' ||
		    c == ' ')
			continue;
		return 0;
	}
	/* `console=` in the extra field would fight the consoles list and
	 * make the rendered line self-contradictory; `root=`/`init=` would
	 * be a different, much worse mistake. Refused by name so the error
	 * can say why. */
	if (strstr(s, "console=") != NULL || strstr(s, "root=") != NULL ||
	    strstr(s, "init=") != NULL)
		return 0;
	return 1;
}

static void ensure_loaded(void);

static void set_defaults(void)
{
	memset(&g_config, 0, sizeof(g_config));
	/* Exactly what was hardcoded into every loader entry before this
	 * module existed -- so an install that never touches this setting
	 * boots identically to one from before it. */
	snprintf(g_config.consoles[0], BOOTCONSOLE_CONSOLE_MAX, "tty0");
	snprintf(g_config.consoles[1], BOOTCONSOLE_CONSOLE_MAX, "ttyS0");
	g_config.console_count = 2;
}

static int save_state(void)
{
	struct json_writer w;
	int i, rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "consoles");
	jw_arr_open(&w);
	for (i = 0; i < g_config.console_count; i++)
		jw_str(&w, g_config.consoles[i]);
	jw_arr_close(&w);
	jw_key(&w, "extra");
	jw_str(&w, g_config.extra);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

void bootconsole_init(const char *path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *arr, *extra;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	set_defaults();
	g_loaded = 1;

	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return;
	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed boot console config, using defaults\n", path);
		return;
	}
	arr = json_object_get(root, "consoles");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		size_t i;

		g_config.console_count = 0;
		for (i = 0; i < arr->u.array.count && g_config.console_count < BOOTCONSOLE_MAX_CONSOLES;
		     i++) {
			const char *c = json_as_string(arr->u.array.items[i]);

			if (!console_name_is_valid(c))
				continue;
			snprintf(g_config.consoles[g_config.console_count], BOOTCONSOLE_CONSOLE_MAX, "%s", c);
			g_config.console_count++;
		}
	}
	extra = json_object_get(root, "extra");
	if (extra != NULL && extra->type == JSON_STRING) {
		const char *e = json_as_string(extra);

		if (extra_is_valid(e))
			snprintf(g_config.extra, sizeof(g_config.extra), "%s", e != NULL ? e : "");
	}
	json_free(root);
}

void bootconsole_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

static void ensure_loaded(void)
{
	if (g_loaded)
		return;
	set_defaults();
	g_loaded = 1;
}

const struct bootconsole_config *bootconsole_get(void)
{
	ensure_loaded();
	return &g_config;
}

void bootconsole_render(char *out, size_t out_size)
{
	size_t off = 0;
	int i;

	ensure_loaded();

	if (out_size == 0)
		return;
	out[0] = '\0';
	for (i = 0; i < g_config.console_count; i++) {
		int n = snprintf(out + off, out_size - off, "%sconsole=%s", off > 0 ? " " : "",
		                 g_config.consoles[i]);

		if (n > 0 && (size_t)n < out_size - off)
			off += (size_t)n;
	}
	if (g_config.extra[0] != '\0') {
		int n = snprintf(out + off, out_size - off, "%s%s", off > 0 ? " " : "", g_config.extra);

		if (n > 0 && (size_t)n < out_size - off)
			off += (size_t)n;
	}
}

enum bootconsole_error bootconsole_set(const char consoles[][BOOTCONSOLE_CONSOLE_MAX],
                                        int console_count, const char *extra)
{
	struct bootconsole_config next;
	int i;

	if (console_count < 0 || console_count > BOOTCONSOLE_MAX_CONSOLES)
		return BOOTCONSOLE_ERR_INVALID;
	if (!extra_is_valid(extra))
		return BOOTCONSOLE_ERR_INVALID;

	memset(&next, 0, sizeof(next));
	for (i = 0; i < console_count; i++) {
		if (!console_name_is_valid(consoles[i]))
			return BOOTCONSOLE_ERR_INVALID;
		snprintf(next.consoles[i], BOOTCONSOLE_CONSOLE_MAX, "%s", consoles[i]);
	}
	next.console_count = console_count;
	snprintf(next.extra, sizeof(next.extra), "%s", extra != NULL ? extra : "");

	g_config = next;
	return save_state() == 0 ? BOOTCONSOLE_OK : BOOTCONSOLE_ERR_PERSIST;
}

void bootconsole_write_json(struct json_writer *w)
{
	char rendered[512];
	int i;

	bootconsole_render(rendered, sizeof(rendered));

	jw_obj_open(w);
	jw_key(w, "consoles");
	jw_arr_open(w);
	for (i = 0; i < g_config.console_count; i++)
		jw_str(w, g_config.consoles[i]);
	jw_arr_close(w);
	jw_key(w, "extra");
	jw_str(w, g_config.extra);
	jw_key(w, "rendered");
	jw_str(w, rendered);
	jw_obj_close(w);
}
