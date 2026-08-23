#include "cpreserve.h"
#include "persist.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Defaults are ON. A reservation that has to be discovered and switched
 * on is a reservation nobody has when they need it -- and the failure
 * it prevents (a box that cannot be reached at all) is the worst one
 * this platform has. Ten percent of CPU and 512 MiB are small enough to
 * be uncontroversial on any machine this runs on and large enough for a
 * daemon whose entire job is answering HTTP.
 */
#define CPRESERVE_DEFAULT_CPU_PERCENT 10
#define CPRESERVE_DEFAULT_MEMORY_BYTES (512LL * 1024 * 1024)

static char g_state_path[512];
static struct cpreserve_config g_config;

static int load_state(void)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const struct json_value *j;

	if (persist_read_file(g_state_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* never configured -- defaults stand */

	root = json_parse(buf, len);
	free(buf);
	if (root == NULL) {
		fprintf(stderr, "%s: malformed control-plane reservation, using defaults\n",
		        g_state_path);
		return -1;
	}
	j = json_object_get(root, "enabled");
	if (j != NULL && j->type == JSON_BOOL)
		g_config.enabled = j->u.boolean;
	j = json_object_get(root, "cpu_percent");
	if (j != NULL)
		g_config.cpu_percent = (int)json_as_number(j);
	j = json_object_get(root, "memory_bytes");
	if (j != NULL)
		g_config.memory_bytes = (long long)json_as_number(j);
	json_free(root);
	return 0;
}

static int save_state(void)
{
	struct json_writer w;
	int rc;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, g_config.enabled);
	jw_key(&w, "cpu_percent");
	jw_int(&w, g_config.cpu_percent);
	jw_key(&w, "memory_bytes");
	jw_int(&w, g_config.memory_bytes);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	rc = persist_atomic_write(g_state_path, w.buf, w.len);
	jw_free(&w);
	return rc;
}

void cpreserve_init(const char *path)
{
	snprintf(g_state_path, sizeof(g_state_path), "%s", path);
	g_config.enabled = 1;
	g_config.cpu_percent = CPRESERVE_DEFAULT_CPU_PERCENT;
	g_config.memory_bytes = CPRESERVE_DEFAULT_MEMORY_BYTES;
	load_state();
}

const struct cpreserve_config *cpreserve_get(void)
{
	return &g_config;
}

int cpreserve_set(int enabled, int cpu_percent, long long memory_bytes)
{
	/*
	 * The upper bounds are the point of the range check: a reservation
	 * of 95% of the CPU would leave workloads a twentieth of the machine
	 * and look like a scheduler bug rather than a setting. The lower
	 * bounds keep it a reservation rather than a rounding error.
	 */
	if (cpu_percent < 1 || cpu_percent > 50)
		return -1;
	if (memory_bytes < 64LL * 1024 * 1024)
		return -1;
	g_config.enabled = enabled ? 1 : 0;
	g_config.cpu_percent = cpu_percent;
	g_config.memory_bytes = memory_bytes;
	return save_state();
}

void cpreserve_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "enabled");
	jw_bool(w, g_config.enabled);
	jw_key(w, "cpu_percent");
	jw_int(w, g_config.cpu_percent);
	jw_key(w, "memory_bytes");
	jw_int(w, g_config.memory_bytes);
	jw_obj_close(w);
}
