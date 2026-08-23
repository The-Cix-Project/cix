#ifndef CPRESERVE_H
#define CPRESERVE_H

#include "json.h"

/*
 * Issue #86: a guaranteed reservation for the control plane.
 *
 * On this platform the REST daemon is not one management path among
 * several -- it is the only one. There is no SSH and no general shell
 * (ADR-0034), so a starved `thincd` is not a degraded box, it is a box
 * nobody can reach until someone walks to the hypervisor. That happened
 * for real on 192.168.15.95: four concurrent package builds oversub-
 * scribed a 2-CPU machine, the kernel stayed perfectly healthy (ping 0%
 * loss, 0.26ms) and the daemon simply stopped answering.
 *
 * Two of that issue's three parts were already done: nice -20 and
 * oom_score_adj -1000 (priority, the cheap 80%), and an aggregate
 * budget for builds (#85). This is the third and the structurally sound
 * one: rather than prioritising the daemon, BOUND EVERYTHING ELSE. Every
 * container and every build lives under one `thinc-workload` cgroup
 * whose ceiling is the machine minus this reservation, so what is left
 * over is not a hope, it is a kernel-enforced remainder.
 *
 * The reservation is expressed as what the CONTROL PLANE keeps, not as
 * what workloads may have: an operator reasons about "leave the daemon a
 * tenth of the box", and that reasoning stays correct when the box is
 * replaced by a bigger one. The workload ceiling is derived from live
 * host totals every time it is applied, so the same config means the
 * same thing on a 2-CPU VM and on the 32-core machine this is about to
 * be installed on.
 *
 * This module owns only the persisted numbers. main.c derives the
 * cgroup limits from them and applies them; src/cgroup.c owns the
 * kernel interaction. One source of truth per concern, as everywhere
 * else here.
 */

struct cpreserve_config {
	int enabled;
	/* Percent of total CPU capacity held back for the control plane
	 * (1..90). 10 on a 4-CPU box means workloads share 3.6 CPUs. */
	int cpu_percent;
	/* Bytes of RAM held back. Workloads get total minus this. */
	long long memory_bytes;
};

/* Loads from <data-dir>/control_plane_reservation.json, or installs the
 * defaults if there is no file yet. Never fails: an unreadable or
 * malformed file leaves the defaults in place rather than refusing to
 * boot, since this is a safety margin, not a correctness input. */
void cpreserve_init(const char *path);

const struct cpreserve_config *cpreserve_get(void);

/* Returns 0 on success, -1 if a value is out of range (the caller
 * answers 400). Persists immediately on success. */
int cpreserve_set(int enabled, int cpu_percent, long long memory_bytes);

void cpreserve_write_json(struct json_writer *w);

#endif /* CPRESERVE_H */
