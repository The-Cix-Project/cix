#ifndef BOOTCONSOLE_H
#define BOOTCONSOLE_H

#include "json.h"

/*
 * Issue #24: the installed system's own boot console.
 *
 * An installed Cix host boots through systemd-boot, and every loader
 * entry's `options` line was hardcoded to `console=tty0 console=ttyS0`
 * -- written that way by the installer, and rewritten that way by every
 * A/B update. That is a reasonable default and a poor one to be stuck
 * with: real hardware needs a serial console at a non-default baud, or
 * a framebuffer argument to get any output at all, or exactly the
 * opposite (`nomodeset`) when the framebuffer is what breaks it. None
 * of it was reachable without reinstalling.
 *
 * This module owns the operator's chosen console parameters. main.c
 * renders them into the loader entries it writes -- at update time and
 * on demand -- so there is one place that decides what a boot line says
 * and one place that writes it.
 *
 * Deliberately NOT a free-form kernel command line. `options` also
 * carries root=, rw, init= and the daemon's own arguments, and letting
 * an operator edit that whole string turns a display preference into a
 * way to make the box unbootable. Consoles and a bounded extra-
 * parameter field are the part that is genuinely theirs to choose.
 */

#define BOOTCONSOLE_MAX_CONSOLES 4
#define BOOTCONSOLE_CONSOLE_MAX 64
#define BOOTCONSOLE_EXTRA_MAX 256

struct bootconsole_config {
	char consoles[BOOTCONSOLE_MAX_CONSOLES][BOOTCONSOLE_CONSOLE_MAX];
	int console_count;
	char extra[BOOTCONSOLE_EXTRA_MAX];
};

/* Installs the defaults (tty0 + ttyS0, no extra parameters -- exactly
 * what was hardcoded before this existed) and loads any persisted
 * override. Never fails: an unreadable file leaves the defaults, since
 * this is a display preference and refusing to boot over it would be
 * far worse than the wrong console. */
void bootconsole_init(const char *path);
void bootconsole_repoint(const char *path);

const struct bootconsole_config *bootconsole_get(void);

/*
 * Renders the console portion of a loader entry's options line, e.g.
 * "console=tty0 console=ttyS0,115200n8 video=efifb:off". Always writes
 * something NUL-terminated; an empty configuration renders empty rather
 * than a stray space.
 */
void bootconsole_render(char *out, size_t out_size);

enum bootconsole_error {
	BOOTCONSOLE_OK = 0,
	BOOTCONSOLE_ERR_INVALID, /* a console name or extra parameter this cannot accept */
	BOOTCONSOLE_ERR_PERSIST
};

/*
 * Replaces the configuration. Each console is a bare tty name with
 * optional comma-separated options (`ttyS0,115200n8`); extra is a
 * space-separated list of plain kernel parameters. Both are validated
 * character by character -- this string is written into the file that
 * decides whether the machine boots, so anything that could split the
 * line or inject an unrelated parameter is refused rather than escaped.
 */
enum bootconsole_error bootconsole_set(const char consoles[][BOOTCONSOLE_CONSOLE_MAX],
                                        int console_count, const char *extra);

void bootconsole_write_json(struct json_writer *w);

#endif /* BOOTCONSOLE_H */
