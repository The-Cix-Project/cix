#ifndef CIX_ROOTSLOT_H
#define CIX_ROOTSLOT_H

/*
 * ADR-0379: which cix-root version each boot slot holds, as the host roll
 * staged it; the slot last confirmed; the version a switch or boot into a
 * slot is attempting; and the version that failed to deploy.
 *
 * ADR-0380: each of those also carries the cixd version that cix-root
 * version held, because once cixd rolls in cix-root a release is a cixd
 * version, and a rejected release must stay off whatever else moves.
 *
 * One small JSON file on the config partition, which outlives both
 * slots. Only the roll sets a slot's version, and only for a root its
 * own assembly just sealed: cixd cannot know what any other file it was
 * handed was sealed from. Any write to a slot clears the slot's version
 * first, so a stale one can never survive a hand update or a restore.
 *
 * Everything here is inert until rootslot_load() is called with a path,
 * which happens only in --init-mode with a slot: a test daemon records
 * nothing. A change is written whole and, once a callback is set,
 * announced through it (GET /v1/system/boot reads this record).
 */

/* Reads the record. A missing or unreadable file is an empty record. */
void rootslot_load(const char *path);

/* Called after every change that was written; NULL for none. */
void rootslot_set_on_change(void (*on_change)(void));

/*
 * ADR-0380: the cixd version this process is (CIX_BUILD_VERSION, which
 * is the cixd package's version string). A bad mark never names it: the
 * daemon recording the mark is running that cixd, so it is not what
 * failed. Without this, a failed root-only roll that moved only a
 * library, where both roots hold the running cixd, would hold every
 * later root until the next release.
 */
void rootslot_set_running_cixd(const char *cixd);

/* The cix-root version slot holds; "" when unknown. Never NULL. */
const char *rootslot_version(const char *slot);

/* The cixd version in that cix-root version; "" when unknown. Never NULL.
 * A package version, so PKG_VERSION_MAX long; pkg.h is not included here. */
#define ROOTSLOT_CIXD_MAX 64
const char *rootslot_cixd(const char *slot);

/* Before anything writes slot: its version is no longer known. */
void rootslot_clear(const char *slot);

/* The roll staged slot with a root sealed from version, whose cixd was
 * cixd ("" when it had none). */
void rootslot_set(const char *slot, const char *version, const char *cixd);

/*
 * Before a switch or reboot into slot: records "attempting the version
 * slot holds, on slot". A slot with no version records nothing, since
 * there is nothing to mark bad.
 */
void rootslot_attempt(const char *slot);

/* What rootslot_resolve_start() found. */
enum rootslot_start {
	ROOTSLOT_START_SAME,     /* nothing to resolve */
	ROOTSLOT_START_FAILED,   /* an attempt on another slot failed: its version is bad */
	ROOTSLOT_START_REJECTED  /* the host left the last confirmed slot with no attempt (an
	                          * operator's boot-next, or a boot that rolled back): the
	                          * version it left is bad, so it is not deployed again */
};

/*
 * At start, once the running slot is known. Fills version with the
 * version marked bad, for FAILED and REJECTED; the bad mark takes that
 * slot's cixd with it.
 */
enum rootslot_start rootslot_resolve_start(const char *running_slot, char *version,
                                           unsigned long version_size);

/*
 * The running slot is confirmed: it becomes the last confirmed slot, an
 * attempt on it succeeded, and a bad mark naming its version, or its
 * cixd, is cleared, since that root has just proved it works.
 */
void rootslot_confirmed(const char *slot);

/*
 * version, holding cixd, failed to deploy: mkbootroot could not build
 * it, or the handover refused the switch into it. A refused switch
 * leaves its attempt recorded, because the staged slot is still the
 * next boot: a later reboot into it is then resolved like any attempt.
 */
void rootslot_mark_bad(const char *version, const char *cixd);

/* The version that failed to deploy, and its cixd; "" for none. Never NULL. */
const char *rootslot_bad(void);
const char *rootslot_bad_cixd(void);

/* cix-root moved past the bad version and its cixd, or an operator
 * cleared it. */
void rootslot_clear_bad(void);

#endif
