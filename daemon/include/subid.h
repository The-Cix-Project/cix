#ifndef THINC_SUBID_H
#define THINC_SUBID_H

/*
 * ADR-0179 (issue #29): the subordinate-ID range allocator backing
 * user-namespaced containers. One flat, atomically-persisted table
 * maps an identity key -> a fixed-width host UID/GID range used as
 * that identity's containers' uid_map/gid_map base.
 *
 * The key is a string: the authenticated caller's own uidnumber
 * (rendered decimal) when host-auth resolves one, else the container's
 * own name -- see ADR-0179's "when host-auth is disabled" reasoning
 * (per-container isolation is unconditional; only the cross-container
 * same-operator consistency property needs an operator identity).
 *
 * Floor 100000 and width 65536 are the same real conventions Docker/
 * Podman/systemd already use -- see the ADR for why the floor is a
 * collision-avoidance property, not an aesthetic: a range must never
 * overlap a real LDAP uidnumber. subid_overlaps_uidnumber() is the
 * belt-and-suspenders active check in the other direction, for
 * ldap.c's own user create/update validation.
 */

#define SUBID_RANGE_LEN 65536LL
#define SUBID_FLOOR 100000LL

int subid_init(const char *state_path);
void subid_repoint(const char *new_state_path);

/*
 * Returns 0 and writes the (existing or freshly-committed) range base
 * for key; -1 on persist failure or table exhaustion. Idempotent per
 * key -- the first call allocates, every later call returns the same
 * base forever.
 */
int subid_lookup_or_assign(const char *key, long long *out_base);

/*
 * 1 if uidnumber falls inside any already-committed range -- ldap.c
 * rejects such a uidnumber for a user record (the symmetric half of
 * the allocator's own check against known uidnumbers at assign time).
 */
int subid_overlaps_uidnumber(long long uidnumber);

#endif
