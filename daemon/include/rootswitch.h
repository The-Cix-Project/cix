#ifndef CIX_ROOTSWITCH_H
#define CIX_ROOTSWITCH_H

/*
 * ADR-0375 phase 4: switching the control plane's root in place.
 *
 * The running cixd (PID 1) mounts the staged slot's squashfs at
 * ROOTSWITCH_DIR, moves every mount that hangs off the running root into
 * it, pivots into it and execs the new root's cixd. The containers have
 * their own mount namespaces and roots and are not touched. These are
 * the steps; the order, the record and the boot entry are main.c's
 * (handover_exec()).
 *
 * Nothing here logs: from the first move on, the log store's path
 * resolves into the new root's read-only tree, so the caller says what
 * happened, on stdout, once it knows.
 */
#include <stddef.h>

/* An empty directory every root carries (mkbootroot stages it), so the
 * root that runs this code has a place to mount the next one. */
#define ROOTSWITCH_DIR "/run/nextroot"

#define ROOTSWITCH_MAX_MOUNTS 64

/* The mounts that hang directly off the running root: what a switch
 * moves. Each carries its own submounts with it. */
struct rootswitch_plan {
	int count;
	char points[ROOTSWITCH_MAX_MOUNTS][256];
};

/*
 * The plan, from mountinfo text (the format of /proc/self/mountinfo).
 * The root is the last mount whose mount point is "/" -- the one on top.
 * A mount point another child of the root also uses is refused: a move
 * would take only the top one and strand the other. 0, or -1 with err
 * written.
 */
int rootswitch_plan_parse(const char *mountinfo, struct rootswitch_plan *plan, char *err,
                          size_t err_size);

/* The same, from this process's own /proc/self/mountinfo. */
int rootswitch_plan_read(struct rootswitch_plan *plan, char *err, size_t err_size);

/*
 * Whether the tree at dir can take the plan: dir/bin/cixd is executable,
 * every planned mount point exists under dir, and, when release is not
 * NULL and this root has /lib/modules/<release>, dir has it too. 0, or
 * -1 with err written.
 */
int rootswitch_check_target(const char *dir, const struct rootswitch_plan *plan,
                            const char *release, char *err, size_t err_size);

/*
 * Moves every planned mount under dir (MS_MOVE). On a failure the moves
 * already made are moved back, so the namespace is as it was. 0, or -1
 * with err written.
 */
int rootswitch_move(const char *dir, const struct rootswitch_plan *plan, char *err,
                    size_t err_size);

/* Moves them back: the undo of a successful rootswitch_move(). */
void rootswitch_move_back(const char *dir, const struct rootswitch_plan *plan);

/*
 * Makes dir the root: pivot_root with the old root stacked beneath,
 * detached, and the working directory "/". Every process in this mount
 * namespace whose root or working directory was the old root follows
 * (pivot_root(2)). 0, or -1 with err written; pivot_root is atomic, so
 * a failure has changed nothing.
 */
int rootswitch_pivot(const char *dir, char *err, size_t err_size);

#endif
