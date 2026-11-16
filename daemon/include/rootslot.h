#ifndef CIX_ROOTSLOT_H
#define CIX_ROOTSLOT_H

/*
 * ADR-0379: which cix-root version each boot slot holds, as the host roll
 * staged it; the version a switch or boot into a slot is attempting; and
 * the version that failed to deploy.
 *
 * One small JSON file on the config partition, which outlives both
 * slots. Only the roll sets a slot's version, and only for a root its
 * own assembly just sealed: cixd cannot know what any other file it was
 * handed was sealed from. Any write to a slot clears the slot's version
 * first, so a stale one can never survive a hand update or a restore.
 *
 * Everything here is inert until rootslot_load() is called with a path,
 * which happens only in --init-mode with a slot: a test daemon records
 * nothing.
 */

/* Reads the record. A missing or unreadable file is an empty record. */
void rootslot_load(const char *path);

/* The cix-root version slot holds; "" when unknown. Never NULL. */
const char *rootslot_version(const char *slot);

/* Before anything writes slot: its version is no longer known. */
void rootslot_clear(const char *slot);

/* The roll staged slot with a root sealed from version. */
void rootslot_set(const char *slot, const char *version);

/*
 * Before a switch or reboot into slot: records "attempting the version
 * slot holds, on slot". A slot with no version records nothing, since
 * there is nothing to mark bad.
 */
void rootslot_attempt(const char *slot);

/*
 * At start, once the running slot is known: an attempt on another slot
 * failed (a refused switch, a reboot fallback or the boot counter's
 * rollback), so its version is marked bad. Returns 1 when it did that,
 * and fills failed_version.
 */
int rootslot_resolve_start(const char *running_slot, char *failed_version,
                           unsigned long failed_size);

/* The running slot is confirmed: an attempt on it succeeded. */
void rootslot_confirmed(const char *slot);

/* An attempt failed in this process (the handover refused the switch). */
void rootslot_attempt_failed(void);

/* A root-only roll could not assemble or stage version: it failed to
 * deploy as surely as a refused switch, and is not tried again. */
void rootslot_mark_bad(const char *version);

/* The version that failed to deploy; "" for none. Never NULL. */
const char *rootslot_bad(void);

/* cix-root moved past the bad version, so the mark no longer applies. */
void rootslot_clear_bad(void);

#endif
