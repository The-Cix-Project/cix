#ifndef KMODPOLICY_H
#define KMODPOLICY_H

#include "json.h"
#include "kmod.h"

/*
 * ADR-0348: one policy decides every module load, whoever asks for it.
 *
 * The policy is GET/PUT /v1/system/kmod-policy, persisted beside
 * kmod-config: an autoload mode, a blacklist and an allowlist. The
 * verdict is kmod_policy_verdict(), a pure function of the policy, the
 * module, its dependency list and the origin of the request, so the
 * gate can test it on supplied inputs. Every load path asks it before
 * modprobe runs: an explicit request, a kmod-config autoload entry, the
 * boot NIC list, a coldplug match and a hotplug match.
 *
 * Per-module options stay in kmod-config (kmodconfig.h) and are not
 * repeated here.
 */

#define KMODPOLICY_LIST_MAX 64
#define KMODPOLICY_DEPS_MAX 64

/* The automatic loads that run. Only the coldplug sweep exists so far;
 * hotplug and both arrive with the hotplug path (ADR-0348's second
 * release), and the never-saved default moves to both with them. */
enum kmod_autoload {
	KMOD_AUTOLOAD_OFF,
	KMOD_AUTOLOAD_COLDPLUG,
};

/* Who asked for a load. The blacklist refuses every origin; the
 * allowlist binds only the automatic ones, coldplug and hotplug: an
 * operator naming a module has allowed it. */
enum kmod_origin {
	KMOD_ORIGIN_REQUEST,  /* POST /v1/system/kmod/{name} */
	KMOD_ORIGIN_CONFIG,   /* a kmod-config autoload entry, at start */
	KMOD_ORIGIN_NIC,      /* the boot NIC list (bootmodules.h) */
	KMOD_ORIGIN_COLDPLUG, /* the sweep over /sys/devices */
	KMOD_ORIGIN_HOTPLUG,  /* a uevent's MODALIAS= */
};

enum kmod_verdict {
	KMOD_ALLOW,
	KMOD_REFUSE_BLACKLISTED,
	KMOD_REFUSE_DEPENDENCY,       /* a dependency is blacklisted */
	KMOD_REFUSE_NOT_ALLOWLISTED,  /* automatic origin, allowlist set, not on it */
};

struct kmod_policy {
	enum kmod_autoload autoload;
	char blacklist[KMODPOLICY_LIST_MAX][KMOD_NAME_MAX];
	int blacklist_count;
	char allowlist[KMODPOLICY_LIST_MAX][KMOD_NAME_MAX];
	int allowlist_count;
};

/* '-' and '_' are the same character, as modprobe and the kernel treat
 * them (usb-storage.ko is usb_storage in /proc/modules). */
int kmod_names_equal(const char *a, const char *b);

/*
 * The verdict. Pure: reads only its arguments. deps is the module's
 * dependency list as modules.dep gives it (kmod_dependencies()), which
 * the caller obtains. why receives one sentence naming the reason for
 * a refusal ("" for KMOD_ALLOW).
 */
enum kmod_verdict kmod_policy_verdict(const struct kmod_policy *p, const char *name,
                                      const char deps[][KMOD_NAME_MAX], int dep_count,
                                      enum kmod_origin origin, char *why, size_t why_size);

/* Whether p's autoload mode covers an automatic origin. 1 for the
 * operator origins, which autoload does not govern. */
int kmod_policy_autoload_covers(const struct kmod_policy *p, enum kmod_origin origin);

/* Whether name is on p's blacklist. */
int kmod_policy_blacklists(const struct kmod_policy *p, const char *name);

const char *kmod_autoload_name(enum kmod_autoload a);

/*
 * The live policy. A host that has never saved one gets autoload
 * "coldplug" with empty lists (the owner's default of 2026-10-21 is
 * "both", which this becomes when hotplug exists), and a saved
 * policy is never replaced by that default (ADR-0315's rule). A missing
 * file is the never-saved state; an unreadable or malformed one fails
 * init.
 */
int kmodpolicy_init(const char *state_path);
const struct kmod_policy *kmodpolicy_current(void);
int kmodpolicy_saved(void);

/*
 * Applies a PUT body onto *inout: each of autoload, blacklist and
 * allowlist that is present replaces that field, and an absent one is
 * kept. Names are checked with kmod_name_is_valid() and a list may not
 * name one module twice. -1 with err set on an invalid body, leaving
 * *inout partly applied, so the caller works on a copy.
 */
int kmodpolicy_apply_json(const struct json_value *body, struct kmod_policy *inout, char *err,
                          size_t err_size);

/* Persists p and makes it the live policy. -1 if the write failed, the
 * live policy then unchanged. */
int kmodpolicy_save(const struct kmod_policy *p);

/* {"autoload":..., "blacklist":[...], "allowlist":[...], "saved":bool} */
void kmodpolicy_write_json(struct json_writer *w);

#endif
