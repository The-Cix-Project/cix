/*
 * ADR-0375 phase 2: the handover record, without a daemon.
 *
 *   - what is written is what is read, field for field;
 *   - a record with a required key missing, or of a newer format, is
 *     refused;
 *   - the kernel checks: a live pid in its cgroup with real descriptors
 *     passes and gets FD_CLOEXEC back; a dead pid, a pid outside its
 *     cgroup, and a regular file posing as cix-init's socket are each
 *     refused.
 *
 * The cgroup root is a scratch directory holding the one cgroup.procs
 * file the check reads, so this runs anywhere a pidfd can be opened.
 */
#include "handover.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_fail;

#define CHECK(cond, ...)                                                                           \
	do {                                                                                       \
		if (!(cond)) {                                                                     \
			fprintf(stderr, "FAIL: " __VA_ARGS__);                                     \
			fputc('\n', stderr);                                                       \
			g_fail = 1;                                                                \
		}                                                                                  \
	} while (0)

static void write_text(const char *path, const char *text)
{
	FILE *f = fopen(path, "w");

	if (f == NULL) {
		fprintf(stderr, "FAIL: cannot write %s: %s\n", path, strerror(errno));
		g_fail = 1;
		return;
	}
	fputs(text, f);
	fclose(f);
}

/* A record of two containers: "alive", running with descriptors, and
 * "gone", exited with a status. */
static void fill(struct handover_record *rec, pid_t pid, int ctl, int rep, int svc_out, int cap)
{
	struct handover_container *a = &rec->containers[0], *g = &rec->containers[1];

	snprintf(rec->written_by, sizeof(rec->written_by), "0.2.57-test");
	rec->written_at = 1793900000;
	snprintf(rec->slot, sizeof(rec->slot), "a");
	snprintf(rec->bind_addr, sizeof(rec->bind_addr), "192.168.15.95");
	snprintf(rec->uplink_unattached, sizeof(rec->uplink_unattached), "eth1");
	rec->procfuse_pid = 4242;
	rec->procfuse_load_fd = 9;
	rec->console_count = 2;
	snprintf(rec->consoles[0].tty, sizeof(rec->consoles[0].tty), "/dev/tty0");
	rec->consoles[0].pid = 0;
	snprintf(rec->consoles[1].tty, sizeof(rec->consoles[1].tty), "/dev/ttyS0");
	rec->consoles[1].pid = 4343;
	rec->count = 2;

	snprintf(a->name, sizeof(a->name), "alive");
	snprintf(a->image, sizeof(a->image), "base");
	snprintf(a->image_version, sizeof(a->image_version), "%064d", 7);
	a->running = 1;
	a->pid = pid;
	a->started_at = 1793899000;
	a->userns_enabled = 1;
	a->disk_quota_bytes = 1048576;
	a->capture_requested = 1;
	a->procfuse_expected = 10;
	a->init_up = 1;
	a->ready = 1;
	a->net_count = 1;
	snprintf(a->nets[0].name, sizeof(a->nets[0].name), "statsnet");
	a->nets[0].ip_be = htonl(0xac280005); /* 172.40.0.5 */
	snprintf(a->nets[0].ifname, sizeof(a->nets[0].ifname), "eth0");
	a->file_count = 1;
	snprintf(a->file_paths[0], sizeof(a->file_paths[0]), "/etc/app.conf");
	a->dns_server_count = 1;
	snprintf(a->dns_servers[0], sizeof(a->dns_servers[0]), "172.40.0.1");
	a->device_count = 1;
	snprintf(a->devices[0].id, sizeof(a->devices[0].id), "char:1:3");
	snprintf(a->devices[0].dev_path, sizeof(a->devices[0].dev_path), "/dev/null");
	a->devices[0].major = 1;
	a->devices[0].minor = 3;
	a->devices[0].live = 1;
	a->interface_count = 1;
	snprintf(a->interfaces[0].granted, sizeof(a->interfaces[0].granted), "net:wlan0");
	snprintf(a->interfaces[0].ifname, sizeof(a->interfaces[0].ifname), "wlan0");
	a->interfaces[0].mac_permanent = 1;
	a->service_count = 1;
	a->services[0].state = 2;
	a->services[0].pid = 17;
	a->services[0].restarts = 3;
	a->services[0].output_fd = svc_out;
	/* A line half written, with what JSON must escape in it and bytes
	 * that are not UTF-8: a program's output is bytes, and the record
	 * carries them as they are. */
	snprintf(a->services[0].output_partial, sizeof(a->services[0].output_partial),
	         "tick 7 \"begin\"\t\x1b[31m \xc3\xa9\xff\xfe");
	snprintf(a->init_output_partial, sizeof(a->init_output_partial), "cix-init: start");
	a->fd_init_control = ctl;
	a->fd_init_report = rep;
	a->fd_capture = cap;
	a->fd_bpf_prog = -1;
	snprintf(a->captured_output, sizeof(a->captured_output), "line one\nline \"two\"\n");

	snprintf(g->name, sizeof(g->name), "gone");
	snprintf(g->image, sizeof(g->image), "base");
	g->running = 0;
	g->pid = 0;
	g->exit_status = 3;
	snprintf(g->last_exit_reason, sizeof(g->last_exit_reason), "exited with status 3");
	g->fd_init_control = g->fd_init_report = g->fd_capture = g->fd_bpf_prog = -1;
}

static void round_trip(const char *dir)
{
	struct handover_record w, r;
	char path[512], err[256];
	const struct handover_container *a, *g;

	snprintf(path, sizeof(path), "%s/%s", dir, HANDOVER_FILE);
	if (handover_record_init(&w, 2) != 0) {
		CHECK(0, "allocating a record");
		return;
	}
	fill(&w, 1234, 5, 6, 7, 8);
	CHECK(handover_write(path, &w, err, sizeof(err)) == 0, "write: %s", err);
	CHECK(handover_read(path, &r, err, sizeof(err)) == 0, "read back: %s", err);
	if (g_fail) {
		handover_record_free(&w);
		return;
	}
	CHECK(r.format == HANDOVER_FORMAT, "format %d", r.format);
	CHECK(strcmp(r.written_by, "0.2.57-test") == 0, "written_by %s", r.written_by);
	CHECK(strcmp(r.slot, "a") == 0 && strcmp(r.bind_addr, "192.168.15.95") == 0 &&
	          strcmp(r.uplink_unattached, "eth1") == 0,
	      "slot %s bind_addr %s uplink_unattached %s", r.slot, r.bind_addr, r.uplink_unattached);
	CHECK(r.procfuse_pid == 4242, "procfuse_pid %d", (int)r.procfuse_pid);
	CHECK(r.procfuse_load_fd == 9, "procfuse_load_fd %d", r.procfuse_load_fd);
	CHECK(r.console_count == 2 && strcmp(r.consoles[0].tty, "/dev/tty0") == 0 &&
	          r.consoles[0].pid == 0 && strcmp(r.consoles[1].tty, "/dev/ttyS0") == 0 &&
	          r.consoles[1].pid == 4343,
	      "consoles");
	CHECK(r.count == 2, "count %d", r.count);
	a = &r.containers[0];
	g = &r.containers[1];
	CHECK(strcmp(a->name, "alive") == 0 && a->running && a->pid == 1234, "alive's identity");
	CHECK(strcmp(a->image_version, w.containers[0].image_version) == 0, "image_version");
	CHECK(a->userns_enabled && a->disk_quota_bytes == 1048576 && a->capture_requested &&
	          a->procfuse_expected == 10 && a->init_up && a->ready,
	      "alive's flags");
	CHECK(a->net_count == 1 && strcmp(a->nets[0].name, "statsnet") == 0 &&
	          a->nets[0].ip_be == htonl(0xac280005) && strcmp(a->nets[0].ifname, "eth0") == 0,
	      "alive's network attachment");
	CHECK(a->file_count == 1 && strcmp(a->file_paths[0], "/etc/app.conf") == 0, "file_paths");
	CHECK(a->dns_server_count == 1 && strcmp(a->dns_servers[0], "172.40.0.1") == 0,
	      "dns_servers");
	CHECK(a->device_count == 1 && strcmp(a->devices[0].dev_path, "/dev/null") == 0 &&
	          a->devices[0].major == 1 && a->devices[0].minor == 3 && a->devices[0].live,
	      "devices");
	CHECK(a->interface_count == 1 && strcmp(a->interfaces[0].granted, "net:wlan0") == 0 &&
	          a->interfaces[0].mac_permanent,
	      "interfaces");
	CHECK(a->service_count == 1 && a->services[0].state == 2 && a->services[0].pid == 17 &&
	          a->services[0].restarts == 3 && a->services[0].output_fd == 7,
	      "services");
	CHECK(a->fd_init_control == 5 && a->fd_init_report == 6 && a->fd_capture == 8 &&
	          a->fd_bpf_prog == -1,
	      "fds");
	CHECK(strcmp(a->captured_output, "line one\nline \"two\"\n") == 0, "captured_output");
	CHECK(strcmp(a->services[0].output_partial, w.containers[0].services[0].output_partial) == 0,
	      "a service's partial line: %s", a->services[0].output_partial);
	CHECK(strcmp(a->init_output_partial, "cix-init: start") == 0, "cix-init's partial line");
	CHECK(strcmp(g->name, "gone") == 0 && !g->running && g->exit_status == 3 &&
	          strcmp(g->last_exit_reason, "exited with status 3") == 0,
	      "gone's exit");
	handover_record_free(&w);
	handover_record_free(&r);
}

static void refusals(const char *dir)
{
	struct handover_record r;
	char path[512], err[256];

#define WHO "\"slot\":\"a\",\"bind_addr\":\"127.0.0.1\",\"uplink_unattached\":\"\","
#define TOP_TAIL WHO "\"procfuse_pid\":0,\"procfuse_load_fd\":-1,\"consoles\":[],"
	snprintf(path, sizeof(path), "%s/bad.json", dir);
	write_text(path, "{\"format\":2,\"written_by\":\"x\",\"written_at\":0," TOP_TAIL
	                 "\"containers\":[]}");
	CHECK(handover_read(path, &r, err, sizeof(err)) != 0 && strstr(err, "newer") != NULL,
	      "a newer format must be refused, got: %s", err);
	write_text(path, "{\"format\":1,\"written_by\":\"x\",\"written_at\":0," TOP_TAIL
	                 "\"containers\":[{\"name\":\"c\"}]}");
	CHECK(handover_read(path, &r, err, sizeof(err)) != 0 && strstr(err, "container 0") != NULL,
	      "a container missing its keys must be refused, got: %s", err);
	write_text(path, "{\"format\":1,\"written_at\":0," TOP_TAIL "\"containers\":[]}");
	CHECK(handover_read(path, &r, err, sizeof(err)) != 0 && strstr(err, "written_by") != NULL,
	      "a record missing written_by must be refused, got: %s", err);
	write_text(path, "{\"format\":1,\"written_by\":\"x\",\"written_at\":0," WHO "\"procfuse_pid\":0,"
	                 "\"containers\":[]}");
	CHECK(handover_read(path, &r, err, sizeof(err)) != 0 &&
	          strstr(err, "procfuse_load_fd") != NULL,
	      "a record missing procfuse_load_fd must be refused, got: %s", err);
	write_text(path, "{\"format\":1,\"written_by\":\"x\",\"written_at\":0," TOP_TAIL
	                 "\"containers\":[],\"future_key\":true}");
#undef TOP_TAIL
#undef WHO
	CHECK(handover_read(path, &r, err, sizeof(err)) == 0 && r.count == 0,
	      "an unknown key must be ignored, got: %s", err);
	handover_record_free(&r);
}

static void kernel_checks(const char *dir)
{
	struct handover_record rec;
	char cgdir[512], procs[600], err[256], pidtext[32];
	int pair[2], pair2[2], pipefd[2], cap[2], pidfds[2];
	int plain;
	pid_t dead, child;

	/* The container: a child of this process, as a container's init is
	 * a child of cixd. */
	child = fork();
	if (child == 0) {
		pause();
		_exit(0);
	}
	if (child < 0) {
		CHECK(0, "fork: %s", strerror(errno));
		return;
	}
	CHECK(handover_pid_is_child(child), "a forked child must read as a child");
	CHECK(!handover_pid_is_child(getpid()), "this process must not read as its own child");
	CHECK(!handover_pid_is_child(1), "pid 1 must not read as a child");

	snprintf(cgdir, sizeof(cgdir), "%s/cgroup", dir);
	mkdir(cgdir, 0755);
	snprintf(procs, sizeof(procs), "%s/alive", cgdir);
	mkdir(procs, 0755);
	snprintf(procs, sizeof(procs), "%s/alive/cgroup.procs", cgdir);
	snprintf(pidtext, sizeof(pidtext), "1\n%d\n%d\n", (int)child, (int)getpid());
	write_text(procs, pidtext);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) != 0 ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair2) != 0 || pipe(pipefd) != 0 ||
	    pipe(cap) != 0) {
		CHECK(0, "making descriptors: %s", strerror(errno));
		return;
	}
	if (handover_record_init(&rec, 2) != 0) {
		CHECK(0, "allocating a record");
		return;
	}

	/* A live child in its cgroup, real descriptors, a console shell that
	 * is a child and one whose respawn was pending: accepted. */
	fill(&rec, child, pair[0], pair2[0], pipefd[0], cap[0]);
	rec.consoles[1].pid = child;
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) == 0, "a valid record: %s",
	      err);
	CHECK(pidfds[0] >= 0 && pidfds[1] == -1, "pidfds %d %d", pidfds[0], pidfds[1]);
	CHECK((fcntl(pair[0], F_GETFD) & FD_CLOEXEC) && (fcntl(pipefd[0], F_GETFD) & FD_CLOEXEC),
	      "FD_CLOEXEC must be set back on each passed descriptor");
	if (pidfds[0] >= 0)
		close(pidfds[0]);

	/* A console shell that is not a child: refused. */
	fill(&rec, child, pair[0], pair2[0], pipefd[0], cap[0]);
	rec.consoles[1].pid = getpid();
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) != 0 &&
	          strstr(err, "console shell") != NULL,
	      "a console shell that is not a child must be refused, got: %s", err);

	/* A dead pid: refused. */
	dead = fork();
	if (dead == 0)
		_exit(0);
	waitpid(dead, NULL, 0);
	fill(&rec, dead, pair[0], pair2[0], pipefd[0], cap[0]);
	rec.console_count = 0;
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) != 0 &&
	          strstr(err, "not alive") != NULL,
	      "a dead pid must be refused, got: %s", err);

	/* A live pid that is not a child: refused, though its cgroup lists it. */
	fill(&rec, getpid(), pair[0], pair2[0], pipefd[0], cap[0]);
	rec.console_count = 0;
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) != 0 &&
	          strstr(err, "not a child") != NULL,
	      "a pid that is not a child must be refused, got: %s", err);

	/* A live child not in its cgroup: refused. */
	write_text(procs, "1\n");
	fill(&rec, child, pair[0], pair2[0], pipefd[0], cap[0]);
	rec.console_count = 0;
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) != 0 &&
	          strstr(err, "not in its cgroup") != NULL,
	      "a pid outside its cgroup must be refused, got: %s", err);
	write_text(procs, pidtext);

	/* A regular file where cix-init's socket should be: refused. */
	plain = open(procs, O_RDONLY);
	fill(&rec, child, plain, pair2[0], pipefd[0], cap[0]);
	rec.console_count = 0;
	CHECK(handover_validate(&rec, cgdir, pidfds, err, sizeof(err)) != 0 &&
	          strstr(err, "SEQPACKET") != NULL,
	      "a regular file posing as the cix-init socket must be refused, got: %s", err);
	close(plain);
	handover_record_free(&rec);
	kill(child, SIGKILL);
	waitpid(child, NULL, 0);
}

int main(void)
{
	char dir[] = "/tmp/cix_handover_XXXXXX";
	char *tmp = getenv("TMPDIR");
	char tmpl[512];

	if (tmp != NULL && tmp[0] != '\0')
		snprintf(tmpl, sizeof(tmpl), "%s/cix_handover_XXXXXX", tmp);
	else
		snprintf(tmpl, sizeof(tmpl), "%s", dir);
	if (mkdtemp(tmpl) == NULL) {
		perror("mkdtemp");
		return 1;
	}
	round_trip(tmpl);
	refusals(tmpl);
	kernel_checks(tmpl);
	printf("HANDOVER RECORD: %s\n", g_fail ? "FAIL" : "PASS");
	return g_fail ? 1 : 0;
}
