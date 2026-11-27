/*
 * ADR-0398: the mode switcher's pure half -- rule parsing, name matching,
 * the verdict and the transfer plan -- on supplied inputs.
 *
 * The rule texts are usb-modeswitch-data-20261111's own, as
 * probe-modeswitch@3-1 printed them on 2026-11-27: the Realtek dongle the
 * ADR was written for and the D-Link rule that shares its id, a
 * TargetProductList rule with NeedResponse and a data stage, and a rule
 * with two messages. The device attributes are the dongle's (its record
 * read "Realtek DISK").
 *
 * Pure, so in SELFTESTS.
 */
#include "modeswitch.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond, ...)                                                                          \
	do {                                                                                      \
		if (!(cond)) {                                                                    \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                      \
			fprintf(stderr, __VA_ARGS__);                                             \
			fprintf(stderr, "\n");                                                    \
			g_failures++;                                                             \
		}                                                                                 \
	} while (0)

static const char REALTEK[] = "# Realtek Wifi Dongle\n"
                              "TargetVendor=0x0bda\n"
                              "TargetProduct=0xb851\n"
                              "StandardEject=1\n"
                              "\n";
static const char DLINK[] = "# D-Link DWA-171 Wifi Dongle\n"
                            "TargetVendor=0x2001\n"
                            "TargetProduct=0x331d\n"
                            "StandardEject=1\n";
static const char OLIVETTI[] =
	"# Olivetti Olicard 145, 155\n"
	"TargetVendor=0x0b3c\n"
	"TargetProductList=\"c003,c004\"\n"
	"MessageContent=\"5553424312345678c000000080010606f50402527000000000000000000000\"\n"
	"NeedResponse=1\n";
static const char HAIER[] =
	"# Haier CE682 (EVDO)\r\n"
	"TargetVendor=0x201e\r\n"
	"TargetProduct=0x1022\r\n"
	"MessageContent=\"55534243123456780000000000000600000000000000000000000000000000\"\r\n"
	"MessageContent2=\"5553424312345679c000000080000671030000000000000000000000000000\"\r\n"
	"NeedResponse=1\r\n";
static const char HUAWEI[] = "# a Huawei-style rule\nTargetVendor=0x12d1\nHuaweiNewMode=1\n";

static struct modeswitch_rule parse(const char *name, const char *text)
{
	struct modeswitch_rule r;
	char err[128] = "";

	CHECK(modeswitch_rule_parse(name, text, strlen(text), &r, err, sizeof(err)) == 0,
	      "parse %s: %s", name, err);
	return r;
}

static void test_parse(void)
{
	struct modeswitch_rule r = parse("0bda:1a2b:uMa=Realtek", REALTEK);
	char err[128];
	unsigned int v, p;

	CHECK(r.vendor == 0x0bda && r.product == 0x1a2b, "ids %04x:%04x", r.vendor, r.product);
	CHECK(r.token_count == 1 && strcmp(r.tokens[0].key, "uMa") == 0 &&
	              strcmp(r.tokens[0].value, "Realtek") == 0,
	      "token");
	CHECK(strcmp(r.comment, "Realtek Wifi Dongle") == 0, "comment '%s'", r.comment);
	CHECK(r.has_target_vendor && r.target_vendor == 0x0bda, "target vendor");
	CHECK(r.target_count == 1 && r.targets[0] == 0xb851, "target product");
	CHECK(r.standard_eject == 1 && r.msg_len[0] == 0 && r.unsupported[0] == '\0', "eject");
	CHECK(r.target_class == -1 && r.interface == 0 && r.message_endpoint == -1, "defaults");

	r = parse("0b3c:f000", OLIVETTI);
	CHECK(r.target_count == 2 && r.targets[0] == 0xc003 && r.targets[1] == 0xc004,
	      "TargetProductList %d", r.target_count);
	CHECK(r.msg_len[0] == 31 && r.msg[0][0] == 'U' && r.msg[0][30] == 0, "message bytes");
	CHECK(r.need_response == 1, "NeedResponse");

	r = parse("201e:1023", HAIER); /* CRLF line ends */
	CHECK(r.msg_len[0] == 31 && r.msg_len[1] == 31 && r.msg_len[2] == 0, "two messages");
	CHECK(r.target_count == 1 && r.targets[0] == 0x1022, "CRLF target");

	/* Endpoints and Interface are hex with or without 0x, as upstream's
	 * ParseParamHex reads them; delays are decimal. */
	r = parse("1234:5678", "MessageEndpoint=01\nResponseEndpoint=81\nInterface=0x1\n"
	                       "ReleaseDelay=4000\nTargetClass=0xff\n");
	CHECK(r.message_endpoint == 0x01 && r.response_endpoint == 0x81 && r.interface == 1,
	      "hex keys: %x %x %d", r.message_endpoint, r.response_endpoint, r.interface);
	CHECK(r.release_delay_ms == 4000 && r.target_class == 0xff, "decimal delay, hex class");

	r = parse("12d1:1446", HUAWEI);
	CHECK(strcmp(r.unsupported, "HuaweiNewMode") == 0, "unsupported '%s'", r.unsupported);
	r = parse("12d1:1446", "HuaweiNewMode=0\nStandardEject=1\n");
	CHECK(r.unsupported[0] == '\0', "a zero value is not a method");

	CHECK(modeswitch_rule_parse("12d1:#android", "", 0, &r, err, sizeof(err)) != 0,
	      "an android name is not a rule cixd reads");
	CHECK(modeswitch_rule_parse("0bda:1a2b:xYz=1", "", 0, &r, err, sizeof(err)) != 0,
	      "unknown attribute");
	CHECK(modeswitch_rule_parse("0bda:1a2b", "MessageContent=\"55534g\"\n", 24, &r, err,
	                            sizeof(err)) != 0,
	      "a message that is not hex is refused");
	CHECK(modeswitch_name_ids("05c6:1000:uMa=Co.,Ltd", &v, &p) == 0 && v == 0x05c6 &&
	              p == 0x1000,
	      "name ids");
	CHECK(modeswitch_name_ids("05c6:100", &v, &p) != 0, "short name");
}

static void test_choose(void)
{
	struct modeswitch_rule rules[3];
	struct modeswitch_device realtek = { 0x0bda, 0x1a2b, "Realtek", "DISK", "" };
	struct modeswitch_device dlink = { 0x0bda, 0x1a2b, "D-Link Corporation", "DWA-171", "" };
	struct modeswitch_device other = { 0x0bda, 0x1a2b, "Generic", "DISK", "" };
	struct modeswitch_device modem = { 0x1c9e, 0xf000, "USB Modem", "", "" };
	int amb, scsi;

	rules[0] = parse("0bda:1a2b:uMa=D-Link", DLINK);
	rules[1] = parse("0bda:1a2b:uMa=Realtek", REALTEK);
	CHECK(modeswitch_choose(rules, 2, &realtek, &amb, &scsi) == 1, "Realtek rule");
	CHECK(modeswitch_choose(rules, 2, &dlink, &amb, &scsi) == 0, "D-Link rule (substring)");
	CHECK(modeswitch_choose(rules, 2, &other, &amb, &scsi) == -1 && !amb && !scsi,
	      "no rule for another maker");

	/* A plain rule beside a matching qualified one loses to it. */
	rules[2] = parse("0bda:1a2b", DLINK);
	CHECK(modeswitch_choose(rules, 3, &realtek, &amb, &scsi) == 1, "qualified over plain");
	CHECK(modeswitch_choose(rules, 3, &other, &amb, &scsi) == 2, "plain when none qualifies");

	/* Two qualified rules that both match are refused, never picked. */
	rules[0] = parse("0bda:1a2b:uMa=Real", REALTEK);
	CHECK(modeswitch_choose(rules, 2, &realtek, &amb, &scsi) == -1 && amb, "ambiguous");

	/* `_` reads as a space, as upstream's MatchDevice does. */
	rules[0] = parse("1c9e:f000:uMa=USB_Modem", DLINK);
	CHECK(modeswitch_choose(rules, 1, &modem, &amb, &scsi) == 0, "underscore is a space");

	/* A SCSI-qualified rule cannot be decided, and a plain rule does not
	 * take its place. */
	rules[0] = parse("05c6:1000:sVe=Option", DLINK);
	rules[1] = parse("05c6:1000", DLINK);
	modem.vendor = 0x05c6;
	modem.product = 0x1000;
	CHECK(modeswitch_choose(rules, 2, &modem, &amb, &scsi) == -1 && scsi, "scsi only");
}

static void test_verdict(void)
{
	struct modeswitch_rule r = parse("0bda:1a2b:uMa=Realtek", REALTEK);
	struct modeswitch_rule h = parse("12d1:1446", HUAWEI);
	struct modeswitch_facts f;
	const char *why;

	memset(&f, 0, sizeof(f));
	f.rule = &r;
	f.device_free = 1;
	f.iface_mass_storage = 1;
	f.iface0_class = 0x08;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_SWITCH, "switch: %s", why);

	f.device_free = 0;
	f.not_free = "held";
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_REFUSE && strcmp(why, "held") == 0,
	      "a held device is never switched");
	f.device_free = 1;
	f.block_below = 1;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_REFUSE, "a disk below refuses");
	f.block_below = 0;
	f.iface_mass_storage = 0;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_REFUSE,
	      "an eject goes only to a mass-storage interface");
	f.iface_mass_storage = 1;

	r.target_class = 0xff;
	f.iface0_class = 0xff;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_ALREADY, "already switched");
	r.target_class = -1;

	f.rule = &h;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_REFUSE, "unsupported method");

	f.rule = NULL;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_NO_RULE, "no rule");
	f.ambiguous = 1;
	CHECK(modeswitch_verdict(&f, &why) == MODESWITCH_VERDICT_REFUSE, "ambiguous refuses");
}

static void test_plan(void)
{
	struct modeswitch_rule r = parse("0bda:1a2b:uMa=Realtek", REALTEK);
	struct modeswitch_rule o = parse("0b3c:f000", OLIVETTI);
	struct modeswitch_rule h = parse("201e:1023", HAIER);
	static struct modeswitch_step s[MODESWITCH_STEPS_MAX];
	int n;

	n = modeswitch_plan(&r, 0, s);
	CHECK(n == 2, "eject on one LUN is two commands, got %d", n);
	CHECK(s[0].len == MODESWITCH_CBW_LEN && memcmp(s[0].bytes, "USBC", 4) == 0, "CBW signature");
	CHECK(s[0].bytes[8] == 0 && s[0].bytes[12] == 0 && s[0].bytes[13] == 0 &&
	              s[0].bytes[14] == 6 && s[0].bytes[15] == 0x1e && s[0].bytes[19] == 0x00,
	      "ALLOW MEDIUM REMOVAL");
	CHECK(s[1].bytes[15] == 0x1b && s[1].bytes[19] == 0x02 && s[1].is_cbw && s[1].data_len == 0,
	      "START STOP UNIT, LoEj");
	CHECK(memcmp(s[0].bytes + 4, s[1].bytes + 4, 4) != 0, "distinct tags");

	n = modeswitch_plan(&r, 1, s);
	CHECK(n == 4 && s[2].bytes[13] == 1 && s[3].bytes[13] == 1, "second LUN");
	n = modeswitch_plan(&r, 99, s);
	CHECK(n == 2 * MODESWITCH_LUNS_MAX, "LUNs are capped, got %d", n);

	n = modeswitch_plan(&o, 0, s);
	CHECK(n == 1 && s[0].is_cbw && s[0].data_len == 0xc0 && s[0].data_in == 1 &&
	              !s[0].read_response,
	      "a rule's CBW carries its data stage");

	n = modeswitch_plan(&h, 0, s);
	CHECK(n == 2 && s[1].data_len == 0xc0 && s[1].data_in, "two messages in order");

	/* How a switch ended, from what the port holds now. */
	CHECK(modeswitch_return_of(0x0bda, 0x1a2b, &r, 0, 0, 0) == MODESWITCH_RETURN_WAIT,
	      "nothing on the port yet");
	CHECK(modeswitch_return_of(0x0bda, 0x1a2b, &r, 1, 0x0bda, 0x1a2b) == MODESWITCH_RETURN_WAIT,
	      "still the device switched from");
	CHECK(modeswitch_return_of(0x0bda, 0x1a2b, &r, 1, 0x0bda, 0xb851) == MODESWITCH_RETURN_TARGET,
	      "a listed target");
	CHECK(modeswitch_return_of(0x0bda, 0x1a2b, &r, 1, 0x0bda, 0xc811) ==
	              MODESWITCH_RETURN_UNLISTED,
	      "another chip behind the same id still switched");

	CHECK(modeswitch_is_target(&r, 0x0bda, 0xb851), "target");
	CHECK(!modeswitch_is_target(&r, 0x0bda, 0x1a2b), "the source id is not a target");
	CHECK(modeswitch_is_target(&o, 0x0b3c, 0xc004), "target list");
}

int main(void)
{
	test_parse();
	test_choose();
	test_verdict();
	test_plan();
	if (g_failures != 0) {
		fprintf(stderr, "test_modeswitch: %d failure(s)\n", g_failures);
		return 1;
	}
	printf("test_modeswitch: PASS\n");
	return 0;
}
