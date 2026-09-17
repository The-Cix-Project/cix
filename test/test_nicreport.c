/*
 * #442: the installer's own explanation of an empty interface list.
 *
 * Pure logic, in SELFTESTS, because the sentence IS the bug. cix-install
 * printed "its driver is a kernel module and this installer carries no
 * module tree" above a list showing only `lo`, on media that had
 * carried both since the afternoon that note was written -- so the one
 * real bare-metal install produced a confident explanation of a
 * mechanism that did not exist, and the issue was filed as "cause not
 * established".
 *
 * Neither end of that can be caught where it happened. cix-install runs
 * as pid 1 on real media and formats a disk, and `test_installer` is
 * not in SELFTESTS (grep the Makefile: it appears in `all` and in its
 * own rule, nowhere else), so an assertion there would never run. The
 * classification is extracted for exactly that reason and this covers
 * every state of it.
 *
 * What each case asserts is not the wording but the CLAIM: that the
 * message blames the media when the media is at fault and the machine
 * when the machine is, and never the other way round. That is the thing
 * that was wrong.
 */
#include "nicreport.h"
#include "bootmodules.h"

#include <stdio.h>
#include <string.h>

static int g_fails;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			printf("  FAIL: ");                                                                    \
			printf(__VA_ARGS__);                                                                   \
			printf("\n");                                                                          \
			g_fails++;                                                                             \
		}                                                                                          \
	} while (0)

/* Case-insensitive substring, so a test does not fail on capitalisation. */
static int has(const char *hay, const char *needle)
{
	size_t nl = strlen(needle), i;

	for (i = 0; hay[i] != '\0'; i++) {
		size_t j;

		for (j = 0; j < nl; j++) {
			char a = hay[i + j], b = needle[j];

			if (a >= 'A' && a <= 'Z')
				a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z')
				b = (char)(b - 'A' + 'a');
			if (a != b)
				break;
		}
		if (j == nl)
			return 1;
	}
	return 0;
}

static void test_no_tools(void)
{
	struct nic_load_result r;
	char out[640];

	printf("1. no module tools on the media\n");
	memset(&r, 0, sizeof(r));
	r.tools_present = 0;
	nicreport_no_nic_reason(&r, 0, out, sizeof(out));
	CHECK(has(out, "no module tools"), "does not say the tools are missing: %s", out);
	CHECK(has(out, "defect in the media"),
	      "does not blame the media -- an operator reading this would suspect their hardware: %s",
	      out);
	CHECK(has(out, "cix-kmod"),
	      "does not name what produced media without modprobe, which is the one actionable "
	      "fact: %s",
	      out);
	/* The old text's actual failure: it must not assert that the media
	 * carries no module TREE, which is a different and now-false
	 * claim from carrying no TOOLS. */
	CHECK(!has(out, "no module tree"), "revives the false \"no module tree\" claim: %s", out);
}

static void test_load_failed(void)
{
	struct nic_load_result r;
	char out[640];

	printf("2. tools present, a load failed\n");
	memset(&r, 0, sizeof(r));
	r.tools_present = 1;
	r.attempted = 5;
	r.loaded = 3;
	r.failed = 2;
	snprintf(r.first_error, sizeof(r.first_error), "e1000e: modprobe: FATAL: Module e1000e not found");
	nicreport_no_nic_reason(&r, 0, out, sizeof(out));
	/*
	 * modprobe's own text, verbatim, is the whole point: it is the only
	 * thing that separates "no tree for this kernel release" from "a
	 * tree built from another config", and discarding it is what left
	 * #442 with no cause.
	 */
	CHECK(has(out, "Module e1000e not found"),
	      "drops modprobe's own message, which is the only thing that names the cause: %s", out);
	CHECK(has(out, "2 of 5"), "does not say how many failed: %s", out);
	CHECK(has(out, "not this machine"), "does not blame the media: %s", out);
}

static void test_all_loaded_no_nic(void)
{
	static const char *const mods[] = CIX_NIC_MODULES;
	struct nic_load_result r;
	char out[640];
	size_t i;

	printf("3. every load succeeded and no interface appeared\n");
	memset(&r, 0, sizeof(r));
	r.tools_present = 1;
	r.attempted = 5;
	r.loaded = 5;
	r.failed = 0;
	nicreport_no_nic_reason(&r, 0, out, sizeof(out));
	/*
	 * This is the ONLY case that is about the machine, and it has to
	 * say so -- the measurement it rests on is that modprobe succeeds
	 * on a machine without the chipset (192.168.15.95, virtio, `kmod
	 * load tg3` -> 0 and Live with used_by=0), so five clean loads and
	 * no interface means unsupported hardware, not broken media.
	 */
	CHECK(has(out, "media is fine"),
	      "blames the media on the one case that is genuinely the machine: %s", out);
	CHECK(has(out, "bnx2") && has(out, "igc"),
	      "does not name the two recorded driver gaps, so an operator with an I226 is told "
	      "nothing: %s",
	      out);
	for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
		CHECK(has(out, mods[i]), "does not name supported driver %s: %s", mods[i], out);
}

static void test_guards(void)
{
	struct nic_load_result r;
	char out[640];

	printf("4. guards\n");
	memset(&r, 0, sizeof(r));
	r.tools_present = 1;
	r.attempted = 5;
	r.loaded = 5;
	/*
	 * Called with a NIC in hand it must say NOTHING. Printing an
	 * explanation next to a list that contradicts it is #442 over
	 * again in the other direction.
	 */
	nicreport_no_nic_reason(&r, 1, out, sizeof(out));
	CHECK(out[0] == '\0', "explains an empty list while a NIC is listed: %s", out);

	/* A NULL result must not be silence: that would be an empty list
	 * with no reason at all, which is the state this whole file
	 * exists to make impossible. */
	nicreport_no_nic_reason(NULL, 0, out, sizeof(out));
	CHECK(out[0] != '\0', "says nothing at all when the load result is missing");
	CHECK(has(out, "bug in the installer"), "does not name itself as the fault: %s", out);

	/* Never writes past a short buffer, and never returns NULL. */
	{
		char tiny[24];
		const char *p;

		memset(&r, 0, sizeof(r));
		r.tools_present = 1;
		r.attempted = 5;
		r.failed = 5;
		p = nicreport_no_nic_reason(&r, 0, tiny, sizeof(tiny));
		CHECK(p == tiny, "does not return the caller's buffer");
		CHECK(strlen(tiny) < sizeof(tiny), "overran a 24-byte buffer");
	}
}

int main(void)
{
	printf("NIC REPORT TEST\n");
	test_no_tools();
	test_load_failed();
	test_all_loaded_no_nic();
	test_guards();
	if (g_fails == 0)
		printf("NIC REPORT TEST: PASS\n");
	else
		printf("NIC REPORT TEST: FAIL (%d failure(s))\n", g_fails);
	return g_fails == 0 ? 0 : 1;
}
