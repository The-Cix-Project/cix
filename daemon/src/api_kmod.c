#include "api_kmod.h"

#include "apiresp.h"
#include "http.h"
#include "json.h"
#include "kmod.h"
#include "kmodconfig.h"
#include "kmodpolicy.h"
#include "kmodscan.h"
#include "logstore.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int blacklisted_now(const char *name)
{
	return kmod_policy_blacklists(kmodpolicy_current(), name);
}

/*
 * ADR-0159 Phase A: POST/DELETE/GET /v1/system/kmod/{name} (real
 * modprobe/modinfo, see kmod.c) and GET /v1/system/kmod (a live
 * /proc/modules dump). A modprobe/modinfo failure -- overwhelmingly
 * "no such module" in practice, the one case a bare exit code can't
 * usefully distinguish from "bad option" or "tool not staged" -- is
 * reported as 404 uniformly across all three, rather than guessing at
 * a finer-grained status from output this project never parses for
 * that purpose.
 *
 * That reasoning holds for the STATUS and never held for the MESSAGE,
 * which is a distinction this file did not make. A real `modprobe -r`
 * refusal on 192.168.15.95 reported "modprobe -r could not unload this
 * module", full stop: modprobe's own explanation was captured into a
 * pipe and thrown away, so the one party that knew why could not say
 * so. Quoting the tool verbatim is not parsing it for a status code,
 * and the message now carries whatever modprobe said.
 */
#define KMOD_TOOL_OUT_MAX 512

/*
 * One error body carrying both what this daemon was trying to do and
 * what the tool said about it. modprobe writes its real reason to
 * stderr -- "FATAL: Module usb_storage is in use.", "is builtin.",
 * "not found in directory /lib/modules/..." -- each pointing at a
 * different fix, and none of which survived being discarded.
 *
 * Newlines fold to spaces because this becomes a JSON string an
 * operator reads on one line, and modprobe's output is usually one
 * sentence with a trailing newline anyway.
 */
static void respond_kmod_tool_error(int fd, const char *what, char *tool_out)
{
	char msg[KMOD_TOOL_OUT_MAX + 128];
	size_t i;

	for (i = 0; tool_out[i] != '\0'; i++) {
		if (tool_out[i] == '\n' || tool_out[i] == '\r' || tool_out[i] == '\t')
			tool_out[i] = ' ';
	}
	while (i > 0 && tool_out[i - 1] == ' ')
		tool_out[--i] = '\0';

	if (tool_out[0] != '\0')
		snprintf(msg, sizeof(msg), "%s: %s", what, tool_out);
	else
		snprintf(msg, sizeof(msg), "%s, and said nothing about why", what);
	respond_error(fd, 404, "Not Found", msg);
}

static void respond_kmod_queue_busy(int fd)
{
	respond_error_hdrs(fd, 503, "Service Unavailable", "Retry-After: 1\r\n",
	                   "the module queue is full -- try again shortly", 0);
}

/*
 * The answer to an explicit load, from its job's report: the first
 * refusal is a 409 naming the module and the reason, a failure is
 * modprobe's own words, and options asked for a module already loaded
 * are a 409 saying why they cannot apply. Otherwise 200, with the
 * options used and what the name resolved to.
 */
static void respond_kmod_load(int fd, const char *name, const char *options,
                              const struct kmodscan_report *rep)
{
	struct json_writer w;
	int i;

	if (rep->entry_count == 0) {
		respond_error(fd, 500, "Internal Server Error",
		              "the module load produced no outcome -- see the log");
		return;
	}
	for (i = 0; i < rep->entry_count; i++) {
		const struct kmodscan_entry *e = &rep->entries[i];

		if (e->outcome == KMODSCAN_REFUSED ||
		    (e->outcome == KMODSCAN_ALREADY_LOADED && e->reason[0] != '\0')) {
			respond_error(fd, 409, "Conflict", e->reason);
			return;
		}
		if (e->outcome == KMODSCAN_FAILED) {
			char tool_out[KMOD_TOOL_OUT_MAX];

			snprintf(tool_out, sizeof(tool_out), "%s", e->reason);
			respond_kmod_tool_error(fd, "modprobe could not load this module", tool_out);
			return;
		}
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "options");
	kmod_options_write_json(options, &w);
	jw_key(&w, "modules");
	jw_arr_open(&w);
	for (i = 0; i < rep->entry_count; i++) {
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, rep->entries[i].module);
		jw_key(&w, "outcome");
		jw_str(&w, kmodscan_outcome_name(rep->entries[i].outcome));
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_kmod_post(int fd, const char *name, const char *body, size_t body_len)
{
	static struct kmodscan_job job; /* a page and more: not on the stack */
	char options[KMOD_OPTIONS_MAX];
	struct json_value *root = NULL;
	const struct json_value *joptions = NULL;
	struct kmodscan_report *rep;
	int has_options = 0;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	if (body_len > 0) {
		root = json_parse(body, body_len);
		if (root == NULL) {
			respond_error(fd, 400, "Bad Request", "invalid JSON body");
			return;
		}
		joptions = json_object_get(root, "options");
	}

	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "options must be an object of string values");
			return;
		}
		has_options = 1;
	} else {
		const char *def = kmodconfig_get_options(name);

		snprintf(options, sizeof(options), "%s", def != NULL ? def : "");
	}
	json_free(root);

	/*
	 * ADR-0348: the decision and the load run in the module queue, off
	 * the loop, and this request waits its turn. The job decides the
	 * name as given and each module it resolves to before anything
	 * loads -- the blacklist beats an explicit request -- and refuses
	 * options for a module already loaded, since modprobe would answer
	 * success for parameters the kernel never saw. That check is the
	 * job's rather than this handler's because this handler runs again,
	 * from the start, once the job has loaded the module.
	 */
	memset(&job, 0, sizeof(job));
	job.kind = KMODSCAN_JOB_LOAD;
	snprintf(job.name, sizeof(job.name), "%s", name);
	if (has_options) {
		snprintf(job.options, sizeof(job.options), "%s", options);
		job.has_options = 1;
	}
	switch (kmodscan_ask(&job, &rep)) {
	case KMODSCAN_PARKED:
		return;
	case KMODSCAN_READY:
		break;
	case KMODSCAN_NO_RESULT:
		respond_error(fd, 500, "Internal Server Error",
		              "the module load ended without a result -- see the log");
		return;
	case KMODSCAN_QUEUED:
	case KMODSCAN_BUSY:
	default:
		respond_kmod_queue_busy(fd);
		return;
	}
	respond_kmod_load(fd, name, options, rep);
	kmodscan_report_free(rep);
	free(rep);
}

void handle_kmod_delete(int fd, const char *name)
{
	static struct kmodscan_job job;
	struct kmodscan_report *rep;
	const struct kmodscan_entry *e;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	memset(&job, 0, sizeof(job));
	job.kind = KMODSCAN_JOB_UNLOAD;
	snprintf(job.name, sizeof(job.name), "%s", name);
	switch (kmodscan_ask(&job, &rep)) {
	case KMODSCAN_PARKED:
		return;
	case KMODSCAN_READY:
		break;
	case KMODSCAN_NO_RESULT:
		respond_error(fd, 500, "Internal Server Error",
		              "the module unload ended without a result -- see the log");
		return;
	case KMODSCAN_QUEUED:
	case KMODSCAN_BUSY:
	default:
		respond_kmod_queue_busy(fd);
		return;
	}
	e = rep->entry_count > 0 ? &rep->entries[0] : NULL;
	if (e == NULL || e->outcome != KMODSCAN_UNLOADED) {
		char tool_out[KMOD_TOOL_OUT_MAX];

		snprintf(tool_out, sizeof(tool_out), "%s", e != NULL ? e->reason : "");
		respond_kmod_tool_error(fd, "modprobe -r could not unload this module", tool_out);
	} else {
		http_set_blocking(fd);
		http_write_response(fd, 204, "No Content", "application/json", "", 0);
	}
	kmodscan_report_free(rep);
	free(rep);
}

void handle_kmod_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "modules");
	kmod_write_json_loaded(&w, blacklisted_now);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_kmod_get(int fd, const char *name)
{
	struct json_writer w;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	jw_init(&w);
	if (kmod_write_json_info(name, &w) != 0) {
		jw_free(&w);
		respond_error(fd, 404, "Not Found", "no such module (not built/available)");
		return;
	}
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

static void respond_kmodconfig_error(int fd, enum kmodconfig_error kerr)
{
	switch (kerr) {
	case KMODCONFIG_ERR_INVALID_NAME:
		respond_error(fd, 400, "Bad Request", "invalid module name, or options too long");
		break;
	case KMODCONFIG_ERR_FULL:
		respond_error(fd, 500, "Internal Server Error", "kmod config table full");
		break;
	case KMODCONFIG_ERR_NOT_FOUND:
		respond_error(fd, 404, "Not Found", "no persisted kmod config for this module");
		break;
	case KMODCONFIG_ERR_PERSIST_FAILED:
	default:
		respond_error(fd, 500, "Internal Server Error", "kmod config operation failed");
		break;
	}
}

void handle_kmodconfig_put(int fd, const char *name, const char *body, size_t body_len)
{
	struct json_value *root;
	const struct json_value *joptions;
	const struct json_value *jautoload;
	char options[KMOD_OPTIONS_MAX];
	const char *options_ptr = NULL;
	int has_autoload = 0;
	int autoload_value = 0;
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}

	joptions = json_object_get(root, "default_options");
	if (joptions != NULL) {
		if (kmod_options_from_json(joptions, options, sizeof(options)) != 0) {
			json_free(root);
			respond_error(fd, 400, "Bad Request",
			              "default_options must be an object of string values");
			return;
		}
		options_ptr = options;
	}

	jautoload = json_object_get(root, "autoload");
	if (jautoload != NULL) {
		if (jautoload->type != JSON_BOOL) {
			json_free(root);
			respond_error(fd, 400, "Bad Request", "autoload must be a boolean");
			return;
		}
		has_autoload = 1;
		autoload_value = jautoload->u.boolean;
	}
	json_free(root);

	if (options_ptr == NULL && !has_autoload) {
		respond_error(fd, 400, "Bad Request",
		              "at least one of default_options/autoload is required");
		return;
	}

	kerr = kmodconfig_set(name, options_ptr, has_autoload, autoload_value);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}

	{
		struct json_writer w;

		jw_init(&w);
		kmodconfig_write_json_one(name, &w);
		respond_json(fd, 200, "OK", &w);
		jw_free(&w);
	}
}

void handle_kmodconfig_delete(int fd, const char *name)
{
	enum kmodconfig_error kerr;

	if (!kmod_name_is_valid(name)) {
		respond_error(fd, 400, "Bad Request", "invalid module name");
		return;
	}
	kerr = kmodconfig_delete(name);
	if (kerr != KMODCONFIG_OK) {
		respond_kmodconfig_error(fd, kerr);
		return;
	}
	http_set_blocking(fd);
	http_write_response(fd, 204, "No Content", "application/json", "", 0);
}

void handle_kmodconfig_list(int fd)
{
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "kmod_config");
	kmodconfig_write_json_list(&w);
	jw_obj_close(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

void handle_kmodpolicy_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	kmodpolicy_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/*
 * ADR-0348 rule 7: blacklisting never unloads, so a PUT that blacklists
 * a module already loaded says so, once, naming it. "Newly" is against
 * the policy before this PUT, so a PUT that repeats the same blacklist
 * says nothing again.
 */
static void warn_newly_blacklisted_loaded(const struct kmod_policy *before,
                                          const struct kmod_policy *after)
{
	int i;

	for (i = 0; i < after->blacklist_count; i++) {
		const char *name = after->blacklist[i];

		if (kmod_policy_blacklists(before, name) || !kmod_is_loaded(name))
			continue;
		logstore_write("cixd", "warning",
		               "kmod policy: %s is now blacklisted and is loaded; it stays loaded "
		               "until it is unloaded (DELETE /v1/system/kmod/%s) (ADR-0348)",
		               name, name);
	}
}

void handle_kmodpolicy_put(int fd, const char *body, size_t body_len)
{
	struct kmod_policy before = *kmodpolicy_current();
	struct kmod_policy next = before;
	struct json_value *root;
	char err[192];

	root = json_parse(body, body_len);
	if (root == NULL) {
		respond_error(fd, 400, "Bad Request", "invalid JSON body");
		return;
	}
	if (kmodpolicy_apply_json(root, &next, err, sizeof(err)) != 0) {
		json_free(root);
		respond_error(fd, 400, "Bad Request", err);
		return;
	}
	json_free(root);
	if (kmodpolicy_save(&next) != 0) {
		respond_error(fd, 500, "Internal Server Error", "could not persist the kmod policy");
		return;
	}
	warn_newly_blacklisted_loaded(&before, &next);
	handle_kmodpolicy_get(fd);
}

void handle_kmodscan_get(int fd)
{
	struct json_writer w;

	jw_init(&w);
	kmodscan_write_json(&w);
	respond_json(fd, 200, "OK", &w);
	jw_free(&w);
}

/* ADR-0348: a sweep on request, queued and answered at once with the
 * report as it stands, which now says one is running. */
void handle_kmodscan_post(int fd)
{
	static struct kmodscan_job job;
	struct kmodscan_report *rep;
	struct json_writer w;

	memset(&job, 0, sizeof(job));
	job.kind = KMODSCAN_JOB_SWEEP;
	job.trigger = KMODSCAN_TRIGGER_REQUEST;
	if (kmodscan_ask(&job, &rep) != KMODSCAN_QUEUED) {
		respond_kmod_queue_busy(fd);
		return;
	}
	jw_init(&w);
	kmodscan_write_json(&w);
	respond_json(fd, 202, "Accepted", &w);
	jw_free(&w);
}
