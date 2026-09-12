#
# What leaves a registry entry in use with teardown_kind NONE after a
# DELETE returned 204? (#421)
#
# v2.57.123's gate failed with:
#
#   FAIL: POST durablehost round 2, status=409: a container with this
#         name already exists
#
# That message is name_conflict_msg()'s NONE branch, so at that moment
# registry_find("durablehost") returned an entry whose teardown_kind
# was REGISTRY_TEARDOWN_NONE. Reading every path in
# handle_delete() finds none that produces it:
#
#   running, no teardown   -> begin_container_stop() sets DELETE
#   running, teardown live -> the intent is upgraded, stays non-NONE
#   not running            -> registry_remove() synchronously, name free
#
# and teardown_kind is written in exactly two places (registry_create()
# NONE for a new entry, registry_begin_kill() the kind) and never
# cleared on exit -- the entry is removed instead.
#
# The one input nobody had is the DELETE's OWN OUTCOME. Both versions of
# the test discarded it, so every explanation had to ASSUME the request
# was delivered. cix_client_request() retries a transport failure three
# times 250ms apart and then returns -1 having produced no HTTP
# exchange at all -- and this test runs POST /v1/pki/reset, which forks
# openssl repeatedly on a single-threaded daemon. A full accept queue
# answers with RST (a fact this platform has already been bitten by),
# which is a transport failure, not an HTTP status. If that is what
# happened, the container was simply still there and "a container with
# this name already exists" was the correct answer to a correct
# question.
#
# So: run the real sequence, not a reconstruction -- the PKI reset only
# exists in the real one. test_pki now prints "durablehost: DELETE
# rc=N status=N" unconditionally and fails immediately when a DELETE
# does not land. The forked cixd's stderr is already interleaved into
# this log, so "<name>: deleting -- shutdown asked of cix-init" proves
# begin_container_stop() ran for an iteration and "<name>: exited
# (status N, signal M) after Ns" is the completion firing.
#
# HOW TO READ THE RESULT:
#
#   DELETE rc=-1                -> the request never landed. #421 is the
#                                  test discarding its outcome, and the
#                                  daemon is not involved at all.
#   DELETE status=204, then a
#   NONE-branch 409             -> a real daemon bug, and the stderr
#                                  ordering around it localises which
#                                  path left the entry behind.
#   five clean runs             -> not reproduced at this rate; widen
#                                  before concluding anything.
#
# Five runs because the failure showed once in three gate runs, and one
# run cannot tell "fixed" from "did not happen this time".
#
# Fails on purpose -- the build log is the product.
#
pkg_name="probe-delete-409"
pkg_version="1"
pkg_source="https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/PROBE_REF.tar.gz"
pkg_sha256="PROBE_SHA"
pkg_build_depends="bash coreutils make tcc linux-headers openssl gcc binutils sed grep gawk"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_build_image="cix-builder"
pkg_changelog="1: runs test_pki five times with the DELETE's own rc/status printed, to answer whether #421's 409 followed a DELETE that landed at all. The forked daemon's stderr is interleaved, so begin_container_stop and the exit completion are visible against each iteration."

pkg_build() {
	echo "=== building what test_pki needs ==="
	make build/cix-init build/cixd build/daemon_child build/test_pki 2>&1 | tail -3
	echo

	i=1
	while [ "$i" -le 5 ]; do
		echo "############ run $i ############"
		./build/test_pki > "/run/pki.$i.log" 2>&1
		rc=$?
		echo "exit status: $rc"
		echo "--- every DELETE outcome, and every daemon line about durablehost ---"
		grep -nE "DELETE rc=|durablehost|RESULT|FAIL" "/run/pki.$i.log" || echo "(nothing matched)"
		if [ "$rc" -ne 0 ]; then
			echo "--- this run FAILED: 60 lines of context around the first failure ---"
			grep -n "FAIL" "/run/pki.$i.log" | head -1
			first=$(grep -n "FAIL" "/run/pki.$i.log" | head -1 | cut -d: -f1)
			start=$((first - 40)); [ "$start" -lt 1 ] && start=1
			sed -n "${start},$((first + 20))p" "/run/pki.$i.log"
		fi
		echo
		i=$((i + 1))
	done

	echo "=== how many of the five failed ==="
	grep -l "PKI RESULT: FAIL" /run/pki.*.log 2>/dev/null | wc -l
	echo "=== every DELETE outcome across all five runs ==="
	grep -h "DELETE rc=" /run/pki.*.log 2>/dev/null | sort | uniq -c

	echo
	echo "PROBE COMPLETE -- failing deliberately so the log is kept"
	return 1
}

pkg_install() {
	:
}
