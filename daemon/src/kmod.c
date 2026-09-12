#include "kmod.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* KMOD_MODPROBE_BIN now lives in kmod.h -- cix-install checks for it
 * before offering to load NIC drivers, and two copies of a path are
 * exactly the kind of drift this codebase refuses. */
#define KMOD_MODINFO_BIN "/usr/bin/modinfo"
#define KMOD_MAX_OPTION_TOKENS 16
#define KMOD_MODINFO_OUT_MAX 8192
#define KMOD_PROC_MODULES_LINE_MAX 512

int kmod_name_is_valid(const char *name)
{
	size_t i, len;

	if (name == NULL || name[0] == '\0')
		return 0;
	len = strlen(name);
	if (len >= KMOD_NAME_MAX)
		return 0;
	for (i = 0; i < len; i++) {
		if (isspace((unsigned char)name[i]) || name[i] == '/')
			return 0;
	}
	return 1;
}

/* Same shape as pki.c's own run_openssl() -- fork+execve, the child's
 * stdout AND stderr merged into a pipe read back into out (if
 * non-NULL), discarded otherwise. Every daemon-owned fd is already
 * CLOEXEC from creation (ADR-0009), so the child only ever inherits
 * the one pipe fd it's meant to. */
static int run_kmod_tool(const char *bin, char *const argv[], char *out, size_t out_size)
{
	int pipefd[2];
	pid_t pid;
	int status;
	size_t total = 0;
	ssize_t n;

	if (out != NULL && out_size > 0)
		out[0] = '\0';

	if (pipe2(pipefd, O_CLOEXEC) != 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		dup2(pipefd[1], STDOUT_FILENO);
		dup2(pipefd[1], STDERR_FILENO);
		close(pipefd[0]);
		close(pipefd[1]);
		execve(bin, argv, environ);
		_exit(127);
	}

	close(pipefd[1]);
	if (out != NULL && out_size > 0) {
		while (total + 1 < out_size) {
			n = read(pipefd[0], out + total, out_size - total - 1);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			total += (size_t)n;
		}
		out[total] = '\0';
	} else {
		char discard[256];

		while (read(pipefd[0], discard, sizeof(discard)) > 0)
			;
	}
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) != pid)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

/* Splits a space-joined "key=value key2=value2" string into argv
 * tokens (modprobe's own native module-parameter syntax, passed
 * straight through unmodified). Returns the token count, capped at
 * KMOD_MAX_OPTION_TOKENS. buf is scratch space the returned pointers
 * point into -- must outlive use of tokens[]. */
static int split_options(char *buf, char *tokens[], int max_tokens)
{
	char *tok, *save = NULL;
	int count = 0;

	tok = strtok_r(buf, " \t\n\r", &save);
	while (tok != NULL && count < max_tokens) {
		tokens[count++] = tok;
		tok = strtok_r(NULL, " \t\n\r", &save);
	}
	return count;
}

int kmod_load(const char *name, const char *options, char *out, size_t out_size)
{
	char buf[KMOD_OPTIONS_MAX];
	char *tokens[KMOD_MAX_OPTION_TOKENS];
	int token_count = 0;
	char *argv[KMOD_MAX_OPTION_TOKENS + 3];
	int i;

	if (!kmod_name_is_valid(name))
		return -1;

	if (options != NULL && options[0] != '\0') {
		if (snprintf(buf, sizeof(buf), "%s", options) >= (int)sizeof(buf))
			return -1;
		token_count = split_options(buf, tokens, KMOD_MAX_OPTION_TOKENS);
	}

	argv[0] = (char *)"modprobe";
	argv[1] = (char *)name;
	for (i = 0; i < token_count; i++)
		argv[2 + i] = tokens[i];
	argv[2 + token_count] = NULL;

	return run_kmod_tool(KMOD_MODPROBE_BIN, argv, out, out_size);
}

int kmod_unload(const char *name, char *out, size_t out_size)
{
	char *argv[4];

	if (!kmod_name_is_valid(name))
		return -1;

	argv[0] = (char *)"modprobe";
	argv[1] = (char *)"-r";
	argv[2] = (char *)name;
	argv[3] = NULL;

	return run_kmod_tool(KMOD_MODPROBE_BIN, argv, out, out_size);
}

/*
 * '-' and '_' are the same character to modprobe, and the kernel
 * always reports the underscore form: usb-storage.ko is usb_storage in
 * /proc/modules. Comparing literally answers "not loaded" about a
 * module that is plainly loaded, which is worse than not asking.
 */
static int kmod_name_eq(const char *a, const char *b)
{
	size_t i;

	for (i = 0;; i++) {
		char ca = a[i] == '-' ? '_' : a[i];
		char cb = b[i] == '-' ? '_' : b[i];

		if (ca != cb)
			return 0;
		if (ca == '\0')
			return 1;
	}
}

int kmod_is_loaded(const char *name)
{
	FILE *f;
	char line[KMOD_PROC_MODULES_LINE_MAX];
	int found = 0;

	if (!kmod_name_is_valid(name))
		return 0;

	f = fopen("/proc/modules", "r");
	if (f == NULL)
		return 0;
	while (fgets(line, sizeof(line), f) != NULL) {
		char *save = NULL;
		const char *mod;

		line[strcspn(line, "\n")] = '\0';
		mod = strtok_r(line, " \t", &save);
		if (mod != NULL && kmod_name_eq(mod, name)) {
			found = 1;
			break;
		}
	}
	fclose(f);
	return found;
}

void kmod_write_json_loaded(struct json_writer *w)
{
	FILE *f;
	char line[KMOD_PROC_MODULES_LINE_MAX];

	jw_arr_open(w);

	f = fopen("/proc/modules", "r");
	if (f == NULL) {
		jw_arr_close(w);
		return;
	}

	while (fgets(line, sizeof(line), f) != NULL) {
		char *name, *size_str, *used_by_count_str, *deps, *state;
		char *save = NULL;
		long long size;
		long long used_by_count;

		line[strcspn(line, "\n")] = '\0';
		name = strtok_r(line, " \t", &save);
		size_str = strtok_r(NULL, " \t", &save);
		used_by_count_str = strtok_r(NULL, " \t", &save);
		deps = strtok_r(NULL, " \t", &save);
		state = strtok_r(NULL, " \t", &save);
		if (name == NULL || size_str == NULL || used_by_count_str == NULL || deps == NULL ||
		    state == NULL)
			continue;
		size = atoll(size_str);
		used_by_count = atoll(used_by_count_str);

		jw_obj_open(w);
		jw_key(w, "name");
		jw_str(w, name);
		jw_key(w, "size");
		jw_int(w, size);
		jw_key(w, "used_by_count");
		jw_int(w, used_by_count);
		jw_key(w, "used_by");
		jw_arr_open(w);
		if (strcmp(deps, "-") != 0) {
			char depbuf[KMOD_PROC_MODULES_LINE_MAX];
			char *dtok, *dsave = NULL;

			snprintf(depbuf, sizeof(depbuf), "%s", deps);
			dtok = strtok_r(depbuf, ",", &dsave);
			while (dtok != NULL) {
				if (dtok[0] != '\0')
					jw_str(w, dtok);
				dtok = strtok_r(NULL, ",", &dsave);
			}
		}
		jw_arr_close(w);
		jw_key(w, "state");
		jw_str(w, state);
		jw_obj_close(w);
	}
	fclose(f);

	jw_arr_close(w);
}

/* Trims leading/trailing whitespace in place, returns s. */
static char *trim(char *s)
{
	char *end;

	while (isspace((unsigned char)*s))
		s++;
	if (*s == '\0')
		return s;
	end = s + strlen(s) - 1;
	while (end > s && isspace((unsigned char)*end))
		*end-- = '\0';
	return s;
}

/*
 * Real kmod modinfo(8) output is "key:\t\tvalue\n" per line, with
 * "depends"/"alias" and (most usefully) "parm" repeated once per real
 * value -- "parm:  name:description (type)". Parsed field-by-field
 * rather than assuming a fixed field order or set, since not every
 * module reports every field (a module with no module_param() calls
 * at all has zero "parm:" lines, an out-of-tree module has no
 * "intree:" line, etc).
 */
/*
 * /sys/module/<name>/parameters/<key>, one file per parameter, each
 * holding the value the kernel is running with right now.
 *
 * The directory is named the way the kernel spells a loaded module, so
 * the caller's "usb-storage" has to become "usb_storage" first -- the
 * same '-'/'_' equivalence kmod_name_eq() exists for. A module that is
 * not loaded has no directory at all, and a module with no writable
 * parameters has an empty one; both are reported as an empty object
 * rather than an absent key, so a client never has to distinguish
 * "no parameters" from "this daemon forgot to look".
 *
 * A value is reported as the kernel's own text, untouched. Types here
 * are the module author's business (bool prints Y/N, int prints
 * digits, charp prints a string), and re-typing them would be this
 * daemon inventing a schema for data it does not own.
 */
static void kmod_write_json_current_params(const char *name, struct json_writer *w)
{
	char dirpath[PATH_MAX];
	char sysname[KMOD_NAME_MAX];
	DIR *dh;
	struct dirent *de;
	size_t i;

	jw_key(w, "current_params");
	jw_obj_open(w);

	for (i = 0; name[i] != '\0' && i + 1 < sizeof(sysname); i++)
		sysname[i] = name[i] == '-' ? '_' : name[i];
	sysname[i] = '\0';

	if (snprintf(dirpath, sizeof(dirpath), "/sys/module/%s/parameters", sysname) >=
	    (int)sizeof(dirpath)) {
		jw_obj_close(w);
		return;
	}

	dh = opendir(dirpath);
	if (dh == NULL) {
		jw_obj_close(w);
		return;
	}
	while ((de = readdir(dh)) != NULL) {
		char path[PATH_MAX];
		char value[256];
		FILE *f;
		size_t vlen;

		if (de->d_name[0] == '.')
			continue;
		if (snprintf(path, sizeof(path), "%s/%s", dirpath, de->d_name) >= (int)sizeof(path))
			continue;
		f = fopen(path, "r");
		if (f == NULL)
			continue; /* write-only parameters exist; not an error */
		if (fgets(value, sizeof(value), f) == NULL)
			value[0] = '\0';
		fclose(f);
		vlen = strlen(value);
		while (vlen > 0 && (value[vlen - 1] == '\n' || value[vlen - 1] == '\r'))
			value[--vlen] = '\0';
		jw_key(w, de->d_name);
		jw_str(w, value);
	}
	closedir(dh);
	jw_obj_close(w);
}

int kmod_write_json_info(const char *name, struct json_writer *w)
{
	char output[KMOD_MODINFO_OUT_MAX];
	char *argv[3];
	char *line, *save = NULL;
	int have_depends_field = 0;
	int in_tree = 0;
	struct json_writer params;

	if (!kmod_name_is_valid(name))
		return -1;

	argv[0] = (char *)"modinfo";
	argv[1] = (char *)name;
	argv[2] = NULL;

	if (run_kmod_tool(KMOD_MODINFO_BIN, argv, output, sizeof(output)) != 0)
		return -1;

	/* params is accumulated into its own writer first so "in_tree"
	 * (only known for certain once every line has been scanned) can
	 * still be written before "params" in the final object -- jw_key()
	 * requires values to be written in the same order as their keys,
	 * so the params array's own text is spliced in afterward via
	 * jw_raw_text(). */
	jw_init(&params);
	jw_arr_open(&params);

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, name);

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		char *colon = strchr(line, ':');

		if (colon != NULL) {
			char key[64];
			char *value;
			size_t key_len = (size_t)(colon - line);

			if (key_len >= sizeof(key))
				key_len = sizeof(key) - 1;
			memcpy(key, line, key_len);
			key[key_len] = '\0';
			value = trim(colon + 1);

			if (strcmp(key, "filename") == 0) {
				jw_key(w, "filename");
				jw_str(w, value);
			} else if (strcmp(key, "description") == 0) {
				jw_key(w, "description");
				jw_str(w, value);
			} else if (strcmp(key, "version") == 0) {
				jw_key(w, "version");
				jw_str(w, value);
			} else if (strcmp(key, "license") == 0) {
				jw_key(w, "license");
				jw_str(w, value);
			} else if (strcmp(key, "author") == 0) {
				jw_key(w, "author");
				jw_str(w, value);
			} else if (strcmp(key, "intree") == 0) {
				in_tree = (value[0] == 'Y' || value[0] == 'y');
			} else if (strcmp(key, "depends") == 0) {
				char *dtok, *dsave = NULL;

				have_depends_field = 1;
				jw_key(w, "depends");
				jw_arr_open(w);
				dtok = strtok_r(value, ",", &dsave);
				while (dtok != NULL) {
					char *d = trim(dtok);

					if (d[0] != '\0')
						jw_str(w, d);
					dtok = strtok_r(NULL, ",", &dsave);
				}
				jw_arr_close(w);
			} else if (strcmp(key, "parm") == 0) {
				/* "name:description (type)" -- type is the last
				 * parenthesized token; its own inner ':' (a
				 * description mentioning one) is not a delimiter,
				 * only the first ':' in the whole parm value is. */
				char *pcolon = strchr(value, ':');
				char *ptype_open = strrchr(value, '(');
				char pname[64];
				char pdesc[192];
				char ptype[32];

				ptype[0] = '\0';
				if (ptype_open != NULL) {
					char *ptype_close = strchr(ptype_open, ')');

					if (ptype_close != NULL && ptype_close > ptype_open) {
						size_t tlen = (size_t)(ptype_close - ptype_open - 1);

						if (tlen >= sizeof(ptype))
							tlen = sizeof(ptype) - 1;
						memcpy(ptype, ptype_open + 1, tlen);
						ptype[tlen] = '\0';
					}
					*ptype_open = '\0';
				}
				if (pcolon != NULL) {
					size_t nlen = (size_t)(pcolon - value);

					if (nlen >= sizeof(pname))
						nlen = sizeof(pname) - 1;
					memcpy(pname, value, nlen);
					pname[nlen] = '\0';
					snprintf(pdesc, sizeof(pdesc), "%s", trim(pcolon + 1));
				} else {
					snprintf(pname, sizeof(pname), "%s", trim(value));
					pdesc[0] = '\0';
				}

				jw_obj_open(&params);
				jw_key(&params, "name");
				jw_str(&params, pname);
				jw_key(&params, "type");
				jw_str(&params, ptype);
				jw_key(&params, "description");
				jw_str(&params, pdesc);
				jw_obj_close(&params);
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}

	if (!have_depends_field) {
		jw_key(w, "depends");
		jw_arr_open(w);
		jw_arr_close(w);
	}
	jw_key(w, "in_tree");
	jw_bool(w, in_tree);

	jw_arr_close(&params);
	jw_key(w, "params");
	jw_raw_text(w, params.buf, params.len);
	jw_free(&params);

	/*
	 * What the kernel ACTUALLY has, as opposed to what the module
	 * accepts. `params` above is modinfo: the parameters this module
	 * declares, with their types and descriptions, and it reads
	 * identically whether the module is loaded or not.
	 *
	 * That left an operator able to set a parameter and unable to
	 * confirm the kernel took it (#354). Setting one is not
	 * self-evidently effective: modprobe on an already-loaded module
	 * exits 0 without applying anything, a parameter can be rejected,
	 * and one declared read-only keeps its built-in default. "It
	 * worked" was an inference from an exit status every time.
	 */
	kmod_write_json_current_params(name, w);

	jw_obj_close(w);
	return 0;
}

int kmod_options_from_json(const struct json_value *jval, char *out, size_t out_size)
{
	size_t i;
	size_t len = 0;

	out[0] = '\0';
	if (jval == NULL)
		return 0;
	if (jval->type != JSON_OBJECT)
		return -1;

	for (i = 0; i < jval->u.object.count; i++) {
		const char *key = jval->u.object.keys[i];
		const char *value = json_as_string(jval->u.object.values[i]);
		size_t piece_len;

		if (value == NULL)
			return -1;
		piece_len = strlen(key) + 1 /* '=' */ + strlen(value);
		if (len + (i > 0 ? 1 : 0) + piece_len >= out_size)
			return -1;
		if (i > 0)
			out[len++] = ' ';
		len += (size_t)snprintf(out + len, out_size - len, "%s=%s", key, value);
	}
	out[len] = '\0';
	return 0;
}

void kmod_options_write_json(const char *options, struct json_writer *w)
{
	char buf[KMOD_OPTIONS_MAX];
	char *tok, *save = NULL;

	jw_obj_open(w);
	if (options != NULL && options[0] != '\0') {
		snprintf(buf, sizeof(buf), "%s", options);
		tok = strtok_r(buf, " \t\n\r", &save);
		while (tok != NULL) {
			char *eq = strchr(tok, '=');

			if (eq != NULL) {
				*eq = '\0';
				jw_key(w, tok);
				jw_str(w, eq + 1);
			} else {
				jw_key(w, tok);
				jw_str(w, "");
			}
			tok = strtok_r(NULL, " \t\n\r", &save);
		}
	}
	jw_obj_close(w);
}
