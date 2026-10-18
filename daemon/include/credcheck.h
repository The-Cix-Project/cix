#ifndef CREDCHECK_H
#define CREDCHECK_H

/*
 * credcheck -- the one place a credential is compared (ADR-0340, #609).
 *
 * A password check is the most expensive thing a request does: one
 * bcrypt comparison at PWHASH_BCRYPT_COST is ~350 ms on 192.168.15.95
 * (pwhash.h), and a login with LDAP enabled can wait out three servers'
 * timeouts first. cixd is one event loop, so for as long as a check ran
 * inline every other request, container exit and console byte waited
 * behind it.
 *
 * So the comparison runs here and only here, in credcheck_work(), and
 * credcheck_work() runs only in a helper child (ADR-0278's helper_run()).
 * pwhash_bcrypt_check() and ldapclient_bind() are called from no other
 * file; test_blocking_waits counts the call sites so that stays true.
 *
 * The child changes nothing: it reads the user records and the LDAP
 * configuration it inherited at the fork, and reports a fixed-size
 * result over a pipe. Anything it logs is held in that result
 * (logstore_hold()) and written by the parent. Everything with an
 * effect -- the throttle, the app-password memo, a session -- is the
 * parent's.
 *
 * The request that needed the check is parked by whoever installed the
 * broker (main.c), and served again from the start when the check
 * finishes. On that second pass the same credcheck_ask() call returns
 * the verdict instead of parking.
 */
#include "hostauth.h"
#include "ldap.h"
#include "pwhash.h"

#include <stddef.h>

/* A login password or an app password longer than this is refused
 * without being checked. Basic credentials are already bounded well
 * below it by the header buffer that carries them. */
#define CREDCHECK_SECRET_MAX 512

/* What a check may log, held for the parent to write. One check logs at
 * most a few lines -- the CA-chain refusal, a TLS failure per server. */
#define CREDCHECK_HELD_LOG_MAX 2048

/* SHA-256: the size of the digest a verdict is bound to. */
#define CREDCHECK_BIND_LEN 32

enum credcheck_kind {
	CREDCHECK_LOGIN,        /* POST /v1/login: LDAP, then the local bcrypt record */
	CREDCHECK_APP_PASSWORD, /* HTTP Basic: the user's app passwords (ADR-0317 section 8) */
};

struct credcheck_request {
	enum credcheck_kind kind;
	char user[HOSTAUTH_USERNAME_MAX];
	char secret[CREDCHECK_SECRET_MAX];
};

struct credcheck_result {
	int verified;
	/* CREDCHECK_APP_PASSWORD only: which app password verified, and its
	 * stored hash, for the parent's memo (ldap_app_password_memoize()). */
	char app_name[LDAP_APP_PASSWORD_NAME_MAX];
	char app_hash[PWHASH_BCRYPT_LEN + 1];
	char held[CREDCHECK_HELD_LOG_MAX]; /* logstore_hold()'s buffer; logstore_replay() it */
};

/*
 * The check itself. HELPER CHILD ONLY: it installs logstore_hold() for
 * the rest of the process's life, which in the daemon itself would
 * silence the log store.
 */
void credcheck_work(const struct credcheck_request *rq, struct credcheck_result *out);

/* SHA-256 over the kind, the user and the secret: what a verdict
 * attached to a connection is bound to, so it cannot answer for any
 * other credential. */
void credcheck_bind(const struct credcheck_request *rq, unsigned char out[CREDCHECK_BIND_LEN]);

enum credcheck_answer {
	CREDCHECK_VERIFIED,
	CREDCHECK_REFUSED,
	CREDCHECK_PARKED, /* the request waits; write no response, it will be served again */
	CREDCHECK_BUSY,   /* no check can start: answer 503 with Retry-After */
};

/*
 * Asks for a verdict on the request being served. The broker is main.c's
 * (credcheck_set_broker()); it answers from the verdict the connection
 * already carries, or parks the request and starts a check. With no
 * broker installed nothing can check, and the answer is BUSY.
 *
 * A user or secret that does not fit a credcheck_request is REFUSED
 * here, unchecked.
 */
typedef enum credcheck_answer (*credcheck_broker_fn)(const struct credcheck_request *rq);
void credcheck_set_broker(credcheck_broker_fn fn);
enum credcheck_answer credcheck_ask(enum credcheck_kind kind, const char *user, const char *secret);

/*
 * The answer to BUSY, wherever it is reached: 503 with Retry-After: 1.
 * Deliberately not 429, which #547 gives a meaning of its own -- this
 * address has failed too often. A 503 here means the box is busy, says
 * nothing about the caller, and does not count as a failure.
 */
void credcheck_respond_busy(int fd, int is_head);

#endif /* CREDCHECK_H */
