#ifndef KMODSCAN_H
#define KMODSCAN_H

#include "json.h"
#include "kmodpolicy.h"

/*
 * ADR-0348: deciding and loading a module, the sweeps that give
 * hardware its driver, and the jobs the module queue runs.
 *
 * kmodscan_load() is the one place a load is decided and run. The NIC
 * list and kmod-config autoload call it at start; every other load --
 * a sweep, a plugged device, an explicit POST -- is a job, run by
 * kmodscan_run(), which calls it too. So each asks the same verdict and
 * passes the module's kmod-config options.
 *
 * A job runs in line only at start, before the loop serves anything.
 * After that it runs only in the helper child of main.c's module queue,
 * one at a time: the child fills a report and writes it down a memory
 * file (kmodscan_report_write()), and the parent reads it back and
 * does everything with an effect -- the log lines, the last sweep's
 * report, the announcements, the answer to a parked request. The child
 * logs nothing and changes nothing the parent reads (ADR-0278).
 */

#define KMODSCAN_ALIAS_MAX 4096
#define KMODSCAN_DEVICE_MAX 192
#define KMODSCAN_REASON_MAX 256

enum kmodscan_outcome {
	KMODSCAN_LOADED,
	KMODSCAN_ALREADY_LOADED,
	KMODSCAN_BUILTIN,
	KMODSCAN_UNCLAIMED,  /* a sweep or plug only: no module claims the alias */
	KMODSCAN_REFUSED,    /* the verdict said no */
	KMODSCAN_FAILED,     /* modprobe said no */
	KMODSCAN_UNLOADED,   /* an explicit unload only; never in a sweep's report */
};

const char *kmodscan_outcome_name(enum kmodscan_outcome o);

/* What ran a sweep. */
enum kmodscan_trigger {
	KMODSCAN_TRIGGER_START,
	KMODSCAN_TRIGGER_REQUEST,  /* POST /v1/system/kmod-scan */
	KMODSCAN_TRIGGER_OVERFLOW, /* the uevent socket dropped events */
};

struct kmodscan_entry {
	int alias; /* index into its report's aliases */
	char device[KMODSCAN_DEVICE_MAX];
	char module[KMOD_NAME_MAX];
	enum kmodscan_outcome outcome;
	/* The verdict's sentence for REFUSED, modprobe's words for FAILED,
	 * and for an explicit load's ALREADY_LOADED why its options could
	 * not be applied; "" otherwise. */
	char reason[KMODSCAN_REASON_MAX];
};

/* A job's result: every alias it met and what happened to each module.
 * A sweep's report is GET /v1/system/kmod-scan; an explicit load's has
 * one alias, the name as asked. */
struct kmodscan_report {
	char **aliases;
	unsigned char *alias_more; /* per alias: more modules claim it than were decided */
	int alias_count;
	int alias_cap;
	struct kmodscan_entry *entries;
	int entry_count;
	int entry_cap;
	int ran;       /* a sweep: it walked /sys/devices */
	int truncated; /* a sweep: it stopped recording at the alias limit */
	enum kmodscan_trigger trigger;
	long long started_at;
	long long duration_ms;
	char not_run[KMODSCAN_REASON_MAX];
};

void kmodscan_report_init(struct kmodscan_report *r);
void kmodscan_report_free(struct kmodscan_report *r);

/* Whether the report loaded or unloaded anything: GET /v1/system/kmod
 * changed. */
int kmodscan_report_changed_modules(const struct kmodscan_report *r);

enum kmodscan_job_kind {
	KMODSCAN_JOB_SWEEP,  /* every modalias under /sys/devices */
	KMODSCAN_JOB_ALIAS,  /* one plugged device's MODALIAS= */
	KMODSCAN_JOB_LOAD,   /* POST /v1/system/kmod/{name} */
	KMODSCAN_JOB_UNLOAD, /* DELETE /v1/system/kmod/{name} */
};

struct kmodscan_job {
	enum kmodscan_job_kind kind;
	enum kmodscan_trigger trigger;   /* SWEEP */
	char alias[KMODSCAN_ALIAS_MAX];  /* ALIAS */
	char device[KMODSCAN_DEVICE_MAX];/* ALIAS: its sysfs directory */
	char name[KMOD_NAME_MAX];        /* LOAD, UNLOAD: a module name or, for LOAD, an alias */
	char options[KMOD_OPTIONS_MAX];  /* LOAD, when has_options: the body's, not kmod-config's */
	int has_options;
};

/*
 * Decides and loads one module for origin. options NULL means the
 * module's kmod-config options. reason receives the verdict's sentence
 * for KMODSCAN_REFUSED, modprobe's own words for KMODSCAN_FAILED, and
 * for an explicit request's KMODSCAN_ALREADY_LOADED with options, why
 * they were not applied; "" otherwise. Logs nothing: what is worth a log
 * line depends on the origin, so the caller says it.
 */
enum kmodscan_outcome kmodscan_load(const char *name, enum kmod_origin origin, const char *options,
                                    char *reason, size_t reason_size);

/* Runs job into out, which the caller has initialised. May run
 * modprobe, so in line only at start; after that, only in the module
 * queue's helper child. Logs nothing. */
void kmodscan_run(const struct kmodscan_job *job, struct kmodscan_report *out);

/* The child's half and the parent's: the report through a file, from
 * its start. 0, or -1 when it could not be written or what was read is
 * not a whole report. */
int kmodscan_report_write(const struct kmodscan_report *r, int fd);
int kmodscan_report_read(struct kmodscan_report *r, int fd);

/* One log line for each load, refusal and failure in r, and one for
 * each alias more modules claim than were decided. origin names the
 * line: coldplug or hotplug. */
void kmodscan_report_say(const struct kmodscan_report *r, enum kmod_origin origin);

/*
 * The setter for GET /v1/system/kmod-scan: r, a finished sweep's report,
 * becomes the last one (r is left empty), its lines and its summary are
 * said, no sweep is pending any more, and the read is announced.
 */
void kmodscan_adopt(struct kmodscan_report *r);

/* Whether a sweep is queued or running, and the setter that says so,
 * announcing the read. */
int kmodscan_sweep_pending(void);
void kmodscan_set_sweep_pending(int pending);

/*
 * The sweep at start, before the loop serves anything (rule 5), when the
 * policy's autoload covers coldplug; otherwise records why it did not
 * run.
 */
void kmodscan_boot_sweep(void);

/* Records that no sweep ran at this start, and why, for a daemon whose
 * start-up loads are not run at all (not --init-mode). */
void kmodscan_not_run(const char *why);

/* GET /v1/system/kmod-scan: the last sweep's report. */
void kmodscan_write_json(struct json_writer *w);

/*
 * How a request reaches the module queue. main.c installs the broker,
 * as it does credcheck's and logread's (ADR-0340, ADR-0342): a LOAD or
 * UNLOAD parks the request being served and is answered when its job
 * has run, from the report the connection then carries; a SWEEP is
 * queued and answered at once.
 */
enum kmodscan_answer {
	KMODSCAN_READY,     /* *out is the job's report; the caller frees it */
	KMODSCAN_PARKED,    /* the request waits; write no response, it will be served again */
	KMODSCAN_QUEUED,    /* a sweep: queued, or one already was */
	KMODSCAN_BUSY,      /* the queue is full: answer 503 with Retry-After */
	KMODSCAN_NO_RESULT, /* the job ran and produced no report: answer 500 */
};

typedef enum kmodscan_answer (*kmodscan_broker_fn)(const struct kmodscan_job *job,
                                                   struct kmodscan_report **out);
void kmodscan_set_broker(kmodscan_broker_fn fn);
enum kmodscan_answer kmodscan_ask(const struct kmodscan_job *job, struct kmodscan_report **out);

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
