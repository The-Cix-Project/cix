#ifndef LDAPCLIENT_H
#define LDAPCLIENT_H

#include <stddef.h>

/*
 * ADR-0144: a minimal, hand-rolled LDAPv3 client -- simple bind plus
 * one-shot search -- for this daemon's own host-authentication LDAP
 * backend (daemon/src/hostauth.c). No libldap dependency, consistent
 * with this project's own established precedent of hand-rolling
 * protocols for the trusted daemon process itself rather than linking
 * external client libraries (rtnetlink instead of iproute2/libnl,
 * hand-rolled HTTP/WebSocket/JSON) -- cixd already speaks several
 * real wire protocols this way, LDAP's own BIND/SEARCH operations are
 * a small, well-specified (RFC 4511) subset to add one more.
 *
 * Deliberately narrow: only what a login flow needs -- a real simple
 * bind (RFC 4511 4.2), and a search (4511 4.5) whose only supported
 * filter shape is a top-level AND of one or more equality-match terms
 * (exactly "(&(uid=X)(memberOf=Y))", this module's one real use case
 * -- "does this exact user, once authenticated, also match this
 * group-membership term"). No SASL, no referrals, no paged results, no
 * generic filter grammar -- a real, audited general-purpose LDAP
 * client (OpenLDAP's own libldap) is what container-side PAM/NSS work
 * (ADR-0142 Section... ADR-0144's later container-LDAP parts) uses
 * instead, inside the isolated container runtime, not this trusted
 * daemon process.
 *
 * TLS: supported since #416, and this comment used to say it was not.
 * LDAPS only -- TLS from the first byte, on its own port -- never
 * StartTLS, which would need the extended-operation machinery this
 * module deliberately does not have, and which buys nothing when the
 * port is ours to choose. Both calls below take a CA trust bundle in
 * memory; NULL selects the plaintext path, unchanged. The anchor is
 * passed in rather than read here so this module keeps knowing nothing
 * about where trust comes from -- hostauth.c hands it the live chain
 * from pki_trust_bundle_pem(), because on this platform the daemon
 * verifying the directory IS the CA that issued its certificate.
 */

enum ldapclient_error {
	LDAPCLIENT_OK = 0,
	LDAPCLIENT_ERR_CONNECT,     /* couldn't reach host:port at all, or it timed out */
	LDAPCLIENT_ERR_PROTOCOL,    /* a real response came back malformed/unparseable */
	LDAPCLIENT_ERR_LDAP_RESULT, /* a well-formed LDAP error response (bad DN, invalid
	                              * credentials, insufficient access, ...) -- see
	                              * out_ldap_result_code for the real RFC 4511 resultCode */
};

/*
 * Real LDAPv3 simple bind (RFC 4511 4.2) against host:port: connects,
 * sends BindRequest{version=3, name=dn, authentication=simple(password)},
 * reads the real BindResponse, then unbinds and closes the connection
 * -- one full round trip, no connection reuse (this module's every
 * call is a fresh TCP connection; a login attempt is rare enough that
 * pooling would be premature). timeout_ms bounds the whole operation
 * (connect + write + read) via SO_SNDTIMEO/SO_RCVTIMEO.
 *
 * Returns LDAPCLIENT_OK only for a real resultCode 0 (success)
 * BindResponse. LDAPCLIENT_ERR_LDAP_RESULT (with *out_ldap_result_code
 * set to the real RFC 4511 value, e.g. 49 = invalidCredentials) for
 * any other well-formed response -- the caller's job to decide
 * whether that means "wrong password" or something else.
 *
 * ca_pem/ca_pem_len: a PEM trust bundle (one or more certificates) to
 * verify the server against, which turns this into an LDAPS bind. NULL
 * means plaintext. Verification is pinned to `host` as given -- by IP
 * SAN when it parses as an address, by DNS SAN otherwise -- so a
 * server reached by address needs an address SAN in its certificate.
 * A handshake or verification failure is LDAPCLIENT_ERR_CONNECT, the
 * same class as an unreachable server (the caller's next server is the
 * right response either way), with the OpenSSL text written to the log
 * store rather than only to stderr.
 */
enum ldapclient_error ldapclient_bind(const char *host, int port, const char *dn,
                                       const char *password, int timeout_ms, const char *ca_pem,
                                       size_t ca_pem_len, int *out_ldap_result_code);

/*
 * Binds as bind_dn/bind_password (same real BindRequest as
 * ldapclient_bind() above -- this module's only supported way to
 * authenticate a search), then, only on a successful bind, sends one
 * SearchRequest under base_dn (whole-subtree scope, RFC 4511 4.5.1's
 * wholeSubtree) with an AND filter built from attr_values -- an array
 * of {attr, value} equality-match pairs, attr_count long (e.g.
 * {"uid","alice"} and {"memberOf","ou=admins,ou=groups,dc=..."} to
 * ask "is alice a member of admins," this module's one real use
 * case). Reads every SearchResultEntry until SearchResultDone,
 * counting entries (attribute values themselves are never parsed out
 * -- the caller only needs "did anything match"), then unbinds.
 *
 * On LDAPCLIENT_OK, *out_match_count is the real number of entries
 * the filter matched (0 is a real, valid "no match," not an error).
 */
enum ldapclient_error ldapclient_bind_and_search(const char *host, int port, const char *bind_dn,
                                                  const char *bind_password, const char *base_dn,
                                                  const char *const attrs[][2], int attr_count,
                                                  int timeout_ms, const char *ca_pem,
                                                  size_t ca_pem_len, int *out_match_count,
                                                  int *out_ldap_result_code);

#endif /* LDAPCLIENT_H */
