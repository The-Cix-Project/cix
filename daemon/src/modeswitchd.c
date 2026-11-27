#include "modeswitchd.h"

#include "devicegrant.h"
#include "events.h"
#include "logstore.h"

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
#define RULES_PER_ID_MAX 16    /* 05c6:1000 has 13, the most of any id (probe-modeswitch@3-3) */
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
	unsigned int from_vendor, from_product;
	char rule[MODESWITCH_NAME_MAX];
	char device[96];
	unsigned int target_vendor;
	unsigned int targets[MODESWITCH_TARGETS_MAX];
	int target_count;
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

static struct port *port_take(const char *port)
{
	struct port *p = port_find(port);
	int i;

	if (p == NULL) {
		for (i = 0; i < PORTS_MAX && p == NULL; i++) {
			if (!g_ports[i].used || g_ports[i].state != PORT_SWITCHING)
				p = &g_ports[i];
		}
	}
	if (p != NULL) {
		memset(p, 0, sizeof(*p));
		p->used = 1;
		snprintf(p->port, sizeof(p->port), "%s", port);
	}
	return p;
}

/* The decision's inputs for record idx, and the chosen rule's index in
 * rules; -1 when the record is not a USB device. */
static int gather(const struct discovered_device *recs, int n, int idx,
                  const struct device_verdict *v, struct modeswitch_rule *rules, int *count,
                  struct modeswitch_facts *f)
{
	const struct discovered_device *d = &recs[idx];
	char manufacturer[128], product[128], serial[128];
	struct modeswitch_device dev;
	int chosen, i;

	memset(f, 0, sizeof(*f));
	f->iface0_class = -1;
	*count = 0;
	if (strcmp(d->bus, "usb") != 0 || parse_hex(d->vendor_id, &dev.vendor) != 0 ||
	    parse_hex(d->product_id, &dev.product) != 0)
		return -1;
	*count = rules_for(dev.vendor, dev.product, rules, RULES_PER_ID_MAX);
	if (*count == 0)
		return 0;
	read_attr(d, "manufacturer", manufacturer, sizeof(manufacturer));
	read_attr(d, "product", product, sizeof(product));
	read_attr(d, "serial", serial, sizeof(serial));
	dev.manufacturer = manufacturer;
	dev.product_name = product;
	dev.serial = serial;
	chosen = modeswitch_choose(rules, *count, &dev, &f->ambiguous, &f->scsi_only);
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

static void write_targets(struct json_writer *w, unsigned int vendor, const unsigned int *t,
                          int count)
{
	char id[16];
	int i;

	jw_key(w, "targets");
	jw_arr_open(w);
	for (i = 0; i < count; i++) {
		snprintf(id, sizeof(id), "%04x:%04x", vendor, t[i]);
		jw_str(w, id);
	}
	jw_arr_close(w);
}

static void write_obj(struct json_writer *w, const char *state, const char *rule,
                      const char *device, unsigned int tvendor, const unsigned int *t, int tcount,
                      const char *reason)
{
	jw_obj_open(w);
	jw_key(w, "state");
	jw_str(w, state);
	jw_key(w, "rule");
	jw_str(w, rule);
	jw_key(w, "device");
	jw_str(w, device);
	write_targets(w, tvendor, t, tcount);
	jw_key(w, "reason");
	jw_str(w, reason);
	jw_obj_close(w);
}

static unsigned int rule_target_vendor(const struct modeswitch_rule *r)
{
	return r->has_target_vendor ? r->target_vendor : r->vendor;
}

/* A port's record still describes this record: the device it switched
 * from is still there, or the one it switched to is. */
static const struct port *port_for_record(const struct discovered_device *d)
{
	const char *port = port_of(d->id);
	const struct port *p = port != NULL ? port_find(port) : NULL;
	unsigned int v, pr;
	int i;

	if (p == NULL || parse_hex(d->vendor_id, &v) != 0 || parse_hex(d->product_id, &pr) != 0)
		return NULL;
	if (p->state != PORT_SWITCHED)
		return v == p->from_vendor && pr == p->from_product ? p : NULL;
	if (v != p->target_vendor)
		return NULL;
	for (i = 0; i < p->target_count; i++) {
		if (p->targets[i] == pr)
			return p;
	}
	return NULL;
}

void modeswitch_write_device(struct json_writer *w, const struct discovered_device *recs, int n,
                             int idx, const struct device_verdict *v)
{
	static struct modeswitch_rule rules[RULES_PER_ID_MAX];
	const struct port *p;
	struct modeswitch_facts f;
	const char *reason, *state;
	char why[200];
	int count;

	if (strcmp(recs[idx].bus, "usb") != 0)
		return;
	jw_key(w, "modeswitch");
	p = port_for_record(&recs[idx]);
	if (p != NULL) {
		state = p->state == PORT_SWITCHING ? "switching"
		        : p->state == PORT_SWITCHED ? "switched"
		                                    : "failed";
		write_obj(w, state, p->rule, p->device, p->target_vendor, p->targets, p->target_count,
		          p->reason);
		return;
	}
	if (gather(recs, n, idx, v, rules, &count, &f) != 0) {
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
	if (f.rule == NULL) {
		write_obj(w, state, "", "", 0, NULL, 0, why);
		return;
	}
	write_obj(w, state, f.rule->name, f.rule->comment, rule_target_vendor(f.rule),
	          f.rule->targets, f.rule->target_count, why);
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
	int idx, count;

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
	if (gather(s->recs, s->n, idx, &v, rules, &count, &f) != 0) {
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
	const struct modeswitch_rule *r = &job->rule;

	if (p == NULL)
		return;
	p->state = PORT_SWITCHING;
	p->from_vendor = r->vendor;
	p->from_product = r->product;
	snprintf(p->rule, sizeof(p->rule), "%s", r->name);
	snprintf(p->device, sizeof(p->device), "%s", r->comment);
	p->target_vendor = rule_target_vendor(r);
	memcpy(p->targets, r->targets, sizeof(p->targets));
	p->target_count = r->target_count;
	p->deadline = now_s() + HELPER_DEADLINE_S;
	logstore_write("cixd", "info", "modeswitch: switching %s by rule %s (%s)", job->id, r->name,
	               r->comment[0] != '\0' ? r->comment : "no description");
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

void modeswitch_usb_added(void)
{
	const struct device_snapshot *s;
	int i, j;

	if (!modeswitch_in_flight())
		return;
	s = devicegrant_snapshot();
	for (i = 0; i < PORTS_MAX; i++) {
		struct port *p = &g_ports[i];

		if (!p->used || p->state != PORT_SWITCHING)
			continue;
		for (j = 0; j < s->n; j++) {
			const char *port = port_of(s->recs[j].id);
			unsigned int v, pr;
			int k;

			if (strcmp(s->recs[j].bus, "usb") != 0 || port == NULL ||
			    strcmp(port, p->port) != 0 || parse_hex(s->recs[j].vendor_id, &v) != 0 ||
			    parse_hex(s->recs[j].product_id, &pr) != 0 || v != p->target_vendor)
				continue;
			for (k = 0; k < p->target_count; k++) {
				if (p->targets[k] != pr)
					continue;
				p->state = PORT_SWITCHED;
				p->reason[0] = '\0';
				logstore_write("cixd", "info", "modeswitch: %04x:%04x on port %s switched to %s",
				               p->from_vendor, p->from_product, p->port, s->recs[j].id);
				events_announce(EV_listDevices);
				break;
			}
		}
	}
}

void modeswitch_tick(void)
{
	long long now;
	int i;

	if (!modeswitch_in_flight())
		return;
	now = now_s();
	for (i = 0; i < PORTS_MAX; i++) {
		if (g_ports[i].used && g_ports[i].state == PORT_SWITCHING && now >= g_ports[i].deadline) {
			char reason[200];
			int k, at = 0;

			modeswitch_usb_added(); /* a last look before saying it failed */
			if (g_ports[i].state != PORT_SWITCHING)
				continue;
			at = snprintf(reason, sizeof(reason), "it did not come back as");
			for (k = 0; k < g_ports[i].target_count && at < (int)sizeof(reason) - 12; k++)
				at += snprintf(reason + at, sizeof(reason) - (size_t)at, "%s %04x:%04x",
				               k == 0 ? "" : ",", g_ports[i].target_vendor,
				               g_ports[i].targets[k]);
			snprintf(reason + at, sizeof(reason) - (size_t)at, " in time");
			port_failed(&g_ports[i], reason);
		}
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
