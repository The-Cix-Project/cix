#include "credcheck.h"

#include "apiresp.h"
#include "ldapclient.h"
#include "logstore.h"
#include "pki.h"

#include <openssl/sha.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * A login's LDAP attempt: every configured server in order. *answered
 * is set to 1 iff some server gave an authoritative answer -- the bind
 * succeeded, or a reachable directory explicitly rejected it -- in
 * which case the return value IS that answer. Left at 0 when every
 * server was unreachable or erroring, so the login falls back to the
 * local record.
 */
static int try_ldap(const struct hostauth_ldap_plan *plan, const char *password, int *answered)
{
	char *ca_pem = NULL;
	size_t ca_pem_len = 0;
	int authenticated = 0;
	int i;

	*answered = 0;

	/*
	 * #416: the trust chain is read once per login, not once per server
	 * -- and here rather than at startup, because the CA can change under
	 * a running daemon (pki_ca_reset(), an intermediate bootstrap,
	 * ADR-0281's import) and a stale anchor fails as a refused connection
	 * against a perfectly correct certificate.
	 *
	 * No chain while ldap_tls is on means this does NOT bind at all: it
	 * returns with *answered still 0, the established "no server gave a
	 * usable answer" outcome, so the login falls back to the local
	 * record. It deliberately does not fall back to a plaintext bind --
	 * that would send the credential in clear on the strength of a
	 * configuration that asked for the opposite. hostauth_set_config()
	 * already refuses this combination; a CA reset under an
	 * already-saved config is how it can still be reached.
	 */
	if (plan->tls && (pki_trust_bundle_pem(&ca_pem, &ca_pem_len) != PKI_OK || ca_pem == NULL)) {
		logstore_write("hostauth", "error",
		               "ldap_tls is on but no CA trust chain is available -- not binding at "
		               "all rather than binding in clear; bootstrap the CA, or set "
		               "ldap_tls false if this directory really is plaintext");
		free(ca_pem);
		return 0;
	}

	for (i = 0; i < plan->server_count; i++) {
		int result_code = -1;
		enum ldapclient_error err =
		    ldapclient_bind(plan->servers[i], plan->port, plan->bind_dn, password,
		                    plan->timeout_ms, ca_pem, ca_pem_len, &result_code);

		if (err == LDAPCLIENT_OK) {
			*answered = 1;
			authenticated = 1;
			break;
		}
		if (err == LDAPCLIENT_ERR_LDAP_RESULT) {
			/* a real, well-formed rejection (invalidCredentials or
			 * similar) from a directory that IS reachable -- this is
			 * authoritative, not "try the next server" */
			*answered = 1;
			break;
		}
		/* LDAPCLIENT_ERR_CONNECT/PROTOCOL: this server didn't give a
		 * usable answer at all -- fall through and try the next one */
	}
	free(ca_pem);
	return authenticated;
}

/*
 * ADR-0144's order: the first LDAP server to answer authoritatively
 * decides; only when none could (or LDAP is off, or there is no local
 * record to build a bind DN from) is the local bcrypt record asked --
 * the same passbcrypt data either way, so the fallback changes
 * availability, never which password is correct.
 */
static int check_login(const char *user, const char *password)
{
	struct hostauth_ldap_plan plan;
	const struct ldap_user *u;

	if (hostauth_ldap_plan(user, &plan)) {
		int answered = 0;
		int ok = try_ldap(&plan, password, &answered);

		if (answered)
			return ok;
	}
	u = ldap_user_find(user);
	if (u == NULL || u->disabled || u->passbcrypt[0] == '\0')
		return 0;
	return pwhash_bcrypt_check(password, u->passbcrypt);
}

/*
 * ADR-0317 section 8: each of the user's app passwords, except those the
 * parent's memo already holds -- the parent asked because lookup said
 * UNKNOWN, and a memoized hash has already been compared against this
 * secret's digest there and did not match. Never the main password.
 */
static int check_app_password(const char *user, const char *secret, struct credcheck_result *out)
{
	const struct ldap_user *u = ldap_user_find(user);
	int i;

	if (u == NULL || u->disabled || secret[0] == '\0')
		return 0;
	for (i = 0; i < u->app_password_count; i++) {
		const struct ldap_app_password *ap = &u->app_passwords[i];

		if (ldap_app_password_memoized(ap->passbcrypt))
			continue;
		if (!pwhash_bcrypt_check(secret, ap->passbcrypt))
			continue;
		snprintf(out->app_name, sizeof(out->app_name), "%s", ap->name);
		snprintf(out->hash, sizeof(out->hash), "%s", ap->passbcrypt);
		return 1;
	}
	return 0;
}

/* #610: a hash for a password being set, or for a secret made here. */
static int make_hash(const char *secret, struct credcheck_result *out)
{
	return pwhash_bcrypt_new(secret, out->hash, sizeof(out->hash)) == 0;
}

void credcheck_work(const struct credcheck_request *rq, struct credcheck_result *out)
{
	memset(out, 0, sizeof(*out));
	logstore_hold(out->held, sizeof(out->held));
	switch (rq->kind) {
	case CREDCHECK_LOGIN:
		out->verified = check_login(rq->user, rq->secret);
		break;
	case CREDCHECK_APP_PASSWORD:
		out->verified = check_app_password(rq->user, rq->secret, out);
		break;
	case CREDCHECK_HASH:
		out->verified = rq->secret[0] != '\0' && make_hash(rq->secret, out);
		break;
	case CREDCHECK_NEW_SECRET:
		out->verified = ldap_generate_secret(out->secret) == 0 && make_hash(out->secret, out);
		break;
	}
}

void credcheck_bind(const struct credcheck_request *rq, unsigned char out[CREDCHECK_BIND_LEN])
{
	unsigned char buf[1 + sizeof(rq->user) + sizeof(rq->secret) + sizeof(rq->name)];
	size_t ulen = strlen(rq->user) + 1;   /* each NUL separates one field from the next */
	size_t slen = strlen(rq->secret) + 1;
	size_t nlen = strlen(rq->name);

	buf[0] = (unsigned char)rq->kind;
	memcpy(buf + 1, rq->user, ulen);
	memcpy(buf + 1 + ulen, rq->secret, slen);
	memcpy(buf + 1 + ulen + slen, rq->name, nlen);
	SHA256(buf, 1 + ulen + slen + nlen, out);
	explicit_bzero(buf, sizeof(buf));
}

static credcheck_broker_fn g_broker;

void credcheck_set_broker(credcheck_broker_fn fn)
{
	g_broker = fn;
}

/* The one way to the broker: every field bounded, the copy wiped after. */
static enum credcheck_answer ask(enum credcheck_kind kind, const char *user, const char *secret,
                                 const char *name, struct credcheck_out *out)
{
	struct credcheck_request rq;
	enum credcheck_answer a;

	if (user == NULL || secret == NULL || name == NULL || strlen(user) >= sizeof(rq.user) ||
	    strlen(secret) >= sizeof(rq.secret) || strlen(name) >= sizeof(rq.name))
		return CREDCHECK_REFUSED;
	if (g_broker == NULL)
		return CREDCHECK_BUSY;
	memset(&rq, 0, sizeof(rq));
	rq.kind = kind;
	snprintf(rq.user, sizeof(rq.user), "%s", user);
	snprintf(rq.secret, sizeof(rq.secret), "%s", secret);
	snprintf(rq.name, sizeof(rq.name), "%s", name);
	a = g_broker(&rq, out);
	explicit_bzero(rq.secret, sizeof(rq.secret));
	return a;
}

enum credcheck_answer credcheck_ask(enum credcheck_kind kind, const char *user, const char *secret)
{
	return ask(kind, user, secret, "", NULL);
}

enum credcheck_answer credcheck_hash(const char *password, char out_hash[PWHASH_BCRYPT_LEN + 1])
{
	struct credcheck_out out;
	enum credcheck_answer a;

	memset(&out, 0, sizeof(out));
	a = ask(CREDCHECK_HASH, "", password, "", &out);
	if (a == CREDCHECK_VERIFIED)
		memcpy(out_hash, out.hash, sizeof(out.hash));
	explicit_bzero(&out, sizeof(out));
	return a;
}

enum credcheck_answer credcheck_new_secret(const char *user, const char *app,
                                           char out_secret[LDAP_PROVISION_SECRET_LEN + 1],
                                           char out_hash[PWHASH_BCRYPT_LEN + 1])
{
	struct credcheck_out out;
	enum credcheck_answer a;

	memset(&out, 0, sizeof(out));
	a = ask(CREDCHECK_NEW_SECRET, user, "", app, &out);
	if (a == CREDCHECK_VERIFIED) {
		memcpy(out_secret, out.secret, sizeof(out.secret));
		memcpy(out_hash, out.hash, sizeof(out.hash));
	}
	explicit_bzero(&out, sizeof(out));
	return a;
}

void credcheck_respond_busy(int fd, int is_head)
{
	respond_error_hdrs(fd, 503, "Service Unavailable", "Retry-After: 1\r\n",
	                   "credential checks are busy -- try again shortly", is_head);
}
