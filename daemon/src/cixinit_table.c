/*
 * ADR-0260: building cix-init's service table from a container's
 * declaration. See cixinit_table.h for the declared shape.
 */

#include "cixinit_table.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define DEFAULT_READY_TIMEOUT 30
#define DEFAULT_RESTART_DELAY 2
#define DEFAULT_STOP_TIMEOUT 10

static const struct {
	const char *name;
	int sig;
} g_signals[] = {
	{ "SIGTERM", SIGTERM }, { "SIGINT", SIGINT },   { "SIGHUP", SIGHUP },
	{ "SIGQUIT", SIGQUIT }, { "SIGUSR1", SIGUSR1 }, { "SIGUSR2", SIGUSR2 },
	{ "SIGKILL", SIGKILL },
};

int cixinit_signal_from_name(const char *name)
{
	size_t i;

	if (name == NULL)
		return -1;
	if (strncmp(name, "SIG", 3) != 0) {
		/* "TERM" is accepted as "SIGTERM"; nothing else is guessed at. */
		for (i = 0; i < sizeof(g_signals) / sizeof(g_signals[0]); i++)
			if (strcmp(g_signals[i].name + 3, name) == 0)
				return g_signals[i].sig;
		return -1;
	}
	for (i = 0; i < sizeof(g_signals) / sizeof(g_signals[0]); i++)
		if (strcmp(g_signals[i].name, name) == 0)
			return g_signals[i].sig;
	return -1;
}

const char *cixinit_signal_name(int sig)
{
	size_t i;

	for (i = 0; i < sizeof(g_signals) / sizeof(g_signals[0]); i++)
		if (g_signals[i].sig == sig)
			return g_signals[i].name;
	return "?";
}

const char *cixinit_type_name(int type)
{
	return type == CIXINIT_TYPE_ONESHOT ? "oneshot" : "daemon";
}

const char *cixinit_on_exit_name(int on_exit)
{
	switch (on_exit) {
	case CIXINIT_ON_EXIT_RESTART: return "restart";
	case CIXINIT_ON_EXIT_STOP: return "stop";
	case CIXINIT_ON_EXIT_FAIL_CONTAINER: return "fail-container";
	default: return "?";
	}
}

static int name_ok(const char *name)
{
	size_t i, n;

	if (name == NULL)
		return 0;
	n = strlen(name);
	if (n == 0 || n >= CIXINIT_NAME_MAX)
		return 0;
	for (i = 0; i < n; i++)
		if (!isalnum((unsigned char)name[i]) && name[i] != '_' && name[i] != '-')
			return 0;
	return 1;
}

/* Packs a JSON array of strings as NUL-separated, double-NUL-terminated argv. */
static int pack_argv(const struct json_value *jarr, char *dst, size_t size, const char *what,
                     const char *svc, char *err, size_t err_size)
{
	size_t i, off = 0;

	if (jarr == NULL || jarr->type != JSON_ARRAY || jarr->u.array.count < 1 ||
	    jarr->u.array.count > CIXINIT_ARGC_MAX) {
		snprintf(err, err_size, "service '%s': %s must be an array of 1 to %d strings", svc, what,
		         CIXINIT_ARGC_MAX);
		return -1;
	}
	memset(dst, 0, size);
	for (i = 0; i < jarr->u.array.count; i++) {
		const char *s = json_as_string(jarr->u.array.items[i]);
		size_t len;

		if (s == NULL) {
			snprintf(err, err_size, "service '%s': %s must be an array of strings", svc, what);
			return -1;
		}
		len = strlen(s) + 1;
		if (i == 0 && s[0] == '\0') {
			snprintf(err, err_size, "service '%s': %s[0] must name a program", svc, what);
			return -1;
		}
		if (off + len + 1 > size) {
			snprintf(err, err_size, "service '%s': %s is longer than %zu bytes packed", svc, what,
			         size - 1);
			return -1;
		}
		memcpy(dst + off, s, len);
		off += len;
	}
	return 0;
}

static int pack_cstr_argv(char *dst, size_t size, char *const argv[])
{
	size_t off = 0;
	int i;

	memset(dst, 0, size);
	for (i = 0; argv[i] != NULL; i++) {
		size_t len = strlen(argv[i]) + 1;

		if (i >= CIXINIT_ARGC_MAX || off + len + 1 > size)
			return -1;
		memcpy(dst + off, argv[i], len);
		off += len;
	}
	return i > 0 ? 0 : -1;
}

static int int_field(const struct json_value *obj, const char *key, int lo, int hi, int dflt, int *out,
                     const char *svc, char *err, size_t err_size)
{
	const struct json_value *j = json_object_get(obj, key);
	double v;

	if (j == NULL) {
		*out = dflt;
		return 0;
	}
	if (j->type != JSON_NUMBER) {
		snprintf(err, err_size, "service '%s': %s must be a number", svc, key);
		return -1;
	}
	v = json_as_number(j);
	if (v < lo || v > hi || v != (double)(int)v) {
		snprintf(err, err_size, "service '%s': %s must be %d-%d", svc, key, lo, hi);
		return -1;
	}
	*out = (int)v;
	return 0;
}

/*
 * One service record from its JSON object. after[] is filled with the
 * NAMES it depends on, resolved to indices once every name is known.
 */
static int parse_one(const struct json_value *item,
                     struct cixinit_service *s, char after[][CIXINIT_NAME_MAX], int *after_count,
                     char *err, size_t err_size)
{
	const char *name = json_as_string(json_object_get(item, "name"));
	const char *type = json_as_string(json_object_get(item, "type"));
	const char *on_exit = json_as_string(json_object_get(item, "on_exit"));
	const char *stop_signal = json_as_string(json_object_get(item, "stop_signal"));
	const struct json_value *jafter = json_object_get(item, "after");
	const struct json_value *jready = json_object_get(item, "ready");
	int v;
	size_t i;

	if (item == NULL || item->type != JSON_OBJECT) {
		snprintf(err, err_size, "each service must be an object");
		return -1;
	}
	if (!name_ok(name)) {
		snprintf(err, err_size, "each service needs a name of 1 to %d characters from letters, "
		                        "digits, - and _", CIXINIT_NAME_MAX - 1);
		return -1;
	}
	memset(s, 0, sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);

	if (type == NULL || strcmp(type, "daemon") == 0) {
		s->type = CIXINIT_TYPE_DAEMON;
	} else if (strcmp(type, "oneshot") == 0) {
		s->type = CIXINIT_TYPE_ONESHOT;
	} else {
		snprintf(err, err_size, "service '%s': type must be \"daemon\" or \"oneshot\"", name);
		return -1;
	}

	if (pack_argv(json_object_get(item, "cmd"), s->argv, sizeof(s->argv), "cmd", name, err,
	              err_size) != 0)
		return -1;

	*after_count = 0;
	if (jafter != NULL) {
		if (jafter->type != JSON_ARRAY || jafter->u.array.count > CIXINIT_MAX_SERVICES) {
			snprintf(err, err_size, "service '%s': after must be an array of service names", name);
			return -1;
		}
		for (i = 0; i < jafter->u.array.count; i++) {
			const char *dep = json_as_string(jafter->u.array.items[i]);

			if (!name_ok(dep)) {
				snprintf(err, err_size, "service '%s': after must be an array of service names", name);
				return -1;
			}
			if (strcmp(dep, name) == 0) {
				snprintf(err, err_size, "service '%s' cannot be after itself", name);
				return -1;
			}
			snprintf(after[(*after_count)++], CIXINIT_NAME_MAX, "%s", dep);
		}
	}

	s->ready_kind = CIXINIT_READY_NONE;
	if (jready != NULL) {
		const struct json_value *jport, *jsock, *jcmd;
		int kinds = 0;

		if (jready->type != JSON_OBJECT) {
			snprintf(err, err_size, "service '%s': ready must be an object", name);
			return -1;
		}
		jport = json_object_get(jready, "tcp_port");
		jsock = json_object_get(jready, "socket");
		jcmd = json_object_get(jready, "command");
		kinds = (jport != NULL) + (jsock != NULL) + (jcmd != NULL);
		if (kinds != 1) {
			snprintf(err, err_size,
			         "service '%s': ready needs exactly one of tcp_port, socket or command", name);
			return -1;
		}
		if (jport != NULL) {
			if (int_field(jready, "tcp_port", 1, 65535, 0, &v, name, err, err_size) != 0)
				return -1;
			s->ready_kind = CIXINIT_READY_TCP;
			s->ready_port = v;
		} else if (jsock != NULL) {
			const char *path = json_as_string(jsock);

			if (path == NULL || path[0] != '/' || strlen(path) >= CIXINIT_PATH_MAX) {
				snprintf(err, err_size, "service '%s': ready.socket must be an absolute path under %d bytes",
				         name, CIXINIT_PATH_MAX);
				return -1;
			}
			s->ready_kind = CIXINIT_READY_SOCKET;
			snprintf(s->ready_path, sizeof(s->ready_path), "%s", path);
		} else {
			if (pack_argv(jcmd, s->ready_path, sizeof(s->ready_path), "ready.command", name, err,
			              err_size) != 0)
				return -1;
			s->ready_kind = CIXINIT_READY_COMMAND;
		}
		if (int_field(jready, "timeout_seconds", 1, 300, DEFAULT_READY_TIMEOUT, &v, name, err,
		              err_size) != 0)
			return -1;
		s->ready_timeout_seconds = v;
	} else {
		s->ready_timeout_seconds = DEFAULT_READY_TIMEOUT;
	}

	if (on_exit == NULL) {
		s->on_exit = s->type == CIXINIT_TYPE_ONESHOT ? CIXINIT_ON_EXIT_FAIL_CONTAINER
		                                             : CIXINIT_ON_EXIT_RESTART;
	} else if (strcmp(on_exit, "restart") == 0) {
		s->on_exit = CIXINIT_ON_EXIT_RESTART;
	} else if (strcmp(on_exit, "stop") == 0) {
		s->on_exit = CIXINIT_ON_EXIT_STOP;
	} else if (strcmp(on_exit, "fail-container") == 0) {
		s->on_exit = CIXINIT_ON_EXIT_FAIL_CONTAINER;
	} else {
		snprintf(err, err_size,
		         "service '%s': on_exit must be \"restart\", \"stop\" or \"fail-container\"", name);
		return -1;
	}
	if (s->type == CIXINIT_TYPE_ONESHOT && s->on_exit == CIXINIT_ON_EXIT_RESTART) {
		snprintf(err, err_size, "service '%s': a oneshot runs to completion and cannot restart", name);
		return -1;
	}

	if (int_field(item, "restart_delay_seconds", 1, 300, DEFAULT_RESTART_DELAY, &v, name, err,
	              err_size) != 0)
		return -1;
	s->restart_delay_seconds = v;

	if (stop_signal == NULL) {
		s->stop_signal = SIGTERM;
	} else {
		s->stop_signal = cixinit_signal_from_name(stop_signal);
		if (s->stop_signal < 0) {
			snprintf(err, err_size, "service '%s': stop_signal must be one of SIGTERM, SIGINT, SIGHUP, "
			                        "SIGQUIT, SIGUSR1, SIGUSR2, SIGKILL", name);
			return -1;
		}
	}
	if (int_field(item, "stop_timeout_seconds", 1, 300, DEFAULT_STOP_TIMEOUT, &v, name, err,
	              err_size) != 0)
		return -1;
	s->stop_timeout_seconds = v;

	if (int_field(item, "uid", 0, 2147483647, -1, &v, name, err, err_size) != 0)
		return -1;
	s->uid = v;
	if (int_field(item, "gid", 0, 2147483647, -1, &v, name, err, err_size) != 0)
		return -1;
	s->gid = v;
	return 0;
}

int cixinit_table_from_json(const struct json_value *jservices, const unsigned int *addr_be,
                            int addr_count, struct cixinit_table *out, char *err, size_t err_size)
{
	struct cixinit_service parsed[CIXINIT_MAX_SERVICES];
	char after[CIXINIT_MAX_SERVICES][CIXINIT_MAX_SERVICES][CIXINIT_NAME_MAX];
	int after_count[CIXINIT_MAX_SERVICES];
	unsigned int deps[CIXINIT_MAX_SERVICES]; /* body-index bitmask of what each waits on */
	int placed[CIXINIT_MAX_SERVICES];
	int body_to_table[CIXINIT_MAX_SERVICES];
	int n, i, j, k;

	memset(out, 0, sizeof(*out));
	if (jservices == NULL || jservices->type != JSON_ARRAY || jservices->u.array.count < 1 ||
	    jservices->u.array.count > CIXINIT_MAX_SERVICES) {
		snprintf(err, err_size, "services must be an array of 1 to %d entries", CIXINIT_MAX_SERVICES);
		return -1;
	}
	n = (int)jservices->u.array.count;

	for (i = 0; i < n; i++) {
		if (parse_one(jservices->u.array.items[i], &parsed[i], after[i],
		              &after_count[i], err, err_size) != 0)
			return -1;
		for (j = 0; j < i; j++) {
			if (strcmp(parsed[j].name, parsed[i].name) == 0) {
				snprintf(err, err_size, "service name '%s' is declared more than once", parsed[i].name);
				return -1;
			}
		}
	}

	/* Resolve `after` names to body indices. */
	for (i = 0; i < n; i++) {
		deps[i] = 0;
		for (j = 0; j < after_count[i]; j++) {
			int found = -1;

			for (k = 0; k < n; k++)
				if (strcmp(parsed[k].name, after[i][j]) == 0)
					found = k;
			if (found < 0) {
				snprintf(err, err_size, "service '%s' is after '%s', which is not declared",
				         parsed[i].name, after[i][j]);
				return -1;
			}
			deps[i] |= 1u << found;
		}
	}

	/*
	 * Topological order, declaration order preserved wherever the
	 * graph allows: each pass takes the first unplaced service whose
	 * dependencies are all placed. A pass that places nothing has found
	 * a cycle.
	 */
	memset(placed, 0, sizeof(placed));
	out->count = 0;
	while (out->count < n) {
		int took = 0;

		for (i = 0; i < n && !took; i++) {
			unsigned int placed_mask = 0;

			if (placed[i])
				continue;
			for (k = 0; k < n; k++)
				if (placed[k])
					placed_mask |= 1u << k;
			if ((deps[i] & ~placed_mask) != 0)
				continue;
			placed[i] = 1;
			body_to_table[i] = out->count;
			out->order[out->count] = i;
			out->svc[out->count] = parsed[i];
			out->count++;
			took = 1;
		}
		if (!took) {
			for (i = 0; i < n; i++) {
				if (!placed[i]) {
					snprintf(err, err_size, "service '%s' is part of an `after` cycle", parsed[i].name);
					return -1;
				}
			}
		}
	}

	/* Rewrite each record's dependencies as a mask of TABLE indices. */
	for (i = 0; i < out->count; i++) {
		int body = out->order[i];

		out->svc[i].after_mask = 0;
		for (k = 0; k < n; k++)
			if ((deps[body] & (1u << k)) != 0)
				out->svc[i].after_mask |= 1u << body_to_table[k];
	}

	out->hello.magic = CIXINIT_MAGIC;
	out->hello.version = CIXINIT_VERSION;
	out->hello.service_count = out->count;
	/*
	 * #477: the container's own addresses, which a TCP readiness probe
	 * tries after loopback. Refused rather than truncated -- a probe
	 * missing the one address a service bound to would report the
	 * service as never ready, and silently dropping addresses is how
	 * that would happen without anything saying so. The caller's array
	 * is bounded by CONTAINER_MAX_NETWORKS and the tripwire in
	 * cixinit_table.h says the two agree, so this is unreachable
	 * today and is here for the day one of them moves.
	 */
	if (addr_count < 0 || addr_count > CIXINIT_MAX_ADDRS) {
		snprintf(err, err_size, "a container with more than %d addresses cannot declare services",
		         CIXINIT_MAX_ADDRS);
		return -1;
	}
	out->hello.addr_count = addr_count;
	for (i = 0; i < addr_count; i++)
		out->hello.addr_be[i] = addr_be[i];
	return 0;
}

int cixinit_table_single(struct cixinit_table *out, const char *name, char *const argv[], int type,
                         int on_exit)
{
	struct cixinit_service *s;

	memset(out, 0, sizeof(*out));
	if (!name_ok(name) || argv == NULL || argv[0] == NULL)
		return -1;
	s = &out->svc[0];
	snprintf(s->name, sizeof(s->name), "%s", name);
	if (pack_cstr_argv(s->argv, sizeof(s->argv), argv) != 0)
		return -1;
	s->type = type;
	s->ready_kind = CIXINIT_READY_NONE;
	s->ready_timeout_seconds = DEFAULT_READY_TIMEOUT;
	s->on_exit = on_exit;
	s->restart_delay_seconds = DEFAULT_RESTART_DELAY;
	s->stop_signal = SIGTERM;
	s->stop_timeout_seconds = DEFAULT_STOP_TIMEOUT;
	s->uid = -1;
	s->gid = -1;
	out->count = 1;
	out->order[0] = 0;
	out->hello.magic = CIXINIT_MAGIC;
	out->hello.version = CIXINIT_VERSION;
	out->hello.service_count = 1;
	return 0;
}

int cixinit_table_index(const struct cixinit_table *t, const char *name)
{
	int i;

	if (name == NULL)
		return -1;
	for (i = 0; i < t->count; i++)
		if (strcmp(t->svc[i].name, name) == 0)
			return i;
	return -1;
}

size_t cixinit_table_socket_bytes(const struct cixinit_table *t)
{
	return sizeof(t->hello) + (size_t)t->count * sizeof(t->svc[0]);
}

int cixinit_table_send(const struct cixinit_table *t, int fd)
{
	int i;

	if (send(fd, &t->hello, sizeof(t->hello), MSG_NOSIGNAL) != (ssize_t)sizeof(t->hello))
		return -1;
	for (i = 0; i < t->count; i++)
		if (send(fd, &t->svc[i], sizeof(t->svc[i]), MSG_NOSIGNAL) != (ssize_t)sizeof(t->svc[i]))
			return -1;
	return 0;
}
