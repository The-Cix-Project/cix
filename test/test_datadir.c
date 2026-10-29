/*
 * test_datadir -- datadir_write_json() (#249): every entry beneath a data
 * directory is reported once, sorted, with its owner, whether that
 * owner's live tree is this one, and the bytes the filesystem allocated
 * to it.
 *
 * The scratch directory holds what a real one can: a tree its owner
 * uses (logs/), a tree its owner has moved away from (rebuildable/, the
 * placement now elsewhere), a tree nothing owns (mystery/) and a state
 * file (diskroles.json). Skipping a separate mount is not exercised
 * here: a build container cannot mount, and the live report on a host
 * is what shows it.
 */
#include "datadir.h"
#include "json.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		fprintf(stderr, "FAIL: %s\n", what);
		g_failures++;
	}
}

/* A file of n bytes of real data, so the filesystem allocates blocks. */
static int write_file(const char *path, size_t n)
{
	char buf[4096];
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	size_t done = 0;

	if (fd < 0)
		return -1;
	memset(buf, 'x', sizeof(buf));
	while (done < n) {
		size_t chunk = n - done < sizeof(buf) ? n - done : sizeof(buf);

		if (write(fd, buf, chunk) != (ssize_t)chunk) {
			close(fd);
			return -1;
		}
		done += chunk;
	}
	fsync(fd);
	close(fd);
	return 0;
}

/* A string field, or "" when it is absent or not a string, so a check
 * fails rather than the test crashing. */
static const char *str_of(const struct json_value *v)
{
	const char *s = json_as_string(v);

	return s != NULL ? s : "";
}

static const struct json_value *entry_named(const struct json_value *entries, const char *name)
{
	size_t i;

	for (i = 0; entries != NULL && i < entries->u.array.count; i++) {
		const char *n = json_as_string(json_object_get(entries->u.array.items[i], "name"));

		if (n != NULL && strcmp(n, name) == 0)
			return entries->u.array.items[i];
	}
	return NULL;
}

static int bool_field(const struct json_value *e, const char *key)
{
	const struct json_value *v = json_object_get(e, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
}

static int only_alive_is_defined(const char *name)
{
	return strcmp(name, "alive") == 0;
}

int main(void)
{
	const char *tmp = getenv("TMPDIR");
	char base[4096], path[4096 + 64], logs[4096 + 16];
	struct json_writer w;
	struct json_value *doc;
	const struct json_value *entries, *e;
	struct datadir_live live;
	const char *order[] = { "containers", "diskroles.json", "logs", "mystery", "rebuildable" };
	size_t i;

	snprintf(base, sizeof(base), "%s/cix_test_datadir_XXXXXX", tmp != NULL ? tmp : "/tmp");
	if (mkdtemp(base) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	snprintf(logs, sizeof(logs), "%s/logs", base);
	mkdir(logs, 0755);
	snprintf(path, sizeof(path), "%s/logs/segment", base);
	check(write_file(path, 64 * 1024) == 0, "write logs/segment");
	snprintf(path, sizeof(path), "%s/rebuildable", base);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/rebuildable/old", base);
	check(write_file(path, 8 * 1024) == 0, "write rebuildable/old");
	snprintf(path, sizeof(path), "%s/mystery", base);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/mystery/blob", base);
	check(write_file(path, 128 * 1024) == 0, "write mystery/blob");
	snprintf(path, sizeof(path), "%s/diskroles.json", base);
	check(write_file(path, 10) == 0, "write diskroles.json");
	/* containers/: a defined container's directory, one for a container
	 * that no longer exists, and the delete queue's trash. */
	snprintf(path, sizeof(path), "%s/containers", base);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/containers/alive", base);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/containers/gone", base);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/containers/gone/upper", base);
	check(write_file(path, 32 * 1024) == 0, "write containers/gone/upper");
	snprintf(path, sizeof(path), "%s/containers/.discard", base);
	mkdir(path, 0755);

	memset(&live, 0, sizeof(live));
	live.log_dir = logs;
	live.rebuildable_dir = "/mnt/elsewhere/rebuildable";
	live.container_defined = only_alive_is_defined;

	jw_init(&w);
	check(datadir_write_json(&w, base, NULL, &live, 1234) == 0, "datadir_write_json succeeds");
	doc = json_parse(w.buf, w.len);
	check(doc != NULL, "the report is valid JSON");
	if (doc == NULL) {
		fprintf(stderr, "%.*s\n", (int)w.len, w.buf);
		return 1;
	}
	check(json_as_string(json_object_get(doc, "path")) != NULL &&
	              strcmp(str_of(json_object_get(doc, "path")), base) == 0,
	      "path is the data directory");
	check((long long)json_as_number(json_object_get(doc, "measured_at")) == 1234,
	      "measured_at is the caller's clock");
	entries = json_object_get(doc, "entries");
	check(entries != NULL && entries->type == JSON_ARRAY && entries->u.array.count == 5,
	      "five entries, one per name");
	for (i = 0; entries != NULL && i < entries->u.array.count && i < 5; i++)
		check(strcmp(str_of(json_object_get(entries->u.array.items[i], "name")),
		             order[i]) == 0,
		      "entries are sorted by name");

	e = entry_named(entries, "logs");
	check(e != NULL && strcmp(str_of(json_object_get(e, "owner")), "log-store") == 0,
	      "logs/ is the log store's");
	check(e != NULL && bool_field(e, "in_use"), "logs/ is in use: its live path is this tree");
	check(e != NULL && json_as_number(json_object_get(e, "bytes")) >= 64 * 1024,
	      "logs/ is charged at least its 64 KiB of data");

	e = entry_named(entries, "rebuildable");
	check(e != NULL &&
	              strcmp(str_of(json_object_get(e, "owner")), "rebuildable-storage") == 0,
	      "rebuildable/ is rebuildable storage's");
	check(e != NULL && !bool_field(e, "in_use"),
	      "rebuildable/ is not in use: the placement has moved elsewhere");
	check(e != NULL && json_as_string(json_object_get(e, "live_path")) != NULL &&
	              strcmp(str_of(json_object_get(e, "live_path")),
	                     "/mnt/elsewhere/rebuildable") == 0,
	      "rebuildable/ names where its owner lives now");

	e = entry_named(entries, "mystery");
	check(e != NULL && strcmp(str_of(json_object_get(e, "owner")), "unknown") == 0,
	      "mystery/ is owned by nothing the daemon knows");
	check(e != NULL && json_as_number(json_object_get(e, "bytes")) >= 128 * 1024,
	      "mystery/ is charged at least its 128 KiB of data");

	e = entry_named(entries, "diskroles.json");
	check(e != NULL && strcmp(str_of(json_object_get(e, "type")), "file") == 0 &&
	              bool_field(e, "in_use"),
	      "diskroles.json is a state file, in use");

	check(json_as_number(json_object_get(doc, "entries_bytes")) >= (64 + 8 + 128) * 1024,
	      "entries_bytes sums the entries");
	check(json_object_get(doc, "filesystem") != NULL &&
	              json_as_number(json_object_get(json_object_get(doc, "filesystem"),
	                                             "size_bytes")) > 0,
	      "the filesystem's own size is reported");

	json_free(doc);
	jw_free(&w);

	/* One level down: containers/ broken into what it holds. */
	jw_init(&w);
	check(datadir_write_json(&w, base, "containers", &live, 1234) == 0,
	      "datadir_write_json succeeds for an entry");
	doc = json_parse(w.buf, w.len);
	check(doc != NULL, "the entry report is valid JSON");
	if (doc != NULL) {
		check(strcmp(str_of(json_object_get(doc, "entry")), "containers") == 0,
		      "the entry report names its entry");
		entries = json_object_get(doc, "entries");
		check(entries != NULL && entries->type == JSON_ARRAY && entries->u.array.count == 3,
		      "containers/ holds three names");
		e = entry_named(entries, "alive");
		check(e != NULL && strcmp(str_of(json_object_get(e, "owner")), "container") == 0 &&
		              bool_field(e, "in_use"),
		      "a defined container's directory is in use");
		e = entry_named(entries, "gone");
		check(e != NULL && strcmp(str_of(json_object_get(e, "owner")), "container") == 0 &&
		              !bool_field(e, "in_use"),
		      "a directory for a container no longer defined is not in use");
		check(e != NULL && json_as_number(json_object_get(e, "bytes")) >= 32 * 1024,
		      "that directory is charged for what it holds");
		e = entry_named(entries, ".discard");
		check(e != NULL &&
		              strcmp(str_of(json_object_get(e, "owner")), "discard-queue") == 0 &&
		              !bool_field(e, "in_use"),
		      ".discard is the delete queue's trash, not in use");
		json_free(doc);
	}
	jw_free(&w);

	snprintf(path, sizeof(path), "rm -rf '%s'", base);
	if (system(path) != 0)
		fprintf(stderr, "warning: cleanup of %s failed\n", base);
	if (g_failures != 0) {
		printf("DATADIR: FAIL (%d)\n", g_failures);
		return 1;
	}
	printf("DATADIR: PASS\n");
	return 0;
}
