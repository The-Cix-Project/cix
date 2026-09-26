/*
 * Issue #100: proof that the control plane stopped answering, written
 * by something that is not the control plane.
 *
 * The real incident this comes from: `cixd` on the production host
 * accepted TCP while answering nothing for minutes, then recovered on
 * its own, and the log store had NOTHING from inside the window --
 * because the loop that would record its own silence is the loop that
 * is silent. This test reproduces that shape honestly: SIGSTOP freezes
 * the daemon exactly as a wedge does (accepts still complete in the
 * kernel, nothing is served), and nothing test-only is compiled into
 * the daemon to make it happen.
 *
 * It now covers both questions the watchdog asks (#247). The original
 * one -- is the loop turning -- is answered from a heartbeat the loop
 * bumps, and it is the question that missed a real five-minute outage:
 * the loop kept turning and answered nobody, so the heartbeat stayed
 * fresh and nothing was recorded. The second -- is the daemon
 * ANSWERING -- is asked by the watchdog issuing a real health request
 * from a process that cannot itself be wedged, and until this test
 * existed that branch had never been observed to fire.
 *
 * What a SIGSTOP can and cannot prove, stated rather than implied: it
 * freezes the loop as well as the service, so it exercises the probe,
 * its two-failure threshold and both of its records -- but it cannot
 * reproduce the production shape, a loop still turning while serving
 * nobody. That would need test-only code inside the daemon, which this
 * file has always refused to add.
 */
#include "httpclient.h"
#include "json.h"
#include "test_image_fixture.h"

#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7650
#define PORT_ARG "--port=7650"
/*
 * The loop-stall threshold is 5s, but the service probe (#247) needs
 * two consecutive unanswered probes 5s apart before it reports -- so a
 * freeze has to outlast roughly 10s for BOTH branches to be exercised.
 * A probe that connects and gets nothing back also burns its own 4s
 * timeout, which pushes the second failure to about 9s in. Fourteen
 * gives that real margin without making the test slow.
 */
#define FREEZE_SECONDS 14

static char g_data_dir[PATH_MAX];

/*
 * How many entries of GET /v1/pipeline/approvals name `target`, across
 * both pending and granted (#382). Returns -1 if the request itself
 * failed, so "the endpoint broke" can never be read as "nothing is
 * waiting".
 */
static int approvals_mention(const struct cix_client *c, const char *target)
{
	struct cix_response r;
	int count = 0;
	size_t i;
	static const char *const arrays[] = { "pending", "granted" };
	size_t a;

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, "GET", "/v1/pipeline/approvals", NULL, &r) != 0 || r.status != 200 ||
	    r.json == NULL) {
		fprintf(stderr, "FAIL: GET /v1/pipeline/approvals, status=%d\n", r.status);
		cix_response_free(&r);
		return -1;
	}
	for (a = 0; a < sizeof(arrays) / sizeof(arrays[0]); a++) {
		const struct json_value *arr = json_object_get(r.json, arrays[a]);

		if (arr == NULL || arr->type != JSON_ARRAY)
			continue;
		for (i = 0; i < arr->u.array.count; i++) {
			const char *t = json_as_string(json_object_get(arr->u.array.items[i], "target"));

			if (t != NULL && strcmp(t, target) == 0)
				count++;
		}
	}
	cix_response_free(&r);
	return count;
}

static int wait_for_daemon(const struct cix_client *c, int max_attempts)
{
	int i;
	struct cix_response r;

	for (i = 0; i < max_attempts; i++) {
		if (cix_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			cix_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static pid_t start_daemon(void)
{
	pid_t pid;
	char *dargv[4];
	static char data_dir_arg[PATH_MAX + 11];

	snprintf(data_dir_arg, sizeof(data_dir_arg), "--data-dir=%s", g_data_dir);
	dargv[0] = "build/cixd";
	dargv[1] = PORT_ARG;
	dargv[2] = data_dir_arg;
	dargv[3] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/cixd", dargv, environ);
		perror("execve build/cixd");
		_exit(127);
	}
	return pid;
}

static int stop_daemon(pid_t pid)
{
	int status;

	kill(pid, SIGTERM);
	if (waitpid(pid, &status, 0) != pid)
		return -1;
	return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

int main(void)
{
	pid_t daemon_pid;
	struct cix_client client;
	struct cix_response r;
	int ok = 1;

	if (test_data_dir_create(g_data_dir, sizeof(g_data_dir)) != 0)
		return 1;

	daemon_pid = start_daemon();
	if (daemon_pid < 0) {
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	cix_client_init(&client, "127.0.0.1", TEST_PORT);
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}

	/* 1. A daemon that is merely idle is not a stalled one -- the loop
	 * ticks on its own so silence means something. */
	sleep(2);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET /v1/system/stalls, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");

		if (stalls == NULL || stalls->type != JSON_ARRAY) {
			fprintf(stderr, "FAIL: stalls is not an array\n");
			ok = 0;
		} else if (stalls->u.array.count != 0) {
			fprintf(stderr, "FAIL: an idle daemon reported %d stall(s)\n",
			        (int)stalls->u.array.count);
			ok = 0;
		}
	}
	cix_response_free(&r);

	/* 2. Freeze it. This is the real thing, not a simulation: the
	 * process stops running, the kernel keeps completing TCP handshakes
	 * on its listening socket, and nothing is served. */
	kill(daemon_pid, SIGSTOP);
	sleep(FREEZE_SECONDS);
	kill(daemon_pid, SIGCONT);
	sleep(2);

	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls after the freeze, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");
		int saw_stall = 0, saw_recovered = 0;
		int saw_service_stall = 0, saw_service_recovered = 0;
		size_t i;

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: the freeze left no record -- which is the whole bug\n");
			ok = 0;
		} else {
			for (i = 0; i < stalls->u.array.count; i++) {
				const struct json_value *s = stalls->u.array.items[i];
				const char *event = json_as_string(json_object_get(s, "event"));
				const char *wchan = json_as_string(json_object_get(s, "wchan"));
				long long seconds = (long long)json_as_number(json_object_get(s, "seconds"));

				if (event == NULL)
					continue;
				if (strcmp(event, "stall") == 0) {
					saw_stall = 1;
					if (seconds < 5) {
						fprintf(stderr, "FAIL: stall recorded at %llds, below the threshold\n",
						        seconds);
						ok = 0;
					}
					/* The kernel's own view of where it was: the single
					 * most useful fact about a wedge, and the one thing
					 * unavailable from inside it. */
					if (wchan == NULL || wchan[0] == '\0') {
						fprintf(stderr, "FAIL: stall record carries no wchan\n");
						ok = 0;
					}
					/*
					 * #399: and WHICH child, when the wchan is a wait.
					 *
					 * A wchan of "do_wait" says the daemon is blocked in
					 * waitpid()/waitid() and nothing more. Three stalls
					 * on 192.168.15.95 on 2026-09-10/11 carried exactly
					 * that and no way to tell them apart; narrowing the
					 * 366-second one took reading every blocking-wait
					 * site in the daemon and correlating against the log
					 * store, because the process being waited on was
					 * named nowhere.
					 *
					 * Asserted as PRESENT, not non-empty: an empty array
					 * is a real and useful answer (the wait is on a child
					 * that has already gone), and a stall this test
					 * induces need not have children at all. What must
					 * never happen again is the field being absent, which
					 * is the state that cost the investigation.
					 */
					{
						const struct json_value *kids = json_object_get(s, "children");

						if (kids == NULL || kids->type != JSON_ARRAY) {
							fprintf(stderr,
							        "FAIL: stall record does not say which children the "
							        "daemon has -- a bare \"do_wait\" names no process\n");
							ok = 0;
						}
					}
				}
				if (strcmp(event, "recovered") == 0)
					saw_recovered = 1;
				/*
				 * #247: the loop-stall records above say the loop
				 * stopped turning. These say something a client
				 * actually cares about -- that nothing was being
				 * answered -- and they come from a watchdog probe
				 * rather than from any state the frozen daemon
				 * maintains about itself.
				 */
				if (strcmp(event, "service-stall") == 0)
					saw_service_stall = 1;
				if (strcmp(event, "service-recovered") == 0)
					saw_service_recovered = 1;
			}
			if (!saw_stall) {
				fprintf(stderr, "FAIL: no \"stall\" record after an %d-second freeze\n",
				        FREEZE_SECONDS);
				ok = 0;
			}
			if (!saw_recovered) {
				fprintf(stderr, "FAIL: no \"recovered\" record -- a stall with no end is only "
				                "half the story\n");
				ok = 0;
			}
			/*
			 * The branch #247 was filed for. A daemon that answers
			 * nothing has to leave a record written by something
			 * that is not the daemon -- and until this test existed,
			 * that branch had never once been observed to fire.
			 *
			 * Honest about what a SIGSTOP proves and what it does
			 * not: it freezes the loop as well, so it exercises the
			 * probe, its threshold, and both records, but it cannot
			 * reproduce the shape that actually happened in
			 * production -- a loop still turning while serving
			 * nobody. Producing that would need test-only code
			 * inside the daemon, which this file has always refused
			 * to add.
			 */
			if (!saw_service_stall) {
				fprintf(stderr,
				        "FAIL: no \"service-stall\" record after a %d-second freeze -- the "
				        "watchdog never noticed the daemon was answering nothing (#247)\n",
				        FREEZE_SECONDS);
				ok = 0;
			}
			if (!saw_service_recovered) {
				fprintf(stderr, "FAIL: no \"service-recovered\" record -- the probe never "
				                "reported the daemon coming back (#247)\n");
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/* 3. And the daemon is fine afterwards -- the watchdog observes,
	 * it does not interfere. */
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/health", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: daemon not healthy after the freeze, status=%d\n", r.status);
		ok = 0;
	}
	cix_response_free(&r);

	stop_daemon(daemon_pid);

	/*
	 * 4. The record outlives the daemon that stalled. A wedge is
	 * usually followed by a restart (or a reboot), and a diagnostic
	 * that dies with the process it was diagnosing is no diagnostic.
	 */
	daemon_pid = start_daemon();
	if (daemon_pid < 0 || wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not come back\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls", NULL, &r) != 0 || r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls after restart, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: the stall record did not survive a restart\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/*
	 * 5. The report keeps the NEWEST records, not the oldest.
	 *
	 * It used to keep the oldest: the reader filled a fixed array from
	 * the front of its read window and stopped, so once the file held
	 * more records than the array, every newer one became unreachable
	 * and the endpoint went on answering 200 with a plausible array of
	 * stale records. On the production box that presented as a wedge
	 * leaving no trace -- the trace was on disk the whole time.
	 *
	 * Written straight into the record file rather than by provoking
	 * hundreds of real stalls: the bug is entirely in the read path,
	 * and this exercises exactly that path over exactly the shape that
	 * broke it.
	 */
	{
		char path[PATH_MAX];
		FILE *rec;
		int i;
		const int written = 400;   /* comfortably past the reader's ring */

		snprintf(path, sizeof(path), "%s/state/control_plane_stalls.jsonl",
		         g_data_dir);
		rec = fopen(path, "a");
		if (rec == NULL) {
			fprintf(stderr, "FAIL: cannot append to %s\n", path);
			ok = 0;
		} else {
			for (i = 0; i < written; i++)
				fprintf(rec, "{\"ts\":%d,\"event\":\"stall\",\"seconds\":5,"
				             "\"state\":\"S\",\"wchan\":\"synthetic\","
				             "\"activity\":\"\"}\n", 1000000 + i);
			fclose(rec);
		}
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls?limit=1000", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls with a full record file, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");
		const struct json_value *newest;
		const struct json_value *ts;

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: no stall records returned\n");
			ok = 0;
		} else {
			newest = stalls->u.array.items[0];
			ts = newest != NULL ? json_object_get(newest, "ts") : NULL;
			if (ts == NULL || ts->type != JSON_NUMBER ||
			    (long)ts->u.number != 1000000 + 399) {
				fprintf(stderr, "FAIL: newest record is ts=%ld, want %d "
				                "(the reader is keeping the oldest)\n",
				        ts != NULL && ts->type == JSON_NUMBER ?
				                (long)ts->u.number : -1L, 1000000 + 399);
				ok = 0;
			}
		}
	}
	cix_response_free(&r);

	/*
	 * Issue #229: a NUL byte in the record file must not hide every
	 * record after it.
	 *
	 * The reader split the buffer with strtok(), which treats it as a
	 * C string, so the first NUL ended the scan -- permanently and
	 * silently. The report froze at one timestamp while the file went
	 * on growing. Measured on 192.168.15.95: newest record stuck at
	 * 2026-09-01 15:08:03 for four days, with the writer logging its
	 * own appends to the same dev/inode at a rising size the whole
	 * time, and none of the event types it had started emitting since
	 * ever appearing.
	 *
	 * A zero run is the expected damage rather than an exotic one:
	 * this file is appended to by a watchdog whose entire purpose is
	 * to be running when the machine is about to be reset by hand,
	 * and a reset mid-append leaves the size updated with the block
	 * still zeroes.
	 *
	 * Written straight into the file for the same reason the #284
	 * case above is: the bug is entirely in the read path.
	 */
	{
		char path[PATH_MAX];
		FILE *rec;
		int i;
		static const char zeros[64] = { 0 };

		snprintf(path, sizeof(path), "%s/state/control_plane_stalls.jsonl", g_data_dir);
		rec = fopen(path, "a");
		if (rec == NULL) {
			fprintf(stderr, "FAIL: cannot append to %s (#229)\n", path);
			ok = 0;
		} else {
			fwrite(zeros, 1, sizeof(zeros), rec);
			fputc('\n', rec);
			for (i = 0; i < 5; i++)
				fprintf(rec, "{\"ts\":%d,\"event\":\"slow-pass\","
				             "\"worst_pass_ms\":15818,\"slow_passes\":%d,"
				             "\"activity\":\"after the zero run\"}\n",
				        2000000 + i, i + 1);
			fclose(rec);
		}
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/stalls?limit=1000", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET stalls across a NUL run, status=%d (#229)\n", r.status);
		ok = 0;
	} else {
		const struct json_value *stalls = json_object_get(r.json, "stalls");
		const struct json_value *store = json_object_get(r.json, "store");
		const struct json_value *newest;
		const struct json_value *ts;
		const struct json_value *nul;

		if (stalls == NULL || stalls->type != JSON_ARRAY || stalls->u.array.count == 0) {
			fprintf(stderr, "FAIL: no records returned across a NUL run (#229)\n");
			ok = 0;
		} else {
			newest = stalls->u.array.items[0];
			ts = newest != NULL ? json_object_get(newest, "ts") : NULL;
			if (ts == NULL || ts->type != JSON_NUMBER || (long)ts->u.number != 2000004) {
				fprintf(stderr,
				        "FAIL: newest record across a NUL run is ts=%ld, want 2000004 -- "
				        "the reader stopped at the zero byte (#229)\n",
				        ts != NULL && ts->type == JSON_NUMBER ? (long)ts->u.number : -1L);
				ok = 0;
			}
		}
		/* Surviving the damage is not the same as reporting it: the
		 * zero run means records really were lost, and that must be
		 * visible rather than inferred from a gap in timestamps. */
		nul = store != NULL ? json_object_get(store, "first_nul_offset") : NULL;
		if (nul == NULL || nul->type != JSON_NUMBER || (long)nul->u.number < 0) {
			fprintf(stderr, "FAIL: store.first_nul_offset does not report the zero run (#229)\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	/*
	 * ADR-0272: the pipeline run store's contract.
	 *
	 * Here rather than in test_pkg, which exercises real installs and
	 * is not in SELFTESTS -- a gate in a binary the release never runs
	 * is not a gate. This asserts the shape and the retention setting,
	 * which need no package; that a real install actually APPENDS a run
	 * is exercised by test_pkg and verified on a real host, since
	 * producing one here would mean building a package.
	 */
	{
		const struct json_value *runs, *ret;

		/* An empty store answers with an empty list and its default,
		 * not with an error and not with a null -- a caller rendering
		 * a history must not have to special-case "never ran". */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pipeline/runs", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/pipeline/runs, status=%d\n", r.status);
			ok = 0;
		} else {
			runs = json_object_get(r.json, "runs");
			ret = json_object_get(r.json, "retention");
			if (runs == NULL || runs->type != JSON_ARRAY) {
				fprintf(stderr, "FAIL: runs is not an array on an empty store\n");
				ok = 0;
			}
			if (ret == NULL || ret->type != JSON_NUMBER || (int)ret->u.number != 1000) {
				fprintf(stderr, "FAIL: default run retention is not 1000\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* Out of range changes NOTHING. Asserted by reading the value
		 * back rather than by trusting the 400: a handler that
		 * validates after applying returns the same 400 and has
		 * already done the damage. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pipeline-config",
		                        "{\"run_retention\":0}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: run_retention 0 expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/pipeline-config", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/system/pipeline-config, status=%d\n", r.status);
			ok = 0;
		} else {
			ret = json_object_get(r.json, "run_retention");
			if (ret == NULL || ret->type != JSON_NUMBER || (int)ret->u.number != 1000) {
				fprintf(stderr, "FAIL: a refused retention was applied anyway\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* A real change is accepted and reported back in the same
		 * response, so a caller never has to read it again to know
		 * what it now is. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pipeline-config",
		                        "{\"run_retention\":50}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT run_retention 50, status=%d\n", r.status);
			ok = 0;
		} else {
			ret = json_object_get(r.json, "run_retention");
			if (ret == NULL || ret->type != JSON_NUMBER || (int)ret->u.number != 50) {
				fprintf(stderr, "FAIL: the change was not reported back\n");
				ok = 0;
			}
		}
		cix_response_free(&r);
	}

	/*
	 * ADR-0273: the gates.
	 *
	 * The load-bearing assertion is the FIRST one -- all three off by
	 * default. Everything this platform does today runs through those
	 * three chokepoints, so a gate that defaulted on would stop every
	 * existing caller, including this project's own deploy path.
	 */
	{
		const struct json_value *v;
		int i;
		static const char *const keys[] = { "gate_publish", "gate_roll", "gate_deploy" };

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/pipeline-config", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET pipeline-config for gates, status=%d\n", r.status);
			ok = 0;
		} else {
			for (i = 0; i < 3; i++) {
				v = json_object_get(r.json, keys[i]);
				if (v == NULL || v->type != JSON_BOOL || v->u.boolean) {
					fprintf(stderr, "FAIL: %s is not false by default\n", keys[i]);
					ok = 0;
				}
			}
		}
		cix_response_free(&r);

		/* Nothing is waiting when every gate is off -- pending is
		 * derived from the gates and the queues, so an off gate holds
		 * nothing by construction rather than by filtering. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/pipeline/approvals", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: GET /v1/pipeline/approvals, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *p = json_object_get(r.json, "pending");
			const struct json_value *g2 = json_object_get(r.json, "granted");

			if (p == NULL || p->type != JSON_ARRAY || p->u.array.count != 0 ||
			    g2 == NULL || g2->type != JSON_ARRAY) {
				fprintf(stderr, "FAIL: something is pending with every gate off\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		/* You cannot approve what nothing is holding. Asserted with the
		 * gate OFF and again with it ON but nothing queued, because
		 * those are two different refusals and only the second one
		 * proves the queue is actually consulted. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pipeline/approve",
		                        "{\"gate\":\"roll\",\"target\":\"nosuchimage\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr, "FAIL: approve with the gate off expected 409, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "PUT", "/v1/system/pipeline-config",
		                        "{\"gate_roll\":true}", &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: PUT gate_roll true, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pipeline/approve",
		                        "{\"gate\":\"roll\",\"target\":\"nosuchimage\"}", &r) != 0 ||
		    r.status != 409) {
			fprintf(stderr,
			        "FAIL: approve of an unqueued image expected 409, got %d -- the queue is "
			        "not being consulted\n",
			        r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* An unknown gate is a 400, not a 409: the request is
		 * malformed rather than untimely. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pipeline/approve",
		                        "{\"gate\":\"nosuchgate\",\"target\":\"x\"}", &r) != 0 ||
		    r.status != 400) {
			fprintf(stderr, "FAIL: an unknown gate expected 400, got %d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
	}

	/*
	 * #382: deleting an image takes its queue entry and its approval
	 * with it.
	 *
	 * The leak this gates was seen on a real host: an image deleted
	 * while it held a roll grant left the grant in
	 * GET /v1/pipeline/approvals forever, naming a target that no
	 * longer existed, with nothing able to clear it.
	 *
	 * The roll gate is what makes this runnable here. A queued image
	 * that has not been approved is HELD, so the drain skips it and no
	 * build, and therefore no container, is ever created -- which is
	 * the whole reason this assertion can live in a SELFTESTS member
	 * (see #224) rather than only on the box.
	 *
	 * What this does NOT cover, stated rather than implied: the other
	 * half of #382, where the drain started the same image once per
	 * free chain slot and filled all ten with one job. Reproducing
	 * that needs a build that really stays in flight across drain
	 * passes, which needs a build container. It is verified on a real
	 * host instead, by watching active_jobs while a rolling rebuild
	 * converges.
	 */
	{
		struct json_writer w;
		/* Modelled on test_pkg_recipe_approval.c's own template -- a
		 * real, publishable recipe, in CPDL since cix#516. It is never
		 * built here: the roll gate holds its image before any build
		 * starts, so what this needs from the recipe is that the daemon
		 * accepts and records it. */
		char recipe[1024];
		int pending_before = 0, pending_after = 0;

		snprintf(recipe, sizeof(recipe),
		         "package \"forgetpkg\" {\n"
		         "    version \"1.0\"\n"
		         "    release 1\n"
		         "    format \"cixpkg\"\n"
		         "\n"
		         "    sources {\n"
		         "        main \"forgetpkg\" {\n"
		         "            url \"https://example.invalid/forgetpkg-1.0.tar.gz\"\n"
		         "            sha256 \"%064d\"\n"
		         "        }\n"
		         "    }\n"
		         "\n"
		         "    requires {\n"
		         "        build {\n"
		         "            tool \"bash\"\n"
		         "            tool \"coreutils\"\n"
		         "        }\n"
		         "    }\n"
		         "\n"
		         "    build {\n"
		         "        run \"true\" {\n"
		         "        }\n"
		         "    }\n"
		         "\n"
		         "    install {\n"
		         "        mkdir \"${dest}/usr/share/forgetpkg\"\n"
		         "    }\n"
		         "}\n",
		         0);

		/* The gate is still on from the block above -- assert that
		 * rather than assume it, since a reordering of this file
		 * would otherwise turn this into a test of nothing. */
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "GET", "/v1/system/pipeline-config", NULL, &r) != 0 ||
		    r.status != 200) {
			fprintf(stderr, "FAIL: #382 setup: GET pipeline-config, status=%d\n", r.status);
			ok = 0;
		} else {
			const struct json_value *gr = json_object_get(r.json, "gate_roll");

			if (gr == NULL || gr->type != JSON_BOOL || !gr->u.boolean) {
				fprintf(stderr, "FAIL: #382 setup needs gate_roll on, and it is not\n");
				ok = 0;
			}
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/images", "{\"name\":\"forgetimg\"}", &r) !=
		        0 ||
		    r.status != 201) {
			fprintf(stderr, "FAIL: POST forgetimg image, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/images/forgetimg/manifest",
		                        "{\"package\":\"forgetpkg\",\"mode\":\"rolling\",\"version\":\"1.0-1\"}",
		                        &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: POST forgetimg manifest, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		/* Publishing is what queues every image tracking the package
		 * rolling (ADR-0107), so the manifest has to exist first. */
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, "forgetpkg");
		jw_key(&w, "content");
		jw_str(&w, recipe);
		jw_key(&w, "format");
		jw_str(&w, "cbs");
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "POST", "/v1/pkg/recipes", w.buf, &r) != 0 ||
		    (r.status != 201 && r.status != 204)) {
			fprintf(stderr, "FAIL: POST forgetpkg recipe, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);
		jw_free(&w);

		pending_before = approvals_mention(&client, "forgetimg");
		if (pending_before != 1) {
			fprintf(stderr,
			        "FAIL: a queued, unapproved image should be pending on the roll gate, "
			        "got %d\n",
			        pending_before);
			ok = 0;
		}

		memset(&r, 0, sizeof(r));
		if (cix_client_request(&client, "DELETE", "/v1/images/forgetimg", NULL, &r) != 0 ||
		    r.status != 204) {
			fprintf(stderr, "FAIL: DELETE forgetimg, status=%d\n", r.status);
			ok = 0;
		}
		cix_response_free(&r);

		pending_after = approvals_mention(&client, "forgetimg");
		if (pending_after != 0) {
			fprintf(stderr,
			        "FAIL: #382 -- a deleted image is still named by "
			        "/v1/pipeline/approvals\n");
			ok = 0;
		}
	}

	stop_daemon(daemon_pid);

	/*
	 * And it survives the restart. A setting that silently reverts on
	 * the next boot is worse than one that cannot be changed: nothing
	 * reports the reversion, so the store quietly returns to a default
	 * nobody chose. This is why the runs file is an object carrying its
	 * own retention rather than a bare array of runs.
	 */
	daemon_pid = start_daemon();
	if (daemon_pid < 0 || wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon did not come back for the run-store check\n");
		test_data_dir_cleanup(g_data_dir);
		return 1;
	}
	memset(&r, 0, sizeof(r));
	if (cix_client_request(&client, "GET", "/v1/system/pipeline-config", NULL, &r) != 0 ||
	    r.status != 200) {
		fprintf(stderr, "FAIL: GET pipeline-config after restart, status=%d\n", r.status);
		ok = 0;
	} else {
		const struct json_value *ret = json_object_get(r.json, "run_retention");
		const struct json_value *gr = json_object_get(r.json, "gate_roll");

		if (ret == NULL || ret->type != JSON_NUMBER || (int)ret->u.number != 50) {
			fprintf(stderr, "FAIL: run retention did not survive a restart\n");
			ok = 0;
		}
		/* ADR-0273: a gate that reverted to off on the next boot would
		 * quietly release exactly the change someone decided to stop,
		 * and nothing would report it. */
		if (gr == NULL || gr->type != JSON_BOOL || !gr->u.boolean) {
			fprintf(stderr, "FAIL: gate_roll did not survive a restart\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("STALLWATCH RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
