#include "modeswitch.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * ADR-0398: the pure half of the mode switcher. See modeswitch.h for the
 * contract. Nothing here touches a device or a file.
 */

/* The keys a rule may set that this release does not carry out. A rule
 * naming one with a nonzero value is refused with the key named, never
 * half-run: upstream combines them with the messages in ways only the
 * whole method gets right. Counted on usb-modeswitch-data-20261111 by
 * probe-modeswitch@3-3: 224 of its 515 rules use one of these. */
static const char *const g_unsupported_keys[] = {
	"HuaweiMode",     "HuaweiNewMode",   "HuaweiAltMode", "OptionMode", "SierraMode",
	"SonyMode",       "QisdaMode",       "QuantaMode",    "BlackberryMode", "PantechMode",
	"CiscoMode",      "GCTMode",         "KobilMode",     "MobileActionMode", "SequansMode",
	"ResetUSB",       "DetachStorageOnly", "Configuration", "AltSetting",
};

static int hex4(const char *s, unsigned int *out)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < 4; i++) {
		int c = (unsigned char)s[i];

		if (!isxdigit(c))
			return -1;
		v = v * 16 + (unsigned int)(isdigit(c) ? c - '0' : tolower(c) - 'a' + 10);
	}
	*out = v;
	return 0;
}

int modeswitch_name_ids(const char *name, unsigned int *vendor, unsigned int *product)
{
	if (strlen(name) < 9 || name[4] != ':' || (name[9] != '\0' && name[9] != ':'))
		return -1;
	if (hex4(name, vendor) != 0 || hex4(name + 5, product) != 0)
		return -1;
	return 0;
}

static int parse_name(const char *name, struct modeswitch_rule *r, char *err, size_t errlen)
{
	const char *p;

	if (strlen(name) >= sizeof(r->name) ||
	    modeswitch_name_ids(name, &r->vendor, &r->product) != 0) {
		snprintf(err, errlen, "the name is not VVVV:PPPP[:key=value]...");
		return -1;
	}
	snprintf(r->name, sizeof(r->name), "%s", name);
	p = name + 9;
	while (*p == ':') {
		const char *start = p + 1;
		const char *end = strchr(start, ':');
		size_t len = end != NULL ? (size_t)(end - start) : strlen(start);
		struct modeswitch_token *t;

		if (r->token_count >= MODESWITCH_TOKENS_MAX || len < 5 || start[3] != '=' ||
		    len - 4 >= sizeof(t->value)) {
			snprintf(err, errlen, "an attribute match in the name is not key=value");
			return -1;
		}
		t = &r->tokens[r->token_count++];
		memcpy(t->key, start, 3);
		t->key[3] = '\0';
		memcpy(t->value, start + 4, len - 4);
		t->value[len - 4] = '\0';
		if (strcmp(t->key, "uMa") != 0 && strcmp(t->key, "uPr") != 0 &&
		    strcmp(t->key, "uSe") != 0 && strcmp(t->key, "sVe") != 0 &&
		    strcmp(t->key, "sMo") != 0 && strcmp(t->key, "sRe") != 0) {
			snprintf(err, errlen, "unknown attribute %s in the name", t->key);
			return -1;
		}
		p = start + len;
	}
	return 0;
}

static int hexval(int c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	c = tolower(c);
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	return -1;
}

static int decode_hex(const char *s, unsigned char *out, size_t max, size_t *len)
{
	size_t n = strlen(s), i;

	if (n == 0 || n % 2 != 0 || n / 2 > max)
		return -1;
	for (i = 0; i < n / 2; i++) {
		int hi = hexval((unsigned char)s[2 * i]);
		int lo = hexval((unsigned char)s[2 * i + 1]);

		if (hi < 0 || lo < 0)
			return -1;
		out[i] = (unsigned char)(hi * 16 + lo);
	}
	*len = n / 2;
	return 0;
}

/*
 * Upstream reads two kinds of number (usb_modeswitch.c, ParseParamInt and
 * ParseParamHex, read 2026-11-27 by probe-modeswitch@3-3): the ids,
 * classes, endpoints and Interface are hex whether or not they carry a
 * `0x`, so `MessageEndpoint=81` is 0x81; delays and flags are decimal.
 */
static long number(const char *v)
{
	return strtol(v, NULL, 10);
}

static long hexnum(const char *v)
{
	return strtol(v, NULL, 16);
}

static void parse_target_list(const char *v, struct modeswitch_rule *r)
{
	const char *p = v;

	r->target_count = 0;
	while (*p != '\0' && r->target_count < MODESWITCH_TARGETS_MAX) {
		char *end;
		unsigned long id;

		while (*p == ',' || *p == ' ')
			p++;
		if (*p == '\0')
			break;
		id = strtoul(p, &end, 16);
		if (end == p)
			break;
		r->targets[r->target_count++] = (unsigned int)id;
		p = end;
	}
}

/* One `Key=value` line. 0, or -1 with err set for a message that is not
 * hex; every other mistyped or unknown entry is ignored, as upstream
 * does (REFERENCE, "General Rules"). */
static int parse_line(char *line, struct modeswitch_rule *r, char *err, size_t errlen)
{
	char *key = line, *eq, *v, *end;
	size_t i;

	while (isspace((unsigned char)*key))
		key++;
	if (*key == '#') {
		if (r->comment[0] == '\0') {
			key++;
			while (isspace((unsigned char)*key))
				key++;
			snprintf(r->comment, sizeof(r->comment), "%s", key);
		}
		return 0;
	}
	eq = strchr(key, '=');
	if (eq == NULL)
		return 0;
	for (end = eq; end > key && isspace((unsigned char)end[-1]); end--)
		;
	*end = '\0';
	v = eq + 1;
	while (isspace((unsigned char)*v))
		v++;
	for (end = v + strlen(v); end > v && isspace((unsigned char)end[-1]); end--)
		;
	*end = '\0';
	if (*v == '"') {
		v++;
		end = strchr(v, '"');
		if (end != NULL)
			*end = '\0';
	}

	if (strcmp(key, "TargetVendor") == 0) {
		r->has_target_vendor = 1;
		r->target_vendor = (unsigned int)hexnum(v);
	} else if (strcmp(key, "TargetProduct") == 0) {
		if (r->target_count == 0) { /* TargetProductList has priority */
			r->targets[0] = (unsigned int)hexnum(v);
			r->target_count = 1;
		}
	} else if (strcmp(key, "TargetProductList") == 0) {
		parse_target_list(v, r);
	} else if (strcmp(key, "TargetClass") == 0) {
		r->target_class = (int)hexnum(v);
	} else if (strcmp(key, "StandardEject") == 0) {
		r->standard_eject = number(v) != 0;
	} else if (strcmp(key, "NeedResponse") == 0) {
		r->need_response = number(v) != 0;
	} else if (strcmp(key, "MessageEndpoint") == 0) {
		r->message_endpoint = (int)hexnum(v);
	} else if (strcmp(key, "ResponseEndpoint") == 0) {
		r->response_endpoint = (int)hexnum(v);
	} else if (strcmp(key, "Interface") == 0) {
		r->interface = (int)hexnum(v);
	} else if (strcmp(key, "ReleaseDelay") == 0) {
		r->release_delay_ms = (int)number(v);
	} else if (strcmp(key, "WaitBefore") == 0) {
		r->wait_before_s = (int)number(v);
	} else if (strncmp(key, "MessageContent", 14) == 0) {
		int slot = key[14] == '\0' ? 0 : key[14] == '2' ? 1 : key[14] == '3' ? 2 : -1;

		if (slot < 0 || (key[14] != '\0' && key[15] != '\0'))
			return 0;
		if (decode_hex(v, r->msg[slot], sizeof(r->msg[slot]), &r->msg_len[slot]) != 0) {
			snprintf(err, errlen, "%s is not a hex string", key);
			return -1;
		}
	} else {
		for (i = 0; i < sizeof(g_unsupported_keys) / sizeof(g_unsupported_keys[0]); i++) {
			if (strcmp(key, g_unsupported_keys[i]) == 0 && number(v) != 0 &&
			    r->unsupported[0] == '\0')
				snprintf(r->unsupported, sizeof(r->unsupported), "%s", key);
		}
	}
	return 0;
}

int modeswitch_rule_parse(const char *name, const char *text, size_t len,
                          struct modeswitch_rule *out, char *err, size_t errlen)
{
	char line[1200];
	size_t at = 0;

	memset(out, 0, sizeof(*out));
	out->target_class = -1;
	out->message_endpoint = -1;
	out->response_endpoint = -1;
	if (parse_name(name, out, err, errlen) != 0)
		return -1;
	while (at < len) {
		size_t n = 0;

		while (at < len && text[at] != '\n') {
			if (text[at] != '\r' && n + 1 < sizeof(line))
				line[n++] = text[at];
			at++;
		}
		at++;
		line[n] = '\0';
		if (parse_line(line, out, err, errlen) != 0)
			return -1;
	}
	return 0;
}

static int contains(const char *hay, const char *needle)
{
	return hay != NULL && strstr(hay, needle) != NULL;
}

enum modeswitch_match modeswitch_match(const struct modeswitch_rule *r,
                                       const struct modeswitch_device *d)
{
	int i;

	if (r->vendor != d->vendor || r->product != d->product)
		return MODESWITCH_MATCH_NO;
	if (r->token_count == 0)
		return MODESWITCH_MATCH_PLAIN;
	for (i = 0; i < r->token_count; i++) {
		const struct modeswitch_token *t = &r->tokens[i];
		const char *attr = strcmp(t->key, "uMa") == 0   ? d->manufacturer
		                   : strcmp(t->key, "uPr") == 0 ? d->product_name
		                   : strcmp(t->key, "uSe") == 0 ? d->serial
		                                                : NULL;
		char blank[MODESWITCH_TOKEN_VALUE_MAX];
		size_t j;

		if (t->key[0] == 's')
			return MODESWITCH_MATCH_SCSI;
		snprintf(blank, sizeof(blank), "%s", t->value);
		for (j = 0; blank[j] != '\0'; j++) {
			if (blank[j] == '_')
				blank[j] = ' ';
		}
		if (!contains(attr, t->value) && !contains(attr, blank))
			return MODESWITCH_MATCH_NO;
	}
	return MODESWITCH_MATCH_QUALIFIED;
}

int modeswitch_choose(const struct modeswitch_rule *rules, int n,
                      const struct modeswitch_device *d, int *ambiguous, int *scsi_only)
{
	int plain = -1, qualified = -1, qualified_count = 0, scsi = 0, i;

	*ambiguous = 0;
	*scsi_only = 0;
	for (i = 0; i < n; i++) {
		switch (modeswitch_match(&rules[i], d)) {
		case MODESWITCH_MATCH_PLAIN:
			plain = i;
			break;
		case MODESWITCH_MATCH_QUALIFIED:
			qualified = i;
			qualified_count++;
			break;
		case MODESWITCH_MATCH_SCSI:
			scsi = 1;
			break;
		case MODESWITCH_MATCH_NO:
			break;
		}
	}
	if (qualified_count > 1) {
		*ambiguous = 1;
		return -1;
	}
	if (qualified >= 0)
		return qualified;
	/*
	 * A SCSI-qualified rule could be the device's, and it cannot be told
	 * without an inquiry: a plain rule beside it is not chosen in its
	 * place, because upstream would have preferred the qualified one
	 * whenever it matched.
	 */
	if (scsi) {
		*scsi_only = 1;
		return -1;
	}
	return plain;
}

static int message_is_cbw(const unsigned char *b, size_t len)
{
	return len == MODESWITCH_CBW_LEN && b[0] == 'U' && b[1] == 'S' && b[2] == 'B' && b[3] == 'C';
}

static int rule_sends_cbw(const struct modeswitch_rule *r)
{
	int i;

	if (r->standard_eject)
		return 1;
	for (i = 0; i < MODESWITCH_MSGS_MAX; i++) {
		if (message_is_cbw(r->msg[i], r->msg_len[i]))
			return 1;
	}
	return 0;
}

static int rule_has_method(const struct modeswitch_rule *r)
{
	return r->standard_eject || r->msg_len[0] > 0;
}

enum modeswitch_verdict modeswitch_verdict(const struct modeswitch_facts *f, const char **reason)
{
	const struct modeswitch_rule *r = f->rule;

	*reason = "";
	if (r == NULL) {
		if (f->ambiguous) {
			*reason = "two rules match it, so neither is used";
			return MODESWITCH_VERDICT_REFUSE;
		}
		if (f->scsi_only) {
			*reason = "its rule matches on a SCSI attribute, which cixd does not read";
			return MODESWITCH_VERDICT_REFUSE;
		}
		return MODESWITCH_VERDICT_NO_RULE;
	}
	if (r->unsupported[0] != '\0') {
		*reason = "its rule uses a method cixd does not carry out yet";
		return MODESWITCH_VERDICT_REFUSE;
	}
	if (r->target_class >= 0 && f->iface0_class == r->target_class)
		return MODESWITCH_VERDICT_ALREADY;
	if (!rule_has_method(r)) {
		*reason = "its rule names no switching method";
		return MODESWITCH_VERDICT_REFUSE;
	}
	if (!f->device_free) {
		*reason = f->not_free != NULL && f->not_free[0] != '\0' ? f->not_free
		                                                         : "it is not free";
		return MODESWITCH_VERDICT_REFUSE;
	}
	if (f->block_below) {
		*reason = "a disk is below it";
		return MODESWITCH_VERDICT_REFUSE;
	}
	if (rule_sends_cbw(r) && !f->iface_mass_storage) {
		*reason = "the interface its rule addresses is not mass storage";
		return MODESWITCH_VERDICT_REFUSE;
	}
	return MODESWITCH_VERDICT_SWITCH;
}

static void put_le32(unsigned char *b, unsigned int v)
{
	b[0] = (unsigned char)(v & 0xff);
	b[1] = (unsigned char)((v >> 8) & 0xff);
	b[2] = (unsigned char)((v >> 16) & 0xff);
	b[3] = (unsigned char)((v >> 24) & 0xff);
}

static unsigned int get_le32(const unsigned char *b)
{
	return (unsigned int)b[0] | (unsigned int)b[1] << 8 | (unsigned int)b[2] << 16 |
	       (unsigned int)b[3] << 24;
}

/* A command wrapper with no data stage: signature, tag, length 0, flags 0,
 * the LUN, and a 6-byte command block (BOT 1.0, 5.1). */
static void cbw6(struct modeswitch_step *s, unsigned int tag, int lun, const unsigned char cdb[6])
{
	memset(s, 0, sizeof(*s));
	s->bytes[0] = 'U';
	s->bytes[1] = 'S';
	s->bytes[2] = 'B';
	s->bytes[3] = 'C';
	put_le32(s->bytes + 4, tag);
	put_le32(s->bytes + 8, 0);
	s->bytes[12] = 0;
	s->bytes[13] = (unsigned char)(lun & 0x0f);
	s->bytes[14] = 6;
	memcpy(s->bytes + 15, cdb, 6);
	s->len = MODESWITCH_CBW_LEN;
	s->is_cbw = 1;
}

static void message_step(struct modeswitch_step *s, const struct modeswitch_rule *r, int i)
{
	memset(s, 0, sizeof(*s));
	memcpy(s->bytes, r->msg[i], r->msg_len[i]);
	s->len = r->msg_len[i];
	s->is_cbw = message_is_cbw(s->bytes, s->len);
	if (s->is_cbw) {
		s->data_len = get_le32(s->bytes + 8);
		s->data_in = (s->bytes[12] & 0x80) != 0;
	} else {
		s->read_response = r->need_response;
	}
}

int modeswitch_plan(const struct modeswitch_rule *r, int max_lun,
                    struct modeswitch_step out[MODESWITCH_STEPS_MAX])
{
	/* SPC: PREVENT ALLOW MEDIUM REMOVAL with Prevent clear; SBC: START
	 * STOP UNIT with LoEj set and Start clear, which ejects the medium. */
	static const unsigned char allow_removal[6] = { 0x1e, 0, 0, 0, 0x00, 0 };
	static const unsigned char eject[6] = { 0x1b, 0, 0, 0, 0x02, 0 };
	unsigned int tag = 0x43495801; /* "\x01XIC": distinct tags, as REFERENCE asks */
	int n = 0, lun, i;

	if (!rule_has_method(r))
		return -1;
	if (max_lun < 0)
		max_lun = 0;
	if (max_lun >= MODESWITCH_LUNS_MAX)
		max_lun = MODESWITCH_LUNS_MAX - 1;
	if (r->standard_eject) {
		for (lun = 0; lun <= max_lun; lun++) {
			cbw6(&out[n++], tag++, lun, allow_removal);
			cbw6(&out[n++], tag++, lun, eject);
		}
		if (r->msg_len[0] > 0)
			message_step(&out[n++], r, 0);
		return n;
	}
	for (i = 0; i < MODESWITCH_MSGS_MAX; i++) {
		if (r->msg_len[i] > 0)
			message_step(&out[n++], r, i);
	}
	return n;
}

int modeswitch_is_target(const struct modeswitch_rule *r, unsigned int vendor,
                         unsigned int product)
{
	unsigned int tv = r->has_target_vendor ? r->target_vendor : r->vendor;
	int i;

	if (vendor != tv)
		return 0;
	for (i = 0; i < r->target_count; i++) {
		if (r->targets[i] == product)
			return 1;
	}
	return 0;
}

enum modeswitch_return modeswitch_return_of(unsigned int from_vendor, unsigned int from_product,
                                            const struct modeswitch_rule *r, int present,
                                            unsigned int vendor, unsigned int product)
{
	if (!present || (vendor == from_vendor && product == from_product))
		return MODESWITCH_RETURN_WAIT;
	return modeswitch_is_target(r, vendor, product) ? MODESWITCH_RETURN_TARGET
	                                                : MODESWITCH_RETURN_UNLISTED;
}
