/*
 * The EFI System Partition's boot configuration, over REST (issue #128,
 * ADR-0202).
 *
 * Every other piece of a running host's state had an endpoint. The ESP
 * did not, and the cost of that showed up as a host stuck on an old
 * build with a correctly staged update it would never boot, no error
 * anywhere, and no way to look at the one file that explained it.
 *
 * The trap is systemd-boot's `default`: it is a glob PATTERN, not an
 * entry name. A pattern left behind by an earlier naming scheme goes on
 * matching stale entries forever and outranks every newly written one.
 * Nothing reports this -- the update succeeds, the entry is written
 * correctly, and the machine reboots into exactly what it was already
 * running. So this module's job is as much to make that visible as it
 * is to make it changeable: esp_write_json() reports, for every entry,
 * whether the current pattern selects it, and names the entry
 * systemd-boot would actually choose.
 */
#include "esp.h"

#include "json.h"
#include "persist.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_loader_dir[PATH_MAX];
static char g_entries_dir[PATH_MAX];
static char g_running_slot[8];

void esp_init(const char *loader_dir, const char *entries_dir)
{
	snprintf(g_loader_dir, sizeof(g_loader_dir), "%s", loader_dir);
	snprintf(g_entries_dir, sizeof(g_entries_dir), "%s", entries_dir);
	g_running_slot[0] = '\0';
}

void esp_set_running_slot(const char *slot)
{
	snprintf(g_running_slot, sizeof(g_running_slot), "%s", slot != NULL ? slot : "");
}

static void loader_conf_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/loader.conf", g_loader_dir);
}

/*
 * systemd-boot's entry id, and its Automatic Boot Assessment counter.
 *
 * The id is the filename with ".conf" removed AND the "+<left>[-<done>]"
 * counter stripped -- so "cix-b+3.conf" has the id "cix-b", not
 * "cix-b+3". That distinction is not cosmetic: it decides whether an
 * operator's pattern matches, and getting it wrong here would mean this
 * API answering differently from the firmware it claims to describe.
 *
 * out_tries_left is the counter's first number, or -1 when the entry
 * carries no counter at all (a confirmed entry). Zero is a real,
 * distinct value: it means "no attempts remain", and systemd-boot sorts
 * such entries last.
 */
static void entry_id(const char *filename, char *out, size_t out_size, int *out_tries_left)
{
	size_t len;
	char *plus;

	if (out_tries_left != NULL)
		*out_tries_left = -1;
	snprintf(out, out_size, "%s", filename);
	len = strlen(out);
	if (len > 5 && strcmp(out + len - 5, ".conf") == 0)
		out[len - 5] = '\0';

	plus = strrchr(out, '+');
	if (plus == NULL || plus[1] == '\0')
		return;
	{
		const char *q = plus + 1;
		int seen_digit = 0;
		int valid = 1;

		while (*q != '\0') {
			if (*q >= '0' && *q <= '9') {
				seen_digit = 1;
			} else if (*q == '-' && seen_digit) {
				/* the "-<done>" half; the rest must be digits */
			} else {
				valid = 0;
				break;
			}
			q++;
		}
		if (!valid || !seen_digit)
			return; /* a literal '+' in the name, not a counter */
		if (out_tries_left != NULL)
			*out_tries_left = atoi(plus + 1);
		*plus = '\0';
	}
}

int esp_pattern_matches(const char *pattern, const char *entry_name)
{
	char stem[ESP_ENTRY_NAME_MAX];
	char id[ESP_ENTRY_NAME_MAX];
	size_t len;

	if (pattern == NULL || pattern[0] == '\0' || entry_name == NULL)
		return 0;
	/*
	 * Three spellings, because a pattern an operator writes should
	 * behave the way the bootloader behaves, not the way a naive
	 * filename match would suggest: the raw filename, the filename
	 * without .conf, and the real entry id with the boot counter
	 * stripped (which is what systemd-boot itself matches against).
	 */
	if (fnmatch(pattern, entry_name, 0) == 0)
		return 1;
	snprintf(stem, sizeof(stem), "%s", entry_name);
	len = strlen(stem);
	if (len > 5 && strcmp(stem + len - 5, ".conf") == 0) {
		stem[len - 5] = '\0';
		if (fnmatch(pattern, stem, 0) == 0)
			return 1;
	}
	entry_id(entry_name, id, sizeof(id), NULL);
	if (id[0] != '\0' && fnmatch(pattern, id, 0) == 0)
		return 1;
	return 0;
}

/* The value of a single-word directive ("default cix-b"), trimmed. */
static void parse_directive(const char *text, const char *key, char *out, size_t out_size)
{
	const char *p = text;
	size_t key_len = strlen(key);

	out[0] = '\0';
	while (*p != '\0') {
		const char *line_end = strchr(p, '\n');
		size_t line_len = (line_end != NULL) ? (size_t)(line_end - p) : strlen(p);
		const char *v;
		size_t vlen;

		while (line_len > 0 && (*p == ' ' || *p == '\t')) {
			p++;
			line_len--;
		}
		if (line_len > key_len && strncmp(p, key, key_len) == 0 &&
		    (p[key_len] == ' ' || p[key_len] == '\t')) {
			v = p + key_len;
			vlen = line_len - key_len;
			while (vlen > 0 && (*v == ' ' || *v == '\t')) {
				v++;
				vlen--;
			}
			while (vlen > 0 && (v[vlen - 1] == ' ' || v[vlen - 1] == '\t' ||
			                    v[vlen - 1] == '\r'))
				vlen--;
			if (vlen >= out_size)
				vlen = out_size - 1;
			memcpy(out, v, vlen);
			out[vlen] = '\0';
			return;
		}
		if (line_end == NULL)
			return;
		p = line_end + 1;
	}
}

enum esp_error esp_loader_get(char *out_default, size_t out_default_size, int *out_timeout,
                              int *out_writable)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	char timeout_str[32];

	out_default[0] = '\0';
	*out_timeout = -1;
	*out_writable = 0;

	if (g_loader_dir[0] == '\0')
		return ESP_ERR_NO_ESP;
	if (access(g_loader_dir, F_OK) != 0)
		return ESP_ERR_NO_ESP;
	*out_writable = (access(g_loader_dir, W_OK) == 0) ? 1 : 0;

	loader_conf_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL) {
		/* A loader directory with no loader.conf is a real, bootable
		 * state (systemd-boot falls back to its own defaults), not an
		 * error -- report it as "nothing set" rather than failing. */
		return ESP_OK;
	}
	parse_directive(buf, "default", out_default, out_default_size);
	parse_directive(buf, "timeout", timeout_str, sizeof(timeout_str));
	if (timeout_str[0] != '\0')
		*out_timeout = atoi(timeout_str);
	free(buf);
	return ESP_OK;
}

/* One entry's file, parsed. Returns 0 on success. */
static int read_entry(const char *name, struct esp_entry *out)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;

	snprintf(path, sizeof(path), "%s/%s", g_entries_dir, name);
	if (persist_read_file(path, &buf, &len) != 0 || buf == NULL)
		return -1;

	memset(out, 0, sizeof(*out));
	snprintf(out->name, sizeof(out->name), "%s", name);
	parse_directive(buf, "title", out->title, sizeof(out->title));
	parse_directive(buf, "linux", out->linux_image, sizeof(out->linux_image));
	parse_directive(buf, "options", out->options, sizeof(out->options));
	parse_directive(buf, "sort-key", out->sort_key, sizeof(out->sort_key));
	parse_directive(buf, "version", out->version, sizeof(out->version));
	free(buf);
	return 0;
}

/* The slot an entry boots, read from the --slot= this daemon itself
 * writes into every entry's options. An entry with no recognizable slot
 * (a rescue entry, say) is nobody's running entry, which is the safe
 * reading -- it means the delete guard never silently protects
 * something it does not understand. */
static int entry_slot(const struct esp_entry *e, char *out, size_t out_size)
{
	const char *p = strstr(e->options, "--slot=");
	size_t i = 0;

	out[0] = '\0';
	if (p == NULL)
		return -1;
	p += 7;
	while (p[i] != '\0' && p[i] != ' ' && p[i] != '\t' && i + 1 < out_size) {
		out[i] = p[i];
		i++;
	}
	out[i] = '\0';
	return 0;
}

int esp_entries_list(struct esp_entry *out, int max)
{
	DIR *d;
	struct dirent *de;
	int count = 0;
	char default_pattern[ESP_DEFAULT_MAX];
	int timeout, writable;

	if (g_entries_dir[0] == '\0')
		return -1;
	d = opendir(g_entries_dir);
	if (d == NULL)
		return -1;

	default_pattern[0] = '\0';
	esp_loader_get(default_pattern, sizeof(default_pattern), &timeout, &writable);

	while ((de = readdir(d)) != NULL && count < max) {
		char slot[8];

		if (de->d_name[0] == '.')
			continue;
		if (read_entry(de->d_name, &out[count]) != 0)
			continue;
		out[count].matches_default = esp_pattern_matches(default_pattern, de->d_name);
		out[count].is_running_slot =
		    (g_running_slot[0] != '\0' && entry_slot(&out[count], slot, sizeof(slot)) == 0 &&
		     strcmp(slot, g_running_slot) == 0)
		        ? 1
		        : 0;
		count++;
	}
	closedir(d);
	return count;
}

/*
 * Rewrites loader.conf with default/timeout replaced, every other line
 * kept verbatim. Preserving the rest matters: loader.conf legitimately
 * carries editor/console-mode/auto-entries settings this module has no
 * opinion about, and rewriting the file from a template would silently
 * discard them.
 */
static enum esp_error write_loader_conf(const char *new_default, const int *new_timeout)
{
	char path[PATH_MAX];
	char *buf = NULL;
	size_t len = 0;
	char *out;
	size_t out_cap;
	size_t out_len = 0;
	int wrote_default = 0;
	int wrote_timeout = 0;
	const char *p;
	enum esp_error rc = ESP_OK;

	loader_conf_path(path, sizeof(path));
	if (persist_read_file(path, &buf, &len) != 0)
		buf = NULL;

	/*
	 * Generous by design, and every append below is checked against it.
	 * A truncated loader.conf is not a cosmetic failure -- it is a host
	 * that does not boot -- so running out of room must fail the write
	 * loudly rather than silently produce a short file.
	 */
	out_cap = len + 2 * ESP_DEFAULT_MAX + 256;
	out = malloc(out_cap);
	if (out == NULL) {
		free(buf);
		return ESP_ERR_PERSIST_FAILED;
	}

	p = (buf != NULL) ? buf : "";
	while (*p != '\0') {
		const char *line_end = strchr(p, '\n');
		size_t line_len = (line_end != NULL) ? (size_t)(line_end - p) : strlen(p);
		int is_default = (line_len >= 7 && strncmp(p, "default", 7) == 0 &&
		                  (line_len == 7 || p[7] == ' ' || p[7] == '\t'));
		int is_timeout = (line_len >= 7 && strncmp(p, "timeout", 7) == 0 &&
		                  (line_len == 7 || p[7] == ' ' || p[7] == '\t'));

		if (is_default && new_default != NULL) {
			int n = snprintf(out + out_len, out_cap - out_len, "default %s\n", new_default);

			if (n < 0 || (size_t)n >= out_cap - out_len)
				goto no_room;
			out_len += (size_t)n;
			wrote_default = 1;
		} else if (is_timeout && new_timeout != NULL) {
			int n = snprintf(out + out_len, out_cap - out_len, "timeout %d\n", *new_timeout);

			if (n < 0 || (size_t)n >= out_cap - out_len)
				goto no_room;
			out_len += (size_t)n;
			wrote_timeout = 1;
		} else {
			if (out_len + line_len + 2 > out_cap)
				goto no_room;
			memcpy(out + out_len, p, line_len);
			out_len += line_len;
			out[out_len++] = '\n';
		}
		if (line_end == NULL)
			break;
		p = line_end + 1;
	}
	if (new_default != NULL && !wrote_default) {
		int n = snprintf(out + out_len, out_cap - out_len, "default %s\n", new_default);

		if (n < 0 || (size_t)n >= out_cap - out_len)
			goto no_room;
		out_len += (size_t)n;
	}
	if (new_timeout != NULL && !wrote_timeout) {
		int n = snprintf(out + out_len, out_cap - out_len, "timeout %d\n", *new_timeout);

		if (n < 0 || (size_t)n >= out_cap - out_len)
			goto no_room;
		out_len += (size_t)n;
	}

	if (persist_atomic_write(path, out, out_len) != 0)
		rc = ESP_ERR_PERSIST_FAILED;
	free(out);
	free(buf);
	return rc;

no_room:
	/* Nothing has been written to the ESP at this point -- the whole
	 * file is assembled in memory first, precisely so a failure here
	 * leaves the existing, working loader.conf untouched. */
	fprintf(stderr, "esp: loader.conf rewrite would not fit in %zu bytes -- not written\n",
	        out_cap);
	free(out);
	free(buf);
	return ESP_ERR_PERSIST_FAILED;
}

enum esp_error esp_loader_set(const char *default_pattern, const int *timeout)
{
	struct esp_entry entries[ESP_MAX_ENTRIES];
	int n, i;
	int matched = 0;

	if (g_loader_dir[0] == '\0' || access(g_loader_dir, F_OK) != 0)
		return ESP_ERR_NO_ESP;
	if (access(g_loader_dir, W_OK) != 0)
		return ESP_ERR_READ_ONLY;
	if (default_pattern != NULL) {
		if (default_pattern[0] == '\0' || strlen(default_pattern) >= ESP_DEFAULT_MAX ||
		    strchr(default_pattern, '\n') != NULL)
			return ESP_ERR_INVALID;
	}
	if (timeout != NULL && (*timeout < 0 || *timeout > 3600))
		return ESP_ERR_INVALID;

	/*
	 * A default matching nothing is the single most dangerous thing an
	 * operator can set here from a machine they cannot physically
	 * reach, and it is silent -- so it is refused, not warned about.
	 * The caller is told what does exist, which turns a bricking
	 * mistake into an obvious typo.
	 */
	if (default_pattern != NULL) {
		n = esp_entries_list(entries, ESP_MAX_ENTRIES);
		for (i = 0; i < n; i++) {
			if (esp_pattern_matches(default_pattern, entries[i].name))
				matched = 1;
		}
		if (n > 0 && !matched)
			return ESP_ERR_WOULD_ORPHAN;
	}
	return write_loader_conf(default_pattern, timeout);
}

enum esp_error esp_entry_delete(const char *name)
{
	struct esp_entry entries[ESP_MAX_ENTRIES];
	char path[PATH_MAX];
	int n, i;
	int found = 0;
	int running_slot_survivors = 0;
	int target_is_running_slot = 0;

	if (name == NULL || name[0] == '\0' || strchr(name, '/') != NULL ||
	    strstr(name, "..") != NULL)
		return ESP_ERR_INVALID;
	if (g_entries_dir[0] == '\0' || access(g_entries_dir, F_OK) != 0)
		return ESP_ERR_NO_ESP;
	if (access(g_entries_dir, W_OK) != 0)
		return ESP_ERR_READ_ONLY;

	n = esp_entries_list(entries, ESP_MAX_ENTRIES);
	for (i = 0; i < n; i++) {
		if (strcmp(entries[i].name, name) == 0) {
			found = 1;
			target_is_running_slot = entries[i].is_running_slot;
		} else if (entries[i].is_running_slot) {
			running_slot_survivors++;
		}
	}
	if (!found)
		return ESP_ERR_NOT_FOUND;
	/*
	 * Removing a duplicate entry for the running slot is exactly the
	 * cleanup this endpoint exists for, so it is allowed -- what is
	 * refused is removing the LAST one, which is how a remote operator
	 * makes a machine they cannot reach unbootable.
	 */
	if (target_is_running_slot && running_slot_survivors == 0)
		return ESP_ERR_WOULD_ORPHAN;
	/*
	 * With no --slot (not the installed control plane, so nothing can
	 * be attributed to a running slot) the guard above can never fire.
	 * Rather than silently protecting nothing, it degrades to the
	 * weaker claim that still always holds: never remove the last
	 * entry on the ESP. A guard that quietly stops applying is worse
	 * than one that is merely coarse.
	 */
	if (g_running_slot[0] == '\0' && n <= 1)
		return ESP_ERR_WOULD_ORPHAN;

	snprintf(path, sizeof(path), "%s/%s", g_entries_dir, name);
	if (unlink(path) != 0)
		return ESP_ERR_PERSIST_FAILED;
	return ESP_OK;
}

/*
 * Which entry systemd-boot would actually select, following its own
 * documented ordering rather than a guess:
 *
 *   1. entries whose counter has reached zero (no attempts left) sort
 *      to the END -- the bootloader will not choose one while any
 *      other candidate exists;
 *   2. the rest sort ASCENDING by entry id;
 *   3. the default pattern selects the FIRST match in that order.
 *
 * The direction matters and was worth checking against a real machine
 * rather than assuming: taking the LAST match instead names an entry
 * for the wrong A/B slot on a host carrying entries for both, which is
 * exactly the situation this endpoint exists to diagnose. An answer
 * that is confidently wrong here would be worse than no answer.
 *
 * Ties (several files sharing one id -- a confirmed entry plus its
 * leftover counted variants, which is common) are broken by filename
 * so the answer is at least deterministic; every such candidate boots
 * the same slot, which is the fact an operator is actually reading
 * this for.
 */
/*
 * systemd-boot's own entry ordering, as implemented by its
 * config_entry_compare() -- the pattern in loader.conf then selects the
 * FIRST entry in this order.
 *
 * Every rule below was pinned against real `bootctl list` output rather
 * than read off documentation, because an earlier version of this
 * function got two of them wrong in a way nothing caught: it ignored
 * sort-key/version entirely and ordered ids ASCENDING. That made it
 * report "will boot: cix-a.conf" on a host where real systemd-boot
 * selects cix-b -- the freshly staged update. Acting on that wrong
 * answer is what makes it dangerous: it says a correctly staged update
 * will not boot, and the obvious "fix" (pinning default to one slot)
 * genuinely breaks the next update in the other direction.
 *
 * The experiments, for anyone revisiting this:
 *   entries cix-a(version 1) + cix-b+3(version <ts>), default cix-*
 *     -> bootctl marks cix-b default                  (version wins, descending)
 *   equal versions, ids cix-a/cix-b   -> cix-b first  (id descending)
 *   no version field at all           -> cix-b first  (id descending)
 *   cix-b+0-3 (exhausted, newer ver)  -> cix-a first  (exhausted sorts last)
 *
 * strverscmp() is glibc's; systemd uses its own strverscmp_improved,
 * which differs on some corner cases (notably '~' and how it treats
 * non-alphanumerics). For the values this project actually writes --
 * "1" from the installer and a unix timestamp from every update -- the
 * two agree, and both order a timestamp above "1" numerically rather
 * than lexically. A recipe-authored entry using an exotic version
 * string could in principle diverge; that is a documented limit of
 * this model, not a silent one.
 */
static int selection_rank_less(const struct esp_entry *a, const struct esp_entry *b)
{
	char id_a[ESP_ENTRY_NAME_MAX];
	char id_b[ESP_ENTRY_NAME_MAX];
	int tries_a = -1;
	int tries_b = -1;
	int cmp;

	entry_id(a->name, id_a, sizeof(id_a), &tries_a);
	entry_id(b->name, id_b, sizeof(id_b), &tries_b);

	/* Entries with no attempts left are never selected while any other
	 * entry exists -- this is the whole rollback mechanism. */
	if ((tries_a == 0) != (tries_b == 0))
		return (tries_b == 0) ? 1 : 0;

	/* New-style ordering applies only when BOTH entries carry a
	 * sort-key, matching systemd-boot's own condition. */
	if (a->sort_key[0] != '\0' && b->sort_key[0] != '\0') {
		cmp = strcmp(a->sort_key, b->sort_key);
		if (cmp != 0)
			return cmp < 0; /* sort-key ascending */
		cmp = strverscmp(a->version, b->version);
		if (cmp != 0)
			return cmp > 0; /* version DESCENDING -- newer first */
	}
	cmp = strverscmp(id_a, id_b);
	if (cmp != 0)
		return cmp > 0; /* id DESCENDING */
	return strcmp(a->name, b->name) > 0;
}

static void selected_entry(const struct esp_entry *entries, int n, const char *pattern, char *out,
                           size_t out_size)
{
	int i;
	const struct esp_entry *best = NULL;

	out[0] = '\0';
	if (pattern == NULL || pattern[0] == '\0')
		return;
	for (i = 0; i < n; i++) {
		if (!entries[i].matches_default)
			continue;
		if (best == NULL || selection_rank_less(&entries[i], best))
			best = &entries[i];
	}
	if (best != NULL)
		snprintf(out, out_size, "%s", best->name);
}

void esp_write_json(struct json_writer *w)
{
	struct esp_entry entries[ESP_MAX_ENTRIES];
	char default_pattern[ESP_DEFAULT_MAX];
	char selected[ESP_ENTRY_NAME_MAX];
	char boot_next[ESP_ENTRY_NAME_MAX] = "";
	int timeout = -1;
	int writable = 0;
	int n, i;
	enum esp_error err;

	err = esp_loader_get(default_pattern, sizeof(default_pattern), &timeout, &writable);
	n = esp_entries_list(entries, ESP_MAX_ENTRIES);
	if (n < 0)
		n = 0;
	selected_entry(entries, n, default_pattern, selected, sizeof(selected));

	/*
	 * An armed one-shot (issue #154) overrides the default pattern
	 * entirely, so reporting the pattern's choice while a one-shot is
	 * set would name the wrong entry. Not hypothetical: this field
	 * said "cix-a.conf" on a real host that then booted cix-b, because
	 * the one-shot was ignored here.
	 *
	 * That is the same failure ADR-0202 records about this very field
	 * -- confidently wrong is worse than absent, because its whole
	 * purpose is to be believed -- so the one-shot is reported
	 * separately as well, rather than silently folded in. An operator
	 * needs to know both that the next boot differs AND that it
	 * differs only once.
	 */
	if (esp_boot_next_get(boot_next, sizeof(boot_next)))
		snprintf(selected, sizeof(selected), "%s", boot_next);

	jw_obj_open(w);
	jw_key(w, "present");
	jw_bool(w, err != ESP_ERR_NO_ESP);
	jw_key(w, "writable");
	jw_bool(w, writable);
	jw_key(w, "loader_dir");
	jw_str(w, g_loader_dir);
	jw_key(w, "default");
	if (default_pattern[0] != '\0')
		jw_str(w, default_pattern);
	else
		jw_null(w);
	jw_key(w, "timeout");
	if (timeout >= 0)
		jw_int(w, timeout);
	else
		jw_null(w);
	jw_key(w, "running_slot");
	if (g_running_slot[0] != '\0')
		jw_str(w, g_running_slot);
	else
		jw_null(w);
	/* The entry the firmware would actually boot next, and whether that
	 * is the slot this daemon is running from -- the two-line answer to
	 * "why did my update not take effect". */
	jw_key(w, "boot_next");
	if (boot_next[0] != '\0')
		jw_str(w, boot_next);
	else
		jw_null(w);
	jw_key(w, "selected_entry");
	if (selected[0] != '\0')
		jw_str(w, selected);
	else
		jw_null(w);
	jw_key(w, "entries");
	jw_arr_open(w);
	for (i = 0; i < n; i++) {
		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, entries[i].name);
		jw_key(w, "title");
		jw_str(w, entries[i].title);
		jw_key(w, "linux");
		jw_str(w, entries[i].linux_image);
		jw_key(w, "options");
		jw_str(w, entries[i].options);
		jw_key(w, "matches_default");
		jw_bool(w, entries[i].matches_default);
		jw_key(w, "is_running_slot");
		jw_bool(w, entries[i].is_running_slot);
		jw_obj_close(w);
	}
	jw_arr_close(w);
	jw_obj_close(w);
}

int esp_confirm_rank(const char *entry_name, const char *slot)
{
	char prefix[32];
	size_t prefix_len;
	const char *suffix;

	if (entry_name == NULL || slot == NULL)
		return 0;
	snprintf(prefix, sizeof(prefix), "cix-%s", slot);
	prefix_len = strlen(prefix);
	if (strncmp(entry_name, prefix, prefix_len) != 0)
		return 0;
	suffix = entry_name + prefix_len;
	/* The prefix alone is not enough: "cix-a" must be followed by '.'
	 * or '+', so "cix-abc.conf" is not taken for slot a. */
	if (suffix[0] == '+')
		return 2;
	if (suffix[0] == '.')
		return 1;
	return 0;
}

int esp_entry_to_confirm(const char *entries_dir, const char *slot, char *out, size_t out_size)
{
	DIR *d;
	struct dirent *de;
	int best = 0;

	if (entries_dir == NULL || slot == NULL || out == NULL || out_size == 0)
		return 0;
	d = opendir(entries_dir);
	if (d == NULL)
		return 0;
	/* Highest rank wins, so the result cannot depend on readdir()
	 * order -- which is the whole point (see esp_confirm_rank). */
	while ((de = readdir(d)) != NULL) {
		int rank = esp_confirm_rank(de->d_name, slot);

		if (rank > best) {
			best = rank;
			snprintf(out, out_size, "%s", de->d_name);
		}
	}
	closedir(d);
	return best > 0;
}

/*
 * ---- LoaderEntryOneShot: boot a chosen slot exactly once ----
 *
 * A different mechanism from everything above: loader entries are files
 * on the ESP, this is a variable in firmware NVRAM exposed through
 * efivarfs. Kept in this file because it answers the same question --
 * what boots next -- and because the guard below needs the entry list
 * this module already owns.
 *
 * Three details are not obvious and each one silently breaks the
 * variable if missed:
 *
 *  - The file's first four bytes are the EFI attribute word, not data.
 *    NV|BS|RT (0x7) is what systemd uses; a variable written without
 *    NON_VOLATILE would not survive the reboot it exists for.
 *  - efivarfs requires attributes and data in a SINGLE write(). A
 *    second write does not append, it replaces.
 *  - The kernel sets the immutable attribute on an existing variable
 *    file, so modifying or removing one means clearing that first. The
 *    ioctl legitimately fails on a plain filesystem, which is what a
 *    test directory is, so failure there is ignored rather than fatal.
 */
#define ESP_LOADER_GUID "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"
#define ESP_ONESHOT_VAR "LoaderEntryOneShot-" ESP_LOADER_GUID
#define ESP_EFI_ATTRS 0x00000007u /* NON_VOLATILE|BOOTSERVICE_ACCESS|RUNTIME_ACCESS */

/* Self-declared rather than via <linux/fs.h>, which clashes with glibc
 * headers under TCC the same way <linux/sched.h> does (see
 * include/linux_compat.h). These are the x86_64 encodings of
 * _IOR('f', 1, long) / _IOW('f', 2, long). */
#define ESP_FS_IOC_GETFLAGS 0x80086601UL
#define ESP_FS_IOC_SETFLAGS 0x40086602UL
#define ESP_FS_IMMUTABLE_FL 0x00000010L

static char g_efivars_dir[PATH_MAX] = "/sys/firmware/efi/efivars";

void esp_set_efivars_dir(const char *dir)
{
	if (dir != NULL && dir[0] != '\0')
		snprintf(g_efivars_dir, sizeof(g_efivars_dir), "%s", dir);
}

static void oneshot_path(char *out, size_t out_size)
{
	snprintf(out, out_size, "%s/%s", g_efivars_dir, ESP_ONESHOT_VAR);
}

/* Best-effort: a real efivarfs file is immutable, a test file is not. */
static void clear_immutable(const char *path)
{
	long flags = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0)
		return;
	if (ioctl(fd, ESP_FS_IOC_GETFLAGS, &flags) == 0 && (flags & ESP_FS_IMMUTABLE_FL) != 0) {
		flags &= ~ESP_FS_IMMUTABLE_FL;
		(void)ioctl(fd, ESP_FS_IOC_SETFLAGS, &flags);
	}
	close(fd);
}

enum esp_error esp_boot_next_set(const char *slot)
{
	struct esp_entry entries[ESP_MAX_ENTRIES];
	char wanted[ESP_ENTRY_NAME_MAX];
	char path[PATH_MAX];
	unsigned char buf[4 + (ESP_ENTRY_NAME_MAX + 1) * 2];
	size_t len, i, n;
	int count, found = 0;
	int fd;

	if (slot == NULL || slot[0] == '\0')
		return ESP_ERR_INVALID;

	/*
	 * systemd-boot identifies an entry by its filename with ".conf"
	 * kept and any boot counter stripped -- "cix-b.conf" for a file
	 * named "cix-b+3.conf". Confirmed against real `bootctl list`
	 * output rather than inferred, since writing the wrong spelling
	 * produces a variable the bootloader silently ignores.
	 */
	snprintf(wanted, sizeof(wanted), "cix-%s.conf", slot);

	count = esp_entries_list(entries, ESP_MAX_ENTRIES);
	if (count <= 0)
		return ESP_ERR_NO_ESP;
	for (i = 0; i < (size_t)count; i++) {
		char id[ESP_ENTRY_NAME_MAX];
		char id_conf[ESP_ENTRY_NAME_MAX];

		entry_id(entries[i].name, id, sizeof(id), NULL);
		snprintf(id_conf, sizeof(id_conf), "%s.conf", id);
		if (strcmp(id_conf, wanted) == 0) {
			found = 1;
			break;
		}
	}
	if (!found)
		return ESP_ERR_NOT_FOUND;

	len = strlen(wanted);
	buf[0] = (unsigned char)(ESP_EFI_ATTRS & 0xff);
	buf[1] = (unsigned char)((ESP_EFI_ATTRS >> 8) & 0xff);
	buf[2] = (unsigned char)((ESP_EFI_ATTRS >> 16) & 0xff);
	buf[3] = (unsigned char)((ESP_EFI_ATTRS >> 24) & 0xff);
	/* UTF-16LE. Entry ids are ASCII by construction here, so the
	 * high byte is always zero -- no general UTF-8 conversion is
	 * needed, and pretending otherwise would be dead code. */
	for (i = 0; i < len; i++) {
		buf[4 + i * 2] = (unsigned char)wanted[i];
		buf[4 + i * 2 + 1] = 0;
	}
	buf[4 + len * 2] = 0;
	buf[4 + len * 2 + 1] = 0;
	n = 4 + (len + 1) * 2;

	oneshot_path(path, sizeof(path));
	clear_immutable(path);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return (errno == EROFS || errno == EACCES || errno == EPERM) ? ESP_ERR_READ_ONLY
		                                                             : ESP_ERR_NO_ESP;
	if (write(fd, buf, n) != (ssize_t)n) {
		close(fd);
		return ESP_ERR_PERSIST_FAILED;
	}
	close(fd);
	return ESP_OK;
}

int esp_boot_next_get(char *out, size_t out_size)
{
	char path[PATH_MAX];
	unsigned char buf[4 + (ESP_ENTRY_NAME_MAX + 1) * 2];
	ssize_t got;
	size_t i, chars;
	int fd;

	if (out == NULL || out_size == 0)
		return 0;
	out[0] = '\0';
	oneshot_path(path, sizeof(path));
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	got = read(fd, buf, sizeof(buf));
	close(fd);
	if (got < 6) /* attributes plus at least one UTF-16 character */
		return 0;
	chars = ((size_t)got - 4) / 2;
	for (i = 0; i < chars && i + 1 < out_size; i++) {
		unsigned char lo = buf[4 + i * 2];

		if (lo == 0)
			break;
		out[i] = (char)lo;
	}
	out[i] = '\0';
	return out[0] != '\0';
}

enum esp_error esp_boot_next_clear(void)
{
	char path[PATH_MAX];

	oneshot_path(path, sizeof(path));
	clear_immutable(path);
	if (unlink(path) != 0) {
		if (errno == ENOENT)
			return ESP_OK; /* already disarmed -- the desired state */
		return (errno == EROFS || errno == EACCES || errno == EPERM) ? ESP_ERR_READ_ONLY
		                                                             : ESP_ERR_PERSIST_FAILED;
	}
	return ESP_OK;
}
