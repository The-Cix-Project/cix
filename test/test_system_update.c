/*
 * Phase 16 part 1 (ADR-0031) fast test: proves POST /v1/system/update's
 * validation logic (do_system_update(), daemon/src/main.c) -- every
 * check that runs BEFORE the real device write (write_file_to_device()
 * onto ROOT_A_DEVICE/ROOT_B_DEVICE, real /dev/vdaN paths that only
 * exist under a real QEMU guest with a virtio-blk disk attached, or on
 * real hardware). None of that requires --init-mode or a real device:
 * kanxeod happily accepts --slot=a without --init-mode (boot_init(),
 * which mounts the ESP, only runs when --init-mode is also given), so
 * every 400 case here is reachable from a plain dev daemon on this
 * dev LXC. The real device-write/loader-entry success path is a
 * separate, genuinely QEMU-dependent test: test/test_boot_update.c.
 */
#include "httpclient.h"
#include "json.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#define TEST_PORT 7632
#define PORT_ARG "--port=7632"

static int wait_for_daemon(const struct kx_client *c, int max_attempts)
{
	int i;
	struct kx_response r;

	for (i = 0; i < max_attempts; i++) {
		if (kx_client_request(c, "GET", "/v1/health", NULL, &r) == 0) {
			kx_response_free(&r);
			return 0;
		}
		usleep(100000);
	}
	return -1;
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

static pid_t start_daemon(const char *extra_arg)
{
	pid_t pid;
	char *dargv[4];
	int argc = 0;

	dargv[argc++] = "build/kanxeod";
	dargv[argc++] = PORT_ARG;
	if (extra_arg != NULL)
		dargv[argc++] = (char *)extra_arg;
	dargv[argc] = NULL;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execve("build/kanxeod", dargv, environ);
		perror("execve build/kanxeod");
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
	struct kx_client client;
	struct kx_response r;
	int ok = 1;
	char not_squashfs_path[] = "/tmp/kanxeo_test_system_update_notsquashfs_XXXXXX";
	int fd;

	kx_client_init(&client, "127.0.0.1", TEST_PORT);

	/* 1. No --slot= at all -> the request is rejected outright, before
	 * even looking at the body -- there is no meaningful "inactive
	 * slot" for a plain dev/interactive daemon. */
	daemon_pid = start_daemon(NULL);
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon (no --slot=) never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/update", "{\"image_path\":\"/nonexistent\"}",
	                       &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: update with no --slot= expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon (no --slot=) did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	/* 2. --slot=a without --init-mode: g_slot is set (boot_init(), the
	 * only thing that would need a real device, never runs without
	 * --init-mode), so every check past the slot check itself is now
	 * reachable. */
	daemon_pid = start_daemon("--slot=a");
	if (daemon_pid < 0)
		return 1;
	if (wait_for_daemon(&client, 50) != 0) {
		fprintf(stderr, "FAIL: daemon (--slot=a) never accepted connections\n");
		kill(daemon_pid, SIGKILL);
		waitpid(daemon_pid, NULL, 0);
		return 1;
	}

	/* 2a. image_path missing from the body entirely */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/update", "{}", &r) != 0 || r.status != 400) {
		fprintf(stderr, "FAIL: update with missing image_path expected 400, got %d\n", r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2b. image_path pointing at a file that doesn't exist */
	memset(&r, 0, sizeof(r));
	if (kx_client_request(&client, "POST", "/v1/system/update",
	                       "{\"image_path\":\"/no/such/path/here\"}", &r) != 0 ||
	    r.status != 400) {
		fprintf(stderr, "FAIL: update with nonexistent image_path expected 400, got %d\n",
		        r.status);
		ok = 0;
	}
	kx_response_free(&r);

	/* 2c. image_path pointing at a real, readable file that is NOT a
	 * squashfs image -- rejected on magic before any device write is
	 * even attempted. */
	if (mkstemp(not_squashfs_path) < 0) {
		fprintf(stderr, "FAIL: mkstemp for not-squashfs fixture\n");
		ok = 0;
	} else {
		fd = open(not_squashfs_path, O_WRONLY | O_TRUNC);
		if (fd >= 0) {
			const char *content = "not a squashfs image, just plain text\n";

			write(fd, content, strlen(content));
			close(fd);
		}

		{
			char body[256];

			snprintf(body, sizeof(body), "{\"image_path\":\"%s\"}", not_squashfs_path);
			memset(&r, 0, sizeof(r));
			if (kx_client_request(&client, "POST", "/v1/system/update", body, &r) != 0 ||
			    r.status != 400) {
				fprintf(stderr, "FAIL: update with non-squashfs image_path expected 400, got %d\n",
				        r.status);
				ok = 0;
			} else {
				const char *err = json_str_field(r.json, "error");

				if (err == NULL || strstr(err, "squashfs") == NULL) {
					fprintf(stderr,
					        "FAIL: non-squashfs image_path error should mention squashfs, got: %s\n",
					        err != NULL ? err : "(null)");
					ok = 0;
				}
			}
			kx_response_free(&r);
		}
		unlink(not_squashfs_path);
	}

	if (stop_daemon(daemon_pid) != 0) {
		fprintf(stderr, "FAIL: daemon (--slot=a) did not exit cleanly on SIGTERM\n");
		ok = 0;
	}

	printf(ok ? "SYSTEM UPDATE RESULT: PASS\n" : "SYSTEM UPDATE RESULT: FAIL\n");
	return ok ? 0 : 1;
}
