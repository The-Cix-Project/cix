#ifndef ESP_H
#define ESP_H

#include <stddef.h>

struct json_writer;

/*
 * The EFI System Partition's boot configuration -- systemd-boot's own
 * loader.conf and the loader entries beside it (issue #128, ADR-0202).
 *
 * This exists because the ESP was the one piece of a running host that
 * no API could reach. A host could be left permanently unable to
 * complete an A/B update by one stale line in loader.conf, with no way
 * to see that line, let alone change it, short of physical console
 * access -- which is exactly what happened, and what took a long
 * investigation to identify because the relevant state was invisible.
 *
 * The subtlety that made it expensive: systemd-boot's `default` is a
 * PATTERN, not an entry name. A pattern left over from an earlier
 * naming scheme silently keeps matching old entries and outranking
 * every new one, so a correctly staged update simply never boots and
 * nothing reports an error anywhere.
 */

#define ESP_DEFAULT_MAX 128
#define ESP_ENTRY_NAME_MAX 128
#define ESP_ENTRY_TITLE_MAX 256
#define ESP_ENTRY_OPTIONS_MAX 1024
#define ESP_ENTRY_LINUX_MAX 256
#define ESP_MAX_ENTRIES 64

enum esp_error {
	ESP_OK = 0,
	ESP_ERR_NO_ESP,        /* no loader directory here (a dev sandbox) */
	ESP_ERR_NOT_FOUND,     /* no such loader entry */
	ESP_ERR_READ_ONLY,     /* the ESP is present but not writable */
	ESP_ERR_INVALID,       /* malformed input (bad pattern, bad timeout) */
	ESP_ERR_WOULD_ORPHAN,  /* a default matching no entry, or deleting the running slot's entry */
	ESP_ERR_PERSIST_FAILED /* the write itself failed */
};

struct esp_entry {
	char name[ESP_ENTRY_NAME_MAX];       /* filename, e.g. "cix-b+3.conf" */
	char title[ESP_ENTRY_TITLE_MAX];
	char linux_image[ESP_ENTRY_LINUX_MAX];
	char options[ESP_ENTRY_OPTIONS_MAX];
	char sort_key[ESP_ENTRY_TITLE_MAX];  /* BLS "sort-key", "" when absent */
	char version[ESP_ENTRY_TITLE_MAX];   /* BLS "version", "" when absent */
	int matches_default;                  /* the current default pattern selects this */
	int is_running_slot;                  /* this is the entry the live system booted from */
};

/*
 * Both paths are resolved once at startup by main.c, which already owns
 * the "where does the ESP live" answer (and its --test-esp-entries-dir=
 * override) -- passing them in keeps that single answer rather than
 * growing a second copy of it here.
 */
void esp_init(const char *loader_dir, const char *entries_dir);

/* Which slot the live system booted from ("a"/"b"), so an entry can be
 * flagged as the running one and refused for deletion. */
void esp_set_running_slot(const char *slot);

/*
 * Reads loader.conf. out_default/out_timeout are filled with what is
 * actually on disk; an absent field yields an empty string / -1 rather
 * than a fabricated default, because "unset" and "set to the default
 * value" are genuinely different states here and the operator needs to
 * tell them apart.
 */
enum esp_error esp_loader_get(char *out_default, size_t out_default_size, int *out_timeout,
                              int *out_writable);

/*
 * Partial update: NULL/negative leaves that field alone. A default
 * pattern matching zero existing entries is refused (ESP_ERR_WOULD_ORPHAN)
 * -- that is precisely the state that strands a host, and the caller is
 * told which entries do exist so the mistake is obvious.
 */
enum esp_error esp_loader_set(const char *default_pattern, const int *timeout);

/* Fills out[] with up to max entries; returns the count, or -1 if there
 * is no ESP here. */
int esp_entries_list(struct esp_entry *out, int max);

/*
 * Removes one loader entry. Refuses the entry the live system booted
 * from (ESP_ERR_WOULD_ORPHAN) -- deleting that is how a remote operator
 * makes a machine they cannot physically reach unbootable.
 */
enum esp_error esp_entry_delete(const char *name);

/* Whether a systemd-boot `default` pattern selects a given entry name --
 * the glob semantics systemd-boot itself uses, which is the whole
 * subtlety behind issue #128. */
int esp_pattern_matches(const char *pattern, const char *entry_name);

/*
 * Names the loader entry for `slot` that a successful boot should
 * confirm, i.e. the one whose boot counter must be stripped. Writes the
 * bare filename into out and returns 1, or returns 0 when there is
 * nothing to confirm.
 *
 * Entry-naming semantics live here rather than in the caller because
 * this module already owns them (see esp_pattern_matches/entry ids),
 * and because getting this wrong is a silent revert rather than a
 * visible failure: after an update the ESP legitimately holds both
 * `cix-<slot>.conf` and `cix-<slot>+N.conf`, and confirming the wrong
 * one leaves the real entry's counter ticking down until systemd-boot
 * gives up on it and falls back.
 */
int esp_entry_to_confirm(const char *entries_dir, const char *slot, char *out, size_t out_size);

/*
 * How strong a candidate `entry_name` is for confirming `slot`:
 *   2 -- matches and still carries a boot counter (the one to confirm)
 *   1 -- matches and is already confirmed (nothing to do)
 *   0 -- not this slot's entry at all
 *
 * Separate from the directory scan so the decision can be tested
 * without depending on readdir() order. That is not a stylistic
 * preference: the bug this replaced was "take the first match", and it
 * is invisible on a filesystem whose readdir happens to return the
 * counter-bearing entry first -- which is exactly what this project's
 * own dev sandbox does, while the real ESP's vfat does not. A test
 * driven through a directory therefore passes against the broken
 * implementation here and fails only on the machine that matters.
 */
int esp_confirm_rank(const char *entry_name, const char *slot);

void esp_write_json(struct json_writer *w);

#endif /* ESP_H */
