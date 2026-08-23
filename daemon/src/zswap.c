#include "zswap.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

#define ZSWAP_PARAM_DIR "/sys/module/zswap/parameters"
/*
 * Where the kernel lists the compression algorithms it actually has.
 * Reading this is what turns "compressor" from a string the kernel may
 * silently ignore into a choice that can be checked before it is made.
 */
#define CRYPTO_PROC "/proc/crypto"

static struct zswap_config g_config;
static char g_state_path[512];
static int g_loaded;

static void set_defaults(void)
{
	/*
	 * Enabled by default, unlike the swap file itself (ADR-0069),
	 * which is opt-in because it consumes real disk. zswap consumes
	 * nothing until the box is already swapping, and at that point it
	 * is strictly better than the alternative -- there is no reading
	 * of "off by default" that helps the operator whose box is
	 * thrashing.
	 *
	 * 20 and "lzo" are the kernel's own defaults, restated here rather
	 * than left implicit: this module writes every parameter on every
	 * apply, so "the default" has to be a value this file knows.
	 */
	g_config.enabled = 1;
	g_config.max_pool_percent = 20;
	snprintf(g_config.compressor, sizeof(g_config.compressor), "%s", "lzo");
}

static int read_param(const char *name, char *out, size_t out_size)
{
	char path[256];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", ZSWAP_PARAM_DIR, name);
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	if (fgets(out, (int)out_size, f) == NULL) {
		fclose(f);
		return -1;
	}
	fclose(f);
	out[strcspn(out, "\n")] = '\0';
	return 0;
}

static int write_param(const char *name, const char *value)
{
	char path[256];
	int fd;
	ssize_t n;
	size_t len = strlen(value);

	snprintf(path, sizeof(path), "%s/%s", ZSWAP_PARAM_DIR, name);
	fd = open(path, O_WRONLY);
	if (fd < 0)
		return -1;
	n = write(fd, value, len);
	close(fd);
	return (n == (ssize_t)len) ? 0 : -1;
}

int zswap_supported(void)
{
	struct stat st;

	return stat(ZSWAP_PARAM_DIR, &st) == 0 && S_ISDIR(st.st_mode);
}

/*
 * True if this kernel carries a crypto algorithm of that name.
 * /proc/crypto lists one "name : <alg>" line per registered algorithm;
 * a compressor that is not there is one the kernel will refuse or
 * ignore, and refusing it here is the difference between a setting
 * that did not take and a setting that says it did.
 */
static int compressor_available(const char *name)
{
	FILE *f = fopen(CRYPTO_PROC, "r");
	char line[256];
	int found = 0;

	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		const char *p;

		if (strncmp(line, "name", 4) != 0)
			continue;
		p = strchr(line, ':');
		if (p == NULL)
			continue;
		p++;
		while (*p == ' ' || *p == '\t')
			p++;
		line[strcspn(line, "\n")] = '\0';
		if (strcmp(p, name) == 0) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

static void write_available_compressors(struct json_writer *w)
{
	static const char *candidates[] = { "lzo", "lz4", "lz4hc", "zstd", "deflate", "842" };
	size_t i;

	jw_arr_open(w);
	for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
		if (compressor_available(candidates[i]))
			jw_str(w, candidates[i]);
	jw_arr_close(w);
}

static int config_is_valid(const struct zswap_config *c)
{
	size_t i;

	if (c == NULL || c->max_pool_percent < 1 || c->max_pool_percent > 100)
		return 0;
	if (c->compressor[0] == '\0' || strlen(c->compressor) >= ZSWAP_COMPRESSOR_MAX)
		return 0;
	/* A compressor name goes straight into a sysfs write; anything that
	 * is not a plain algorithm name is refused rather than escaped. */
	for (i = 0; c->compressor[i] != '\0'; i++) {
		char ch = c->compressor[i];

		if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_')
			continue;
		return 0;
	}
	return 1;
}

static int save_state(void)
{
	char buf[256];
	int n;

	if (g_state_path[0] == '\0')
		return -1;
	n = snprintf(buf, sizeof(buf),
	             "{\"enabled\":%s,\"max_pool_percent\":%d,\"compressor\":\"%s\"}\n",
	             g_config.enabled ? "true" : "false", g_config.max_pool_percent,
	             g_config.compressor);
	if (n < 0 || (size_t)n >= sizeof(buf))
		return -1;
	return persist_atomic_write(g_state_path, buf, (size_t)n);
}

/*
 * Order matters: the pool percentage and compressor are set BEFORE
 * enabling, and the enable flag last. zswap allocates its pool when it
 * is first enabled, and a compressor written afterwards applies only to
 * pages compressed from then on -- so applying in the other order
 * would leave the first pages in the pool compressed with whatever the
 * previous setting was.
 */
static enum zswap_error apply(void)
{
	char value[64];
	int failed = 0;

	if (!zswap_supported())
		return ZSWAP_ERR_UNSUPPORTED;
	snprintf(value, sizeof(value), "%d", g_config.max_pool_percent);
	failed |= write_param("max_pool_percent", value) != 0;
	failed |= write_param("compressor", g_config.compressor) != 0;
	failed |= write_param("enabled", g_config.enabled ? "Y" : "N") != 0;
	return failed ? ZSWAP_ERR_APPLY_FAILED : ZSWAP_OK;
}

void zswap_init(const char *path)
{
	char *buf = NULL;
	size_t len = 0;
	struct json_value *root;

	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	set_defaults();
	g_loaded = 1;
	if (persist_read_file(path, &buf, &len) == 0 && buf != NULL) {
		root = json_parse(buf, len);
		if (root != NULL) {
			const struct json_value *en = json_object_get(root, "enabled");
			const struct json_value *pct = json_object_get(root, "max_pool_percent");
			const char *comp = json_as_string(json_object_get(root, "compressor"));
			struct zswap_config next = g_config;

			if (en != NULL && en->type == JSON_BOOL)
				next.enabled = en->u.boolean;
			if (pct != NULL && pct->type == JSON_NUMBER)
				next.max_pool_percent = (int)json_as_number(pct);
			if (comp != NULL)
				snprintf(next.compressor, sizeof(next.compressor), "%s", comp);
			/* A persisted file that has gone bad (hand-edited, or
			 * written by a build that allowed something this one does
			 * not) leaves the defaults rather than refusing to boot. */
			if (config_is_valid(&next))
				g_config = next;
			json_free(root);
		}
	}
	free(buf);
	/*
	 * Applied at startup, not just on PUT: this is the setting's only
	 * chance to survive a reboot, since the kernel's own default is
	 * deliberately off (see image/kernel/qemu-part1.config).
	 */
	(void)apply();
}

void zswap_repoint(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
}

const struct zswap_config *zswap_get(void)
{
	if (!g_loaded) {
		set_defaults();
		g_loaded = 1;
	}
	return &g_config;
}

enum zswap_error zswap_set(const struct zswap_config *next)
{
	struct zswap_config previous;
	enum zswap_error err;

	if (!config_is_valid(next))
		return ZSWAP_ERR_INVALID;
	if (!zswap_supported())
		return ZSWAP_ERR_UNSUPPORTED;
	if (!compressor_available(next->compressor))
		return ZSWAP_ERR_UNSUPPORTED;

	previous = g_config;
	g_config = *next;
	err = apply();
	if (err != ZSWAP_OK) {
		/* Nothing is persisted that the kernel would not accept: a
		 * config file describing a state the machine is not in is
		 * worse than the failure it was trying to record. */
		g_config = previous;
		(void)apply();
		return err;
	}
	return save_state() == 0 ? ZSWAP_OK : ZSWAP_ERR_PERSIST_FAILED;
}

void zswap_write_json(struct json_writer *w)
{
	char actual[64];

	zswap_get();
	jw_obj_open(w);
	jw_key(w, "supported");
	jw_bool(w, zswap_supported());
	jw_key(w, "enabled");
	jw_bool(w, g_config.enabled);
	jw_key(w, "max_pool_percent");
	jw_int(w, g_config.max_pool_percent);
	jw_key(w, "compressor");
	jw_str(w, g_config.compressor);
	/*
	 * What the kernel actually has, read back rather than echoed. The
	 * two differ on a kernel without zswap, and on any parameter the
	 * kernel declined -- which is exactly the case a page that only
	 * repeated its own input could never show.
	 */
	jw_key(w, "kernel");
	jw_obj_open(w);
	jw_key(w, "enabled");
	if (read_param("enabled", actual, sizeof(actual)) == 0)
		jw_bool(w, actual[0] == 'Y' || actual[0] == 'y' || actual[0] == '1');
	else
		jw_null(w);
	jw_key(w, "max_pool_percent");
	if (read_param("max_pool_percent", actual, sizeof(actual)) == 0)
		jw_int(w, strtol(actual, NULL, 10));
	else
		jw_null(w);
	jw_key(w, "compressor");
	if (read_param("compressor", actual, sizeof(actual)) == 0)
		jw_str(w, actual);
	else
		jw_null(w);
	jw_obj_close(w);
	jw_key(w, "available_compressors");
	write_available_compressors(w);
	jw_obj_close(w);
}
