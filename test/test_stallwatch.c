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

		if (ret == NULL || ret->type != JSON_NUMBER || (int)ret->u.number != 50) {
			fprintf(stderr, "FAIL: run retention did not survive a restart\n");
			ok = 0;
		}
	}
	cix_response_free(&r);

	stop_daemon(daemon_pid);
	test_data_dir_cleanup(g_data_dir);

	printf("STALLWATCH RESULT: %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
