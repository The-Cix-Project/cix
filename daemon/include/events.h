#ifndef CIX_EVENTS_H
#define CIX_EVENTS_H

#include <stddef.h>
#include <stdint.h>

#include "generated/events_slots.h"

/*
 * ADR-0339 (#599): the box tells its clients what changed.
 *
 * Every read the contract marks as announced (`x-cix-read` state,
 * sample or session) has a slot -- EV_<operationId>, generated from
 * docs/api/openapi.yaml -- and a generation counter. A change raises
 * the counters of the reads it makes stale, from one of two places:
 * dispatch(), after a change made through the API answers 2xx, which
 * raises its `x-cix-invalidates` list; and the daemon's own state
 * changes (a container exiting, a build changing state), which call
 * events_announce() with the slot they affect.
 *
 * Counters start again at every daemon start, so each is meaningful
 * only with the boot epoch: a random value chosen in events_init().
 */

enum events_read_kind {
	EVENTS_READ_STATE,
	EVENTS_READ_SAMPLE,
	EVENTS_READ_SESSION,
};

/* One announced read, from the generated table. */
struct events_read {
	const char *op_id;
	enum events_read_kind kind;
	int sample_period;           /* seconds; 0 unless kind is SAMPLE */
	const char *permission;      /* its x-cix-permission */
	const char *volatile_fields; /* "a,b": left out of the drift hash; "" for none */
	const char *path;            /* its path, "/v1/..." */
	int self_readable;           /* no path parameter: the daemon can run it itself */
};

/* A view and one read it is joined from (x-cix-composed-of): announcing
 * the component announces the view. */
struct events_composed {
	int view;
	int component;
};

/* One change, and the slots it makes stale (n == 0 for an explicit []). */
struct events_change {
	const char *op_id;
	const unsigned short *slots;
	int n;
};

/* Chooses the boot epoch. Called once, before anything announces. */
void events_init(void);

/* The epoch as sixteen hex digits and a NUL. */
const char *events_epoch(void);

/* Raises one read's generation. Out-of-range slots are ignored. */
void events_announce(int slot);

/*
 * Raises every read a change makes stale, by the change's operationId.
 * Returns the number of reads raised, or -1 when op_id is not a change
 * the contract declares (which dispatch() never passes).
 */
int events_announce_change(const char *op_id);

/* A read's slot by operationId, or -1 when it is not announced. */
int events_slot_of(const char *op_id);

/* A slot's current generation. */
uint64_t events_generation(int slot);

/* A slot's operationId, or NULL out of range. */
const char *events_op_id(int slot);

/* A slot's x-cix-permission -- what a session must hold to hear it. */
const char *events_permission(int slot);

/*
 * Called with the slot after every announcement, so the stream in
 * main.c can mark it pending on each connection that may hear it. One
 * listener; NULL removes it.
 */
void events_set_listener(void (*fn)(int slot));

/* A slot's table row, or NULL out of range. */
const struct events_read *events_read_at(int slot);

/*
 * ADR-0339 point 7: the daemon checks its own announcements.
 *
 * events_hash_body() hashes an answer's body -- canonically, as parsed
 * JSON, leaving out every key named in `volatile_fields` ("a,b") at any
 * depth -- so a field that moves on its own does not read as a change.
 * A body that is not JSON is hashed as bytes.
 *
 * events_observe() records what a read answered now. For a sample it
 * announces the read when the answer changed -- that is how a sample is
 * announced at all. For a state read it returns 1 when the answer
 * changed while its generation did not: a change nothing announced,
 * which is drift. The first observation of a read is its baseline and
 * is never drift. Returns 0 otherwise.
 */
uint64_t events_hash_body(const char *body, size_t len, const char *volatile_fields);
int events_observe(int slot, uint64_t hash);

/* Counts since start, for GET /v1/events/status. */
struct events_check_counts {
	unsigned long checked;    /* state reads compared */
	unsigned long drift;      /* of which changed with no announcement */
	unsigned long unreadable; /* reads whose last self-read produced no 200 */
};
/* Records whether a read could be run by the daemon itself, last time. */
void events_mark_readable(int slot, int readable);
struct events_check_counts events_check_counts(void);

#endif
