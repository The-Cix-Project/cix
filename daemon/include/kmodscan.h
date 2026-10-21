#ifndef KMODSCAN_H
#define KMODSCAN_H

#include "json.h"
#include "kmodpolicy.h"

/*
 * ADR-0348: deciding and loading a module, and the coldplug sweep that
 * gives hardware present at boot its driver.
 *
 * kmodscan_load() is the one place a load from the start-up paths is
 * decided and run: the NIC list, the sweep and kmod-config autoload all
 * call it, so each asks the same verdict and passes the module's
 * kmod-config options. The explicit POST asks the verdict in api_kmod.c,
 * where the answer is a status code rather than a log line.
 */

enum kmodscan_outcome {
	KMODSCAN_LOADED,
	KMODSCAN_ALREADY_LOADED,
	KMODSCAN_BUILTIN,
	KMODSCAN_UNCLAIMED,  /* the sweep only: no module claims the alias */
	KMODSCAN_REFUSED,    /* the verdict said no */
	KMODSCAN_FAILED,     /* modprobe said no */
};

const char *kmodscan_outcome_name(enum kmodscan_outcome o);

/*
 * Decides and loads one module for origin. reason receives the
 * verdict's sentence for KMODSCAN_REFUSED and modprobe's own words for
 * KMODSCAN_FAILED, "" otherwise. Logs nothing: what is worth a log line
 * depends on the origin, so the caller says it.
 */
enum kmodscan_outcome kmodscan_load(const char *name, enum kmod_origin origin, char *reason,
                                    size_t reason_size);

/*
 * The coldplug sweep, at start, before the loop serves anything (rule
 * 5): every distinct modalias under /sys/devices, resolved with
 * modprobe -R, each module decided and loaded. Runs only when the
 * policy's autoload covers coldplug; otherwise records why it did not.
 * Logs each load, refusal and failure once, and one summary line.
 */
void kmodscan_boot_sweep(void);

/* Records that the sweep did not run at this start, and why, for a
 * daemon whose start-up loads are not run at all (not --init-mode). */
void kmodscan_not_run(const char *why);

/* GET /v1/system/kmod-scan: the last sweep's report. */
void kmodscan_write_json(struct json_writer *w);

/*
 * One log line about a module load. The start-up loads run before
 * logstore_init(), and a line written then is dropped, so until
 * kmodscan_log_held() runs each line is held, and that call writes them
 * in order, the devicemap_log_skipped() pattern. After it, a line is
 * written at once.
 */
void kmodscan_say(const char *level, const char *fmt, ...);
void kmodscan_log_held(void);

#endif
