#ifndef MODESWITCH_H
#define MODESWITCH_H

/*
 * ADR-0398: switching a multi-mode USB device into its working mode.
 *
 * This header is the pure half: a rule file read into a struct, a rule
 * file's name matched against a device, the one verdict, and the plan of
 * bulk transfers a switch sends. Nothing here opens a device or a file;
 * modeswitch_usb.c does the transfers in a helper child, and main.c owns
 * the per-port state. Every function is tested on supplied inputs in
 * test_modeswitch.
 *
 * The rules are upstream usb-modeswitch-data's, installed by the
 * `usb-modeswitch-data` package under MODESWITCH_RULES_DIR. Their format
 * is upstream's REFERENCE (read 2026-11-27, probe-modeswitch@3-1): `#`
 * comments, `Key=value` lines, numbers decimal or `0x` hex, message
 * strings quoted hex without `0x`, TargetProductList a quoted list of hex
 * ids. Mistyped and unknown keys are ignored, as upstream does.
 */

#include <stddef.h>

#define MODESWITCH_RULES_DIR "/usr/share/usb_modeswitch"

#define MODESWITCH_NAME_MAX 128   /* a rule file's name */
#define MODESWITCH_TOKENS_MAX 4   /* `:uMa=...` attribute matches in a name */
#define MODESWITCH_TOKEN_VALUE_MAX 64
#define MODESWITCH_TARGETS_MAX 16
#define MODESWITCH_MSGS_MAX 3     /* MessageContent, MessageContent2, MessageContent3 */
#define MODESWITCH_MSG_MAX 512    /* decoded bytes in one message; upstream's are 31 */
#define MODESWITCH_UNSUPPORTED_MAX 32
#define MODESWITCH_LUNS_MAX 8     /* StandardEject's LUNs, from GET MAX LUN (0..7) */
/* StandardEject sends two commands per LUN, then the rule's own message. */
#define MODESWITCH_STEPS_MAX (2 * MODESWITCH_LUNS_MAX + MODESWITCH_MSGS_MAX)

/* The bulk-only transport's command and status wrappers (USB Mass
 * Storage Class, Bulk-Only Transport 1.0, sections 5.1 and 5.2). Built
 * and read as byte arrays at these offsets, never as structs: TCC
 * ignores __attribute__((packed)). */
#define MODESWITCH_CBW_LEN 31
#define MODESWITCH_CSW_LEN 13

/* An attribute match in a rule file's name, `:uMa=Realtek`: the key and
 * the value, as written. */
struct modeswitch_token {
	char key[4];
	char value[MODESWITCH_TOKEN_VALUE_MAX];
};

struct modeswitch_rule {
	char name[MODESWITCH_NAME_MAX];
	char comment[96];             /* the file's first `#` line, the device it names */
	unsigned int vendor, product; /* from the name */
	struct modeswitch_token tokens[MODESWITCH_TOKENS_MAX];
	int token_count;

	int has_target_vendor;
	unsigned int target_vendor;
	unsigned int targets[MODESWITCH_TARGETS_MAX]; /* TargetProductList, or TargetProduct */
	int target_count;
	int target_class;             /* TargetClass, or -1 */

	int standard_eject;
	unsigned char msg[MODESWITCH_MSGS_MAX][MODESWITCH_MSG_MAX];
	size_t msg_len[MODESWITCH_MSGS_MAX];
	int need_response;
	int interface;                /* Interface: the one the messages go to, 0 by default as upstream */
	int message_endpoint;         /* -1: the interface's first bulk OUT */
	int response_endpoint;        /* -1: the interface's first bulk IN */
	int release_delay_ms;
	int wait_before_s;

	/* The first key a release does not implement, with a nonzero value;
	 * "" when the engine can carry the whole rule. */
	char unsupported[MODESWITCH_UNSUPPORTED_MAX];
};

/*
 * Reads a rule file's name and text. 0, or -1 with err set when the name
 * is not `VVVV:PPPP[:key=value]...` or a message is not hex.
 */
int modeswitch_rule_parse(const char *name, const char *text, size_t len,
                          struct modeswitch_rule *out, char *err, size_t errlen);

/* `VVVV:PPPP` of a name, or -1. Used to pick the files worth reading. */
int modeswitch_name_ids(const char *name, unsigned int *vendor, unsigned int *product);

/* What a device offers a rule file's name to match against: sysfs's
 * idVendor, idProduct, manufacturer, product and serial. */
struct modeswitch_device {
	unsigned int vendor, product;
	const char *manufacturer;
	const char *product_name;
	const char *serial;
};

enum modeswitch_match {
	MODESWITCH_MATCH_NO,
	MODESWITCH_MATCH_PLAIN,     /* the name is `VVVV:PPPP` alone */
	MODESWITCH_MATCH_QUALIFIED, /* and every attribute in it matches */
	MODESWITCH_MATCH_SCSI,      /* it needs a SCSI attribute, which release A does not read */
};

/*
 * Upstream's own rule (usb_modeswitch_dispatcher.tcl, MatchDevice, read
 * 2026-11-27): a token matches when its value, or its value with every
 * `_` read as a space, occurs anywhere in the device's attribute.
 */
enum modeswitch_match modeswitch_match(const struct modeswitch_rule *r,
                                       const struct modeswitch_device *d);

/*
 * The one rule for a device among the candidates for its id: a matching
 * qualified rule over a plain one; two matching qualified rules are
 * ambiguous and none is chosen. The chosen index, or -1; *ambiguous and
 * *scsi_only say why there is none.
 */
int modeswitch_choose(const struct modeswitch_rule *rules, int n,
                      const struct modeswitch_device *d, int *ambiguous, int *scsi_only);

/* The facts the verdict decides from, gathered by the caller from the
 * device records (ADR-0344) and the record's interfaces. */
struct modeswitch_facts {
	const struct modeswitch_rule *rule; /* the chosen rule, or NULL */
	int ambiguous;
	int scsi_only;
	int device_free;      /* device_state_of() says DEVICE_STATE_FREE */
	const char *not_free; /* the state's name when it is not, for the reason */
	int block_below;      /* a disk record sits below it */
	int iface_mass_storage; /* the rule's interface (its Interface, or 0) has class 08 */
	int iface0_class;     /* interface 0's class, or -1 */
};

enum modeswitch_verdict {
	MODESWITCH_VERDICT_NO_RULE,  /* no rule names this device: nothing to say */
	MODESWITCH_VERDICT_SWITCH,   /* switching it is right */
	MODESWITCH_VERDICT_ALREADY,  /* interface 0 already has the rule's TargetClass */
	MODESWITCH_VERDICT_REFUSE,   /* a rule applies and the switch is refused; reason says why */
};

enum modeswitch_verdict modeswitch_verdict(const struct modeswitch_facts *f, const char **reason);

/* One bulk OUT message of a switch, and what to read back after it. */
struct modeswitch_step {
	unsigned char bytes[MODESWITCH_MSG_MAX];
	size_t len;
	int is_cbw;            /* a bulk-only command wrapper: its data stage and status follow */
	unsigned int data_len; /* the CBW's dCBWDataTransferLength */
	int data_in;           /* its bmCBWFlags direction bit */
	int read_response;     /* not a CBW, and the rule says NeedResponse */
};

/*
 * The transfers a switch sends, in order. StandardEject is built from
 * the SCSI commands REFERENCE names -- PREVENT ALLOW MEDIUM REMOVAL
 * (0x1e, allow) and START STOP UNIT (0x1b, LoEj set, Start clear) -- for
 * every LUN up to max_lun, then the rule's MessageContent; upstream, too,
 * ignores MessageContent2/3 beside StandardEject. Otherwise the rule's
 * messages in order. The number of steps, or -1 when there is no method.
 */
int modeswitch_plan(const struct modeswitch_rule *r, int max_lun,
                    struct modeswitch_step out[MODESWITCH_STEPS_MAX]);

/* Whether a target product of the rule is this device's: the device has
 * switched. */
int modeswitch_is_target(const struct modeswitch_rule *r, unsigned int vendor,
                         unsigned int product);

#endif
