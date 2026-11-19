#ifndef CIX_FWACQUIRE_VERDICT_H
#define CIX_FWACQUIRE_VERDICT_H

/*
 * ADR-0381 release 3: what cixd does about one firmware file a loaded
 * driver declares and the host's firmware image does not hold.
 *
 * The question is asked of the IMAGE, never of the root: a name the root
 * still carries is acquired into the image too, so a later drop from the
 * root leaves no gap (the image's copy is searched first, and is what a
 * driver loads at its next probe).
 *
 * provided       an output's files selectors claim the name (the index)
 * ambiguous      two outputs claim it: refused, never picked (ADR-0324's
 *                rule for a name two sources offer)
 * policy_allows  the kmod policy would load the driver automatically
 *                (kmod_policy_verdict(), an automatic origin): a
 *                blacklisted module, or a host with autoload off,
 *                fetches nothing
 * installed      the output is installed in the firmware image
 * declared       the output is declared in the firmware image's
 *                manifest, so the drain installs it (or failed to: that
 *                is the drain's own state, never retried from here)
 * source_trusted the recipe's source has trust_keys: the owner's
 *                decision of 2026-11-19, "Trusted sources only"
 */
enum fwacquire_action {
	FWACQUIRE_NOTHING,     /* no output provides it */
	FWACQUIRE_DECLARE,     /* declare the output, rolling, and queue the image */
	FWACQUIRE_PENDING,     /* declared already: the drain installs it */
	FWACQUIRE_NOT_CARRIED, /* installed, and its selector claims a file it does not ship */
	FWACQUIRE_AMBIGUOUS,   /* two outputs claim it */
	FWACQUIRE_UNTRUSTED,   /* its source has no trust_keys: an operator installs it */
	FWACQUIRE_REFUSED      /* the kmod policy would not load the driver automatically */
};

struct fwacquire_facts {
	int provided;
	int ambiguous;
	int policy_allows;
	int installed;
	int declared;
	int source_trusted;
};

static inline enum fwacquire_action fwacquire_verdict(const struct fwacquire_facts *f)
{
	if (!f->provided)
		return FWACQUIRE_NOTHING;
	if (f->ambiguous)
		return FWACQUIRE_AMBIGUOUS;
	if (!f->policy_allows)
		return FWACQUIRE_REFUSED;
	if (f->installed)
		return FWACQUIRE_NOT_CARRIED;
	if (f->declared)
		return FWACQUIRE_PENDING;
	if (!f->source_trusted)
		return FWACQUIRE_UNTRUSTED;
	return FWACQUIRE_DECLARE;
}

#endif
