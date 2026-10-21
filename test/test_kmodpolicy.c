/*
 * ADR-0348: one policy decides every module load, whoever asks for it.
 *
 * The verdict is pure -- the policy, the module, its dependency list and
 * the origin in, an answer and its sentence out -- so it is tested here
 * on supplied inputs, the dependency lists included. Nothing in a build
 * container can load a module, so the loads themselves are verified on
 * a real host. The store is tested against a file in a fresh directory:
 * the never-saved default, a round trip, and a malformed file refusing
 * to start.
 *
 * In SELFTESTS: it needs no daemon, no modprobe and no /sys.
 */
#include "kmodpolicy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;

static void check(int ok, const char *what)
{
	if (!ok) {
		fprintf(stderr, "  FAIL: %s\n", what);
		g_failures++;
	}
}

static void policy(struct kmod_policy *p, const char *const *black, const char *const *allow)
{
	memset(p, 0, sizeof(*p));
	p->autoload = KMOD_AUTOLOAD_COLDPLUG;
	for (; black != NULL && *black != NULL; black++)
		snprintf(p->blacklist[p->blacklist_count++], KMOD_NAME_MAX, "%s", *black);
	for (; allow != NULL && *allow != NULL; allow++)
		snprintf(p->allowlist[p->allowlist_count++], KMOD_NAME_MAX, "%s", *allow);
}

static const enum kmod_origin g_all_origins[] = {
	KMOD_ORIGIN_REQUEST, KMOD_ORIGIN_CONFIG, KMOD_ORIGIN_NIC, KMOD_ORIGIN_COLDPLUG,
	KMOD_ORIGIN_HOTPLUG,
};
#define N_ORIGINS ((int)(sizeof(g_all_origins) / sizeof(g_all_origins[0])))

static void test_names(void)
{
	check(kmod_names_equal("usb-storage", "usb_storage"), "'-' and '_' are one character");
	check(kmod_names_equal("rtw88_8822bu", "rtw88_8822bu"), "a name equals itself");
	check(!kmod_names_equal("rtw88_8822b", "rtw88_8822bu"), "a prefix is not the name");
}

/* The owner's rule of 2026-08-27: the blacklist beats everything. */
static void test_blacklist_every_origin(void)
{
	static const char *const black[] = { "rtw88_8822bu", NULL };
	struct kmod_policy p;
	char why[256];
	int i;

	policy(&p, black, NULL);
	for (i = 0; i < N_ORIGINS; i++) {
		check(kmod_policy_verdict(&p, "rtw88_8822bu", NULL, 0, g_all_origins[i], why,
		                          sizeof(why)) == KMOD_REFUSE_BLACKLISTED,
		      "a blacklisted module is refused from every origin");
		check(strstr(why, "rtw88_8822bu") != NULL && strstr(why, "blacklisted") != NULL,
		      "the refusal names the module and the blacklist");
	}
	check(kmod_policy_verdict(&p, "rtw88-8822bu", NULL, 0, KMOD_ORIGIN_REQUEST, why,
	                          sizeof(why)) == KMOD_REFUSE_BLACKLISTED,
	      "the blacklist matches across '-' and '_'");
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_REQUEST, why, sizeof(why)) ==
	              KMOD_ALLOW &&
	          why[0] == '\0',
	      "a module off the blacklist is allowed, with no sentence");
}

/* modprobe loads dependencies, so a blacklisted one would load by the
 * back door: rtw88_8822bu's own closure as modules.dep lists it. */
static void test_dependency(void)
{
	static const char *const black[] = { "rtw88_usb", NULL };
	char deps[3][KMOD_NAME_MAX] = { "rtw88_8822b", "rtw88_usb", "rtw88_core" };
	struct kmod_policy p;
	char why[256];
	int i;

	policy(&p, black, NULL);
	for (i = 0; i < N_ORIGINS; i++) {
		check(kmod_policy_verdict(&p, "rtw88_8822bu", deps, 3, g_all_origins[i], why,
		                          sizeof(why)) == KMOD_REFUSE_DEPENDENCY,
		      "a module with a blacklisted dependency is refused from every origin");
		check(strstr(why, "rtw88_8822bu") != NULL && strstr(why, "rtw88_usb") != NULL,
		      "the refusal names the module and the dependency");
	}
	check(kmod_policy_verdict(&p, "rtw88_8822bu", deps, 1, KMOD_ORIGIN_REQUEST, why,
	                          sizeof(why)) == KMOD_ALLOW,
	      "only the dependencies passed are asked about");
}

/* The allowlist binds the automatic origins only: an operator naming a
 * module has allowed it. */
static void test_allowlist(void)
{
	static const char *const allow[] = { "rtw88_8822bu", NULL };
	static const char *const black[] = { "rtw88_8822bu", NULL };
	struct kmod_policy p;
	char why[256];

	policy(&p, NULL, allow);
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_COLDPLUG, why, sizeof(why)) ==
	          KMOD_REFUSE_NOT_ALLOWLISTED,
	      "a coldplug load off a non-empty allowlist is refused");
	check(strstr(why, "tg3") != NULL && strstr(why, "allowlist") != NULL,
	      "the allowlist refusal names the module and the allowlist");
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_HOTPLUG, why, sizeof(why)) ==
	          KMOD_REFUSE_NOT_ALLOWLISTED,
	      "a hotplug load off a non-empty allowlist is refused");
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_REQUEST, why, sizeof(why)) ==
	          KMOD_ALLOW,
	      "the allowlist does not bind an explicit request");
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_CONFIG, why, sizeof(why)) ==
	          KMOD_ALLOW,
	      "the allowlist does not bind a kmod-config entry");
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_NIC, why, sizeof(why)) ==
	          KMOD_ALLOW,
	      "the allowlist does not bind the boot NIC list");
	check(kmod_policy_verdict(&p, "rtw88-8822bu", NULL, 0, KMOD_ORIGIN_COLDPLUG, why,
	                          sizeof(why)) == KMOD_ALLOW,
	      "a module on the allowlist loads automatically, across '-' and '_'");

	policy(&p, NULL, NULL);
	check(kmod_policy_verdict(&p, "tg3", NULL, 0, KMOD_ORIGIN_COLDPLUG, why, sizeof(why)) ==
	          KMOD_ALLOW,
	      "an empty allowlist allows every automatic load");

	policy(&p, black, allow);
	check(kmod_policy_verdict(&p, "rtw88_8822bu", NULL, 0, KMOD_ORIGIN_COLDPLUG, why,
	                          sizeof(why)) == KMOD_REFUSE_BLACKLISTED,
	      "the blacklist beats the allowlist");
}

static void test_autoload_covers(void)
{
	struct kmod_policy p;

	policy(&p, NULL, NULL);
	check(kmod_policy_autoload_covers(&p, KMOD_ORIGIN_COLDPLUG), "coldplug covers the sweep");
	check(!kmod_policy_autoload_covers(&p, KMOD_ORIGIN_HOTPLUG),
	      "no mode covers hotplug while no hotplug path exists");
	p.autoload = KMOD_AUTOLOAD_OFF;
	check(!kmod_policy_autoload_covers(&p, KMOD_ORIGIN_COLDPLUG), "off covers no sweep");
	check(kmod_policy_autoload_covers(&p, KMOD_ORIGIN_REQUEST) &&
	          kmod_policy_autoload_covers(&p, KMOD_ORIGIN_CONFIG) &&
	          kmod_policy_autoload_covers(&p, KMOD_ORIGIN_NIC),
	      "autoload does not govern the operator origins");
}

static int apply(const char *body, struct kmod_policy *p, char *err, size_t err_size)
{
	struct json_value *root = json_parse(body, strlen(body));
	int rc = kmodpolicy_apply_json(root, p, err, err_size);

	json_free(root);
	return rc;
}

static void test_apply_json(void)
{
	static const char *const black[] = { "floppy", NULL };
	struct kmod_policy p;
	char err[192];

	policy(&p, black, NULL);
	check(apply("{\"allowlist\":[\"tg3\",\"r8169\"]}", &p, err, sizeof(err)) == 0 &&
	          p.allowlist_count == 2 && p.blacklist_count == 1 &&
	          strcmp(p.blacklist[0], "floppy") == 0 && p.autoload == KMOD_AUTOLOAD_COLDPLUG,
	      "a field left out of a PUT is kept");
	check(apply("{\"autoload\":\"off\",\"blacklist\":[]}", &p, err, sizeof(err)) == 0 &&
	          p.autoload == KMOD_AUTOLOAD_OFF && p.blacklist_count == 0,
	      "a field given replaces that field, an empty list included");
	check(apply("{\"autoload\":\"both\"}", &p, err, sizeof(err)) != 0 &&
	          strstr(err, "autoload") != NULL,
	      "an autoload mode with no path behind it is refused");
	check(apply("{\"blacklist\":[\"tg3\",\"tg3\"]}", &p, err, sizeof(err)) != 0 &&
	          strstr(err, "twice") != NULL,
	      "a list naming one module twice is refused");
	check(apply("{\"blacklist\":[\"a b\"]}", &p, err, sizeof(err)) != 0,
	      "a name with whitespace is refused");
	check(apply("{\"blacklist\":\"tg3\"}", &p, err, sizeof(err)) != 0,
	      "a list that is not an array is refused");
	check(apply("[]", &p, err, sizeof(err)) != 0, "a body that is not an object is refused");
}

static void write_file(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		check(0, "the test can write its fixture file");
		return;
	}
	fputs(text, f);
	fclose(f);
}

static void test_store(void)
{
	char dir[] = "/tmp/test_kmodpolicy_XXXXXX";
	char path[256];
	struct kmod_policy p;
	char err[192];

	if (mkdtemp(dir) == NULL) {
		check(0, "the test can make a scratch directory");
		return;
	}
	snprintf(path, sizeof(path), "%s/kmod_policy.json", dir);

	check(kmodpolicy_init(path) == 0, "a never-saved policy starts");
	check(!kmodpolicy_saved() && kmodpolicy_current()->autoload == KMOD_AUTOLOAD_COLDPLUG &&
	          kmodpolicy_current()->blacklist_count == 0 &&
	          kmodpolicy_current()->allowlist_count == 0,
	      "a never-saved host has the default: coldplug, empty lists, saved false");

	p = *kmodpolicy_current();
	check(apply("{\"autoload\":\"off\",\"blacklist\":[\"floppy\"]}", &p, err, sizeof(err)) == 0 &&
	          kmodpolicy_save(&p) == 0,
	      "a policy saves");
	check(kmodpolicy_init(path) == 0 && kmodpolicy_saved() &&
	          kmodpolicy_current()->autoload == KMOD_AUTOLOAD_OFF &&
	          kmodpolicy_current()->blacklist_count == 1 &&
	          strcmp(kmodpolicy_current()->blacklist[0], "floppy") == 0,
	      "a saved policy survives a restart, and is never replaced by the default");

	write_file(path, "{\"autoload\":\"sometimes\"}");
	check(kmodpolicy_init(path) != 0, "a malformed saved policy refuses to start");

	unlink(path);
	rmdir(dir);
}

int main(void)
{
	printf("test_kmodpolicy:\n");
	test_names();
	test_blacklist_every_origin();
	test_dependency();
	test_allowlist();
	test_autoload_covers();
	test_apply_json();
	test_store();
	if (g_failures != 0) {
		printf("test_kmodpolicy: %d FAILED\n", g_failures);
		return 1;
	}
	printf("test_kmodpolicy: PASS\n");
	return 0;
}
