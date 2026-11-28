#include "modeswitchd.h"

#include "devicegrant.h"
#include "events.h"
#include "logstore.h"
#include "persist.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/*
 * ADR-0398: the daemon's half of the mode switcher. See modeswitchd.h.
 */

#define RULE_NAMES_MAX 2048    /* usb-modeswitch-data-20261111 has 515 */
/* 05c6:1000 has 12 attribute-qualified rules, the most of any id in
 * usb-modeswitch-data-20261111 (probe-modeswitch@3-3's listing). */
#define RULES_PER_ID_MAX 16
#define RULE_FILE_MAX 8192     /* the largest rule is a few hundred bytes */
#define PORTS_MAX 32
#define HELPER_DEADLINE_S 600  /* queued and sent: 4 may wait, each transfer has a timeout */
#define RETURN_DEADLINE_S 20   /* the device's re-enumeration after the last message */

enum port_state { PORT_SWITCHING, PORT_SWITCHED, PORT_FAILED };

/* What a switch did on one port: the location half of a USB record's id
 * (ADR-0345), which is what survives the id changing as it switches. */
struct port {
	int used;
	char port[DEVICE_ID_MAX];
	enum port_state state;
	struct modeswitch_rule rule;        /* its vendor:product is the id switched from */
	unsigned int to_vendor, to_product; /* switched: the id it came back as */
	char reason[200];
	long long deadline;
};

static int g_active;
static struct port g_ports[PORTS_MAX];
static char (*g_names)[MODESWITCH_NAME_MAX];
static int g_name_count = -1;

void modeswitch_set_active(int active)
{
	g_active = active;
}

static long long now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec;
}

/* The rule files' names, read once: the root they are in does not change
 * under a running cixd (a root switch is a new process). */
static void load_names(void)
{
	DIR *d;
	struct dirent *de;

	if (g_name_count >= 0)
		return;
	g_name_count = 0;
	g_names = calloc(RULE_NAMES_MAX, sizeof(*g_names));
	d = g_names != NULL ? opendir(MODESWITCH_RULES_DIR) : NULL;
	if (d == NULL)
		return;
	while ((de = readdir(d)) != NULL && g_name_count < RULE_NAMES_MAX) {
		unsigned int v, p;

		if (strlen(de->d_name) >= MODESWITCH_NAME_MAX ||
		    modeswitch_name_ids(de->d_name, &v, &p) != 0)
			continue;
		snprintf(g_names[g_name_count++], MODESWITCH_NAME_MAX, "%s", de->d_name);
	}
	closedir(d);
}

static int read_small(const char *path, char *buf, size_t size)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, size - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = '\0';
	return (int)n;
}

/* Every rule whose name carries vendor:product. A file that does not
 * parse is skipped and said once per daemon, naming it. */
static int rules_for(unsigned int vendor, unsigned int product, struct modeswitch_rule *out,
                     int max)
{
	static char text[RULE_FILE_MAX];
	int count = 0, i;

	load_names();
	for (i = 0; i < g_name_count && count < max; i++) {
		char path[256], err[128];
		unsigned int v, p;
		int len;

		if (modeswitch_name_ids(g_names[i], &v, &p) != 0 || v != vendor || p != product)
			continue;
		snprintf(path, sizeof(path), "%s/%s", MODESWITCH_RULES_DIR, g_names[i]);
		len = read_small(path, text, sizeof(text));
		if (len < 0)
			continue;
		if (modeswitch_rule_parse(g_names[i], text, (size_t)len, &out[count], err,
		                          sizeof(err)) != 0) {
			logstore_write("cixd", "warning", "modeswitch: rule %s is not used: %s",
			               g_names[i], err);
			g_names[i][0] = '\0'; /* said once */
			continue;
		}
		count++;
	}
	return count;
}

static int parse_hex(const char *s, unsigned int *out)
{
	char *end;
	unsigned long v;

	if (s == NULL || s[0] == '\0')
		return -1;
	v = strtoul(s, &end, 16);
	if (*end != '\0')
		return -1;
	*out = (unsigned int)v;
	return 0;
}

static int record_ids(const struct discovered_device *d, unsigned int *vendor,
                      unsigned int *product)
{
	if (strcmp(d->bus, "usb") != 0)
		return -1;
	return parse_hex(d->vendor_id, vendor) == 0 && parse_hex(d->product_id, product) == 0 ? 0
	                                                                                       : -1;
}

static void read_attr(const struct discovered_device *d, const char *name, char *out,
                      size_t size)
{
	char path[320];
	int n;

	out[0] = '\0';
	snprintf(path, sizeof(path), "%s/%s", d->sysfs, name);
	n = read_small(path, out, size);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
		out[--n] = '\0';
}

static const char *port_of(const char *id)
{
	const char *at = strchr(id, '@');

	return at != NULL ? at + 1 : NULL;
}

static struct port *port_find(const char *port)
{
	int i;

	for (i = 0; i < PORTS_MAX; i++) {
		if (g_ports[i].used && strcmp(g_ports[i].port, port) == 0)
			return &g_ports[i];
	}
	return NULL;
}

/* A port's record, replacing the one it had; or a slot no switch is
 * using. NULL only when every slot holds a switch in flight. */
static struct port *port_take(const char *port)
{
	struct port *p = port_find(port);
	int i;

	for (i = 0; i < PORTS_MAX && p == NULL; i++) {
		if (!g_ports[i].used || g_ports[i].state != PORT_SWITCHING)
			p = &g_ports[i];
	}
	if (p != NULL) {
		memset(p, 0, sizeof(*p));
		p->used = 1;
		snprintf(p->port, sizeof(p->port), "%s", port);
	}
	return p;
}

/* The decision's inputs for record idx, and the chosen rule; -1 when the
 * record is not a USB device. */
static int gather(const struct discovered_device *recs, int n, int idx,
                  const struct device_verdict *v, struct modeswitch_rule *rules,
                  struct modeswitch_facts *f)
{
	const struct discovered_device *d = &recs[idx];
	char manufacturer[128], product[128], serial[128];
	struct modeswitch_device dev;
	int count, chosen, i;

	memset(f, 0, sizeof(*f));
	f->iface0_class = -1;
	if (record_ids(d, &dev.vendor, &dev.product) != 0)
		return -1;
	count = rules_for(dev.vendor, dev.product, rules, RULES_PER_ID_MAX);
	if (count == 0)
		return 0;
	read_attr(d, "manufacturer", manufacturer, sizeof(manufacturer));
	read_attr(d, "product", product, sizeof(product));
	read_attr(d, "serial", serial, sizeof(serial));
	dev.manufacturer = manufacturer;
	dev.product_name = product;
	dev.serial = serial;
	chosen = modeswitch_choose(rules, count, &dev, &f->ambiguous, &f->scsi_only);
	f->rule = chosen >= 0 ? &rules[chosen] : NULL;

	f->device_free = v->state == DEVICE_STATE_FREE;
	f->not_free = device_state_word(v->state);
	for (i = 0; i < n; i++) {
		if (i != idx && strcmp(recs[i].bus, "disk") == 0 &&
		    device_ids_related(recs, n, recs[i].id, d->id))
			f->block_below = 1;
	}
	for (i = 0; i < d->interface_count; i++) {
		unsigned int cls;

		if (parse_hex(d->interfaces[i].class_hex, &cls) != 0)
			continue;
		if (d->interfaces[i].number == 0)
			f->iface0_class = (int)cls;
		if (f->rule != NULL && d->interfaces[i].number == f->rule->interface && cls == 0x08)
			f->iface_mass_storage = 1;
	}
	return 0;
}

static unsigned int rule_target_vendor(const struct modeswitch_rule *r)
{
	return r->has_target_vendor ? r->target_vendor : r->vendor;
}

static void write_obj(struct json_writer *w, const char *state, const struct modeswitch_rule *r,
                      const char *reason)
{
	char id[16];
	int i;

	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state);
	jw_key(w, "rule");
	jw_str(w, r != NULL ? r->name : "");
	jw_key(w, "device");
	jw_str(w, r != NULL ? r->comment : "");
	jw_key(w, "targets");
	jw_arr_open(w);
	for (i = 0; r != NULL && i < r->target_count; i++) {
		snprintf(id, sizeof(id), "%04x:%04x", rule_target_vendor(r), r->targets[i]);
		jw_str(w, id);
	}
	jw_arr_close(w);
	jw_key(w, "reason");
	jw_str(w, reason);
	jw_obj_close(w);
}

/* A port's record still describes this record: the device it switched
 * from is still there, or the device it came back as is. */
static const struct port *port_for_record(const struct discovered_device *d)
{
	const char *port = port_of(d->id);
	const struct port *p = port != NULL ? port_find(port) : NULL;
	unsigned int v, pr;

	if (p == NULL || record_ids(d, &v, &pr) != 0)
		return NULL;
	if (p->state == PORT_SWITCHED)
		return v == p->to_vendor && pr == p->to_product ? p : NULL;
	return v == p->rule.vendor && pr == p->rule.product ? p : NULL;
}

void modeswitch_write_device(struct json_writer *w, const struct discovered_device *recs, int n,
                             int idx, const struct device_verdict *v)
{
	static struct modeswitch_rule rules[RULES_PER_ID_MAX];
	const struct port *p;
	struct modeswitch_facts f;
	const char *reason, *state;
	char why[200];

	if (strcmp(recs[idx].bus, "usb") != 0)
		return;
	jw_key(w, "modeswitch");
	p = port_for_record(&recs[idx]);
	if (p != NULL) {
		state = p->state == PORT_SWITCHING ? "switching"
		        : p->state == PORT_SWITCHED ? "switched"
		                                    : "failed";
		write_obj(w, state, &p->rule, p->reason);
		return;
	}
	if (gather(recs, n, idx, v, rules, &f) != 0) {
		jw_null(w);
		return;
	}
	switch (modeswitch_verdict(&f, &reason)) {
	case MODESWITCH_VERDICT_NO_RULE:
		jw_null(w);
		return;
	case MODESWITCH_VERDICT_SWITCH:
		state = "switchable";
		break;
	case MODESWITCH_VERDICT_ALREADY:
		state = "already";
		break;
	case MODESWITCH_VERDICT_REFUSE:
	default:
		state = "refused";
		break;
	}
	if (f.rule != NULL && f.rule->unsupported[0] != '\0')
		snprintf(why, sizeof(why), "%s (%s)", reason, f.rule->unsupported);
	else
		snprintf(why, sizeof(why), "%s", reason);
	write_obj(w, state, f.rule, why);
}

int modeswitch_prepare(const char *id, struct modeswitch_job *job, char *why, size_t why_size)
{
	static struct modeswitch_rule rules[RULES_PER_ID_MAX];
	const struct device_snapshot *s;
	const struct discovered_device *d;
	const struct port *p;
	struct device_verdict v;
	struct modeswitch_facts f;
	const char *reason;
	int idx;

	if (!g_active) {
		snprintf(why, why_size, "switching runs only on an installed host (ADR-0398)");
		return 409;
	}
	s = devicegrant_snapshot();
	idx = device_record_index(s->recs, s->n, id);
	if (idx < 0) {
		snprintf(why, why_size, "no device %s (see GET /v1/devices)", id);
		return 404;
	}
	d = &s->recs[idx];
	p = port_for_record(d);
	if (p != NULL && p->state == PORT_SWITCHING) {
		snprintf(why, why_size, "%s is being switched already", id);
		return 409;
	}
	device_state_of(s->recs, s->n, idx, s->claims, s->claim_count, &v);
	if (gather(s->recs, s->n, idx, &v, rules, &f) != 0) {
		snprintf(why, why_size, "%s is not a USB device", id);
		return 422;
	}
	switch (modeswitch_verdict(&f, &reason)) {
	case MODESWITCH_VERDICT_NO_RULE:
		snprintf(why, why_size, "no rule in usb-modeswitch-data names %s:%s", d->vendor_id,
		         d->product_id);
		return 422;
	case MODESWITCH_VERDICT_ALREADY:
		snprintf(why, why_size, "%s is in its target mode already", id);
		return 409;
	case MODESWITCH_VERDICT_REFUSE:
		if (f.rule != NULL && f.rule->unsupported[0] != '\0')
			snprintf(why, why_size, "%s: %s (%s)", id, reason, f.rule->unsupported);
		else
			snprintf(why, why_size, "%s: %s", id, reason);
		return 409;
	case MODESWITCH_VERDICT_SWITCH:
		break;
	}
	if (d->dev_path[0] == '\0') {
		snprintf(why, why_size, "%s has no usbfs node", id);
		return 409;
	}
	memset(job, 0, sizeof(*job));
	snprintf(job->id, sizeof(job->id), "%s", d->id);
	snprintf(job->dev_path, sizeof(job->dev_path), "%s", d->dev_path);
	job->rule = *f.rule;
	return 0;
}

void modeswitch_started(const struct modeswitch_job *job)
{
	const char *port = port_of(job->id);
	struct port *p = port != NULL ? port_take(port) : NULL;

	if (p == NULL)
		return;
	p->state = PORT_SWITCHING;
	p->rule = job->rule;
	p->deadline = now_s() + HELPER_DEADLINE_S;
	logstore_write("cixd", "info", "modeswitch: switching %s by rule %s (%s)", job->id,
	               job->rule.name, job->rule.comment[0] != '\0' ? job->rule.comment : "no description");
	events_announce(EV_listDevices);
}

static void port_failed(struct port *p, const char *reason)
{
	p->state = PORT_FAILED;
	snprintf(p->reason, sizeof(p->reason), "%s", reason);
	logstore_write("cixd", "error", "modeswitch: the switch on port %s failed: %s", p->port,
	               reason);
	events_announce(EV_listDevices);
}

void modeswitch_finished(const struct modeswitch_job *job, int ok, const char *error)
{
	const char *port = port_of(job->id);
	struct port *p = port != NULL ? port_find(port) : NULL;
	char reason[200];

	if (p == NULL || p->state != PORT_SWITCHING)
		return;
	if (!ok) {
		snprintf(reason, sizeof(reason), "the switch could not be sent: %s", error);
		port_failed(p, reason);
		return;
	}
	p->deadline = now_s() + RETURN_DEADLINE_S;
	modeswitch_usb_added(); /* it may already be back */
}

/* What the port of p holds now, from a snapshot: whether a USB record is
 * there, and its ids. */
static int port_now(const struct device_snapshot *s, const struct port *p, unsigned int *vendor,
                    unsigned int *product)
{
	int j;

	for (j = 0; j < s->n; j++) {
		const char *port = port_of(s->recs[j].id);

		if (port != NULL && strcmp(port, p->port) == 0 &&
		    record_ids(&s->recs[j], vendor, product) == 0)
			return 1;
	}
	return 0;
}

void modeswitch_usb_added(void)
{
	const struct device_snapshot *s;
	int i;

	if (!modeswitch_in_flight())
		return;
	s = devicegrant_snapshot();
	for (i = 0; i < PORTS_MAX; i++) {
		struct port *p = &g_ports[i];
		unsigned int v = 0, pr = 0;
		int present;

		if (!p->used || p->state != PORT_SWITCHING)
			continue;
		present = port_now(s, p, &v, &pr);
		switch (modeswitch_return_of(p->rule.vendor, p->rule.product, &p->rule, present, v, pr)) {
		case MODESWITCH_RETURN_WAIT:
			continue;
		case MODESWITCH_RETURN_TARGET:
			p->reason[0] = '\0';
			break;
		case MODESWITCH_RETURN_UNLISTED:
			snprintf(p->reason, sizeof(p->reason),
			         "it came back as %04x:%04x, which its rule does not list", v, pr);
			break;
		}
		p->state = PORT_SWITCHED;
		p->to_vendor = v;
		p->to_product = pr;
		logstore_write("cixd", "info", "modeswitch: %04x:%04x on port %s switched to %04x:%04x%s",
		               p->rule.vendor, p->rule.product, p->port, v, pr,
		               p->reason[0] != '\0' ? ", an id its rule does not list" : "");
		events_announce(EV_listDevices);
	}
}

/* Release B: the setting, the pass it drives, and the handover text. */
#define WANT_DELAY_S 2 /* a device's interfaces arrive after its own add */

static int g_mode_off;      /* 0: auto (the default) */
static int g_mode_saved;
static char g_mode_path[512];
static long long g_want_at; /* when a pass is due; 0 for none */
static int (*g_submit)(const struct modeswitch_job *job);

int modeswitch_policy_init(const char *state_path)
{
	char *buf;
	size_t len;
	struct json_value *root;
	const char *mode;

	snprintf(g_mode_path, sizeof(g_mode_path), "%s", state_path);
	g_mode_off = 0;
	g_mode_saved = 0;
	if (persist_read_file(g_mode_path, &buf, &len) != 0)
		return -1;
	if (buf == NULL)
		return 0; /* never saved: auto */
	root = json_parse(buf, len);
	free(buf);
	mode = root != NULL ? json_as_string(json_object_get(root, "mode")) : NULL;
	if (mode == NULL || (strcmp(mode, "auto") != 0 && strcmp(mode, "off") != 0)) {
		json_free(root);
		fprintf(stderr, "%s: malformed persisted mode switch setting\n", g_mode_path);
		return -1;
	}
	g_mode_off = strcmp(mode, "off") == 0;
	g_mode_saved = 1;
	json_free(root);
	return 0;
}

int modeswitch_policy_put(const struct json_value *body, char *err, size_t err_size)
{
	const char *mode = body != NULL ? json_as_string(json_object_get(body, "mode")) : NULL;
	struct json_writer w;
	int rc;

	if (mode == NULL || (strcmp(mode, "auto") != 0 && strcmp(mode, "off") != 0)) {
		snprintf(err, err_size, "mode must be \"auto\" or \"off\"");
		return 400;
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "mode");
	jw_str(&w, mode);
	jw_obj_close(&w);
	rc = persist_atomic_write(g_mode_path, w.buf, w.len);
	jw_free(&w);
	if (rc != 0) {
		snprintf(err, err_size, "could not save the setting -- nothing was changed");
		return 500;
	}
	g_mode_off = strcmp(mode, "off") == 0;
	g_mode_saved = 1;
	logstore_write("cixd", "info", "modeswitch: automatic switching is %s (ADR-0398)",
	               g_mode_off ? "off" : "on");
	if (!g_mode_off)
		modeswitch_want(); /* devices already waiting are switched now */
	return 0;
}

void modeswitch_policy_write_json(struct json_writer *w)
{
	jw_obj_open(w);
	jw_key(w, "mode");
	jw_str(w, g_mode_off ? "off" : "auto");
	jw_key(w, "saved");
	jw_bool(w, g_mode_saved);
	jw_obj_close(w);
}

void modeswitch_want(void)
{
	g_want_at = now_s() + WANT_DELAY_S;
}

void modeswitch_set_submitter(int (*submit)(const struct modeswitch_job *job))
{
	g_submit = submit;
}

/*
 * One pass over the USB records: every ended switch's port is looked at
 * through the loop guard, then, when automatic switching is on, every
 * device the verdict allows and no port record holds back is queued.
 */
static void modeswitch_pass(void)
{
	static struct modeswitch_rule rules[RULES_PER_ID_MAX];
	const struct device_snapshot *s = devicegrant_snapshot();
	int i, j;

	for (i = 0; i < PORTS_MAX; i++) {
		struct port *p = &g_ports[i];
		unsigned int v = 0, pr = 0;
		enum modeswitch_seen seen;

		if (!p->used || p->state == PORT_SWITCHING)
			continue;
		seen = !port_now(s, p, &v, &pr)                               ? MODESWITCH_SEEN_EMPTY
		       : v == p->rule.vendor && pr == p->rule.product         ? MODESWITCH_SEEN_FROM
		                                                              : MODESWITCH_SEEN_OTHER;
		switch (modeswitch_port_next(p->state == PORT_SWITCHED, seen)) {
		case MODESWITCH_NEXT_CLEAR:
			p->used = 0;
			events_announce(EV_listDevices);
			break;
		case MODESWITCH_NEXT_REVERTED:
			port_failed(p, "it came back in its fake-disk mode after switching, so it is not "
			               "switched again until it is unplugged");
			break;
		case MODESWITCH_NEXT_KEEP:
			break;
		}
	}
	if (g_mode_off || g_submit == NULL)
		return;
	for (j = 0; j < s->n; j++) {
		const struct discovered_device *d = &s->recs[j];
		const char *port = port_of(d->id);
		struct device_verdict v;
		struct modeswitch_facts f;
		const char *reason;
		static struct modeswitch_job job;

		if (strcmp(d->bus, "usb") != 0 || port == NULL || port_find(port) != NULL ||
		    d->dev_path[0] == '\0')
			continue;
		device_state_of(s->recs, s->n, j, s->claims, s->claim_count, &v);
		if (gather(s->recs, s->n, j, &v, rules, &f) != 0 ||
		    modeswitch_verdict(&f, &reason) != MODESWITCH_VERDICT_SWITCH)
			continue;
		memset(&job, 0, sizeof(job));
		snprintf(job.id, sizeof(job.id), "%s", d->id);
		snprintf(job.dev_path, sizeof(job.dev_path), "%s", d->dev_path);
		job.rule = *f.rule;
		if (g_submit(&job) != 0)
			logstore_write("cixd", "warning",
			               "modeswitch: %s could not be queued -- the queue is full or no "
			               "helper could start; the next USB event tries again",
			               d->id);
	}
}

char *modeswitch_handover_json(void)
{
	struct json_writer w;
	char *out;
	int i, any = 0;

	jw_init(&w);
	jw_arr_open(&w);
	for (i = 0; i < PORTS_MAX; i++) {
		const struct port *p = &g_ports[i];

		/* A switch in flight never crosses a handover (modeswitch_in_flight()). */
		if (!p->used || p->state == PORT_SWITCHING)
			continue;
		any = 1;
		jw_obj_open(&w);
		jw_key(&w, "port");
		jw_str(&w, p->port);
		jw_key(&w, "state");
		jw_str(&w, p->state == PORT_SWITCHED ? "switched" : "failed");
		jw_key(&w, "rule");
		jw_str(&w, p->rule.name);
		jw_key(&w, "to");
		jw_int(&w, (long long)(p->to_vendor << 16 | p->to_product));
		jw_key(&w, "reason");
		jw_str(&w, p->reason);
		jw_obj_close(&w);
	}
	jw_arr_close(&w);
	out = any && w.buf != NULL ? strndup(w.buf, w.len) : NULL;
	jw_free(&w);
	return out;
}

void modeswitch_handover_adopt(const char *json)
{
	static struct modeswitch_rule rules[RULES_PER_ID_MAX];
	struct json_value *root = json != NULL ? json_parse(json, strlen(json)) : NULL;
	size_t i;

	if (root == NULL || root->type != JSON_ARRAY) {
		json_free(root);
		return;
	}
	for (i = 0; i < root->u.array.count; i++) {
		const struct json_value *e = root->u.array.items[i];
		const char *port = json_as_string(json_object_get(e, "port"));
		const char *state = json_as_string(json_object_get(e, "state"));
		const char *rule = json_as_string(json_object_get(e, "rule"));
		const char *reason = json_as_string(json_object_get(e, "reason"));
		long long to = (long long)json_as_number(json_object_get(e, "to"));
		unsigned int v, pr;
		struct port *p;
		int n, k, found = -1;

		/* The rule is read again from the root this cixd runs from. */
		if (port == NULL || state == NULL || rule == NULL ||
		    modeswitch_name_ids(rule, &v, &pr) != 0)
			continue;
		n = rules_for(v, pr, rules, RULES_PER_ID_MAX);
		for (k = 0; k < n && found < 0; k++)
			if (strcmp(rules[k].name, rule) == 0)
				found = k;
		p = found >= 0 ? port_take(port) : NULL;
		if (p == NULL)
			continue;
		p->rule = rules[found];
		p->state = strcmp(state, "switched") == 0 ? PORT_SWITCHED : PORT_FAILED;
		p->to_vendor = (unsigned int)(to >> 16) & 0xffff;
		p->to_product = (unsigned int)to & 0xffff;
		snprintf(p->reason, sizeof(p->reason), "%s", reason != NULL ? reason : "");
	}
	json_free(root);
}

void modeswitch_tick(void)
{
	const struct device_snapshot *s = NULL;
	long long now;
	int i;

	if (g_active && g_want_at != 0 && now_s() >= g_want_at) {
		g_want_at = 0;
		modeswitch_pass();
	}
	if (!modeswitch_in_flight())
		return;
	now = now_s();
	for (i = 0; i < PORTS_MAX; i++) {
		struct port *p = &g_ports[i];
		unsigned int v = 0, pr = 0;

		if (!p->used || p->state != PORT_SWITCHING || now < p->deadline)
			continue;
		modeswitch_usb_added(); /* a last look before saying it failed */
		if (p->state != PORT_SWITCHING)
			continue;
		if (s == NULL)
			s = devicegrant_snapshot();
		if (port_now(s, p, &v, &pr))
			port_failed(p, "it is still in its fake-disk mode after the switch");
		else
			port_failed(p, "nothing came back on its port after the switch");
	}
}

int modeswitch_in_flight(void)
{
	int i;

	for (i = 0; i < PORTS_MAX; i++) {
		if (g_ports[i].used && g_ports[i].state == PORT_SWITCHING)
			return 1;
	}
	return 0;
}
