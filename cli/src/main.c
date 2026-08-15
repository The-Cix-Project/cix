/*
 * thincctl: a pure REST client for the thinC host API
 * (docs/api/openapi.yaml). Per the project's API-First Mandate
 * (CLAUDE.md, ADR-0005), this file holds no namespace/cgroup/mount
 * logic of its own -- every subcommand is exactly one HTTP call via
 * client/src/httpclient.c, the same library test_daemon.c uses to
 * verify the daemon.
 */
#include "console.h"
#include "httpclient.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 80

static void print_usage(FILE *out)
{
	fprintf(out,
	        "usage: thincctl [--host=ADDR] [--port=N] [--json] <command> [args]\n"
	        "\n"
	        "commands:\n"
	        "  health\n"
	        "  boot      -- build version/time, A/B slot, kernel version (uname)\n"
	        "  shutdown  -- stop thincd; powers off the host too when it's running as\n"
	        "               real PID 1 (an installed system) -- a dev/interactive thincd\n"
	        "               just exits, same as it always has on SIGTERM\n"
	        "  reboot    -- stop thincd; restarts the host too when running as PID 1\n"
	        "  update [--image=PATH] [--kernel=PATH]  -- writes a fresh control-plane\n"
	        "               squashfs and/or a fresh kernel (already transferred onto the\n"
	        "               box, e.g. via scp) onto this daemon's own inactive A/B slot and\n"
	        "               stages a fresh loader entry for it; at least one of --image=/\n"
	        "               --kernel= required, either or both; does NOT reboot -- call\n"
	        "               reboot separately once ready to cut over\n"
	        "  backup [--output=PATH]  -- bundles platform configuration state (container\n"
	        "               defs, networks, DNS records, pkg install state + recipes) --\n"
	        "               NOT workload data, NOT image content, NEVER PKI keys. Prints the\n"
	        "               bundle (or --json) by default; --output= saves it verbatim for\n"
	        "               use with restore --input=\n"
	        "  restore --input=PATH  -- writes a previously-saved backup bundle's fields\n"
	        "               back to their real state files; does NOT reboot or hot-reload --\n"
	        "               call reboot separately for it to take effect on the next boot\n"
	        "  backup-config show  -- which disk (if any) automatic backup snapshots write\n"
	        "               to, whether enabled, and the auto-snapshot interval (ADR-0141)\n"
	        "  backup-config set [--disk=NAME | --clear-disk] [--enable | --disable]\n"
	        "               [--interval-hours=N]  -- only the fields given are changed; the\n"
	        "               target disk must carry the backup role; 0 hours means no automatic\n"
	        "               schedule (manual snapshot-now only)\n"
	        "  backup-config status  -- state/last_attempt_unixtime/error of the most recent\n"
	        "               snapshot attempt (manual or automatic)\n"
	        "  backup-config snapshot-now  -- write the same bundle GET /system/backup\n"
	        "               produces to the configured disk right now, regardless of schedule\n"
	        "  ps\n"
	        "  container ls  -- same as `ps` (every provisioned container and its current state);\n"
	        "               a noun-based synonym matching dns/ldap/ntp/syslog/network/etc.'s own\n"
	        "               <noun> <verb> shape\n"
	        "  run --name=NAME --image=IMAGE [--memory-max=BYTES] [--pids-max=N] [--cpu-max=\"Q P\"]\n"
	        "      [--cpuset=0-1,3] [--disk-quota=BYTES] [--network=NAME[:IP] ...]\n"
	        "      [--ip-forward] [--dns-register] [--pki-issue] [--pki-cert-dir=PATH]\n"
	        "      [--pki-days=N] [--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME]\n"
	        "      [--ldap-uid=N] [--ldap-secret-dir=PATH]\n"
	        "      [--route=DEST/PREFIX:VIA ...] [--device=ID ...] [--optional-device=ID ...]\n"
	        "      [--interface=IFNAME ...] [--restart=always|on-failure|unless-stopped]\n"
	        "      [--restart-delay=N] [--follow-rolling] [--follow-rolling-jitter-seconds=N]\n"
	        "      [--depends-on=NAME ...] [--dns-server=A.B.C.D ...]\n"
	        "      [--readiness-tcp-port=N [--readiness-timeout=N]] -- CMD [ARGS...]\n"
	        "  inspect NAME\n"
	        "  stop NAME  -- kill it now, keep its persisted definition (unlike rm)\n"
	        "  start NAME  -- bring a stopped-but-defined container back, no daemon restart needed\n"
	        "  pause NAME  -- freeze via the cgroup v2 freezer (real kernel freeze, not SIGSTOP)\n"
	        "  unpause NAME\n"
	        "  stats NAME  -- real, host-side CPU/memory/disk/network usage, gathered from\n"
	        "               cgroups + the host's own veth (no in-container agent)\n"
	        "  migrate-storage NAME [--disk=NAME]  -- move this container's own overlay\n"
	        "               storage to a disk already carrying the container-storage role and\n"
	        "               currently mounted (ADR-0142); omit --disk= to migrate back to the\n"
	        "               default OS-disk placement; requires restart_policy != \"no\" (there\n"
	        "               must be a persisted definition to restart from); briefly stops and\n"
	        "               automatically restarts the container for the final cutover -- poll\n"
	        "               migrate-storage-status\n"
	        "  migrate-storage-status NAME  -- state/disk/error of the most recent (or\n"
	        "               running) container-storage migration\n"
	        "  console NAME [--cmd=PATH]  -- interactive shell inside a running container\n"
	        "               (like `docker exec -it`), over the daemon's own WebSocket\n"
	        "               upgrade; --cmd= overrides the default /usr/bin/bash\n"
	        "  rm NAME\n"
	        "  network create --name=NAME --subnet=A.B.C.D --prefix=N [--address=A.B.C.D]\n"
	        "               -- no --address= means pure L2, no host-owned address (the\n"
	        "               default); pass it only when the host itself should have an\n"
	        "               address on this network\n"
	        "  network ls\n"
	        "  network rm NAME\n"
	        "  network attach-interface NAME --interface=IFNAME [--vlan=N]  -- enslaves a real\n"
	        "               host interface (see device ls's net: entries) to this network's\n"
	        "               bridge; --vlan= creates and enslaves an 802.1q sub-interface\n"
	        "               instead, leaving the parent free for other VLANs/networks\n"
	        "  network detach-interface NAME --interface=IFNAME\n"
	        "  image create --name=NAME  -- an empty image, C runtime pre-seeded, ready for\n"
	        "               pkg install --image=NAME\n"
	        "  image ls\n"
	        "  image rm NAME  -- refused for \"base\", for an image still in use, or with\n"
	        "               packages still installed into it\n"
	        "  image recipe add --name=NAME --file=PATH  -- a declarative package-list\n"
	        "               definition for an image (ADR-0123); recipe name == image name\n"
	        "  image recipe show|rm NAME / image recipe ls\n"
	        "  image apply-recipe NAME  -- bulk-declares the manifest; a fully-pinned\n"
	        "               recipe with a matching configured artifact server instead does\n"
	        "               an async whole-rootfs fetch -- poll image recipe-apply-status\n"
	        "  device ls  -- lists host PCI/USB/GPU devices discoverable via sysfs, with\n"
	        "               each one's id (pass to run --device=ID) and whether it's\n"
	        "               assignable. A GPU's own bare \"gpu:N\" id (not itself listed --\n"
	        "               only its individual gpu:N:card0/gpu:N:renderD128/... member\n"
	        "               nodes are) grants everything that GPU needs in one --device=\n"
	        "               (see ADR-0028)\n"
	        "  devicemap create --name=NAME --kind=exact|vendor_model --selector=SELECTOR\n"
	        "               -- a persisted, named device binding (ADR-0048), usable in place\n"
	        "               of a raw id in run --device=; \"exact\" selector is a device.id\n"
	        "               verbatim, \"vendor_model\" selector is \"<vendor_id>:<product_id>\"\n"
	        "               and follows the device across USB ports\n"
	        "  devicemap ls  -- shows whether each mapping currently resolves to real hardware\n"
	        "  devicemap rm NAME\n"
	        "  dns record create --name=NAME --ip=A.B.C.D\n"
	        "  dns record ls\n"
	        "  dns record rm NAME\n"
	        "  dns server register --container=NAME --hosts-path=PATH\n"
	        "  dns server ls\n"
	        "  dns server unregister CONTAINER\n"
	        "  ldap server register --container=NAME --config-path=PATH  -- registers a running\n"
	        "               container as the LDAP-serving target (task #725); config_path is\n"
	        "               its own absolute view of glauth's own config file\n"
	        "  ldap server ls\n"
	        "  ldap server unregister CONTAINER\n"
	        "  ldap group add --name=NAME --gidnumber=N\n"
	        "  ldap group ls\n"
	        "  ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]  -- renaming an\n"
	        "               admin group keeps hostauth-config's own admin_groups pointed at it\n"
	        "  ldap group rm NAME\n"
	        "  ldap user add --name=NAME --uidnumber=N --primarygroup=N\n"
	        "               [--secondary-groups=N,N,...] [--givenname=S]\n"
	        "               [--sn=S] [--mail=S] [--loginshell=S] [--homedirectory=S]\n"
	        "               [--password=S] [--disabled] [--ssh-key=S] [--can-search]\n"
	        "  ldap user update --name=NAME [--new-name=NEWNAME] [--uidnumber=N]\n"
	        "               [--primarygroup=N] [--secondary-groups=N,N,...] ...\n"
	        "  ldap user ls\n"
	        "  ldap user rm NAME\n"
	        "  login [--username=NAME] [--password=PASS]  -- ADR-0144: authenticates against\n"
	        "               the daemon's own host-auth backend (prompts for whichever of\n"
	        "               username/password isn't given as a flag, with echo off for the\n"
	        "               password); on success persists the session token to\n"
	        "               ~/.thincctl_token (mode 0600) so every subsequent thincctl\n"
	        "               invocation authenticates automatically -- a no-op, harmlessly, on a\n"
	        "               daemon where write-gating was never activated (no admin-group user\n"
	        "               exists yet)\n"
	        "  logout  -- invalidates the current session (if any) and removes the persisted\n"
	        "               token; always succeeds, even if not currently logged in\n"
	        "  pki ca bootstrap [--common-name=NAME] [--days=N]\n"
	        "  pki ca show\n"
	        "  pki intermediate bootstrap [--common-name=NAME] [--days=N]  -- second CA tier,\n"
	        "               root must already be bootstrapped; once done, every future\n"
	        "               pki cert create is signed by it instead of the root\n"
	        "  pki intermediate show\n"
	        "  pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n"
	        "  pki cert ls\n"
	        "  pki cert rm NAME\n"
	        "  pkg bootstrap [--toolchain=PATH]  -- stages a package-build toolchain into the\n"
	        "               pkgbuild image; no --toolchain= copies live from this daemon's own\n"
	        "               host (dev/test convenience, empty on a real minimal install);\n"
	        "               --toolchain=PATH imports a real, portable artifact already scp'd\n"
	        "               onto this box (build one with image/src/mktoolchainimage.c)\n"
	        "  pkg bootstrap --toolchain-url=URL --toolchain-sha256=SHA256 [--wait]  -- the\n"
	        "               daemon fetches the artifact itself, host-side (ADR-0065) -- for a\n"
	        "               real minimal install with no SSH server and no other way to get a\n"
	        "               real toolchain onto the box; async, 202, poll with bootstrap-status\n"
	        "  pkg bootstrap-status  -- state/error of the most recent toolchain_url fetch\n"
	        "  pkg recipes  -- lists every published (name,version) recipe (ADR-0107)\n"
	        "  pkg recipe add --name=NAME --file=PATH  -- publishes a new recipe version on\n"
	        "               this running system directly, no reinstall needed (ADR-0040);\n"
	        "               an already-published (name,version) is rejected, not overwritten\n"
	        "               (ADR-0107) -- bump pkg_version= to publish a fix\n"
	        "  pkg recipe show NAME [--version=VERSION]  -- print a recipe's own raw content,\n"
	        "               omitted version resolves to the highest available\n"
	        "  pkg recipe rm NAME [--version=VERSION]  -- omitted removes every version\n"
	        "  pkg install --name=NAME [--image=IMAGE] [--version=VERSION] [--upgrade]\n"
	        "  pkg ls\n"
	        "  pkg rm NAME[@IMAGE]\n"
	        "  pkg update-all  -- starts an upgrade for the first installed package whose\n"
	        "               recipe version has drifted (one at a time, same v1 single-job\n"
	        "               constraint as pkg install --upgrade); call again once that job\n"
	        "               finishes to pick up the next one\n"
	        "  site show  -- this install's own instance_name/site_name/domain_suffix\n"
	        "               (ADR-0046); convenience for identification + suggesting FQDNs,\n"
	        "               never enforced\n"
	        "  site set [--instance-name=NAME] [--site-name=NAME] [--domain-suffix=NAME]\n"
	        "  daemon-config show  -- thincd's own listen port, HTTP/HTTPS exposure, and\n"
	        "               which network is currently its management one\n"
	        "  daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http]\n"
	        "               [--enable-https] [--disable-https] [--management-network=NAME]\n"
	        "               [--bind-ip=A.B.C.D | --clear-bind-ip]\n"
	        "               -- live, no-restart; only the fields given are changed. bind_ip\n"
	        "               (ADR-0068) is a second, dedicated address on the management\n"
	        "               network's own bridge -- thincd binds there instead of that\n"
	        "               network's own address; --clear-bind-ip reverts to it\n"
	        "  hostauth-config show  -- current admin_groups/idle_timeout_seconds/live-LDAP\n"
	        "               backend settings (ADR-0144)\n"
	        "  hostauth-config set [--admin-group=NAME ...] [--idle-timeout-seconds=N]\n"
	        "               [--ldap-enable | --ldap-disable] [--ldap-server=HOST ...]\n"
	        "               [--ldap-port=N] [--ldap-base-dn=NAME]\n"
	        "               -- read-modify-write; only the flags given are changed, everything\n"
	        "               else keeps its current value. Write-gating activates the instant\n"
	        "               a real user is a member of one of admin_groups\n"
	        "  hostauth-sessions ls  -- every active session (username, expires-in) -- never\n"
	        "               a raw token, before or after issuance\n"
	        "  hostauth-sessions revoke USERNAME  -- log that user out everywhere (ADR-0152)\n"
	        "  rolling-config show  -- current daemon-wide rolling-restart jitter window default\n"
	        "               (Part 5, ADR-0124)\n"
	        "  rolling-config set --jitter-window-seconds=N  -- 0-3600, 0 = no jitter (restart\n"
	        "               immediately); spreads out simultaneous restarts of every\n"
	        "               follow_rolling container sharing an image that just rebuilt --\n"
	        "               a single follow_rolling container can override this default via\n"
	        "               run --follow-rolling-jitter-seconds=N at creation time\n"
	        "  pkg-build-config show  -- how many pkg install/hostbuild jobs may genuinely\n"
	        "               run at once (ADR-0157)\n"
	        "  pkg-build-config set --max-concurrent-jobs=N  -- 1-10; lowering it doesn't\n"
	        "               disrupt jobs already in flight, only future ones\n"
	        "  iso build [--disk=DEV --ip=A.B.C.D --prefix=N --gateway=A.B.C.D\n"
	        "               --interface=IFNAME] [--wait]  -- assembles a fresh installer ISO\n"
	        "               server-side (ADR-0064), from the most recent \"thinc\"/\"kernel\"/\n"
	        "               \"isotools\" hostbuild artifacts; every flag is optional, an empty\n"
	        "               call reproduces the original edit-at-the-GRUB-menu placeholder ISO\n"
	        "  iso status  -- state/iso_path/error of the most recent ISO build\n"
	        "  routes  -- the box's own real kernel IPv4 routing table (ADR-0066); the\n"
	        "               only way to see this on a real install, no SSH/general shell\n"
	        "  routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]  -- add a real\n"
	        "               kernel route (ADR-0067 Part 3); or --default --gateway=A.B.C.D\n"
	        "  routes rm --dest=A.B.C.D --prefix=N  -- remove one; or --default\n"
	        "  disks  -- real host block devices, including any partitions on them\n"
	        "               (task #844); which one is the fixed OS disk vs. assignable is\n"
	        "               flagged per entry, and a partition entry names its parent disk\n"
	        "  disks partition-table NAME  -- destructive: writes a fresh, empty GPT\n"
	        "               partition table to a non-OS whole disk with no role or\n"
	        "               partitions of its own currently in use\n"
	        "  disks add-partition NAME --name=PART_NAME [--size-mib=N]  -- appends one\n"
	        "               new partition to a disk's existing table (never touches an\n"
	        "               existing partition); omit --size-mib for \"rest of the disk\"\n"
	        "  disks rm-partition DISK_NAME PARTITION_NAME  -- removes one partition;\n"
	        "               refused (409) if it still has a role assigned (diskrole rm first)\n"
	        "  diskrole create --disk=NAME\n"
	        "               --role=container-storage|backup|state-storage|rebuildable-storage|log-storage\n"
	        "               -- assign a persisted role to a disk (never the OS disk)\n"
	        "  diskrole ls / diskrole rm NAME  -- list assigned roles (with whether each\n"
	        "               disk is currently present) / remove one; refused (409) if the\n"
	        "               disk is the active state-storage placement (storage state migrate\n"
	        "               away first)\n"
	        "  disks format NAME [--fs-type=ext4|btrfs]  -- destructive: mkfs + mount an\n"
	        "               already role-assigned, non-OS disk (assign a role first via\n"
	        "               diskrole create); fs_type defaults to ext4; refused (409) if the\n"
	        "               disk is the active state-storage placement\n"
	        "  disks format-status NAME  -- state/mount_path/error of the most recent\n"
	        "               format job for this disk\n"
	        "  storage state [show]  -- which disk (if any) is the active placement for\n"
	        "               thinC's own state (ADR-0141); default (null) is the OS disk\n"
	        "  storage state migrate [--disk=NAME]  -- move thinC's own state to a disk\n"
	        "               already carrying the state-storage role and currently mounted;\n"
	        "               omit --disk= to migrate back to the default OS-disk placement;\n"
	        "               async, no pause -- poll storage state migrate-status\n"
	        "  storage state migrate-status  -- state/disk/error of the most recent (or\n"
	        "               running) state-storage migration\n"
	        "  storage logs [show|migrate [--disk=NAME]|migrate-status]  -- same shape as\n"
	        "               storage state, for where thinC's own consolidated log store\n"
	        "               (ADR-0070/ADR-0126) lives instead\n"
	        "  storage rebuildable [show|migrate [--disk=NAME]|migrate-status]  -- same shape\n"
	        "               again, for where images/packages/artifacts (regenerable from\n"
	        "               recipes/sources, never irreplaceable) live instead\n"
	        "  swap  -- show whether the host swap file is enabled (ADR-0069)\n"
	        "  swap enable --size-mb=N  -- create and activate a swap file of this size\n"
	        "  swap disable  -- deactivate and remove it\n"
	        "  host-stats  -- host-wide load/CPU/memory/disk/network snapshot (ADR-0073)\n"
	        "  process ls  -- every real process on the box (a direct /proc scan), each\n"
	        "               correlated to a container by its own real host ppid chain, if any\n"
	        "               (ADR-0131)\n"
	        "  process kill PID  -- a real, immediate SIGKILL; refuses pid 1 and this\n"
	        "               daemon's own pid\n"
	        "  ping HOST  -- real ICMP echo against an IPv4 address, waits for the result\n"
	        "               (~2s max) and exits nonzero if unreachable\n"
	        "  resolv [show]  -- the host's own outbound DNS resolver config (ADR-0076)\n"
	        "  resolv set [--nameserver=A.B.C.D ...]  -- replace it; no flags clears it\n"
	        "  sysctl [show]  -- every persisted (daemon-managed) host-level sysctl (ADR-0160)\n"
	        "  sysctl get KEY  -- live current value (e.g. net.ipv4.ip_forward), persisted or not\n"
	        "  sysctl set KEY --value=V [--value=V ...] [--no-persist]  -- live write to\n"
	        "               /proc/sys; fully open, no allowlist -- host-auth write-gating is the\n"
	        "               only access control. Repeat --value= for a tuple-shaped key (e.g.\n"
	        "               net.ipv4.ip_local_port_range); persists by default (reapplied on\n"
	        "               every boot), --no-persist for a one-shot change\n"
	        "  sysctl rm KEY  -- stop reapplying at boot; never touches the live value\n"
	        "  kmod [ls]  -- every currently-loaded kernel module (/proc/modules, ADR-0159)\n"
	        "  kmod show NAME  -- modinfo: description, params, depends, in-tree/out-of-tree\n"
	        "  kmod load NAME [--option=KEY=VALUE ...]  -- real modprobe; no --option= falls\n"
	        "               back to this module's own persisted kmod-config default_options\n"
	        "  kmod unload NAME  -- real modprobe -r (reverse-dependency-aware)\n"
	        "  kmod-config [ls]  -- every module with a persisted default_options/autoload\n"
	        "  kmod-config set NAME [--option=KEY=VALUE ...] [--autoload|--no-autoload]  --\n"
	        "               read-modify-write, only the fields given are touched\n"
	        "  kmod-config rm NAME  -- clears both fields\n"
	        "  kmod-build --build-image=IMAGE [--version=VERSION] [--symbol=CONFIG_FOO ...]\n"
	        "               [--upgrade] [--wait]  -- an ordinary `pkg hostbuild kernel` under\n"
	        "               the hood, gaining extra =m module symbols merged into the same\n"
	        "               curated kernel config; needs a reboot onto the new bzImage (see\n"
	        "               kernel-build-and-ab-updates.md) to actually take effect\n"
	        "  time [show]  -- the host's current date/time (ADR-0110)\n"
	        "  time set --unixtime=N  -- manually set the host clock (clock_settime())\n"
	        "  ntp config [show]  -- upstream NTP server address list used to sync the\n"
	        "               host clock\n"
	        "  ntp config set [--server=A.B.C.D ...]  -- replace it; no flags clears it\n"
	        "  ntp status  -- most recent sync attempt's outcome/source/time\n"
	        "  ntp sync  -- trigger a sync attempt now (default: hourly automatic)\n"
	        "  ntp server register CONTAINER  -- registers a running container as an\n"
	        "               available internal NTP time source (mirrors dns/ldap server)\n"
	        "  ntp server ls\n"
	        "  ntp server unregister CONTAINER\n"
	        "  syslog target register --container=NAME  -- forwards every container-\n"
	        "               sourced log line to this running container (e.g. syslog-1/\n"
	        "               syslog-2 running sysklogd) as a real RFC 3164 UDP syslog\n"
	        "               datagram, alongside (never instead of) the consolidated log\n"
	        "               store 'logs' below already reads from (ADR-0127)\n"
	        "  syslog target ls\n"
	        "  syslog target unregister CONTAINER\n"
	        "  logs [--source=kernel|thincd|audit|container] [--level=...] [--container=NAME]\n"
	        "               [--regex=PATTERN] [--tail=N] [--since=UNIXTS]\n"
	        "               -- the consolidated log (kernel dmesg + thincd's own\n"
	        "               diagnostics + a per-request audit trail + every container's\n"
	        "               own stdout/stderr, transparently, ADR-0070/ADR-0126);\n"
	        "               --container= filters to one container's own lines,\n"
	        "               --regex= is a POSIX extended regex (case-insensitive)\n"
	        "               matched against the message text\n"
	        "  logs config [--max-bytes=N] [--min-level=LEVEL]  -- show or set the log's\n"
	        "               total size cap and/or its minimum severity floor\n"
	        "               (emerg/alert/crit/err|error/warning|warn/notice/info/debug,\n"
	        "               default debug -- log everything)\n");
}

static const char *json_str_field(const struct json_value *obj, const char *key)
{
	return json_as_string(json_object_get(obj, key));
}

/* Writes v (recursively) into w using the same escaping/structure
 * logic the daemon itself uses to build responses -- so --json mode
 * has no separate JSON-rendering implementation of its own. */
static void jw_write_value(struct json_writer *w, const struct json_value *v)
{
	size_t i;

	if (v == NULL) {
		jw_null(w);
		return;
	}
	switch (v->type) {
	case JSON_NULL:
		jw_null(w);
		break;
	case JSON_BOOL:
		jw_bool(w, v->u.boolean);
		break;
	case JSON_NUMBER:
		jw_int(w, (long long)v->u.number); /* this API never emits non-integral numbers */
		break;
	case JSON_STRING:
		jw_str(w, v->u.string);
		break;
	case JSON_ARRAY:
		jw_arr_open(w);
		for (i = 0; i < v->u.array.count; i++)
			jw_write_value(w, v->u.array.items[i]);
		jw_arr_close(w);
		break;
	case JSON_OBJECT:
		jw_obj_open(w);
		for (i = 0; i < v->u.object.count; i++) {
			jw_key(w, v->u.object.keys[i]);
			jw_write_value(w, v->u.object.values[i]);
		}
		jw_obj_close(w);
		break;
	}
}

static void print_raw_json(const struct json_value *v)
{
	struct json_writer w;

	jw_init(&w);
	jw_write_value(&w, v);
	printf("%.*s\n", (int)w.len, w.buf);
	jw_free(&w);
}

static void fmt_health(const struct json_value *v)
{
	printf("%s\n", json_str_field(v, "status"));
}

static void fmt_boot(const struct json_value *v)
{
	const char *slot = json_str_field(v, "slot");
	const char *kernel = json_str_field(v, "kernel_version");

	printf("build:   %s (%s)\n", json_str_field(v, "build_version"), json_str_field(v, "build_time"));
	printf("slot:    %s\n", (slot != NULL) ? slot : "(none)");
	printf("kernel:  %s\n", (kernel != NULL) ? kernel : "(unknown)");
}

static void fmt_container_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *status = json_str_field(v, "status");
	long pid = (long)json_as_number(json_object_get(v, "pid"));
	const struct json_value *exitv = json_object_get(v, "exit_status");
	const struct json_value *networks = json_object_get(v, "networks");
	const struct json_value *ip_forward = json_object_get(v, "ip_forward");
	const char *restart = json_str_field(v, "restart");
	const struct json_value *restart_delay = json_object_get(v, "restart_delay_seconds");
	const struct json_value *stopped = json_object_get(v, "stopped");
	const struct json_value *follow_rolling = json_object_get(v, "follow_rolling");
	const struct json_value *follow_rolling_jitter =
	    json_object_get(v, "follow_rolling_jitter_seconds");
	const struct json_value *readiness = json_object_get(v, "readiness");
	const struct json_value *files = json_object_get(v, "files");
	const struct json_value *sysctls = json_object_get(v, "sysctls");
	const struct json_value *cmd = json_object_get(v, "cmd");
	char exit_buf[16];
	char net_buf[256];
	char readiness_buf[32];
	char delay_buf[16];
	char jitter_buf[16];
	char cmd_buf[256];
	size_t off = 0;
	size_t cmd_off = 0;
	size_t i;

	if (exitv != NULL && exitv->type == JSON_NUMBER)
		snprintf(exit_buf, sizeof(exit_buf), "%ld", (long)json_as_number(exitv));
	else
		snprintf(exit_buf, sizeof(exit_buf), "-");

	net_buf[0] = '\0';
	if (networks != NULL && networks->type == JSON_ARRAY) {
		for (i = 0; i < networks->u.array.count && off < sizeof(net_buf) - 1; i++) {
			const struct json_value *item = networks->u.array.items[i];
			const char *n = json_str_field(item, "name");
			const char *ip = json_str_field(item, "ip");
			int written = snprintf(net_buf + off, sizeof(net_buf) - off, "%s%s:%s",
			                        i > 0 ? "," : "", n != NULL ? n : "?",
			                        ip != NULL ? ip : "?");

			if (written > 0)
				off += (size_t)written;
		}
	}

	if (readiness != NULL && readiness->type == JSON_OBJECT) {
		snprintf(readiness_buf, sizeof(readiness_buf), "tcp/%ld",
		         (long)json_as_number(json_object_get(readiness, "tcp_port")));
	} else {
		snprintf(readiness_buf, sizeof(readiness_buf), "-");
	}

	if (restart_delay != NULL && restart_delay->type == JSON_NUMBER)
		snprintf(delay_buf, sizeof(delay_buf), "%ld", (long)json_as_number(restart_delay));
	else
		snprintf(delay_buf, sizeof(delay_buf), "-");

	if (follow_rolling_jitter != NULL && follow_rolling_jitter->type == JSON_NUMBER)
		snprintf(jitter_buf, sizeof(jitter_buf), "%ld", (long)json_as_number(follow_rolling_jitter));
	else
		snprintf(jitter_buf, sizeof(jitter_buf), "-");

	cmd_buf[0] = '\0';
	if (cmd != NULL && cmd->type == JSON_ARRAY) {
		for (i = 0; i < cmd->u.array.count && cmd_off < sizeof(cmd_buf) - 1; i++) {
			const char *s = json_as_string(cmd->u.array.items[i]);
			int written = snprintf(cmd_buf + cmd_off, sizeof(cmd_buf) - cmd_off, "%s%s",
			                        i > 0 ? " " : "", s != NULL ? s : "?");

			if (written > 0)
				cmd_off += (size_t)written;
		}
	}

	printf("%-20s %-8s pid=%-8ld exit_status=%-6s networks=%-20s fwd=%-4s restart=%-15s "
	       "delay=%-4s roll=%-4s jitter=%-4s stopped=%-5s readiness=%-10s files=%-3zu sysctls=%-3zu cmd=%s\n",
	       name, status, pid, exit_buf, net_buf[0] != '\0' ? net_buf : "-",
	       (ip_forward != NULL && ip_forward->type == JSON_BOOL && ip_forward->u.boolean) ? "yes"
	                                                                                        : "no",
	       restart != NULL ? restart : "no", delay_buf,
	       (follow_rolling != NULL && follow_rolling->type == JSON_BOOL && follow_rolling->u.boolean)
	           ? "yes"
	           : "no",
	       jitter_buf,
	       (stopped != NULL && stopped->type == JSON_BOOL && stopped->u.boolean) ? "yes" : "no",
	       readiness_buf, files != NULL && files->type == JSON_ARRAY ? files->u.array.count : 0,
	       sysctls != NULL && sysctls->type == JSON_OBJECT ? sysctls->u.object.count : 0,
	       cmd_buf[0] != '\0' ? cmd_buf : "-");
}

static void fmt_list(const struct json_value *v)
{
	const struct json_value *containers = json_object_get(v, "containers");
	size_t i;

	if (containers == NULL || containers->type != JSON_ARRAY)
		return;
	for (i = 0; i < containers->u.array.count; i++)
		fmt_container_line(containers->u.array.items[i]);
}

static void fmt_removed(const struct json_value *v)
{
	(void)v;
	printf("removed\n");
}

static void fmt_added(const struct json_value *v)
{
	(void)v;
	printf("added\n");
}

static void fmt_swap(const struct json_value *v)
{
	int enabled = json_object_get(v, "enabled") != NULL &&
	              json_object_get(v, "enabled")->type == JSON_BOOL &&
	              json_object_get(v, "enabled")->u.boolean;
	long size_mb = (long)json_as_number(json_object_get(v, "size_mb"));
	const char *path = json_str_field(v, "path");

	if (!enabled) {
		printf("swap: disabled\n");
		return;
	}
	printf("swap: enabled size_mb=%ld path=%s\n", size_mb, path != NULL ? path : "");
}

static void fmt_logs(const struct json_value *v)
{
	size_t i;

	if (v == NULL || v->type != JSON_ARRAY)
		return;
	for (i = 0; i < v->u.array.count; i++) {
		const struct json_value *e = v->u.array.items[i];
		long long ts = (long long)json_as_number(json_object_get(e, "ts"));
		const char *source = json_str_field(e, "source");
		const char *level = json_str_field(e, "level");
		const char *container = json_str_field(e, "container");
		const char *msg = json_str_field(e, "msg");

		if (container != NULL && container[0] != '\0')
			printf("[%lld] %-8s %-7s (%s) %s\n", ts, source != NULL ? source : "",
			       level != NULL ? level : "", container, msg != NULL ? msg : "");
		else
			printf("[%lld] %-8s %-7s %s\n", ts, source != NULL ? source : "",
			       level != NULL ? level : "", msg != NULL ? msg : "");
	}
}

static void fmt_logs_config(const struct json_value *v)
{
	const char *min_level = json_str_field(v, "min_level");

	printf("max_bytes=%lld min_level=%s\n", (long long)json_as_number(json_object_get(v, "max_bytes")),
	       min_level != NULL ? min_level : "?");
}

static void fmt_network_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *subnet = json_str_field(v, "subnet");
	long prefix_len = (long)json_as_number(json_object_get(v, "prefix_len"));
	const char *address = json_str_field(v, "address"); /* NULL when this network has none */

	printf("%-20s %s/%-3ld address=%s\n", name, subnet, prefix_len,
	       address != NULL ? address : "none");
}

static void fmt_network_list(const struct json_value *v)
{
	const struct json_value *networks = json_object_get(v, "networks");
	size_t i;

	if (networks == NULL || networks->type != JSON_ARRAY)
		return;
	for (i = 0; i < networks->u.array.count; i++)
		fmt_network_line(networks->u.array.items[i]);
}

static void fmt_route_line(const struct json_value *v)
{
	const char *dest = json_str_field(v, "dest");
	long prefix = (long)json_as_number(json_object_get(v, "prefix"));
	const char *gateway = json_str_field(v, "gateway"); /* NULL for an on-link/gateway-less route */
	const char *iface = json_str_field(v, "interface"); /* NULL if the kernel didn't report one */
	int is_default = dest != NULL && strcmp(dest, "default") == 0;
	char dest_buf[40];

	if (is_default)
		snprintf(dest_buf, sizeof(dest_buf), "default");
	else
		snprintf(dest_buf, sizeof(dest_buf), "%s/%ld", dest != NULL ? dest : "", prefix);
	printf("%-20s via %-16s dev %s\n", dest_buf, gateway != NULL ? gateway : "-", iface != NULL ? iface : "-");
}

static void fmt_route_list(const struct json_value *v)
{
	const struct json_value *routes = json_object_get(v, "routes");
	size_t i;

	if (routes == NULL || routes->type != JSON_ARRAY)
		return;
	for (i = 0; i < routes->u.array.count; i++)
		fmt_route_line(routes->u.array.items[i]);
}

static void fmt_image_line(const struct json_value *v)
{
	printf("%s\n", json_str_field(v, "name"));
}

static void fmt_image_list(const struct json_value *v)
{
	const struct json_value *images = json_object_get(v, "images");
	size_t i;

	if (images == NULL || images->type != JSON_ARRAY)
		return;
	for (i = 0; i < images->u.array.count; i++)
		fmt_image_line(images->u.array.items[i]);
}

/* ADR-0107: one image's own manifest -- operator-declared package
 * intent, distinct from image_list's bare name-only shape. */
static void fmt_image_detail(const struct json_value *v)
{
	const struct json_value *manifest = json_object_get(v, "manifest");
	const struct json_value *versions = json_object_get(v, "versions");
	const char *current_version = json_str_field(v, "current_version");
	size_t i;

	printf("%s\n", json_str_field(v, "name"));
	printf("  current version: %s\n",
	       current_version != NULL && current_version[0] != '\0' ? current_version : "(none)");

	if (manifest == NULL || manifest->type != JSON_ARRAY || manifest->u.array.count == 0) {
		printf("  (no manifest entries)\n");
	} else {
		for (i = 0; i < manifest->u.array.count; i++) {
			const struct json_value *entry = manifest->u.array.items[i];

			printf("  %s: %s %s\n", json_str_field(entry, "package"),
			       json_str_field(entry, "mode"), json_str_field(entry, "version"));
		}
	}

	if (versions == NULL || versions->type != JSON_ARRAY || versions->u.array.count == 0) {
		printf("  (no version history)\n");
		return;
	}
	printf("  versions (newest first):\n");
	for (i = 0; i < versions->u.array.count; i++) {
		const struct json_value *entry = versions->u.array.items[i];
		const char *version = json_str_field(entry, "version");
		long long created_at = (long long)json_as_number(json_object_get(entry, "created_at"));
		int is_current = current_version != NULL && version != NULL &&
		                  strcmp(current_version, version) == 0;

		printf("    %s  created_at=%lld%s\n", version != NULL ? version : "?", created_at,
		       is_current ? "  (current)" : "");
	}
}

static void fmt_device_line(const struct json_value *v)
{
	const char *id = json_str_field(v, "id");
	const char *bus = json_str_field(v, "bus");
	const char *description = json_str_field(v, "description");
	const char *driver = json_str_field(v, "driver");
	const char *dev_path = json_str_field(v, "dev_path");
	const struct json_value *jassignable = json_object_get(v, "assignable");
	int assignable = jassignable != NULL && jassignable->type == JSON_BOOL &&
	                  jassignable->u.boolean;

	printf("%-48s %-4s %-40s %-12s %-20s %s\n", id, bus, description != NULL ? description : "",
	       driver != NULL && driver[0] != '\0' ? driver : "-",
	       dev_path != NULL && dev_path[0] != '\0' ? dev_path : "-",
	       assignable ? "assignable" : "unassignable");
}

static void fmt_device_list(const struct json_value *v)
{
	const struct json_value *devices = json_object_get(v, "devices");
	size_t i;

	if (devices == NULL || devices->type != JSON_ARRAY)
		return;
	for (i = 0; i < devices->u.array.count; i++)
		fmt_device_line(devices->u.array.items[i]);
}

static void fmt_disk_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *dev_path = json_str_field(v, "dev_path");
	const char *model = json_str_field(v, "model");
	long long size_bytes = (long long)json_as_number(json_object_get(v, "size_bytes"));
	const struct json_value *jremovable = json_object_get(v, "removable");
	const struct json_value *jos = json_object_get(v, "is_os_disk");
	const struct json_value *jmounted = json_object_get(v, "mounted");
	const struct json_value *jpart = json_object_get(v, "is_partition");
	const char *parent_disk = json_str_field(v, "parent_disk");
	const char *mount_path = json_str_field(v, "mount_path");
	int removable = jremovable != NULL && jremovable->type == JSON_BOOL && jremovable->u.boolean;
	int is_os_disk = jos != NULL && jos->type == JSON_BOOL && jos->u.boolean;
	int mounted = jmounted != NULL && jmounted->type == JSON_BOOL && jmounted->u.boolean;
	int is_partition = jpart != NULL && jpart->type == JSON_BOOL && jpart->u.boolean;
	double size_gib = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
	char mount_col[288];
	char kind_col[32];

	if (mounted)
		snprintf(mount_col, sizeof(mount_col), "mounted@%s",
		         mount_path != NULL ? mount_path : "?");
	else
		snprintf(mount_col, sizeof(mount_col), "not-mounted");

	if (is_os_disk)
		snprintf(kind_col, sizeof(kind_col), "os-disk");
	else if (is_partition)
		snprintf(kind_col, sizeof(kind_col), "partition");
	else
		snprintf(kind_col, sizeof(kind_col), "assignable");

	printf("%-12s %-16s %8.1f GiB  %-32s %-9s %-9s %s", dev_path, name, size_gib,
	       model != NULL && model[0] != '\0' ? model : "-", removable ? "removable" : "fixed",
	       kind_col, mount_col);
	if (is_partition && parent_disk != NULL && parent_disk[0] != '\0')
		printf("  (part of %s)", parent_disk);
	printf("\n");

	/* ADR-0142: real, live I/O counters + (when mounted) capacity, on
	 * their own indented line -- keeps the primary row's own already-
	 * wide layout unchanged for anyone scripting against its columns. */
	{
		long long reads = (long long)json_as_number(json_object_get(v, "reads_completed"));
		long long writes = (long long)json_as_number(json_object_get(v, "writes_completed"));
		long long io_time_ms = (long long)json_as_number(json_object_get(v, "io_time_ms"));

		printf("             io: reads=%lld writes=%lld io_time_ms=%lld", reads, writes, io_time_ms);
		if (mounted) {
			double used_gib =
			        (double)(long long)json_as_number(json_object_get(v, "used_bytes")) /
			        (1024.0 * 1024.0 * 1024.0);
			double free_gib =
			        (double)(long long)json_as_number(json_object_get(v, "free_bytes")) /
			        (1024.0 * 1024.0 * 1024.0);

			printf(" used=%.1fGiB free=%.1fGiB", used_gib, free_gib);
		}
		printf("\n");
	}
}

static void fmt_disk_list(const struct json_value *v)
{
	const struct json_value *disks = json_object_get(v, "disks");
	size_t i;

	if (disks == NULL || disks->type != JSON_ARRAY)
		return;
	for (i = 0; i < disks->u.array.count; i++)
		fmt_disk_line(disks->u.array.items[i]);
}

static void fmt_diskrole_line(const struct json_value *v)
{
	const char *disk_name = json_str_field(v, "disk_name");
	const char *role = json_str_field(v, "role");
	const struct json_value *jpresent = json_object_get(v, "present");
	int present = jpresent != NULL && jpresent->type == JSON_BOOL && jpresent->u.boolean;

	printf("%-16s %-20s %s\n", disk_name, role, present ? "present" : "absent");
}

static void fmt_diskrole_list(const struct json_value *v)
{
	const struct json_value *roles = json_object_get(v, "diskroles");
	size_t i;

	if (roles == NULL || roles->type != JSON_ARRAY)
		return;
	for (i = 0; i < roles->u.array.count; i++)
		fmt_diskrole_line(roles->u.array.items[i]);
}

static void fmt_devicemap_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *kind = json_str_field(v, "kind");
	const char *selector = json_str_field(v, "selector");
	const struct json_value *jpresent = json_object_get(v, "present");
	int present = jpresent != NULL && jpresent->type == JSON_BOOL && jpresent->u.boolean;
	const struct json_value *resolved = json_object_get(v, "resolved_ids");
	char resolved_buf[256] = "-";
	size_t i;

	if (resolved != NULL && resolved->type == JSON_ARRAY && resolved->u.array.count > 0) {
		size_t off = 0;

		for (i = 0; i < resolved->u.array.count && off < sizeof(resolved_buf); i++) {
			const char *id = json_as_string(resolved->u.array.items[i]);

			off += (size_t)snprintf(resolved_buf + off, sizeof(resolved_buf) - off, "%s%s",
			                         i > 0 ? "," : "", id != NULL ? id : "?");
		}
	}

	printf("%-24s %-13s %-40s %-11s %s\n", name, kind, selector,
	       present ? "present" : "absent", resolved_buf);
}

static void fmt_devicemap_list(const struct json_value *v)
{
	const struct json_value *maps = json_object_get(v, "devicemaps");
	size_t i;

	if (maps == NULL || maps->type != JSON_ARRAY)
		return;
	for (i = 0; i < maps->u.array.count; i++)
		fmt_devicemap_line(maps->u.array.items[i]);
}

/* "__host" is a reserved owner sentinel (daemon/src/main.c's
 * reissue_host_pki_cert()/reconcile_instance_dns_record(), ADR-0128) --
 * never a real container name, rendered distinctly here rather than as
 * the raw sentinel string, mirroring web/app.js's own formatOwner(). */
static const char *owner_display(const char *owner)
{
	if (owner == NULL || owner[0] == '\0')
		return "-";
	if (strcmp(owner, "__host") == 0)
		return "host (this daemon)";
	return owner;
}

static void fmt_dns_record_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *ip = json_str_field(v, "ip");
	const char *owner = json_str_field(v, "owner");

	printf("%-30s %-16s %s\n", name, ip, owner_display(owner));
}

static void fmt_dns_record_list(const struct json_value *v)
{
	const struct json_value *records = json_object_get(v, "records");
	size_t i;

	if (records == NULL || records->type != JSON_ARRAY)
		return;
	for (i = 0; i < records->u.array.count; i++)
		fmt_dns_record_line(records->u.array.items[i]);
}

static void fmt_dns_server_line(const struct json_value *v)
{
	const char *container = json_str_field(v, "container");
	const char *hosts_path = json_str_field(v, "hosts_path");

	printf("%-20s %s\n", container, hosts_path);
}

static void fmt_dns_server_list(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return;
	for (i = 0; i < servers->u.array.count; i++)
		fmt_dns_server_line(servers->u.array.items[i]);
}

static void fmt_ldap_server_line(const struct json_value *v)
{
	const char *container = json_str_field(v, "container");
	const char *config_path = json_str_field(v, "config_path");

	printf("%-20s %s\n", container, config_path);
}

static void fmt_ldap_server_list(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return;
	for (i = 0; i < servers->u.array.count; i++)
		fmt_ldap_server_line(servers->u.array.items[i]);
}

static void fmt_ldap_config_line(const struct json_value *v)
{
	long start_uid = (long)json_as_number(json_object_get(v, "start_uid"));
	long start_gid = (long)json_as_number(json_object_get(v, "start_gid"));

	printf("start_uid=%ld start_gid=%ld\n", start_uid, start_gid);
}

static void fmt_ldap_group_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	long gidnumber = (long)json_as_number(json_object_get(v, "gidnumber"));

	printf("%-20s gidnumber=%ld\n", name, gidnumber);
}

static void fmt_ldap_group_list(const struct json_value *v)
{
	const struct json_value *groups = json_object_get(v, "groups");
	size_t i;

	if (groups == NULL || groups->type != JSON_ARRAY)
		return;
	for (i = 0; i < groups->u.array.count; i++)
		fmt_ldap_group_line(groups->u.array.items[i]);
}

static void fmt_ldap_user_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *mail = json_str_field(v, "mail");
	long uidnumber = (long)json_as_number(json_object_get(v, "uidnumber"));
	long primarygroup = (long)json_as_number(json_object_get(v, "primarygroup"));
	const char *ssh_public_key = json_str_field(v, "ssh_public_key");
	const struct json_value *jhas_password = json_object_get(v, "has_password");
	const struct json_value *jdisabled = json_object_get(v, "disabled");
	const struct json_value *jsecondary = json_object_get(v, "secondary_groups");
	const struct json_value *jcan_search = json_object_get(v, "can_search");
	int has_password = jhas_password != NULL && jhas_password->type == JSON_BOOL &&
	                    jhas_password->u.boolean;
	int disabled = jdisabled != NULL && jdisabled->type == JSON_BOOL && jdisabled->u.boolean;
	int has_ssh_key = ssh_public_key != NULL && ssh_public_key[0] != '\0';
	int can_search = jcan_search != NULL && jcan_search->type == JSON_BOOL && jcan_search->u.boolean;
	char secondary_buf[128];

	secondary_buf[0] = '\0';
	if (jsecondary != NULL && jsecondary->type == JSON_ARRAY && jsecondary->u.array.count > 0) {
		size_t off = 0, i;

		for (i = 0; i < jsecondary->u.array.count && off < sizeof(secondary_buf) - 1; i++) {
			int written = snprintf(secondary_buf + off, sizeof(secondary_buf) - off, "%s%ld",
			                        i > 0 ? "," : "",
			                        (long)json_as_number(jsecondary->u.array.items[i]));

			if (written > 0)
				off += (size_t)written;
		}
	}

	printf("%-20s uid=%-6ld gid=%-6ld secondary_gids=%-12s mail=%-30s password=%-4s disabled=%-4s "
	       "ssh_key=%-6s can_search=%s\n",
	       name, uidnumber, primarygroup, secondary_buf[0] != '\0' ? secondary_buf : "-",
	       mail != NULL ? mail : "", has_password ? "set" : "unset", disabled ? "yes" : "no",
	       has_ssh_key ? "set" : "unset", can_search ? "yes" : "no");
}

static void fmt_ldap_user_list(const struct json_value *v)
{
	const struct json_value *users = json_object_get(v, "users");
	size_t i;

	if (users == NULL || users->type != JSON_ARRAY)
		return;
	for (i = 0; i < users->u.array.count; i++)
		fmt_ldap_user_line(users->u.array.items[i]);
}

static void print_sans_csv(const struct json_value *v)
{
	const struct json_value *sans = json_object_get(v, "sans");
	size_t i;

	if (sans == NULL || sans->type != JSON_ARRAY)
		return;
	for (i = 0; i < sans->u.array.count; i++) {
		if (i > 0)
			printf(",");
		printf("%s", json_as_string(sans->u.array.items[i]));
	}
}

static void fmt_pki_cert_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *serial = json_str_field(v, "serial");
	const char *not_after = json_str_field(v, "not_after");
	const char *owner = json_str_field(v, "owner");

	printf("%-24s %-42s %-30s %s\n", name, serial, not_after, owner_display(owner));
}

static void fmt_pki_cert_list(const struct json_value *v)
{
	const struct json_value *certs = json_object_get(v, "certs");
	size_t i;

	if (certs == NULL || certs->type != JSON_ARRAY)
		return;
	for (i = 0; i < certs->u.array.count; i++)
		fmt_pki_cert_line(certs->u.array.items[i]);
}

/* Multi-line: a PEM cert (and, for a fresh issuance, a PEM key) can't
 * sensibly fit the %-Ns single-row table format every other list uses. */
static void fmt_pki_cert_issued(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *serial = json_str_field(v, "serial");
	const char *not_after = json_str_field(v, "not_after");
	const char *cert_pem = json_str_field(v, "cert_pem");
	const char *key_pem = json_str_field(v, "key_pem");

	printf("name:      %s\n", name);
	printf("serial:    %s\n", serial);
	printf("not_after: %s\n", not_after);
	printf("sans:      ");
	print_sans_csv(v);
	printf("\n\nCertificate:\n%s\n", cert_pem != NULL ? cert_pem : "");
	printf("Private Key (shown once -- save it now):\n%s\n", key_pem != NULL ? key_pem : "");
}

static void fmt_pki_ca(const struct json_value *v)
{
	const char *subject = json_str_field(v, "subject");
	const char *serial = json_str_field(v, "serial");
	const char *not_before = json_str_field(v, "not_before");
	const char *not_after = json_str_field(v, "not_after");
	const char *cert_pem = json_str_field(v, "cert_pem");

	printf("subject:    %s\n", subject);
	printf("serial:     %s\n", serial);
	printf("not_before: %s\n", not_before);
	printf("not_after:  %s\n", not_after);
	printf("\nCertificate:\n%s\n", cert_pem != NULL ? cert_pem : "");
}

/* Reissued leaves' own cert_pem/key_pem are real (shown once, same
 * as a fresh cert create()) but omitted here -- this is a summary
 * view; thincctl pki cert ls / a saved --json capture is how an
 * operator gets the full material for every reissued leaf at once. */
static void fmt_pki_reset(const struct json_value *v)
{
	const struct json_value *root = json_object_get(v, "root");
	const struct json_value *intermediate = json_object_get(v, "intermediate");
	const struct json_value *reissued = json_object_get(v, "reissued");
	const char *root_subject = root != NULL ? json_str_field(root, "subject") : NULL;
	size_t i;

	printf("root:         %s\n", root_subject != NULL ? root_subject : "?");
	if (intermediate != NULL && intermediate->type == JSON_OBJECT) {
		const char *isub = json_str_field(intermediate, "subject");

		printf("intermediate: %s\n", isub != NULL ? isub : "?");
	} else {
		printf("intermediate: (none)\n");
	}
	if (reissued != NULL && reissued->type == JSON_ARRAY) {
		printf("\nreissued %zu leaf cert(s):\n", reissued->u.array.count);
		for (i = 0; i < reissued->u.array.count; i++)
			fmt_pki_cert_line(reissued->u.array.items[i]);
	}
}

/*
 * Common success/failure handling for every subcommand: 2xx prints
 * either the raw JSON (--json, or when the subcommand has no special
 * formatting) or a formatted rendering via fmt; anything else prints
 * the API's {"error": "..."} message to stderr. Always frees r.
 * Returns the process exit code.
 */
static int emit(struct kx_response *r, int json_mode, void (*fmt)(const struct json_value *))
{
	int rc;

	if (r->status < 200 || r->status >= 300) {
		const char *msg = json_str_field(r->json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r->status);
		rc = 1;
	} else if (json_mode || fmt == NULL) {
		print_raw_json(r->json);
		rc = 0;
	} else {
		fmt(r->json);
		rc = 0;
	}
	kx_response_free(r);
	return rc;
}

static int cmd_health(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/health", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_boot(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/boot", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_boot);
}

static int cmd_routes_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/routes", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_route_list);
}

static int cmd_routes_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *dest = NULL;
	const char *gateway = NULL;
	const char *prefix = NULL;
	int is_default = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--dest=", 7) == 0)
			dest = argv[i] + 7;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = argv[i] + 9;
		else if (strncmp(argv[i], "--gateway=", 10) == 0)
			gateway = argv[i] + 10;
		else if (strcmp(argv[i], "--default") == 0)
			is_default = 1;
		else {
			fprintf(stderr, "thincctl: unknown routes add option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (!is_default && (dest == NULL || prefix == NULL)) {
		fprintf(stderr,
		        "usage: thincctl routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]\n"
		        "       thincctl routes add --default --gateway=A.B.C.D\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (!is_default) {
		jw_key(&w, "dest");
		jw_str(&w, dest);
		jw_key(&w, "prefix");
		jw_int(&w, atol(prefix));
	}
	if (gateway != NULL) {
		jw_key(&w, "gateway");
		jw_str(&w, gateway);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/routes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_added);
}

static int cmd_routes_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *dest = NULL;
	const char *prefix = NULL;
	int is_default = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--dest=", 7) == 0)
			dest = argv[i] + 7;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = argv[i] + 9;
		else if (strcmp(argv[i], "--default") == 0)
			is_default = 1;
		else {
			fprintf(stderr, "thincctl: unknown routes rm option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (!is_default && (dest == NULL || prefix == NULL)) {
		fprintf(stderr,
		        "usage: thincctl routes rm --dest=A.B.C.D --prefix=N\n"
		        "       thincctl routes rm --default\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (!is_default) {
		jw_key(&w, "dest");
		jw_str(&w, dest);
		jw_key(&w, "prefix");
		jw_int(&w, atol(prefix));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "DELETE", "/v1/system/routes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_removed);
}

static int cmd_routes(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_routes_ls(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_routes_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_routes_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_routes_ls(c, json_mode);

	fprintf(stderr,
	        "usage: thincctl routes [ls]\n"
	        "       thincctl routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]\n"
	        "       thincctl routes add --default --gateway=A.B.C.D\n"
	        "       thincctl routes rm --dest=A.B.C.D --prefix=N\n"
	        "       thincctl routes rm --default\n");
	return 2;
}

static int cmd_disks_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/disks", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_disk_list);
}

static void fmt_diskformat_status(const struct json_value *v)
{
	const char *disk_name = json_str_field(v, "disk_name");
	const char *state = json_str_field(v, "state");
	const char *fs_type = json_str_field(v, "fs_type");
	const char *mount_path = json_str_field(v, "mount_path");
	const char *error = json_str_field(v, "error");

	if (state == NULL || strcmp(state, "none") == 0) {
		printf("no format job has run\n");
		return;
	}
	printf("%s: %s", disk_name != NULL ? disk_name : "?", state);
	if (fs_type != NULL)
		printf(" fs_type=%s", fs_type);
	if (mount_path != NULL)
		printf(" mount_path=%s", mount_path);
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/*
 * Multi-disk management Phase C (ROADMAP.md): format + mount an
 * already-role-assigned disk (POST /v1/diskroles first). Destructive
 * and deliberately explicit -- disk_name is sent as the request body's
 * own confirm_disk_name (matching what the operator already typed once
 * in the URL path), the same double-confirmation the REST layer itself
 * requires (see main.c's handle_disk_format_post()). No further
 * interactive "are you sure" prompt here, matching this CLI's existing
 * "pki reset" precedent for destructive operations -- the request body
 * confirmation IS the safety gate.
 */
static int cmd_disks_format(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	const char *fs_type = NULL;
	char path[256];
	struct json_writer w;
	struct kx_response r;
	int i;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl disks format NAME [--fs-type=ext4|btrfs]\n");
		return 2;
	}
	disk_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--fs-type=", 10) == 0)
			fs_type = argv[i] + 10;
	}
	snprintf(path, sizeof(path), "/v1/disks/%s/format", disk_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "confirm_disk_name");
	jw_str(&w, disk_name);
	if (fs_type != NULL) {
		jw_key(&w, "fs_type");
		jw_str(&w, fs_type);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_diskformat_status);
}

static int cmd_disks_format_status(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	char path[256];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl disks format-status NAME\n");
		return 2;
	}
	disk_name = argv[0];
	snprintf(path, sizeof(path), "/v1/disks/%s/format", disk_name);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_diskformat_status);
}

/*
 * Partition-level disk management (ROADMAP.md task #844). Same
 * double-confirmation posture as "disks format" for the destructive
 * partition-table-create -- adding or removing one partition is not
 * itself destructive to the rest of the table, so those two skip the
 * confirm field.
 */
static void fmt_disk_or_partitions(const struct json_value *v)
{
	const struct json_value *partitions = json_object_get(v, "partitions");

	if (partitions != NULL && partitions->type == JSON_ARRAY) {
		size_t i;

		for (i = 0; i < partitions->u.array.count; i++)
			fmt_disk_line(partitions->u.array.items[i]);
		return;
	}
	fmt_disk_line(v);
}

static int cmd_disks_partition_table(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	char path[256];
	struct json_writer w;
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl disks partition-table NAME\n");
		return 2;
	}
	disk_name = argv[0];
	snprintf(path, sizeof(path), "/v1/disks/%s/partition-table", disk_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "confirm_disk_name");
	jw_str(&w, disk_name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_or_partitions);
}

static int cmd_disks_add_partition(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	const char *part_name = NULL;
	unsigned long long size_mib = 0;
	char path[256];
	struct json_writer w;
	struct kx_response r;
	int i;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: thincctl disks add-partition NAME --name=PART_NAME [--size-mib=N]\n"
		        "       (omit --size-mib to consume all remaining space on the disk)\n");
		return 2;
	}
	disk_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			part_name = argv[i] + 7;
		else if (strncmp(argv[i], "--size-mib=", 11) == 0)
			size_mib = strtoull(argv[i] + 11, NULL, 10);
	}
	if (part_name == NULL) {
		fprintf(stderr, "thincctl: disks add-partition requires --name=PART_NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/disks/%s/partitions", disk_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, part_name);
	if (size_mib > 0) {
		jw_key(&w, "size_mib");
		jw_int(&w, (long long)size_mib);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_or_partitions);
}

static int cmd_disks_rm_partition(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[256];
	struct kx_response r;

	if (argc < 2) {
		fprintf(stderr, "usage: thincctl disks rm-partition DISK_NAME PARTITION_NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/disks/%s/partitions/%s", argv[0], argv[1]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		kx_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("partition removed\n");
	else
		printf("error: status %d\n", r.status);
	kx_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_disks(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_disks_ls(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_disks_ls(c, json_mode);
	if (strcmp(sub, "format") == 0)
		return cmd_disks_format(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "format-status") == 0)
		return cmd_disks_format_status(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "partition-table") == 0)
		return cmd_disks_partition_table(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "add-partition") == 0)
		return cmd_disks_add_partition(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm-partition") == 0)
		return cmd_disks_rm_partition(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "usage: thincctl disks [ls]\n"
	                "       thincctl disks format NAME [--fs-type=ext4|btrfs]\n"
	                "       thincctl disks format-status NAME\n"
	                "       thincctl disks partition-table NAME\n"
	                "       thincctl disks add-partition NAME --name=PART_NAME [--size-mib=N]\n"
	                "       thincctl disks rm-partition DISK_NAME PARTITION_NAME\n");
	return 2;
}

/*
 * ADR-0141 Phase 2: only "state" is wired to a real REST resource yet
 * (GET/POST /v1/system/state-storage(/migrate)) -- "rebuildable"/"logs"
 * reuse the identical daemon-side machinery once their own phases land
 * (Phase 3/4), at which point they'll take this exact same shape.
 */
static void fmt_storage_placement(const struct json_value *v)
{
	const struct json_value *jdisk = json_object_get(v, "disk");
	const char *disk = json_as_string(jdisk);

	if (jdisk == NULL || jdisk->type == JSON_NULL || disk == NULL)
		printf("default OS-disk placement\n");
	else
		printf("%s\n", disk);
}

static void fmt_storage_migrate_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const struct json_value *jdisk = json_object_get(v, "disk");
	const char *disk = json_as_string(jdisk);
	const char *error = json_str_field(v, "error");

	if (state == NULL || strcmp(state, "none") == 0) {
		printf("no migration has run\n");
		return;
	}
	printf("%s", state);
	if (jdisk != NULL && jdisk->type != JSON_NULL && disk != NULL)
		printf(" disk=%s", disk);
	else if (jdisk != NULL)
		printf(" disk=(default OS-disk placement)");
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/* endpoint is "state-storage" or "log-storage" -- thincctl storage
 * {state,logs} both share this identical shape, only the REST path
 * segment (and thus which daemon-side storage_kind ends up acted on)
 * differs. */
static int cmd_storage_kind_show(const struct kx_client *c, int json_mode, const char *endpoint)
{
	char path[64];
	struct kx_response r;

	snprintf(path, sizeof(path), "/v1/system/%s", endpoint);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_placement);
}

static int cmd_storage_kind_migrate(const struct kx_client *c, int json_mode, const char *endpoint,
                                     int argc, char **argv)
{
	const char *disk = NULL;
	char path[64];
	struct json_writer w;
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown storage migrate option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk");
	if (disk != NULL)
		jw_str(&w, disk);
	else
		jw_null(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/system/%s/migrate", endpoint);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_storage_kind_migrate_status(const struct kx_client *c, int json_mode, const char *endpoint)
{
	char path[64];
	struct kx_response r;

	snprintf(path, sizeof(path), "/v1/system/%s/migrate", endpoint);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_storage_kind(const struct kx_client *c, int json_mode, const char *name,
                             const char *endpoint, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_storage_kind_show(c, json_mode, endpoint);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_storage_kind_show(c, json_mode, endpoint);
	if (strcmp(sub, "migrate") == 0)
		return cmd_storage_kind_migrate(c, json_mode, endpoint, argc - 1, argv + 1);
	if (strcmp(sub, "migrate-status") == 0)
		return cmd_storage_kind_migrate_status(c, json_mode, endpoint);

	fprintf(stderr,
	        "usage: thincctl storage %s [show]\n"
	        "       thincctl storage %s migrate [--disk=NAME]  -- omit for the default "
	        "OS-disk placement\n"
	        "       thincctl storage %s migrate-status\n",
	        name, name, name);
	return 2;
}

static int cmd_storage(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl storage state|logs|rebuildable [show|migrate|migrate-status]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "state") == 0)
		return cmd_storage_kind(c, json_mode, "state", "state-storage", argc - 1, argv + 1);
	if (strcmp(sub, "logs") == 0)
		return cmd_storage_kind(c, json_mode, "logs", "log-storage", argc - 1, argv + 1);
	if (strcmp(sub, "rebuildable") == 0)
		return cmd_storage_kind(c, json_mode, "rebuildable", "rebuildable-storage", argc - 1, argv + 1);

	fprintf(stderr, "usage: thincctl storage state|logs|rebuildable [show|migrate|migrate-status]\n");
	return 2;
}

/* ADR-0160: thincctl sysctl show|get|set|rm -- host-level /proc/sys,
 * live, fully open (no allowlist, host-auth write-gating is the only
 * access control). Distinct from run's own --sysctl= (create-time,
 * net.*-only, per-container) -- this is a separate host-wide surface,
 * not an extension of that one. */
static void fmt_sysctl_value(const struct json_value *v)
{
	const struct json_value *jval = json_object_get(v, "value");
	const char *scalar;

	if (jval == NULL) {
		printf("(no value)\n");
		return;
	}
	scalar = json_as_string(jval);
	if (scalar != NULL) {
		printf("%s\n", scalar);
		return;
	}
	if (jval->type == JSON_ARRAY) {
		size_t i;

		for (i = 0; i < jval->u.array.count; i++) {
			if (i > 0)
				printf(" ");
			printf("%s", json_as_string(jval->u.array.items[i]));
		}
		printf("\n");
		return;
	}
	printf("(unrecognized value shape)\n");
}

static void fmt_sysctl_one(const struct json_value *v)
{
	printf("%s = ", json_str_field(v, "key"));
	fmt_sysctl_value(v);
}

static void fmt_sysctl_list(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "sysctls");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("(no sysctls persisted)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++)
		fmt_sysctl_one(arr->u.array.items[i]);
}

static int cmd_sysctl_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/sysctl", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_sysctl_list);
}

static int cmd_sysctl_get(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl sysctl get KEY\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/sysctl/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_sysctl_one);
}

/* thincctl sysctl set KEY --value=V [--value=V ...] [--no-persist]
 * -- repeatable --value= (matching resolv set's own --nameserver=
 * repeatable-flag convention) builds a JSON array when given more
 * than once, a bare JSON string when given exactly once -- either
 * shape the daemon already accepts. */
#define CLI_SYSCTL_MAX_VALUES 8

static int cmd_sysctl_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *key;
	const char *values[CLI_SYSCTL_MAX_VALUES];
	int value_count = 0;
	int persist = 1;
	int i;
	char path[192];
	struct json_writer w;
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl sysctl set KEY --value=V [--value=V ...] [--no-persist]\n");
		return 2;
	}
	key = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--value=", 8) == 0) {
			if (value_count >= CLI_SYSCTL_MAX_VALUES) {
				fprintf(stderr, "thincctl: too many --value= flags (max %d)\n",
				        CLI_SYSCTL_MAX_VALUES);
				return 2;
			}
			values[value_count++] = argv[i] + 8;
		} else if (strcmp(argv[i], "--no-persist") == 0) {
			persist = 0;
		} else {
			fprintf(stderr, "thincctl: unknown sysctl set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (value_count == 0) {
		fprintf(stderr, "thincctl: sysctl set requires at least one --value=\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "value");
	if (value_count == 1) {
		jw_str(&w, values[0]);
	} else {
		jw_arr_open(&w);
		for (i = 0; i < value_count; i++)
			jw_str(&w, values[i]);
		jw_arr_close(&w);
	}
	if (!persist) {
		jw_key(&w, "persist");
		jw_bool(&w, 0);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/system/sysctl/%s", key);
	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_sysctl_one);
}

static int cmd_sysctl_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl sysctl rm KEY\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/sysctl/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		kx_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("removed from persisted config (live value untouched)\n");
	else
		printf("error: status %d\n", r.status);
	kx_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_sysctl(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_sysctl_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_sysctl_show(c, json_mode);
	if (strcmp(sub, "get") == 0)
		return cmd_sysctl_get(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "set") == 0)
		return cmd_sysctl_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_sysctl_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "usage: thincctl sysctl [show]  -- every persisted (daemon-managed) key\n"
	                "       thincctl sysctl get KEY  -- live current value, persisted or not\n"
	                "       thincctl sysctl set KEY --value=V [--value=V ...] [--no-persist]\n"
	                "       thincctl sysctl rm KEY  -- stop reapplying at boot (live value untouched)\n");
	return 2;
}

/* ADR-0159 Phase A: `thincctl kmod ls|show|load|unload` (management)
 * and `thincctl kmod-config set|ls|rm` (persisted default options +
 * boot autoload). */

static void fmt_kmod_options_obj(const struct json_value *obj)
{
	size_t i;

	if (obj == NULL || obj->type != JSON_OBJECT || obj->u.object.count == 0) {
		printf("(none)");
		return;
	}
	for (i = 0; i < obj->u.object.count; i++) {
		if (i > 0)
			printf(" ");
		printf("%s=%s", obj->u.object.keys[i], json_as_string(obj->u.object.values[i]));
	}
}

static void fmt_kmod_module_one(const struct json_value *v)
{
	const struct json_value *deps = json_object_get(v, "used_by");
	size_t i;

	printf("%-24s %8.0f  used_by=%.0f", json_str_field(v, "name"), json_as_number(json_object_get(v, "size")),
	       json_as_number(json_object_get(v, "used_by_count")));
	if (deps != NULL && deps->type == JSON_ARRAY && deps->u.array.count > 0) {
		printf(" [");
		for (i = 0; i < deps->u.array.count; i++) {
			if (i > 0)
				printf(",");
			printf("%s", json_as_string(deps->u.array.items[i]));
		}
		printf("]");
	}
	printf("  %s\n", json_str_field(v, "state"));
}

static void fmt_kmod_list(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "modules");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("(no modules loaded)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++)
		fmt_kmod_module_one(arr->u.array.items[i]);
}

static void fmt_kmod_info(const struct json_value *v)
{
	const struct json_value *depends = json_object_get(v, "depends");
	const struct json_value *params = json_object_get(v, "params");
	size_t i;

	printf("name: %s\n", json_str_field(v, "name"));
	if (json_object_get(v, "filename") != NULL)
		printf("filename: %s\n", json_str_field(v, "filename"));
	if (json_object_get(v, "description") != NULL)
		printf("description: %s\n", json_str_field(v, "description"));
	if (json_object_get(v, "version") != NULL)
		printf("version: %s\n", json_str_field(v, "version"));
	if (json_object_get(v, "license") != NULL)
		printf("license: %s\n", json_str_field(v, "license"));
	if (json_object_get(v, "author") != NULL)
		printf("author: %s\n", json_str_field(v, "author"));
	printf("in_tree: %s\n",
	       json_object_get(v, "in_tree") != NULL && json_as_number(json_object_get(v, "in_tree")) != 0
	           ? "yes"
	           : "no");
	printf("depends: ");
	if (depends == NULL || depends->type != JSON_ARRAY || depends->u.array.count == 0) {
		printf("(none)\n");
	} else {
		for (i = 0; i < depends->u.array.count; i++) {
			if (i > 0)
				printf(",");
			printf("%s", json_as_string(depends->u.array.items[i]));
		}
		printf("\n");
	}
	if (params != NULL && params->type == JSON_ARRAY && params->u.array.count > 0) {
		printf("params:\n");
		for (i = 0; i < params->u.array.count; i++) {
			const struct json_value *p = params->u.array.items[i];

			printf("  %s (%s): %s\n", json_str_field(p, "name"), json_str_field(p, "type"),
			       json_str_field(p, "description"));
		}
	}
}

static int cmd_kmod_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/kmod", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmod_list);
}

static int cmd_kmod_show(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl kmod show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/kmod/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmod_info);
}

#define CLI_KMOD_MAX_OPTIONS 8

static int cmd_kmod_load(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name;
	const char *options[CLI_KMOD_MAX_OPTIONS];
	int option_count = 0;
	int i;
	char path[192];
	struct json_writer w;
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl kmod load NAME [--option=KEY=VALUE ...]\n");
		return 2;
	}
	name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--option=", 9) == 0) {
			if (option_count >= CLI_KMOD_MAX_OPTIONS) {
				fprintf(stderr, "thincctl: too many --option= flags (max %d)\n",
				        CLI_KMOD_MAX_OPTIONS);
				return 2;
			}
			options[option_count++] = argv[i] + 9;
		} else {
			fprintf(stderr, "thincctl: unknown kmod load option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "options");
	jw_obj_open(&w);
	for (i = 0; i < option_count; i++) {
		char buf[128];
		char *eq;

		snprintf(buf, sizeof(buf), "%s", options[i]);
		eq = strchr(buf, '=');
		if (eq == NULL) {
			fprintf(stderr, "thincctl: --option= must be KEY=VALUE, got '%s'\n", options[i]);
			jw_free(&w);
			return 2;
		}
		*eq = '\0';
		jw_key(&w, buf);
		jw_str(&w, eq + 1);
	}
	jw_obj_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/system/kmod/%s", name);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		kx_response_free(&r);
		return r.status == 200 ? 0 : 1;
	}
	if (r.status == 200)
		printf("loaded %s\n", name);
	else
		printf("error: status %d\n", r.status);
	kx_response_free(&r);
	return r.status == 200 ? 0 : 1;
}

static int cmd_kmod_unload(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl kmod unload NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/kmod/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		kx_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("unloaded %s\n", argv[0]);
	else
		printf("error: status %d\n", r.status);
	kx_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_kmod(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_kmod_ls(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_kmod_ls(c, json_mode);
	if (strcmp(sub, "show") == 0)
		return cmd_kmod_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "load") == 0)
		return cmd_kmod_load(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "unload") == 0)
		return cmd_kmod_unload(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "usage: thincctl kmod [ls]  -- every currently-loaded module (/proc/modules)\n"
	                "       thincctl kmod show NAME  -- modinfo: description, params, depends\n"
	                "       thincctl kmod load NAME [--option=KEY=VALUE ...]  -- real modprobe;\n"
	                "               no --option= falls back to this module's own persisted\n"
	                "               kmod-config default_options, if any\n"
	                "       thincctl kmod unload NAME  -- real modprobe -r\n");
	return 2;
}

static void fmt_kmodconfig_one(const struct json_value *v)
{
	printf("%s: default_options=", json_str_field(v, "name"));
	fmt_kmod_options_obj(json_object_get(v, "default_options"));
	printf(" autoload=%s\n",
	       json_as_number(json_object_get(v, "autoload")) != 0 ? "yes" : "no");
}

static void fmt_kmodconfig_list(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "kmod_config");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("(no kmod config persisted)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++)
		fmt_kmodconfig_one(arr->u.array.items[i]);
}

static int cmd_kmodconfig_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/kmod-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmodconfig_list);
}

static int cmd_kmodconfig_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name;
	const char *options[CLI_KMOD_MAX_OPTIONS];
	int option_count = 0;
	int have_options = 0;
	int have_autoload = 0;
	int autoload_value = 0;
	int i;
	char path[192];
	struct json_writer w;
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl kmod-config set NAME [--option=KEY=VALUE ...] "
		                "[--autoload|--no-autoload]\n");
		return 2;
	}
	name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--option=", 9) == 0) {
			if (option_count >= CLI_KMOD_MAX_OPTIONS) {
				fprintf(stderr, "thincctl: too many --option= flags (max %d)\n",
				        CLI_KMOD_MAX_OPTIONS);
				return 2;
			}
			options[option_count++] = argv[i] + 9;
			have_options = 1;
		} else if (strcmp(argv[i], "--autoload") == 0) {
			have_autoload = 1;
			autoload_value = 1;
		} else if (strcmp(argv[i], "--no-autoload") == 0) {
			have_autoload = 1;
			autoload_value = 0;
		} else {
			fprintf(stderr, "thincctl: unknown kmod-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (!have_options && !have_autoload) {
		fprintf(stderr, "thincctl: kmod-config set requires --option= and/or "
		                "--autoload/--no-autoload\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (have_options) {
		jw_key(&w, "default_options");
		jw_obj_open(&w);
		for (i = 0; i < option_count; i++) {
			char buf[128];
			char *eq;

			snprintf(buf, sizeof(buf), "%s", options[i]);
			eq = strchr(buf, '=');
			if (eq == NULL) {
				fprintf(stderr, "thincctl: --option= must be KEY=VALUE, got '%s'\n", options[i]);
				jw_free(&w);
				return 2;
			}
			*eq = '\0';
			jw_key(&w, buf);
			jw_str(&w, eq + 1);
		}
		jw_obj_close(&w);
	}
	if (have_autoload) {
		jw_key(&w, "autoload");
		jw_bool(&w, autoload_value);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/system/kmod-config/%s", name);
	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_kmodconfig_one);
}

static int cmd_kmodconfig_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl kmod-config rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/kmod-config/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		kx_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("removed kmod-config for %s\n", argv[0]);
	else
		printf("error: status %d\n", r.status);
	kx_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_kmodconfig(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_kmodconfig_ls(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_kmodconfig_ls(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_kmodconfig_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_kmodconfig_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "usage: thincctl kmod-config [ls]  -- every module with a persisted default\n"
	                "       thincctl kmod-config set NAME [--option=KEY=VALUE ...] "
	                "[--autoload|--no-autoload]\n"
	                "       thincctl kmod-config rm NAME  -- clears both fields\n");
	return 2;
}

#define CLI_RESOLV_MAX_NAMESERVERS 3 /* mirrors daemon/include/resolv.h's own RESOLV_MAX_NAMESERVERS -- not shared via a header since this CLI never links daemon internals, only talks REST */

static void fmt_resolv(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "nameservers");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("(no nameservers configured)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++)
		printf("nameserver %s\n", json_as_string(arr->u.array.items[i]));
}

static int cmd_resolv_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/resolv", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_resolv);
}

/* thincctl resolv set --nameserver=A.B.C.D [--nameserver=A.B.C.D ...]
 * -- repeatable, same convention run's own --network=/--device=/
 * --interface= already use. No flags at all means an empty list --
 * clears the host's own resolver config entirely, same "the absence
 * of the flag is a real, valid choice" precedent --clear-bind-ip
 * established for daemon-config. */
static int cmd_resolv_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *nameservers[CLI_RESOLV_MAX_NAMESERVERS];
	int count = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--nameserver=", 13) == 0) {
			if (count >= CLI_RESOLV_MAX_NAMESERVERS) {
				fprintf(stderr, "thincctl: too many --nameserver= flags (max %d)\n",
				        CLI_RESOLV_MAX_NAMESERVERS);
				return 2;
			}
			nameservers[count++] = argv[i] + 13;
		} else {
			fprintf(stderr, "thincctl: unknown resolv set option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "nameservers");
	jw_arr_open(&w);
	for (i = 0; i < count; i++)
		jw_str(&w, nameservers[i]);
	jw_arr_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/resolv", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_resolv);
}

static int cmd_resolv(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_resolv_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_resolv_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_resolv_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr,
	        "usage: thincctl resolv [show]\n"
	        "       thincctl resolv set [--nameserver=A.B.C.D ...]\n");
	return 2;
}

/* thincctl ntp config show/set, ntp status, ntp server register/ls/
 * unregister, and the separate thincctl time show/set -- task
 * #751-755, mirroring resolv's own upstream-list shape (config) plus
 * dns/ldap server's own registration shape (server), kept as two
 * genuinely different resources exactly like the daemon's own REST
 * surface does (GET/PUT /v1/system/ntp vs POST/GET/DELETE /v1/ntp/
 * servers vs GET/PUT /v1/system/time). */
#define CLI_NTP_MAX_UPSTREAM 3 /* mirrors daemon/include/ntp.h's own NTP_MAX_UPSTREAM */

static void fmt_ntp_config(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "upstream");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("(no upstream NTP servers configured)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++)
		printf("server %s\n", json_as_string(arr->u.array.items[i]));
}

static int cmd_ntp_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/ntp", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_config);
}

static int cmd_ntp_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *upstream[CLI_NTP_MAX_UPSTREAM];
	int count = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--server=", 9) == 0) {
			if (count >= CLI_NTP_MAX_UPSTREAM) {
				fprintf(stderr, "thincctl: too many --server= flags (max %d)\n",
				        CLI_NTP_MAX_UPSTREAM);
				return 2;
			}
			upstream[count++] = argv[i] + 9;
		} else {
			fprintf(stderr, "thincctl: unknown ntp config set option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "upstream");
	jw_arr_open(&w);
	for (i = 0; i < count; i++)
		jw_str(&w, upstream[i]);
	jw_arr_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/ntp", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_ntp_config);
}

static int cmd_ntp_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_ntp_config_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_ntp_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_ntp_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr,
	        "usage: thincctl ntp config [show]\n"
	        "       thincctl ntp config set [--server=A.B.C.D ...]\n");
	return 2;
}

static void fmt_ntp_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *synced_from = json_str_field(v, "synced_from");
	const struct json_value *last = json_object_get(v, "last_sync_unixtime");

	printf("state=%s", state != NULL ? state : "?");
	if (synced_from != NULL)
		printf(" synced_from=%s", synced_from);
	if (last != NULL && last->type == JSON_NUMBER)
		printf(" last_sync_unixtime=%lld", (long long)last->u.number);
	printf("\n");
}

static int cmd_ntp_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/ntp/status", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_status);
}

/* Triggers one sync attempt on demand rather than waiting for the
 * next hourly automatic fire -- 202 on success, no body; check
 * `ntp status` afterward for the outcome. */
static int cmd_ntp_sync(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/ntp/sync", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "sync failed to start",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	if (!json_mode)
		printf("sync started\n");
	else
		print_raw_json(r.json);
	kx_response_free(&r);
	return 0;
}

static void fmt_ntp_server_line(const struct json_value *v)
{
	const char *container = json_str_field(v, "container");

	printf("%s\n", container);
}

static void fmt_ntp_server_list(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY)
		return;
	for (i = 0; i < servers->u.array.count; i++)
		fmt_ntp_server_line(servers->u.array.items[i]);
}

static int cmd_ntp_server_register(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else {
			fprintf(stderr, "thincctl: unknown ntp server register option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (container == NULL) {
		fprintf(stderr, "usage: thincctl ntp server register --container=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/ntp/servers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_ntp_server_line);
}

static int cmd_ntp_server_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/ntp/servers", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_server_list);
}

static int cmd_ntp_server_unregister(const struct kx_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: ntp server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/ntp/servers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ntp_server(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl ntp server register --container=NAME\n"
		        "       thincctl ntp server ls\n"
		        "       thincctl ntp server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_ntp_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ntp_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_ntp_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ntp server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ntp(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl ntp config [show]\n"
		        "       thincctl ntp config set [--server=A.B.C.D ...]\n"
		        "       thincctl ntp status\n"
		        "       thincctl ntp sync  -- trigger a sync attempt now, don't wait for the\n"
		        "               next hourly automatic one\n"
		        "       thincctl ntp server register --container=NAME\n"
		        "       thincctl ntp server ls\n"
		        "       thincctl ntp server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "config") == 0)
		return cmd_ntp_config(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_ntp_status(c, json_mode);
	if (strcmp(sub, "sync") == 0)
		return cmd_ntp_sync(c, json_mode);
	if (strcmp(sub, "server") == 0)
		return cmd_ntp_server(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ntp subcommand '%s'\n", sub);
	return 2;
}

static void fmt_syslog_target_line(const struct json_value *v)
{
	const char *container = json_str_field(v, "container");

	printf("%s\n", container);
}

static void fmt_syslog_target_list(const struct json_value *v)
{
	const struct json_value *targets = json_object_get(v, "targets");
	size_t i;

	if (targets == NULL || targets->type != JSON_ARRAY)
		return;
	for (i = 0; i < targets->u.array.count; i++)
		fmt_syslog_target_line(targets->u.array.items[i]);
}

static int cmd_syslog_target_register(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else {
			fprintf(stderr, "thincctl: unknown syslog target register option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (container == NULL) {
		fprintf(stderr, "usage: thincctl syslog target register --container=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/syslog/targets", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_syslog_target_line);
}

static int cmd_syslog_target_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/syslog/targets", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_syslog_target_list);
}

static int cmd_syslog_target_unregister(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: syslog target unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/syslog/targets/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_syslog_target(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl syslog target register --container=NAME\n"
		        "       thincctl syslog target ls\n"
		        "       thincctl syslog target unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_syslog_target_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_syslog_target_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_syslog_target_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown syslog target subcommand '%s'\n", sub);
	return 2;
}

static int cmd_syslog(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl syslog target register --container=NAME  -- forward every\n"
		        "               container-sourced log line to this running container as a real\n"
		        "               RFC 3164 UDP syslog datagram (e.g. syslog-1/syslog-2 running\n"
		        "               sysklogd), alongside (never instead of) the consolidated log\n"
		        "               store every other 'thincctl logs' call already reads from\n"
		        "               (ADR-0127)\n"
		        "       thincctl syslog target ls\n"
		        "       thincctl syslog target unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "target") == 0)
		return cmd_syslog_target(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown syslog subcommand '%s'\n", sub);
	return 2;
}

static void fmt_time(const struct json_value *v)
{
	const struct json_value *unixtime = json_object_get(v, "unixtime");

	if (unixtime != NULL && unixtime->type == JSON_NUMBER)
		printf("unixtime=%lld\n", (long long)unixtime->u.number);
}

static int cmd_time_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/time", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_time);
}

static int cmd_time_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *unixtime = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--unixtime=", 11) == 0)
			unixtime = argv[i] + 11;
		else {
			fprintf(stderr, "thincctl: unknown time set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (unixtime == NULL) {
		fprintf(stderr, "usage: thincctl time set --unixtime=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "unixtime");
	jw_num(&w, atof(unixtime));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/time", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_time);
}

static int cmd_time(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_time_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_time_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_time_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr,
	        "usage: thincctl time [show]\n"
	        "       thincctl time set --unixtime=N\n");
	return 2;
}

static int cmd_swap_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/swap", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_swap);
}

static int cmd_swap_enable(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *size_mb = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--size-mb=", 10) == 0)
			size_mb = argv[i] + 10;
		else {
			fprintf(stderr, "thincctl: unknown swap enable option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (size_mb == NULL) {
		fprintf(stderr, "usage: thincctl swap enable --size-mb=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "size_mb");
	jw_int(&w, atol(size_mb));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/swap", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_swap);
}

static int cmd_swap_disable(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "DELETE", "/v1/system/swap", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

/* Forward-declared -- its own definition lives further down the file
 * (cmd_files_get's own neighborhood), needed here first for --container=/
 * --regex=, both free-text values that can contain characters ('&',
 * '=', regex metacharacters) a raw, unencoded query string would
 * corrupt. */
static void url_encode_query_value(const char *in, char *out, size_t out_size);

static int cmd_logs(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *source = NULL;
	const char *level = NULL;
	const char *container = NULL;
	const char *msg_regex = NULL;
	const char *tail = NULL;
	const char *since = NULL;
	char path[640];
	char encoded_container[64 * 3]; /* 64 matches LOGSTORE_CONTAINER_MAX (daemon/include/
	                                  * logstore.h) by value, no header dependency -- this
	                                  * is a pure REST client, API-First Mandate */
	char encoded_regex[256 * 3];
	int i;
	size_t o;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--source=", 9) == 0)
			source = argv[i] + 9;
		else if (strncmp(argv[i], "--level=", 8) == 0)
			level = argv[i] + 8;
		else if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--regex=", 8) == 0)
			msg_regex = argv[i] + 8;
		else if (strncmp(argv[i], "--tail=", 7) == 0)
			tail = argv[i] + 7;
		else if (strncmp(argv[i], "--since=", 8) == 0)
			since = argv[i] + 8;
		else {
			fprintf(stderr, "thincctl: unknown logs option '%s'\n", argv[i]);
			return 2;
		}
	}

	o = (size_t)snprintf(path, sizeof(path), "/v1/system/logs?");
	if (source != NULL)
		o += (size_t)snprintf(path + o, sizeof(path) - o, "source=%s&", source);
	if (level != NULL)
		o += (size_t)snprintf(path + o, sizeof(path) - o, "level=%s&", level);
	if (container != NULL) {
		url_encode_query_value(container, encoded_container, sizeof(encoded_container));
		o += (size_t)snprintf(path + o, sizeof(path) - o, "container=%s&", encoded_container);
	}
	if (msg_regex != NULL) {
		url_encode_query_value(msg_regex, encoded_regex, sizeof(encoded_regex));
		o += (size_t)snprintf(path + o, sizeof(path) - o, "regex=%s&", encoded_regex);
	}
	if (tail != NULL)
		o += (size_t)snprintf(path + o, sizeof(path) - o, "tail=%s&", tail);
	if (since != NULL)
		(void)snprintf(path + o, sizeof(path) - o, "since=%s&", since);

	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status == 400) {
		fprintf(stderr, "thincctl: %s\n",
		        json_str_field(r.json, "error") != NULL ? json_str_field(r.json, "error") :
		                                                   "bad request");
		kx_response_free(&r);
		return 1;
	}
	return emit(&r, json_mode, fmt_logs);
}

static int cmd_logs_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *max_bytes = NULL;
	const char *min_level = NULL;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-bytes=", 12) == 0)
			max_bytes = argv[i] + 12;
		else if (strncmp(argv[i], "--min-level=", 12) == 0)
			min_level = argv[i] + 12;
		else {
			fprintf(stderr, "thincctl: unknown logs config option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (max_bytes == NULL && min_level == NULL) {
		if (kx_client_request(c, "GET", "/v1/system/logs/config", NULL, &r) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_logs_config);
	}

	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		if (max_bytes != NULL) {
			jw_key(&w, "max_bytes");
			jw_int(&w, atoll(max_bytes));
		}
		if (min_level != NULL) {
			jw_key(&w, "min_level");
			jw_str(&w, min_level);
		}
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		if (kx_client_request(c, "PUT", "/v1/system/logs/config", w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
	}
	return emit(&r, json_mode, fmt_logs_config);
}

static int cmd_logs_top(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	if (argc > 0 && strcmp(argv[0], "config") == 0)
		return cmd_logs_config(c, json_mode, argc - 1, argv + 1);
	return cmd_logs(c, json_mode, argc, argv);
}

static int cmd_swap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_swap_status(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "status") == 0)
		return cmd_swap_status(c, json_mode);
	if (strcmp(sub, "enable") == 0)
		return cmd_swap_enable(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "disable") == 0)
		return cmd_swap_disable(c, json_mode);

	fprintf(stderr,
	        "usage: thincctl swap [status]\n"
	        "       thincctl swap enable --size-mb=N\n"
	        "       thincctl swap disable\n");
	return 2;
}

static int cmd_shutdown(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/shutdown", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_reboot(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/reboot", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static void fmt_update(const struct json_value *v)
{
	const struct json_value *updated = json_object_get(v, "updated");
	size_t i;

	printf("%s slot=%s updated=", json_str_field(v, "status"), json_str_field(v, "slot"));
	if (updated != NULL && updated->type == JSON_ARRAY) {
		for (i = 0; i < updated->u.array.count; i++) {
			printf("%s%s", i > 0 ? "," : "", json_as_string(updated->u.array.items[i]));
		}
	}
	printf("\n");
}

static int cmd_update(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *kernel = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--kernel=", 9) == 0)
			kernel = argv[i] + 9;
		else {
			fprintf(stderr, "thincctl: unknown update option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL && kernel == NULL) {
		fprintf(stderr, "usage: thincctl update [--image=PATH] [--kernel=PATH]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (image != NULL) {
		jw_key(&w, "image_path");
		jw_str(&w, image);
	}
	if (kernel != NULL) {
		jw_key(&w, "kernel_path");
		jw_str(&w, kernel);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/update", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_update);
}

static int cmd_ps(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/containers", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_list);
}

/* `container ls` -- a noun-based synonym for `ps` (identical output,
 * same GET /v1/containers), matching the <noun> <verb> shape every
 * other resource command here already uses (dns/ldap/ntp/syslog/
 * network/image/devicemap/pkg/pki) -- `ps`/`run`/`stop`/`rm`/etc. stay
 * exactly as they are, deliberately: Docker-familiar short verbs for
 * the operations themselves are their own real usability value, not
 * something this adds to replace, only to give a second, equally
 * discoverable entry point into for a plain "list what's running". */
static int cmd_container_recipe(const struct kx_client *c, int json_mode, int argc, char **argv);
static int cmd_container_apply_recipe(const struct kx_client *c, int json_mode, int argc,
                                       char **argv);

/*
 * ADR-0156/task #861: attach/detach a network on an already-running
 * container, live -- no recreate. See daemon/src/main.c's own doc
 * comment on handle_container_network_attach() for the full
 * live/ephemeral design reasoning.
 */
static int cmd_container_network_attach(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *name = NULL;
	const char *network = NULL;
	const char *ip = NULL;
	struct json_writer w;
	char path[300];
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--network=", 10) == 0)
			network = argv[i] + 10;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown container network attach option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || network == NULL) {
		fprintf(stderr,
		        "usage: thincctl container network attach NAME --network=NETWORK [--ip=A.B.C.D]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, network);
	if (ip != NULL) {
		jw_key(&w, "ip");
		jw_str(&w, ip);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/containers/%s/networks", name);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_network_detach(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 2) {
		fprintf(stderr, "usage: thincctl container network detach NAME NETWORK\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/networks/%s", argv[0], argv[1]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_network(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "attach") == 0)
		return cmd_container_network_attach(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "detach") == 0)
		return cmd_container_network_detach(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "usage: thincctl container network attach NAME --network=NETWORK [--ip=A.B.C.D]\n"
	                "       thincctl container network detach NAME NETWORK\n");
	return 2;
}

/* ADR-0161 Phase D: `thincctl container device attach|detach` -- the
 * manual REST primitive POST/DELETE /v1/containers/{name}/devices,
 * same shape as container network attach/detach above (a real,
 * callable-by-hand primitive first; Phase C's own hotplug listener is
 * just another, internal caller of the identical daemon-side
 * mechanism). */
static int cmd_container_device_attach(const struct kx_client *c, int json_mode, int argc,
                                        char **argv)
{
	struct kx_response r;
	char path[300];
	struct json_writer w;

	if (argc < 2) {
		fprintf(stderr, "usage: thincctl container device attach NAME ID\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "id");
	jw_str(&w, argv[1]);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/containers/%s/devices", argv[0]);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_device_detach(const struct kx_client *c, int json_mode, int argc,
                                        char **argv)
{
	struct kx_response r;
	char path[400];

	if (argc < 2) {
		fprintf(stderr, "usage: thincctl container device detach NAME ID\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/devices/%s", argv[0], argv[1]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_device(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "attach") == 0)
		return cmd_container_device_attach(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "detach") == 0)
		return cmd_container_device_detach(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "usage: thincctl container device attach NAME ID  -- a real device id or "
	                "devicemap name, resolved live\n"
	                "       thincctl container device detach NAME ID  -- refuses a device "
	                "granted at container creation (409)\n");
	return 2;
}

static int cmd_container(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "recipe") == 0)
		return cmd_container_recipe(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "apply-recipe") == 0)
		return cmd_container_apply_recipe(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "network") == 0)
		return cmd_container_network(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "device") == 0)
		return cmd_container_device(c, json_mode, argc - 1, argv + 1);
	if (argc < 1 || strcmp(argv[0], "ls") != 0) {
		fprintf(stderr, "usage: thincctl container ls  -- every provisioned container and its "
		                "current state (same as `ps`)\n"
		                "       thincctl container recipe add --name=NAME --file=PATH\n"
		                "       thincctl container recipe show|rm NAME / container recipe ls\n"
		                "       thincctl container apply-recipe NAME [--secret=KEY=VALUE ...]\n"
		                "       thincctl container network attach NAME --network=NETWORK "
		                "[--ip=A.B.C.D]\n"
		                "       thincctl container network detach NAME NETWORK\n"
		                "       thincctl container device attach NAME ID\n"
		                "       thincctl container device detach NAME ID\n");
		return 2;
	}
	return cmd_ps(c, json_mode);
}

static int cmd_inspect(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: inspect requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: rm requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_stop(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "thincctl: stop requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/stop", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

/*
 * ADR-0142 Section 4: same {"disk": "name"|null} contract and
 * fmt_storage_migrate_status() rendering every `storage <kind> migrate`
 * subcommand above already uses, narrowed to one container's own
 * overlay directory -- see main.c's handle_container_migrate_storage_
 * post()/get() for the full cutover this triggers daemon-side.
 */
static int cmd_migrate_storage(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	char path[300];
	struct json_writer w;
	struct kx_response r;
	int i;

	if (argc < 1) {
		fprintf(stderr, "thincctl: migrate-storage requires a container name\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown migrate-storage option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk");
	if (disk != NULL)
		jw_str(&w, disk);
	else
		jw_null(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/containers/%s/migrate-storage", argv[0]);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_migrate_storage_status(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct kx_response r;

	if (argc < 1) {
		fprintf(stderr, "thincctl: migrate-storage-status requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/migrate-storage", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_start(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "thincctl: start requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/start", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_pause(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "thincctl: pause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/pause", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_unpause(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "thincctl: unpause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/unpause", argv[0]);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

/*
 * Prints one cpu.pressure/io.pressure/memory.pressure object's own
 * "some"/"full" avg10/avg60/avg300 (percentages) -- shared by host and
 * per-container stats output.
 */
static void print_pressure_line(const char *label, const struct json_value *pressure)
{
	const struct json_value *some = json_object_get(pressure, "some");
	const struct json_value *full = json_object_get(pressure, "full");

	printf("%s.pressure some.avg10=%.2f some.avg60=%.2f some.avg300=%.2f "
	       "full.avg10=%.2f full.avg60=%.2f full.avg300=%.2f\n",
	       label,
	       json_as_number(json_object_get(some, "avg10")),
	       json_as_number(json_object_get(some, "avg60")),
	       json_as_number(json_object_get(some, "avg300")),
	       json_as_number(json_object_get(full, "avg10")),
	       json_as_number(json_object_get(full, "avg60")),
	       json_as_number(json_object_get(full, "avg300")));
}

/*
 * thincctl host-stats -- the host-wide counterpart to `stats NAME`,
 * same one-shot fetch-and-print/raw-counters convention (no rate or
 * percentage computed here; a live-refreshing view is the web
 * dashboard's own job).
 */
static void fmt_host_stats(const struct json_value *v)
{
	const struct json_value *load = json_object_get(v, "load");
	const struct json_value *cpu = json_object_get(v, "cpu");
	const struct json_value *mem = json_object_get(v, "memory");
	const struct json_value *disk = json_object_get(v, "disk");
	const struct json_value *nets = json_object_get(v, "networks");
	size_t i;

	printf("load1=%.2f load5=%.2f load15=%.2f\n",
	       json_as_number(json_object_get(load, "load1")),
	       json_as_number(json_object_get(load, "load5")),
	       json_as_number(json_object_get(load, "load15")));

	printf("cpu.user=%lld cpu.nice=%lld cpu.system=%lld cpu.idle=%lld "
	       "cpu.iowait=%lld cpu.irq=%lld cpu.softirq=%lld cpu.steal=%lld\n",
	       (long long)json_as_number(json_object_get(cpu, "user_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "nice_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "system_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "idle_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "iowait_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "irq_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "softirq_jiffies")),
	       (long long)json_as_number(json_object_get(cpu, "steal_jiffies")));
	print_pressure_line("cpu", json_object_get(cpu, "pressure"));

	printf("memory.total=%lld memory.free=%lld memory.available=%lld "
	       "memory.buffers=%lld memory.cached=%lld memory.swap_total=%lld memory.swap_free=%lld\n",
	       (long long)json_as_number(json_object_get(mem, "total_bytes")),
	       (long long)json_as_number(json_object_get(mem, "free_bytes")),
	       (long long)json_as_number(json_object_get(mem, "available_bytes")),
	       (long long)json_as_number(json_object_get(mem, "buffers_bytes")),
	       (long long)json_as_number(json_object_get(mem, "cached_bytes")),
	       (long long)json_as_number(json_object_get(mem, "swap_total_bytes")),
	       (long long)json_as_number(json_object_get(mem, "swap_free_bytes")));
	print_pressure_line("memory", json_object_get(mem, "pressure"));

	printf("disk.total=%lld disk.free=%lld disk.avail=%lld\n",
	       (long long)json_as_number(json_object_get(disk, "total_bytes")),
	       (long long)json_as_number(json_object_get(disk, "free_bytes")),
	       (long long)json_as_number(json_object_get(disk, "avail_bytes")));
	print_pressure_line("io", json_object_get(disk, "pressure"));

	if (nets != NULL && nets->type == JSON_ARRAY) {
		for (i = 0; i < nets->u.array.count; i++) {
			const struct json_value *n = nets->u.array.items[i];

			printf("network[%s]: rx_bytes=%lld tx_bytes=%lld rx_packets=%lld tx_packets=%lld\n",
			       json_str_field(n, "name"),
			       (long long)json_as_number(json_object_get(n, "rx_bytes")),
			       (long long)json_as_number(json_object_get(n, "tx_bytes")),
			       (long long)json_as_number(json_object_get(n, "rx_packets")),
			       (long long)json_as_number(json_object_get(n, "tx_packets")));
		}
	}
}

static int cmd_host_stats(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/stats", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_host_stats);
}

/* GET/DELETE /v1/system/processes (logging/web-UI epic Part 6,
 * ADR-0131) -- a real /proc scan, container column correlated
 * server-side via a ppid-chain walk. Same style as `ps`'s own
 * fmt_container_line() above: bare leading identity columns (pid,
 * comm), then key=value pairs, one line per entry, no header row. */
static void fmt_process_line(const struct json_value *v)
{
	long long pid = (long long)json_as_number(json_object_get(v, "pid"));
	long long ppid = (long long)json_as_number(json_object_get(v, "ppid"));
	long long uid = (long long)json_as_number(json_object_get(v, "user_id"));
	long long gid = (long long)json_as_number(json_object_get(v, "group_id"));
	const char *comm = json_str_field(v, "comm");
	const char *container = json_str_field(v, "container");
	const char *cmdline = json_str_field(v, "command_line");

	printf("%-8lld %-20s ppid=%-8lld uid=%-6lld gid=%-6lld container=%-16s cmd=%s\n", pid,
	       comm != NULL ? comm : "-", ppid, uid, gid,
	       container != NULL && container[0] != '\0' ? container : "-",
	       cmdline != NULL ? cmdline : "-");
}

static void fmt_process_list(const struct json_value *v)
{
	size_t i;

	if (v == NULL || v->type != JSON_ARRAY)
		return;
	for (i = 0; i < v->u.array.count; i++)
		fmt_process_line(v->u.array.items[i]);
}

static int cmd_process_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/processes", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_process_list);
}

static int cmd_process_kill(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[64];

	if (argc < 1) {
		fprintf(stderr, "thincctl: process kill requires a pid\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/processes/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_process(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl process ls  -- every real process on the box, correlated to a\n"
		        "               container by its own real host ppid chain (ADR-0131)\n"
		        "       thincctl process kill PID  -- a real, immediate SIGKILL; refuses pid 1\n"
		        "               and this daemon's own pid\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_process_ls(c, json_mode);
	if (strcmp(sub, "kill") == 0)
		return cmd_process_kill(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown process subcommand '%s'\n", sub);
	return 2;
}

static void fmt_ping(const struct json_value *v)
{
	const char *host = json_str_field(v, "host");
	const struct json_value *reachable_v = json_object_get(v, "reachable");
	const struct json_value *timed_out_v = json_object_get(v, "timed_out");

	if (reachable_v != NULL && reachable_v->type == JSON_BOOL && reachable_v->u.boolean) {
		printf("%s: reachable, rtt=%.2fms\n", host, json_as_number(json_object_get(v, "rtt_ms")));
	} else if (timed_out_v != NULL && timed_out_v->type == JSON_BOOL && timed_out_v->u.boolean) {
		printf("%s: unreachable (timed out)\n", host);
	} else {
		printf("%s: unreachable\n", host);
	}
}

/* Polls GET /v1/system/ping until state leaves "pending" -- same
 * shape as poll_bootstrap_fetch()/poll_hostbuild()/poll_iso(); the
 * server side itself is bounded to a fixed ~2s timeout, so this loop
 * always terminates. */
static int poll_ping(const struct kx_client *c, struct kx_response *out)
{
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", "/v1/system/ping", NULL, out) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "pending") != 0)
			return 0;
		kx_response_free(out);
		usleep(100000);
	}
}

static int cmd_ping(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct kx_response r;
	int rc;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl ping HOST\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "host");
	jw_str(&w, argv[0]);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/ping", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status == 409) {
		kx_response_free(&r);
		fprintf(stderr, "thincctl: another ping is already in flight\n");
		return 1;
	}
	if (r.status != 202) {
		return emit(&r, json_mode, fmt_ping);
	}
	kx_response_free(&r);

	if (poll_ping(c, &r) != 0)
		return 1;

	{
		const struct json_value *reachable_v = json_object_get(r.json, "reachable");
		int was_reachable = reachable_v != NULL && reachable_v->type == JSON_BOOL &&
		                     reachable_v->u.boolean;

		rc = emit(&r, json_mode, fmt_ping); /* frees r.json -- read reachable_v above first */
		if (rc == 0 && !was_reachable)
			rc = 1; /* unreachable is a real failure exit code, same as any other check command */
	}
	return rc;
}

/*
 * thincctl container stats <name> -- one-shot fetch-and-print, plain
 * key/value lines. Raw counters exactly as the daemon returns them
 * (ADR-0054: no rate/percentage computed here) -- a live-refreshing
 * view belongs to the web dashboard's own Stats tab, not this CLI.
 */
static void fmt_container_stats(const struct json_value *v)
{
	const struct json_value *cpu = json_object_get(v, "cpu");
	const struct json_value *mem = json_object_get(v, "memory");
	const struct json_value *disk = json_object_get(v, "disk");
	const struct json_value *nets = json_object_get(v, "networks");
	const struct json_value *max_v;
	size_t i;

	printf("cpu.usage_usec=%lld cpu.user_usec=%lld cpu.system_usec=%lld\n",
	       (long long)json_as_number(json_object_get(cpu, "usage_usec")),
	       (long long)json_as_number(json_object_get(cpu, "user_usec")),
	       (long long)json_as_number(json_object_get(cpu, "system_usec")));
	print_pressure_line("cpu", json_object_get(cpu, "pressure"));

	max_v = json_object_get(mem, "max");
	if (max_v == NULL || max_v->type == JSON_NULL) {
		printf("memory.current=%lld memory.peak=%lld memory.max=unlimited\n",
		       (long long)json_as_number(json_object_get(mem, "current")),
		       (long long)json_as_number(json_object_get(mem, "peak")));
	} else {
		printf("memory.current=%lld memory.peak=%lld memory.max=%lld\n",
		       (long long)json_as_number(json_object_get(mem, "current")),
		       (long long)json_as_number(json_object_get(mem, "peak")),
		       (long long)json_as_number(max_v));
	}
	print_pressure_line("memory", json_object_get(mem, "pressure"));

	printf("disk.upper_bytes=%lld disk.read_bytes=%lld disk.write_bytes=%lld "
	       "disk.read_ios=%lld disk.write_ios=%lld\n",
	       (long long)json_as_number(json_object_get(disk, "upper_bytes")),
	       (long long)json_as_number(json_object_get(disk, "read_bytes")),
	       (long long)json_as_number(json_object_get(disk, "write_bytes")),
	       (long long)json_as_number(json_object_get(disk, "read_ios")),
	       (long long)json_as_number(json_object_get(disk, "write_ios")));
	print_pressure_line("io", json_object_get(disk, "pressure"));

	if (nets != NULL && nets->type == JSON_ARRAY) {
		for (i = 0; i < nets->u.array.count; i++) {
			const struct json_value *n = nets->u.array.items[i];

			printf("network[%s]: rx_bytes=%lld tx_bytes=%lld rx_packets=%lld tx_packets=%lld\n",
			       json_str_field(n, "name"),
			       (long long)json_as_number(json_object_get(n, "rx_bytes")),
			       (long long)json_as_number(json_object_get(n, "tx_bytes")),
			       (long long)json_as_number(json_object_get(n, "rx_packets")),
			       (long long)json_as_number(json_object_get(n, "tx_packets")));
		}
	}
}

static int cmd_container_stats(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "thincctl: stats requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/%s/stats", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_stats);
}

static int cmd_console(const struct kx_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *cmd = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--cmd=", 6) == 0)
			cmd = argv[i] + 6;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown console option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl console NAME [--cmd=PATH]\n");
		return 2;
	}

	return kx_console_run(c, name, cmd) == 0 ? 0 : 1;
}

/* Matches daemon's CONTAINER_MAX_NETWORKS -- see include/container.h. */
#define CLI_MAX_NETWORKS 64
#define CLI_MAX_ROUTES 8
/* Matches daemon's CONTAINER_MAX_DEVICES -- see include/container.h. */
#define CLI_MAX_DEVICES 16
/* Matches daemon's CONTAINER_MAX_INTERFACES -- see include/container.h. */
#define CLI_MAX_INTERFACES 16
/* Matches daemon's CONTAINERDEF_MAX_DEPENDS -- see daemon/include/containerdef.h. */
#define CLI_MAX_DEPENDS 16
/* Matches daemon's CONTAINER_MAX_FILES -- see include/container.h. */
#define CLI_MAX_FILES 16
/* Matches daemon's CONTAINER_MAX_SYSCTLS -- see include/container.h. */
#define CLI_MAX_SYSCTLS 32
/* Matches daemon's RESOLV_MAX_NAMESERVERS -- see daemon/include/resolv.h (ADR-0143). */
#define CLI_MAX_DNS_SERVERS 3

struct cli_route {
	char dest[64];
	int prefix_len;
	char via[64];
};

struct cli_network_attach {
	char name[64];
	char ip[64];
	int has_ip;
};

/* Parses "NAME" or "NAME:IP" (e.g. "lan1:172.30.1.50") -- an explicit,
 * operator-chosen address instead of an auto-allocated one. Purely a
 * CLI presentation-syntax split, same spirit as parse_route_flag() --
 * whether ip is actually well-formed IPv4 is the daemon's job to
 * validate, not duplicated here. */
static int parse_network_flag(const char *s, struct cli_network_attach *out)
{
	const char *colon = strchr(s, ':');
	size_t name_len;

	memset(out, 0, sizeof(*out));
	if (colon == NULL) {
		if (s[0] == '\0' || strlen(s) >= sizeof(out->name))
			return -1;
		strcpy(out->name, s);
		return 0;
	}
	name_len = (size_t)(colon - s);
	if (name_len == 0 || name_len >= sizeof(out->name))
		return -1;
	memcpy(out->name, s, name_len);
	out->name[name_len] = '\0';
	if (strlen(colon + 1) == 0 || strlen(colon + 1) >= sizeof(out->ip))
		return -1;
	strcpy(out->ip, colon + 1);
	out->has_ip = 1;
	return 0;
}

/* Parses "DEST/PREFIX:VIA" (e.g. "172.34.0.0/24:172.33.0.5") into its
 * three parts. Purely a CLI presentation-syntax split -- whether
 * dest/via are actually well-formed IPv4 is the daemon's job to
 * validate, not duplicated here. */
static int parse_route_flag(const char *s, struct cli_route *out)
{
	const char *slash = strchr(s, '/');
	const char *colon;
	size_t dest_len, via_len;

	if (slash == NULL)
		return -1;
	colon = strchr(slash + 1, ':');
	if (colon == NULL)
		return -1;

	dest_len = (size_t)(slash - s);
	if (dest_len == 0 || dest_len >= sizeof(out->dest))
		return -1;
	memcpy(out->dest, s, dest_len);
	out->dest[dest_len] = '\0';

	out->prefix_len = atoi(slash + 1);

	via_len = strlen(colon + 1);
	if (via_len == 0 || via_len >= sizeof(out->via))
		return -1;
	strcpy(out->via, colon + 1);

	return 0;
}

struct cli_file_attach {
	char container_path[256]; /* matches daemon's CONTAINER_FILE_PATH_MAX */
	char local_path[PATH_MAX];
	char mode[8]; /* e.g. "0755"; empty means "let the daemon default it" */
	char owner[16]; /* raw numeric uid as text; empty means "let the daemon default it (root)" */
	char group[16]; /* raw numeric gid as text; empty means "let the daemon default it (root)" */
};

/*
 * Parses "CONTAINER_PATH=LOCAL_PATH[:MODE]" -- splits on the FIRST '='
 * (container_path never contains one; local paths could in principle,
 * though not in practice on Linux) and, if the text after the LAST
 * ':' in what remains is 1-4 octal digits, treats it as an explicit
 * mode override and strips it; otherwise the whole remainder is the
 * local path (no mode given, daemon defaults to 0644). File content
 * itself is read from local_path by the caller, not here -- this
 * function only splits the flag's own text. owner/group (ADR-0144)
 * are deliberately NOT part of this same flag's grammar: a numeric
 * uid/gid and a 1-4-digit octal mode look identical (e.g. "75" is
 * simultaneously a plausible uid AND a valid two-digit octal mode --
 * confirmed the hard way, a first attempt at layering OWNER:GROUP
 * onto this same trailing-colon heuristic silently mis-parsed real
 * values), so there is no way to add them here without real,
 * silent ambiguity. See parse_file_owner_flag()/--file-owner= below
 * instead -- a separate flag, matched by container_path, keeps this
 * one's own existing MODE-only grammar completely unambiguous.
 */
static int parse_file_flag(const char *s, struct cli_file_attach *out)
{
	const char *eq = strchr(s, '=');
	const char *local_start;
	const char *colon;
	size_t container_len, local_len;

	memset(out, 0, sizeof(*out));
	if (eq == NULL)
		return -1;
	container_len = (size_t)(eq - s);
	if (container_len == 0 || container_len >= sizeof(out->container_path))
		return -1;
	memcpy(out->container_path, s, container_len);
	out->container_path[container_len] = '\0';

	local_start = eq + 1;
	if (local_start[0] == '\0')
		return -1;

	colon = strrchr(local_start, ':');
	local_len = strlen(local_start);
	if (colon != NULL) {
		const char *m = colon + 1;
		size_t mode_len = strlen(m);
		int looks_octal = mode_len >= 1 && mode_len <= 4;
		size_t k;

		for (k = 0; looks_octal && k < mode_len; k++) {
			if (m[k] < '0' || m[k] > '7')
				looks_octal = 0;
		}
		if (looks_octal) {
			snprintf(out->mode, sizeof(out->mode), "%s", m);
			local_len = (size_t)(colon - local_start);
		}
	}
	if (local_len == 0 || local_len >= sizeof(out->local_path))
		return -1;
	memcpy(out->local_path, local_start, local_len);
	out->local_path[local_len] = '\0';

	return 0;
}

/*
 * Parses "--file-owner=CONTAINER_PATH:UID:GID" -- a separate flag
 * (not layered onto --file= itself, see that function's own comment
 * for why) that annotates an already-given --file= entry, matched by
 * its exact container_path. Applied after every --file= flag has been
 * parsed (run's own option loop calls this immediately, but the
 * actual match against `files[]` happens once all of them are known --
 * see the caller). Returns 0 and fills *out_uid/*out_gid, or -1 on a
 * malformed flag (not a matching container_path -- that's a runtime
 * "unknown --file-owner= target" error the caller reports instead).
 */
static int parse_file_owner_flag(const char *s, char *out_path, size_t out_path_size, long *out_uid,
                                  long *out_gid)
{
	const char *first_colon = strchr(s, ':');
	const char *second_colon;
	size_t path_len;
	char uid_buf[16], gid_buf[16];
	size_t uid_len, gid_len;

	if (first_colon == NULL)
		return -1;
	second_colon = strchr(first_colon + 1, ':');
	if (second_colon == NULL)
		return -1;

	path_len = (size_t)(first_colon - s);
	if (path_len == 0 || path_len >= out_path_size)
		return -1;
	memcpy(out_path, s, path_len);
	out_path[path_len] = '\0';

	uid_len = (size_t)(second_colon - (first_colon + 1));
	gid_len = strlen(second_colon + 1);
	if (uid_len == 0 || uid_len >= sizeof(uid_buf) || gid_len == 0 || gid_len >= sizeof(gid_buf) ||
	    strspn(first_colon + 1, "0123456789") != uid_len ||
	    strspn(second_colon + 1, "0123456789") != gid_len)
		return -1;
	memcpy(uid_buf, first_colon + 1, uid_len);
	uid_buf[uid_len] = '\0';
	snprintf(gid_buf, sizeof(gid_buf), "%s", second_colon + 1);

	*out_uid = atol(uid_buf);
	*out_gid = atol(gid_buf);
	return 0;
}

/* Reads path's entire content into a malloc'd buffer (NUL-terminated,
 * *out_len excludes the NUL -- matches json_as_string()'s own "plain C
 * string" shape everywhere else in this file). Returns 0, or -1 (with
 * perror on the failing path) if the file can't be opened/stat'd/read. */
static int read_local_file(const char *path, char **out_buf, size_t *out_len)
{
	FILE *f;
	long size;
	char *buf;

	f = fopen(path, "rb");
	if (f == NULL) {
		perror(path);
		return -1;
	}
	if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
		perror(path);
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)size + 1);
	if (buf == NULL) {
		fclose(f);
		return -1;
	}
	if (size > 0 && fread(buf, 1, (size_t)size, f) != (size_t)size) {
		perror(path);
		free(buf);
		fclose(f);
		return -1;
	}
	buf[size] = '\0';
	fclose(f);
	*out_buf = buf;
	*out_len = (size_t)size;
	return 0;
}

/*
 * Percent-encodes a query-string value (this CLI's first one -- see
 * daemon/src/main.c's own url_query_param(), the first query-string
 * *parser* this project has ever needed either). Only "/" needs
 * encoding for the file-read endpoint's own "path" values in practice
 * (container-relative paths are always "/"-leading), but every
 * non-alphanumeric byte is encoded for real correctness rather than
 * hand-picking just the one character known to matter today.
 */
static void url_encode_query_value(const char *in, char *out, size_t out_size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t oi = 0;

	while (*in != '\0' && oi + 1 < out_size) {
		unsigned char c = (unsigned char)*in;

		if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
			out[oi++] = (char)c;
		} else if (oi + 3 < out_size) {
			out[oi++] = '%';
			out[oi++] = hex[c >> 4];
			out[oi++] = hex[c & 0xf];
		} else {
			break;
		}
		in++;
	}
	out[oi] = '\0';
}

static int cmd_files_get(const struct kx_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *path_arg = NULL;
	const char *output = NULL;
	char encoded_path[256 * 3]; /* 256 matches daemon's CONTAINER_FILE_PATH_MAX */
	char path[400];
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--path=", 7) == 0)
			path_arg = argv[i] + 7;
		else if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown files get option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || path_arg == NULL) {
		fprintf(stderr, "usage: thincctl files get NAME --path=/some/path [--output=PATH]\n");
		return 2;
	}

	url_encode_query_value(path_arg, encoded_path, sizeof(encoded_path));
	snprintf(path, sizeof(path), "/v1/containers/%s/files?path=%s", name, encoded_path);

	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed", r.status);
		kx_response_free(&r);
		return 1;
	}

	/* Raw bytes, not JSON -- this is the CLI's one non-JSON response
	 * body (mirrors the daemon's own single non-JSON response
	 * primitive, http_write_response(), used only for this endpoint
	 * and the web dashboard's static assets). Written byte-for-byte,
	 * same "exact round trip" discipline cmd_backup()'s own --output=
	 * already established. */
	if (output != NULL) {
		FILE *f = fopen(output, "wb");

		if (f == NULL || (r.body != NULL && fwrite(r.body, 1, r.body_len, f) != r.body_len)) {
			perror(output);
			if (f != NULL)
				fclose(f);
			kx_response_free(&r);
			return 1;
		}
		fclose(f);
	} else if (r.body != NULL) {
		fwrite(r.body, 1, r.body_len, stdout);
	}

	kx_response_free(&r);
	return 0;
}

/*
 * PUT /v1/containers/{name}/files?path=... (ADR-0153, task #861): a
 * live, immediate hotfix to one file inside a running (or stopped-
 * but-defined) container -- deliberately not persisted into the
 * container's own definition (a future restart replays the original
 * persisted body unchanged). See the daemon's own doc comment on
 * handle_container_file_write() for why: the durable "this should
 * survive a recreate" path is a container recipe (`container recipe
 * add`/`container apply-recipe`), not this command.
 */
static int cmd_files_put(const struct kx_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *path_arg = NULL;
	const char *file_arg = NULL;
	const char *mode_arg = NULL;
	char *content = NULL;
	size_t content_len;
	char encoded_path[256 * 3];
	char path[400];
	struct json_writer w;
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--path=", 7) == 0)
			path_arg = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file_arg = argv[i] + 7;
		else if (strncmp(argv[i], "--mode=", 7) == 0)
			mode_arg = argv[i] + 7;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown files put option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || path_arg == NULL || file_arg == NULL) {
		fprintf(stderr,
		        "usage: thincctl files put NAME --path=/some/path --file=LOCAL_PATH "
		        "[--mode=0644]\n");
		return 2;
	}
	if (read_local_file(file_arg, &content, &content_len) != 0) {
		fprintf(stderr, "thincctl: could not read %s\n", file_arg);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "content");
	jw_str(&w, content);
	if (mode_arg != NULL) {
		jw_key(&w, "mode");
		jw_str(&w, mode_arg);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(content);

	url_encode_query_value(path_arg, encoded_path, sizeof(encoded_path));
	snprintf(path, sizeof(path), "/v1/containers/%s/files?path=%s", name, encoded_path);

	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed", r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("wrote %s on %s\n", path_arg, name);
	kx_response_free(&r);
	return 0;
}

static int cmd_files(const struct kx_client *c, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl files get NAME --path=/some/path [--output=PATH]\n"
		                "       thincctl files put NAME --path=/some/path --file=LOCAL_PATH "
		                "[--mode=0644]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "get") == 0)
		return cmd_files_get(c, argc - 1, argv + 1);
	if (strcmp(sub, "put") == 0)
		return cmd_files_put(c, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown files subcommand '%s'\n", sub);
	return 2;
}

static int cmd_backup(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *output = NULL;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else {
			fprintf(stderr, "thincctl: unknown backup option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (kx_client_request(c, "GET", "/v1/system/backup", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}

	if (output != NULL) {
		/* The raw response body IS the bundle `restore --input=` expects
		 * verbatim -- written byte-for-byte, not re-serialized through
		 * the JSON writer, so a backup/restore round trip is exact. */
		FILE *f = fopen(output, "wb");

		if (f == NULL || (r.body != NULL &&
		                   fwrite(r.body, 1, r.body_len, f) != r.body_len)) {
			perror(output);
			if (f != NULL)
				fclose(f);
			kx_response_free(&r);
			return 1;
		}
		fclose(f);
		printf("backup written to %s (%zu bytes)\n", output, r.body_len);
		kx_response_free(&r);
		return 0;
	}

	return emit(&r, json_mode, NULL);
}

static int cmd_restore(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *input = NULL;
	int i;
	char *buf;
	size_t len;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--input=", 8) == 0)
			input = argv[i] + 8;
		else {
			fprintf(stderr, "thincctl: unknown restore option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (input == NULL) {
		fprintf(stderr, "usage: thincctl restore --input=PATH\n");
		return 2;
	}
	if (read_local_file(input, &buf, &len) != 0) {
		fprintf(stderr, "thincctl: could not read %s\n", input);
		return 1;
	}

	if (kx_client_request(c, "POST", "/v1/system/restore", buf, &r) != 0) {
		free(buf);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	free(buf);

	return emit(&r, json_mode, fmt_health);
}

static void fmt_site_config(const struct json_value *v)
{
	const char *instance_name = json_str_field(v, "instance_name");
	const char *site_name = json_str_field(v, "site_name");
	const char *domain_suffix = json_str_field(v, "domain_suffix");

	printf("instance_name=%s site_name=%s domain_suffix=%s\n",
	       instance_name != NULL ? instance_name : "?",
	       site_name != NULL && site_name[0] != '\0' ? site_name : "(none)",
	       domain_suffix != NULL ? domain_suffix : "?");
}

static int cmd_site_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/site", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_site_config);
}

/*
 * PUT /v1/system/site requires all three fields together (server-side
 * "never silently drop a field the operator didn't mean to touch"
 * rule -- see siteconfig.h). So an operator changing just one field
 * (e.g. only --domain-suffix=) must not blow away the other two: this
 * fetches the current config first and only overrides what was
 * actually passed on the command line.
 */
static int cmd_site_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	char instance_name[128];
	char site_name[128];
	char domain_suffix[128];
	const char *new_instance_name = NULL;
	const char *new_site_name = NULL;
	const char *new_domain_suffix = NULL;
	const char *cur;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--instance-name=", 16) == 0)
			new_instance_name = argv[i] + 16;
		else if (strncmp(argv[i], "--site-name=", 12) == 0)
			new_site_name = argv[i] + 12;
		else if (strncmp(argv[i], "--domain-suffix=", 16) == 0)
			new_domain_suffix = argv[i] + 16;
		else {
			fprintf(stderr, "thincctl: unknown site set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (new_instance_name == NULL && new_site_name == NULL && new_domain_suffix == NULL) {
		fprintf(stderr, "usage: thincctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}

	if (kx_client_request(c, "GET", "/v1/system/site", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		fprintf(stderr, "thincctl: could not read current site config (HTTP %d)\n", r.status);
		kx_response_free(&r);
		return 1;
	}
	cur = json_str_field(r.json, "instance_name");
	snprintf(instance_name, sizeof(instance_name), "%s", cur != NULL ? cur : "thinc");
	cur = json_str_field(r.json, "site_name");
	snprintf(site_name, sizeof(site_name), "%s", cur != NULL ? cur : "");
	cur = json_str_field(r.json, "domain_suffix");
	snprintf(domain_suffix, sizeof(domain_suffix), "%s", cur != NULL ? cur : "internal");
	kx_response_free(&r);

	if (new_instance_name != NULL)
		snprintf(instance_name, sizeof(instance_name), "%s", new_instance_name);
	if (new_site_name != NULL)
		snprintf(site_name, sizeof(site_name), "%s", new_site_name);
	if (new_domain_suffix != NULL)
		snprintf(domain_suffix, sizeof(domain_suffix), "%s", new_domain_suffix);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "instance_name");
	jw_str(&w, instance_name);
	jw_key(&w, "site_name");
	jw_str(&w, site_name);
	jw_key(&w, "domain_suffix");
	jw_str(&w, domain_suffix);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/site", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_site_config);
}

static int cmd_site(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl site show\n"
		                "       thincctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_site_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_site_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown site subcommand '%s'\n", sub);
	return 2;
}

static void fmt_daemon_config(const struct json_value *v)
{
	const char *bind = json_str_field(v, "bind");
	const char *mgmt = json_str_field(v, "management_network");
	const char *bind_ip = json_str_field(v, "bind_ip");
	const struct json_value *jhttp = json_object_get(v, "http_enabled");
	const struct json_value *jhttps = json_object_get(v, "https_enabled");

	printf("port=%ld bind=%s bind_ip=%s management_network=%s http_enabled=%s "
	       "https_enabled=%s https_port=%ld\n",
	       (long)json_as_number(json_object_get(v, "port")), bind != NULL ? bind : "?",
	       bind_ip != NULL ? bind_ip : "(none -- using management network's own address)",
	       mgmt != NULL ? mgmt : "(none)",
	       (jhttp != NULL && jhttp->type == JSON_BOOL && jhttp->u.boolean) ? "true" : "false",
	       (jhttps != NULL && jhttps->type == JSON_BOOL && jhttps->u.boolean) ? "true" : "false",
	       (long)json_as_number(json_object_get(v, "https_port")));
}

static int cmd_daemon_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/daemon-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_daemon_config);
}

/*
 * Unlike PUT /v1/system/site, the daemon-config PUT is a genuine
 * partial update server-side (handle_daemon_config_put(), daemon/src/
 * main.c) -- fields not present in the body are left exactly as they
 * are. So this sends only what the operator actually gave on the
 * command line, no fetch-then-merge dance needed.
 */
static int cmd_daemon_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *port = NULL;
	const char *https_port = NULL;
	const char *management_network = NULL;
	const char *bind_ip = NULL;
	int clear_bind_ip = 0;
	int want_http = -1;  /* -1: untouched, 0: disable, 1: enable */
	int want_https = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--port=", 7) == 0)
			port = argv[i] + 7;
		else if (strncmp(argv[i], "--https-port=", 13) == 0)
			https_port = argv[i] + 13;
		else if (strncmp(argv[i], "--management-network=", 21) == 0)
			management_network = argv[i] + 21;
		else if (strncmp(argv[i], "--bind-ip=", 10) == 0)
			bind_ip = argv[i] + 10;
		else if (strcmp(argv[i], "--clear-bind-ip") == 0)
			clear_bind_ip = 1;
		else if (strcmp(argv[i], "--enable-http") == 0)
			want_http = 1;
		else if (strcmp(argv[i], "--disable-http") == 0)
			want_http = 0;
		else if (strcmp(argv[i], "--enable-https") == 0)
			want_https = 1;
		else if (strcmp(argv[i], "--disable-https") == 0)
			want_https = 0;
		else {
			fprintf(stderr, "thincctl: unknown daemon-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (bind_ip != NULL && clear_bind_ip) {
		fprintf(stderr, "thincctl: --bind-ip= and --clear-bind-ip are mutually exclusive\n");
		return 2;
	}
	if (port == NULL && https_port == NULL && management_network == NULL && bind_ip == NULL &&
	    !clear_bind_ip && want_http == -1 && want_https == -1) {
		fprintf(stderr,
		        "usage: thincctl daemon-config set [--port=N] [--https-port=N] "
		        "[--enable-http] [--disable-http] [--enable-https] [--disable-https] "
		        "[--management-network=NAME] [--bind-ip=A.B.C.D | --clear-bind-ip]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (port != NULL) {
		jw_key(&w, "port");
		jw_int(&w, atol(port));
	}
	if (https_port != NULL) {
		jw_key(&w, "https_port");
		jw_int(&w, atol(https_port));
	}
	if (management_network != NULL) {
		jw_key(&w, "management_network");
		jw_str(&w, management_network);
	}
	if (bind_ip != NULL) {
		jw_key(&w, "bind_ip");
		jw_str(&w, bind_ip);
	} else if (clear_bind_ip) {
		jw_key(&w, "bind_ip");
		jw_null(&w);
	}
	if (want_http != -1) {
		jw_key(&w, "http_enabled");
		jw_bool(&w, want_http);
	}
	if (want_https != -1) {
		jw_key(&w, "https_enabled");
		jw_bool(&w, want_https);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/daemon-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_daemon_config);
}

static int cmd_daemon_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl daemon-config show\n"
		                "       thincctl daemon-config set [--port=N] [--https-port=N] "
		                "[--enable-http] [--disable-http] [--enable-https] [--disable-https] "
		                "[--management-network=NAME] [--bind-ip=A.B.C.D | --clear-bind-ip]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_daemon_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_daemon_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown daemon-config subcommand '%s'\n", sub);
	return 2;
}

/* ADR-0141 Phase 5: thincctl backup-config show|set|status|snapshot-now
 * -- turns the `backup` disk role from a pure inert label into a real,
 * scheduled state-storage snapshot mechanism. */
static void fmt_backup_config(const struct json_value *v)
{
	const char *disk = json_str_field(v, "disk");
	const struct json_value *jenabled = json_object_get(v, "enabled");

	printf("disk=%s enabled=%s interval_hours=%ld\n", disk != NULL ? disk : "(none)",
	       (jenabled != NULL && jenabled->type == JSON_BOOL && jenabled->u.boolean) ? "true" : "false",
	       (long)json_as_number(json_object_get(v, "interval_hours")));
}

static void fmt_backup_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const struct json_value *jattempt = json_object_get(v, "last_attempt_unixtime");
	const char *error = json_str_field(v, "error");

	printf("%s", state != NULL ? state : "?");
	if (jattempt != NULL)
		printf(" last_attempt_unixtime=%lld", (long long)json_as_number(jattempt));
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

static int cmd_backup_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/backup-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_config);
}

static int cmd_backup_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	int clear_disk = 0;
	int want_enabled = -1; /* -1: not given */
	const char *interval_hours = NULL;
	struct json_writer w;
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else if (strcmp(argv[i], "--clear-disk") == 0)
			clear_disk = 1;
		else if (strcmp(argv[i], "--enable") == 0)
			want_enabled = 1;
		else if (strcmp(argv[i], "--disable") == 0)
			want_enabled = 0;
		else if (strncmp(argv[i], "--interval-hours=", 17) == 0)
			interval_hours = argv[i] + 17;
		else {
			fprintf(stderr, "thincctl: unknown backup-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (disk != NULL && clear_disk) {
		fprintf(stderr, "thincctl: --disk= and --clear-disk are mutually exclusive\n");
		return 2;
	}
	if (disk == NULL && !clear_disk && want_enabled == -1 && interval_hours == NULL) {
		fprintf(stderr,
		        "usage: thincctl backup-config set [--disk=NAME | --clear-disk] "
		        "[--enable | --disable] [--interval-hours=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (disk != NULL) {
		jw_key(&w, "disk");
		jw_str(&w, disk);
	} else if (clear_disk) {
		jw_key(&w, "disk");
		jw_null(&w);
	}
	if (want_enabled != -1) {
		jw_key(&w, "enabled");
		jw_bool(&w, want_enabled);
	}
	if (interval_hours != NULL) {
		jw_key(&w, "interval_hours");
		jw_int(&w, atol(interval_hours));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/backup-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_backup_config);
}

static int cmd_backup_config_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/backup-config/status", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_status);
}

static int cmd_backup_config_snapshot_now(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/system/backup-config/snapshot-now", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_status);
}

static int cmd_backup_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl backup-config show|set|status|snapshot-now\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_backup_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_backup_config_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_backup_config_status(c, json_mode);
	if (strcmp(sub, "snapshot-now") == 0)
		return cmd_backup_config_snapshot_now(c, json_mode);

	fprintf(stderr, "thincctl: unknown backup-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * Part 5 (ADR-0124): thincctl rolling-config show|set -- mirrors
 * cmd_daemon_config's own shape exactly, one field instead of several.
 */
static void fmt_rolling_config(const struct json_value *v)
{
	printf("jitter_window_seconds=%ld\n",
	       (long)json_as_number(json_object_get(v, "jitter_window_seconds")));
}

static int cmd_rolling_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/rolling-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_rolling_config);
}

static int cmd_rolling_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *window = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--jitter-window-seconds=", 24) == 0)
			window = argv[i] + 24;
		else {
			fprintf(stderr, "thincctl: unknown rolling-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (window == NULL) {
		fprintf(stderr, "usage: thincctl rolling-config set --jitter-window-seconds=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "jitter_window_seconds");
	jw_int(&w, atol(window));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/rolling-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_rolling_config);
}

static int cmd_rolling_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl rolling-config show\n"
		                "       thincctl rolling-config set --jitter-window-seconds=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_rolling_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_rolling_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown rolling-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * ADR-0157 Phase 3: thincctl pkg-build-config show|set -- mirrors
 * cmd_rolling_config's own shape exactly, one field instead of several.
 */
static void fmt_pkg_build_config(const struct json_value *v)
{
	printf("max_concurrent_jobs=%ld\n",
	       (long)json_as_number(json_object_get(v, "max_concurrent_jobs")));
}

static int cmd_pkg_build_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/pkg-build-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_build_config);
}

static int cmd_pkg_build_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *max_jobs = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-concurrent-jobs=", 23) == 0)
			max_jobs = argv[i] + 23;
		else {
			fprintf(stderr, "thincctl: unknown pkg-build-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (max_jobs == NULL) {
		fprintf(stderr, "usage: thincctl pkg-build-config set --max-concurrent-jobs=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "max_concurrent_jobs");
	jw_int(&w, atol(max_jobs));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/pkg-build-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_build_config);
}

static int cmd_pkg_build_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg-build-config show\n"
		                "       thincctl pkg-build-config set --max-concurrent-jobs=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_build_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_build_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown pkg-build-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * ADR-0134: thincctl tls-throttle show|set|status -- per-source-IP
 * throttling for repeated failed HTTPS handshakes. show/set mirror
 * daemon-config's own multi-field partial-update shape; status has no
 * daemon-config equivalent (there's nothing analogous to show there)
 * since it's live, read-only tracking state, not configuration.
 */
static void fmt_tls_throttle(const struct json_value *v)
{
	const struct json_value *jenabled = json_object_get(v, "enabled");

	printf("enabled=%s threshold=%ld window_seconds=%ld block_seconds=%ld log_interval_seconds=%ld\n",
	       (jenabled != NULL && jenabled->type == JSON_BOOL && jenabled->u.boolean) ? "true" : "false",
	       (long)json_as_number(json_object_get(v, "threshold")),
	       (long)json_as_number(json_object_get(v, "window_seconds")),
	       (long)json_as_number(json_object_get(v, "block_seconds")),
	       (long)json_as_number(json_object_get(v, "log_interval_seconds")));
}

static int cmd_tls_throttle_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/tls-throttle", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_tls_throttle);
}

static int cmd_tls_throttle_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *threshold = NULL;
	const char *window = NULL;
	const char *block = NULL;
	const char *log_interval = NULL;
	int want_enabled = -1; /* -1: untouched, 0: disable, 1: enable */
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--enabled") == 0)
			want_enabled = 1;
		else if (strcmp(argv[i], "--disabled") == 0)
			want_enabled = 0;
		else if (strncmp(argv[i], "--threshold=", 12) == 0)
			threshold = argv[i] + 12;
		else if (strncmp(argv[i], "--window-seconds=", 17) == 0)
			window = argv[i] + 17;
		else if (strncmp(argv[i], "--block-seconds=", 16) == 0)
			block = argv[i] + 16;
		else if (strncmp(argv[i], "--log-interval-seconds=", 23) == 0)
			log_interval = argv[i] + 23;
		else {
			fprintf(stderr, "thincctl: unknown tls-throttle set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (want_enabled == -1 && threshold == NULL && window == NULL && block == NULL && log_interval == NULL) {
		fprintf(stderr, "usage: thincctl tls-throttle set [--enabled | --disabled] "
		                "[--threshold=N] [--window-seconds=N] [--block-seconds=N] "
		                "[--log-interval-seconds=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (want_enabled != -1) {
		jw_key(&w, "enabled");
		jw_bool(&w, want_enabled);
	}
	if (threshold != NULL) {
		jw_key(&w, "threshold");
		jw_int(&w, atol(threshold));
	}
	if (window != NULL) {
		jw_key(&w, "window_seconds");
		jw_int(&w, atol(window));
	}
	if (block != NULL) {
		jw_key(&w, "block_seconds");
		jw_int(&w, atol(block));
	}
	if (log_interval != NULL) {
		jw_key(&w, "log_interval_seconds");
		jw_int(&w, atol(log_interval));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/system/tls-throttle", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_tls_throttle);
}

static void fmt_tls_throttle_status_line(const struct json_value *v)
{
	const struct json_value *jblocked = json_object_get(v, "blocked");
	const char *ip = json_str_field(v, "ip");
	long blocked_until = (long)json_as_number(json_object_get(v, "blocked_until"));

	printf("%s fail_count=%ld blocked=%s", ip != NULL ? ip : "?",
	       (long)json_as_number(json_object_get(v, "fail_count")),
	       (jblocked != NULL && jblocked->type == JSON_BOOL && jblocked->u.boolean) ? "true" : "false");
	if (blocked_until > 0)
		printf(" blocked_until=%ld", blocked_until);
	printf("\n");
}

static void fmt_tls_throttle_status(const struct json_value *v)
{
	const struct json_value *entries = json_object_get(v, "entries");
	size_t i;

	if (entries == NULL || entries->type != JSON_ARRAY)
		return;
	for (i = 0; i < entries->u.array.count; i++)
		fmt_tls_throttle_status_line(entries->u.array.items[i]);
}

static int cmd_tls_throttle_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/tls-throttle/status", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_tls_throttle_status);
}

static int cmd_tls_throttle(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl tls-throttle show\n"
		                "       thincctl tls-throttle set [--enabled | --disabled] "
		                "[--threshold=N] [--window-seconds=N] [--block-seconds=N] "
		                "[--log-interval-seconds=N]\n"
		                "       thincctl tls-throttle status\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_tls_throttle_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_tls_throttle_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_tls_throttle_status(c, json_mode);

	fprintf(stderr, "thincctl: unknown tls-throttle subcommand '%s'\n", sub);
	return 2;
}

/* ADR-0144: PUT /system/hostauth-config is full-replacement (unlike
 * tls-throttle's own partial-merge PUT above) -- admin_groups and
 * idle_timeout_seconds are always required in the body. "set" here
 * does a real client-side read-modify-write: GET the current config
 * first, apply only the flags actually given, then PUT the complete
 * result -- so `hostauth-config set --ldap-enable` alone doesn't
 * silently wipe an already-configured admin_groups list back to
 * empty. */
static void fmt_hostauth_config(const struct json_value *v)
{
	const struct json_value *jgroups = json_object_get(v, "admin_groups");
	const struct json_value *jldap_enabled = json_object_get(v, "ldap_enabled");
	const struct json_value *jservers = json_object_get(v, "ldap_servers");
	const char *base_dn = json_str_field(v, "ldap_base_dn");
	size_t i;

	printf("admin_groups=");
	if (jgroups != NULL && jgroups->type == JSON_ARRAY && jgroups->u.array.count > 0) {
		for (i = 0; i < jgroups->u.array.count; i++)
			printf("%s%s", i > 0 ? "," : "", json_as_string(jgroups->u.array.items[i]));
	} else {
		printf("-");
	}
	printf(" idle_timeout_seconds=%ld ldap_enabled=%s ldap_servers=",
	       (long)json_as_number(json_object_get(v, "idle_timeout_seconds")),
	       (jldap_enabled != NULL && jldap_enabled->type == JSON_BOOL && jldap_enabled->u.boolean)
	           ? "true"
	           : "false");
	if (jservers != NULL && jservers->type == JSON_ARRAY && jservers->u.array.count > 0) {
		for (i = 0; i < jservers->u.array.count; i++)
			printf("%s%s", i > 0 ? "," : "", json_as_string(jservers->u.array.items[i]));
	} else {
		printf("-");
	}
	printf(" ldap_port=%ld ldap_base_dn=%s\n",
	       (long)json_as_number(json_object_get(v, "ldap_port")),
	       base_dn != NULL && base_dn[0] != '\0' ? base_dn : "-");
}

static int cmd_hostauth_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/hostauth-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_hostauth_config);
}

static int cmd_hostauth_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *admin_groups[8];
	int admin_group_count = -1; /* -1: not given, keep current */
	const char *idle_timeout = NULL;
	int want_ldap_enabled = -1; /* -1: untouched, 0: disable, 1: enable */
	const char *ldap_servers[3];
	int ldap_server_count = -1;
	const char *ldap_port = NULL;
	const char *ldap_base_dn = NULL;
	int i;
	struct json_value *current;
	struct kx_response r;
	struct json_writer w;

	admin_group_count = 0;
	ldap_server_count = 0;
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--admin-group=", 14) == 0) {
			if (admin_group_count >= 8) {
				fprintf(stderr, "thincctl: too many --admin-group= flags (max 8)\n");
				return 2;
			}
			admin_groups[admin_group_count++] = argv[i] + 14;
		} else if (strncmp(argv[i], "--idle-timeout-seconds=", 23) == 0) {
			idle_timeout = argv[i] + 23;
		} else if (strcmp(argv[i], "--ldap-enable") == 0) {
			want_ldap_enabled = 1;
		} else if (strcmp(argv[i], "--ldap-disable") == 0) {
			want_ldap_enabled = 0;
		} else if (strncmp(argv[i], "--ldap-server=", 14) == 0) {
			if (ldap_server_count >= 3) {
				fprintf(stderr, "thincctl: too many --ldap-server= flags (max 3)\n");
				return 2;
			}
			ldap_servers[ldap_server_count++] = argv[i] + 14;
		} else if (strncmp(argv[i], "--ldap-port=", 12) == 0) {
			ldap_port = argv[i] + 12;
		} else if (strncmp(argv[i], "--ldap-base-dn=", 15) == 0) {
			ldap_base_dn = argv[i] + 15;
		} else {
			fprintf(stderr, "thincctl: unknown hostauth-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (admin_group_count == 0)
		admin_group_count = -1;
	if (ldap_server_count == 0)
		ldap_server_count = -1;
	if (admin_group_count == -1 && idle_timeout == NULL && want_ldap_enabled == -1 &&
	    ldap_server_count == -1 && ldap_port == NULL && ldap_base_dn == NULL) {
		fprintf(stderr,
		        "usage: thincctl hostauth-config set [--admin-group=NAME ...] "
		        "[--idle-timeout-seconds=N] [--ldap-enable | --ldap-disable] "
		        "[--ldap-server=HOST ...] [--ldap-port=N] [--ldap-base-dn=NAME]\n");
		return 2;
	}

	/* Read-modify-write: fetch the current config so any flag NOT
	 * given here is preserved exactly, not reset by the server's own
	 * full-replacement PUT semantics. */
	if (kx_client_request(c, "GET", "/v1/system/hostauth-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	current = r.json;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "admin_groups");
	jw_arr_open(&w);
	if (admin_group_count >= 0) {
		for (i = 0; i < admin_group_count; i++)
			jw_str(&w, admin_groups[i]);
	} else {
		const struct json_value *jcur = json_object_get(current, "admin_groups");

		if (jcur != NULL && jcur->type == JSON_ARRAY) {
			size_t j;

			for (j = 0; j < jcur->u.array.count; j++)
				jw_str(&w, json_as_string(jcur->u.array.items[j]));
		}
	}
	jw_arr_close(&w);
	jw_key(&w, "idle_timeout_seconds");
	jw_int(&w, idle_timeout != NULL ? atol(idle_timeout)
	                                 : (long)json_as_number(json_object_get(
	                                       current, "idle_timeout_seconds")));
	{
		const struct json_value *jcur_enabled = json_object_get(current, "ldap_enabled");
		int cur_enabled = jcur_enabled != NULL && jcur_enabled->type == JSON_BOOL &&
		                   jcur_enabled->u.boolean;

		jw_key(&w, "ldap_enabled");
		jw_bool(&w, want_ldap_enabled != -1 ? want_ldap_enabled : cur_enabled);
	}
	jw_key(&w, "ldap_servers");
	jw_arr_open(&w);
	if (ldap_server_count >= 0) {
		for (i = 0; i < ldap_server_count; i++)
			jw_str(&w, ldap_servers[i]);
	} else {
		const struct json_value *jcur = json_object_get(current, "ldap_servers");

		if (jcur != NULL && jcur->type == JSON_ARRAY) {
			size_t j;

			for (j = 0; j < jcur->u.array.count; j++)
				jw_str(&w, json_as_string(jcur->u.array.items[j]));
		}
	}
	jw_arr_close(&w);
	jw_key(&w, "ldap_port");
	jw_int(&w, ldap_port != NULL
	               ? atol(ldap_port)
	               : (long)json_as_number(json_object_get(current, "ldap_port")));
	{
		const char *cur_base_dn = json_str_field(current, "ldap_base_dn");

		jw_key(&w, "ldap_base_dn");
		jw_str(&w, ldap_base_dn != NULL ? ldap_base_dn : (cur_base_dn != NULL ? cur_base_dn : ""));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	kx_response_free(&r);

	if (kx_client_request(c, "PUT", "/v1/system/hostauth-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_hostauth_config);
}

static int cmd_hostauth_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl hostauth-config show\n"
		        "       thincctl hostauth-config set [--admin-group=NAME ...] "
		        "[--idle-timeout-seconds=N] [--ldap-enable | --ldap-disable] "
		        "[--ldap-server=HOST ...] [--ldap-port=N] [--ldap-base-dn=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_hostauth_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_hostauth_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown hostauth-config subcommand '%s'\n", sub);
	return 2;
}

static void fmt_hostauth_session_line(const struct json_value *v)
{
	const char *username = json_str_field(v, "username");
	const struct json_value *jexpires = json_object_get(v, "expires_in_seconds");

	printf("%-24s", username != NULL ? username : "");
	if (jexpires != NULL && jexpires->type == JSON_NUMBER)
		printf("expires_in=%lds\n", (long)json_as_number(jexpires));
	else
		printf("expires_in=- (single-use)\n");
}

static void fmt_hostauth_sessions_list(const struct json_value *v)
{
	const struct json_value *sessions = json_object_get(v, "sessions");
	size_t i;

	if (sessions == NULL || sessions->type != JSON_ARRAY)
		return;
	if (sessions->u.array.count == 0) {
		printf("no active sessions\n");
		return;
	}
	for (i = 0; i < sessions->u.array.count; i++)
		fmt_hostauth_session_line(sessions->u.array.items[i]);
}

static int cmd_hostauth_sessions_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/hostauth/sessions", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_hostauth_sessions_list);
}

static int cmd_hostauth_sessions_revoke(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl hostauth-sessions revoke USERNAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/system/hostauth/sessions/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_hostauth_sessions(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl hostauth-sessions ls\n"
		                "       thincctl hostauth-sessions revoke USERNAME  -- log out everywhere\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_hostauth_sessions_ls(c, json_mode);
	if (strcmp(sub, "revoke") == 0)
		return cmd_hostauth_sessions_revoke(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown hostauth-sessions subcommand '%s'\n", sub);
	return 2;
}

struct cli_sysctl {
	char key[128]; /* matches daemon's CONTAINER_SYSCTL_KEY_MAX */
	char value[64]; /* matches daemon's CONTAINER_SYSCTL_VALUE_MAX */
};

/* Parses "KEY=VALUE" (e.g. "net.ipv4.conf.all.rp_filter=0") -- whether
 * KEY is actually a safe net.* sysctl name is the daemon's job to
 * validate, not duplicated here (same "presentation-syntax split only"
 * precedent every other parse_*_flag() in this file already has). */
static int parse_sysctl_flag(const char *s, struct cli_sysctl *out)
{
	const char *eq = strchr(s, '=');
	size_t key_len, value_len;

	memset(out, 0, sizeof(*out));
	if (eq == NULL)
		return -1;
	key_len = (size_t)(eq - s);
	if (key_len == 0 || key_len >= sizeof(out->key))
		return -1;
	memcpy(out->key, s, key_len);
	out->key[key_len] = '\0';

	value_len = strlen(eq + 1);
	if (value_len == 0 || value_len >= sizeof(out->value))
		return -1;
	strcpy(out->value, eq + 1);

	return 0;
}

static int cmd_run(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	struct cli_network_attach networks[CLI_MAX_NETWORKS];
	int network_count = 0;
	const char *devices[CLI_MAX_DEVICES];
	int device_count = 0;
	/* ADR-0161 Phase B: --optional-device=ID's own separate list --
	 * kept apart from devices[] above rather than a shared array with
	 * a per-entry flag, so the existing --device= parsing/JSON-writing
	 * code above/below needed zero changes at all. */
	const char *optional_devices[CLI_MAX_DEVICES];
	int optional_device_count = 0;
	const char *interfaces[CLI_MAX_INTERFACES];
	int interface_count = 0;
	const char *restart = NULL;
	long restart_delay = -1;
	int follow_rolling = 0;
	long follow_rolling_jitter = -1;
	const char *depends_on[CLI_MAX_DEPENDS];
	int depends_on_count = 0;
	long readiness_tcp_port = -1;
	long readiness_timeout = -1;
	int ip_forward = 0;
	int dns_register = 0;
	int pki_issue = 0;
	const char *pki_cert_dir = NULL;
	long pki_days = -1;
	int ldap_provision = 0;
	const char *ldap_user = NULL;
	const char *ldap_group = NULL;
	long ldap_uid = -1;
	const char *ldap_secret_dir = NULL;
	struct cli_route routes[CLI_MAX_ROUTES];
	int route_count = 0;
	struct cli_file_attach files[CLI_MAX_FILES];
	int file_count = 0;
	struct {
		char path[256];
		long uid;
		long gid;
	} file_owners[CLI_MAX_FILES];
	int file_owner_count = 0;
	struct cli_sysctl sysctls[CLI_MAX_SYSCTLS];
	int sysctl_count = 0;
	long memory_max = -1;
	long pids_max = -1;
	const char *cpu_max = NULL;
	const char *cpuset_cpus = NULL;
	long long disk_quota_bytes = -1;
	const char *disk = NULL;
	const char *dns_servers[CLI_MAX_DNS_SERVERS];
	int dns_server_count = 0;
	int i = 0;
	int cmd_start = -1;
	struct json_writer w;
	struct kx_response r;

	while (i < argc) {
		if (strcmp(argv[i], "--") == 0) {
			cmd_start = i + 1;
			break;
		}
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--memory-max=", 13) == 0)
			memory_max = atol(argv[i] + 13);
		else if (strncmp(argv[i], "--pids-max=", 11) == 0)
			pids_max = atol(argv[i] + 11);
		else if (strncmp(argv[i], "--cpu-max=", 10) == 0)
			cpu_max = argv[i] + 10;
		else if (strncmp(argv[i], "--cpuset=", 9) == 0)
			cpuset_cpus = argv[i] + 9;
		else if (strncmp(argv[i], "--disk-quota=", 13) == 0)
			disk_quota_bytes = atoll(argv[i] + 13);
		else if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else if (strncmp(argv[i], "--network=", 10) == 0) {
			if (network_count >= CLI_MAX_NETWORKS) {
				fprintf(stderr, "thincctl: too many --network= flags (max %d)\n",
				        CLI_MAX_NETWORKS);
				return 2;
			}
			if (parse_network_flag(argv[i] + 10, &networks[network_count]) != 0) {
				fprintf(stderr, "thincctl: invalid --network= value '%s'\n", argv[i] + 10);
				return 2;
			}
			network_count++;
		} else if (strncmp(argv[i], "--device=", 9) == 0) {
			if (device_count >= CLI_MAX_DEVICES) {
				fprintf(stderr, "thincctl: too many --device= flags (max %d)\n",
				        CLI_MAX_DEVICES);
				return 2;
			}
			devices[device_count++] = argv[i] + 9;
		} else if (strncmp(argv[i], "--optional-device=", 18) == 0) {
			if (optional_device_count >= CLI_MAX_DEVICES) {
				fprintf(stderr, "thincctl: too many --optional-device= flags (max %d)\n",
				        CLI_MAX_DEVICES);
				return 2;
			}
			optional_devices[optional_device_count++] = argv[i] + 18;
		} else if (strncmp(argv[i], "--interface=", 12) == 0) {
			if (interface_count >= CLI_MAX_INTERFACES) {
				fprintf(stderr, "thincctl: too many --interface= flags (max %d)\n",
				        CLI_MAX_INTERFACES);
				return 2;
			}
			interfaces[interface_count++] = argv[i] + 12;
		} else if (strncmp(argv[i], "--restart=", 10) == 0) {
			restart = argv[i] + 10;
		} else if (strncmp(argv[i], "--restart-delay=", 16) == 0) {
			restart_delay = atol(argv[i] + 16);
		} else if (strcmp(argv[i], "--follow-rolling") == 0) {
			follow_rolling = 1;
		} else if (strncmp(argv[i], "--follow-rolling-jitter-seconds=", 32) == 0) {
			follow_rolling_jitter = atol(argv[i] + 32);
		} else if (strncmp(argv[i], "--depends-on=", 13) == 0) {
			if (depends_on_count >= CLI_MAX_DEPENDS) {
				fprintf(stderr, "thincctl: too many --depends-on= flags (max %d)\n",
				        CLI_MAX_DEPENDS);
				return 2;
			}
			depends_on[depends_on_count++] = argv[i] + 13;
		} else if (strncmp(argv[i], "--readiness-tcp-port=", 21) == 0) {
			readiness_tcp_port = atol(argv[i] + 21);
		} else if (strncmp(argv[i], "--readiness-timeout=", 20) == 0) {
			readiness_timeout = atol(argv[i] + 20);
		} else if (strcmp(argv[i], "--ip-forward") == 0) {
			ip_forward = 1;
		} else if (strcmp(argv[i], "--dns-register") == 0) {
			dns_register = 1;
		} else if (strcmp(argv[i], "--pki-issue") == 0) {
			pki_issue = 1;
		} else if (strncmp(argv[i], "--pki-cert-dir=", 15) == 0) {
			pki_cert_dir = argv[i] + 15;
		} else if (strncmp(argv[i], "--pki-days=", 11) == 0) {
			pki_days = atol(argv[i] + 11);
		} else if (strcmp(argv[i], "--ldap-provision") == 0) {
			ldap_provision = 1;
		} else if (strncmp(argv[i], "--ldap-user=", 12) == 0) {
			ldap_user = argv[i] + 12;
		} else if (strncmp(argv[i], "--ldap-group=", 13) == 0) {
			ldap_group = argv[i] + 13;
		} else if (strncmp(argv[i], "--ldap-uid=", 11) == 0) {
			ldap_uid = atol(argv[i] + 11);
		} else if (strncmp(argv[i], "--ldap-secret-dir=", 18) == 0) {
			ldap_secret_dir = argv[i] + 18;
		} else if (strncmp(argv[i], "--route=", 8) == 0) {
			if (route_count >= CLI_MAX_ROUTES) {
				fprintf(stderr, "thincctl: too many --route= flags (max %d)\n",
				        CLI_MAX_ROUTES);
				return 2;
			}
			if (parse_route_flag(argv[i] + 8, &routes[route_count]) != 0) {
				fprintf(stderr,
				        "thincctl: invalid --route= value '%s' (expected DEST/PREFIX:VIA)\n",
				        argv[i] + 8);
				return 2;
			}
			route_count++;
		} else if (strncmp(argv[i], "--file=", 7) == 0) {
			if (file_count >= CLI_MAX_FILES) {
				fprintf(stderr, "thincctl: too many --file= flags (max %d)\n", CLI_MAX_FILES);
				return 2;
			}
			if (parse_file_flag(argv[i] + 7, &files[file_count]) != 0) {
				fprintf(stderr,
				        "thincctl: invalid --file= value '%s' (expected "
				        "CONTAINER_PATH=LOCAL_PATH[:MODE])\n",
				        argv[i] + 7);
				return 2;
			}
			file_count++;
		} else if (strncmp(argv[i], "--file-owner=", 13) == 0) {
			if (file_owner_count >= CLI_MAX_FILES) {
				fprintf(stderr, "thincctl: too many --file-owner= flags (max %d)\n",
				        CLI_MAX_FILES);
				return 2;
			}
			if (parse_file_owner_flag(argv[i] + 13, file_owners[file_owner_count].path,
			                          sizeof(file_owners[file_owner_count].path),
			                          &file_owners[file_owner_count].uid,
			                          &file_owners[file_owner_count].gid) != 0) {
				fprintf(stderr,
				        "thincctl: invalid --file-owner= value '%s' (expected "
				        "CONTAINER_PATH:UID:GID)\n",
				        argv[i] + 13);
				return 2;
			}
			file_owner_count++;
		} else if (strncmp(argv[i], "--sysctl=", 9) == 0) {
			if (sysctl_count >= CLI_MAX_SYSCTLS) {
				fprintf(stderr, "thincctl: too many --sysctl= flags (max %d)\n",
				        CLI_MAX_SYSCTLS);
				return 2;
			}
			if (parse_sysctl_flag(argv[i] + 9, &sysctls[sysctl_count]) != 0) {
				fprintf(stderr,
				        "thincctl: invalid --sysctl= value '%s' (expected KEY=VALUE)\n",
				        argv[i] + 9);
				return 2;
			}
			sysctl_count++;
		} else if (strncmp(argv[i], "--dns-server=", 13) == 0) {
			if (dns_server_count >= CLI_MAX_DNS_SERVERS) {
				fprintf(stderr, "thincctl: too many --dns-server= flags (max %d)\n",
				        CLI_MAX_DNS_SERVERS);
				return 2;
			}
			dns_servers[dns_server_count++] = argv[i] + 13;
		} else {
			fprintf(stderr, "thincctl: unknown run option '%s'\n", argv[i]);
			return 2;
		}
		i++;
	}

	if (name == NULL || image == NULL || cmd_start < 0 || cmd_start >= argc) {
		fprintf(stderr,
		        "usage: thincctl run --name=NAME --image=IMAGE [--memory-max=N] "
		        "[--pids-max=N] [--cpu-max=\"QUOTA PERIOD\"] [--cpuset=0-1,3] "
		        "[--disk-quota=BYTES] [--disk=NAME] "
		        "[--network=NAME[:IP] ...] [--ip-forward] [--dns-register] "
		        "[--pki-issue] [--pki-cert-dir=PATH] [--pki-days=N] "
		        "[--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME] "
		        "[--ldap-uid=N] [--ldap-secret-dir=PATH] "
		        "[--route=DEST/PREFIX:VIA ...] [--device=ID ...] [--optional-device=ID ...] "
		        "[--interface=IFNAME ...] "
		        "[--restart=always|on-failure|unless-stopped] [--restart-delay=N] "
		        "[--follow-rolling] [--follow-rolling-jitter-seconds=N] "
		        "[--depends-on=NAME ...] "
		        "[--readiness-tcp-port=N [--readiness-timeout=N]] "
		        "[--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] "
		        "[--file-owner=CONTAINER_PATH:UID:GID ...] [--sysctl=KEY=VALUE ...] "
		        "[--dns-server=A.B.C.D ...] "
		        "-- CMD [ARGS...]\n");
		return 2;
	}

	/* Match each --file-owner= against its --file= target by
	 * container_path (see parse_file_owner_flag()'s own comment for
	 * why this is a separate flag, not part of --file='s own
	 * grammar) -- an unmatched --file-owner= is a real usage error,
	 * not silently ignored. */
	for (i = 0; i < file_owner_count; i++) {
		int j, matched = 0;

		for (j = 0; j < file_count; j++) {
			if (strcmp(files[j].container_path, file_owners[i].path) == 0) {
				snprintf(files[j].owner, sizeof(files[j].owner), "%ld", file_owners[i].uid);
				snprintf(files[j].group, sizeof(files[j].group), "%ld", file_owners[i].gid);
				matched = 1;
				break;
			}
		}
		if (!matched) {
			fprintf(stderr,
			        "thincctl: --file-owner=%s:... has no matching --file=%s=... entry\n",
			        file_owners[i].path, file_owners[i].path);
			return 2;
		}
	}

	/*
	 * Built with the json writer (jw_*), not snprintf string
	 * concatenation, because name/image/cmd come from arbitrary
	 * user-supplied argv and must be properly JSON-escaped.
	 */
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "image");
	jw_str(&w, image);
	jw_key(&w, "cmd");
	jw_arr_open(&w);
	for (i = cmd_start; i < argc; i++)
		jw_str(&w, argv[i]);
	jw_arr_close(&w);
	if (memory_max >= 0) {
		jw_key(&w, "memory_max");
		jw_int(&w, memory_max);
	}
	if (pids_max >= 0) {
		jw_key(&w, "pids_max");
		jw_int(&w, pids_max);
	}
	if (cpu_max != NULL) {
		jw_key(&w, "cpu_max");
		jw_str(&w, cpu_max);
	}
	if (cpuset_cpus != NULL) {
		jw_key(&w, "cpuset_cpus");
		jw_str(&w, cpuset_cpus);
	}
	if (disk_quota_bytes >= 0) {
		jw_key(&w, "disk_quota_bytes");
		jw_int(&w, disk_quota_bytes);
	}
	if (disk != NULL) {
		jw_key(&w, "disk");
		jw_str(&w, disk);
	}
	if (network_count > 0) {
		jw_key(&w, "networks");
		jw_arr_open(&w);
		for (i = 0; i < network_count; i++) {
			if (networks[i].has_ip) {
				jw_obj_open(&w);
				jw_key(&w, "name");
				jw_str(&w, networks[i].name);
				jw_key(&w, "ip");
				jw_str(&w, networks[i].ip);
				jw_obj_close(&w);
			} else {
				jw_str(&w, networks[i].name);
			}
		}
		jw_arr_close(&w);
	}
	if (ip_forward) {
		jw_key(&w, "ip_forward");
		jw_bool(&w, 1);
	}
	if (dns_register) {
		jw_key(&w, "dns_register");
		jw_bool(&w, 1);
	}
	if (pki_issue) {
		jw_key(&w, "pki_issue");
		jw_bool(&w, 1);
		if (pki_cert_dir != NULL) {
			jw_key(&w, "pki_cert_dir");
			jw_str(&w, pki_cert_dir);
		}
		if (pki_days >= 0) {
			jw_key(&w, "pki_days");
			jw_int(&w, pki_days);
		}
	}
	if (ldap_provision) {
		jw_key(&w, "ldap_provision");
		jw_bool(&w, 1);
		if (ldap_user != NULL) {
			jw_key(&w, "ldap_user");
			jw_str(&w, ldap_user);
		}
		if (ldap_group != NULL) {
			jw_key(&w, "ldap_group");
			jw_str(&w, ldap_group);
		}
		if (ldap_uid >= 0) {
			jw_key(&w, "ldap_uid");
			jw_int(&w, ldap_uid);
		}
		if (ldap_secret_dir != NULL) {
			jw_key(&w, "ldap_secret_dir");
			jw_str(&w, ldap_secret_dir);
		}
	}
	if (route_count > 0) {
		jw_key(&w, "routes");
		jw_arr_open(&w);
		for (i = 0; i < route_count; i++) {
			jw_obj_open(&w);
			jw_key(&w, "dest");
			jw_str(&w, routes[i].dest);
			jw_key(&w, "prefix_len");
			jw_int(&w, routes[i].prefix_len);
			jw_key(&w, "via");
			jw_str(&w, routes[i].via);
			jw_obj_close(&w);
		}
		jw_arr_close(&w);
	}
	if (device_count > 0 || optional_device_count > 0) {
		jw_key(&w, "devices");
		jw_arr_open(&w);
		for (i = 0; i < device_count; i++)
			jw_str(&w, devices[i]);
		for (i = 0; i < optional_device_count; i++) {
			jw_obj_open(&w);
			jw_key(&w, "id");
			jw_str(&w, optional_devices[i]);
			jw_key(&w, "optional");
			jw_bool(&w, 1);
			jw_obj_close(&w);
		}
		jw_arr_close(&w);
	}
	if (interface_count > 0) {
		jw_key(&w, "interfaces");
		jw_arr_open(&w);
		for (i = 0; i < interface_count; i++)
			jw_str(&w, interfaces[i]);
		jw_arr_close(&w);
	}
	if (restart != NULL) {
		jw_key(&w, "restart");
		jw_str(&w, restart);
	}
	if (restart_delay >= 0) {
		jw_key(&w, "restart_delay_seconds");
		jw_int(&w, restart_delay);
	}
	if (follow_rolling) {
		jw_key(&w, "follow_rolling");
		jw_bool(&w, 1);
	}
	if (follow_rolling_jitter >= 0) {
		jw_key(&w, "follow_rolling_jitter_seconds");
		jw_int(&w, follow_rolling_jitter);
	}
	if (depends_on_count > 0) {
		jw_key(&w, "depends_on");
		jw_arr_open(&w);
		for (i = 0; i < depends_on_count; i++)
			jw_str(&w, depends_on[i]);
		jw_arr_close(&w);
	}
	if (readiness_tcp_port >= 0) {
		jw_key(&w, "readiness");
		jw_obj_open(&w);
		jw_key(&w, "tcp_port");
		jw_int(&w, readiness_tcp_port);
		if (readiness_timeout >= 0) {
			jw_key(&w, "timeout_seconds");
			jw_int(&w, readiness_timeout);
		}
		jw_obj_close(&w);
	}
	if (file_count > 0) {
		jw_key(&w, "files");
		jw_arr_open(&w);
		for (i = 0; i < file_count; i++) {
			char *content;
			size_t content_len;

			if (read_local_file(files[i].local_path, &content, &content_len) != 0) {
				jw_free(&w);
				return 1;
			}
			jw_obj_open(&w);
			jw_key(&w, "path");
			jw_str(&w, files[i].container_path);
			jw_key(&w, "content");
			jw_str(&w, content);
			if (files[i].mode[0] != '\0') {
				jw_key(&w, "mode");
				jw_str(&w, files[i].mode);
			}
			if (files[i].owner[0] != '\0') {
				jw_key(&w, "owner");
				jw_int(&w, atol(files[i].owner));
			}
			if (files[i].group[0] != '\0') {
				jw_key(&w, "group");
				jw_int(&w, atol(files[i].group));
			}
			jw_obj_close(&w);
			free(content);
		}
		jw_arr_close(&w);
	}
	if (sysctl_count > 0) {
		jw_key(&w, "sysctls");
		jw_obj_open(&w);
		for (i = 0; i < sysctl_count; i++) {
			jw_key(&w, sysctls[i].key);
			jw_str(&w, sysctls[i].value);
		}
		jw_obj_close(&w);
	}
	if (dns_server_count > 0) {
		jw_key(&w, "dns_servers");
		jw_arr_open(&w);
		for (i = 0; i < dns_server_count; i++)
			jw_str(&w, dns_servers[i]);
		jw_arr_close(&w);
	}
	jw_obj_close(&w);

	/*
	 * kx_client_request() needs a NUL-terminated C string; w.buf isn't
	 * one, but jw_ensure()'s growth policy always keeps at least one
	 * spare byte of capacity beyond w.len, so writing the NUL directly
	 * here is safe without a further allocation.
	 */
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/containers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_network_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *subnet = NULL;
	const char *address = NULL;
	long prefix_len = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--subnet=", 9) == 0)
			subnet = argv[i] + 9;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix_len = atol(argv[i] + 9);
		else if (strncmp(argv[i], "--address=", 10) == 0)
			address = argv[i] + 10;
		else {
			fprintf(stderr, "thincctl: unknown network create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || subnet == NULL || prefix_len < 0) {
		fprintf(stderr,
		        "usage: thincctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
		        "[--address=A.B.C.D]\n"
		        "  no --address= means the bridge stays pure L2 (no host-owned address) --\n"
		        "  the default. Pass --address= only when the host itself should have an\n"
		        "  address on this network (e.g. to route through it).\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "subnet");
	jw_str(&w, subnet);
	jw_key(&w, "prefix_len");
	jw_int(&w, prefix_len);
	if (address != NULL) {
		jw_key(&w, "address");
		jw_str(&w, address);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/networks", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/networks", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_network_list);
}

static int cmd_network_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: network rm requires a network name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/networks/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_network_attach_interface(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	long vlan_id = 0;
	int i;
	struct json_writer w;
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: network attach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else if (strncmp(argv[i], "--vlan=", 7) == 0)
			vlan_id = atol(argv[i] + 7);
		else {
			fprintf(stderr, "thincctl: unknown network attach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr,
		        "usage: thincctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ifname");
	jw_str(&w, ifname);
	if (vlan_id != 0) {
		jw_key(&w, "vlan_id");
		jw_int(&w, vlan_id);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/networks/%s/interfaces", net_name);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_detach_interface(const struct kx_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	int i;
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: network detach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else {
			fprintf(stderr, "thincctl: unknown network detach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr, "usage: thincctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}

	snprintf(path, sizeof(path), "/v1/networks/%s/interfaces/%s", net_name, ifname);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_network(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
		        "[--address=A.B.C.D]\n"
		        "       thincctl network ls\n"
		        "       thincctl network rm NAME\n"
		        "       thincctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n"
		        "       thincctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_network_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_network_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_network_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "attach-interface") == 0)
		return cmd_network_attach_interface(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "detach-interface") == 0)
		return cmd_network_detach_interface(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown network subcommand '%s'\n", sub);
	return 2;
}

static int cmd_image_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown image create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: thincctl image create --name=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/images", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_image_line);
}

static int cmd_image_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/images", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_list);
}

static int cmd_image_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: image rm requires an image name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image_show(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl image show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/%s", argv[0]);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_detail);
}

/* ADR-0107: declares package intent on an image -- does not itself
 * trigger a rebuild (task #720's own job). */
static int cmd_image_manifest_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *package = NULL;
	const char *mode = NULL;
	const char *version = NULL;
	int i;
	char path[256];
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--package=", 10) == 0)
			package = argv[i] + 10;
		else if (strncmp(argv[i], "--mode=", 7) == 0)
			mode = argv[i] + 7;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else {
			fprintf(stderr, "thincctl: unknown image manifest set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL || package == NULL || mode == NULL || version == NULL) {
		fprintf(stderr,
		        "usage: thincctl image manifest set --image=NAME --package=NAME "
		        "--mode=pinned|rolling --version=VERSION\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "package");
	jw_str(&w, package);
	jw_key(&w, "mode");
	jw_str(&w, mode);
	jw_key(&w, "version");
	jw_str(&w, version);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/images/%s/manifest", image);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("%s@%s (%s) set on image '%s'\n", package, version, mode, image);
	kx_response_free(&r);
	return 0;
}

static int cmd_image_manifest_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *package = NULL;
	int i;
	char path[300];
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--package=", 10) == 0)
			package = argv[i] + 10;
		else {
			fprintf(stderr, "thincctl: unknown image manifest rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL || package == NULL) {
		fprintf(stderr, "usage: thincctl image manifest rm --image=NAME --package=NAME\n");
		return 2;
	}

	snprintf(path, sizeof(path), "/v1/images/%s/manifest/%s", image, package);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image_manifest(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl image manifest set --image=NAME --package=NAME "
		        "--mode=pinned|rolling --version=VERSION\n"
		        "       thincctl image manifest rm --image=NAME --package=NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "set") == 0)
		return cmd_image_manifest_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_image_manifest_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown image manifest subcommand '%s'\n", sub);
	return 2;
}

/* ---- ADR-0123: image recipes (Part 4 of the pkg/ redesign) ---- */

static void fmt_image_recipe_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");

	printf("%s\n", name != NULL ? name : "");
}

static void fmt_image_recipe_list(const struct json_value *v)
{
	const struct json_value *recipes = json_object_get(v, "recipes");
	size_t i;

	if (recipes == NULL || recipes->type != JSON_ARRAY)
		return;
	for (i = 0; i < recipes->u.array.count; i++)
		fmt_image_recipe_line(recipes->u.array.items[i]);
}

static int cmd_image_recipe_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/images/recipes", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_recipe_list);
}

/* Same shape as `pkg recipe add` -- --name= is the image name this
 * recipe declares intent for (recipe name == image name, a 1:1
 * relationship, ADR-0123), --file= a local path to the recipe text. */
static int cmd_image_recipe_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown image recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: thincctl image recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "thincctl: could not read %s\n", file);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(content);

	if (kx_client_request(c, "POST", "/v1/images/recipes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("image recipe '%s' added\n", name);
	kx_response_free(&r);
	return 0;
}

static void fmt_image_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

static int cmd_image_recipe_show(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown image recipe show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl image recipe show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/recipes/%s", name);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_recipe_show);
}

static int cmd_image_recipe_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown image recipe rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl image recipe rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/recipes/%s", name);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image_recipe(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl image recipe add --name=NAME --file=PATH\n"
		                "       thincctl image recipe show NAME\n"
		                "       thincctl image recipe rm NAME\n"
		                "       thincctl image recipe ls\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_image_recipe_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_image_recipe_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_image_recipe_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_image_recipe_ls(c, json_mode);

	fprintf(stderr, "thincctl: unknown image recipe subcommand '%s'\n", sub);
	return 2;
}

/*
 * Applies NAME's own already-stored recipe (ADR-0123): the common case
 * bulk-declares the manifest and returns immediately; a fully-pinned
 * recipe with a matching configured artifact server instead starts an
 * async whole-rootfs fetch -- 202 means poll `image recipe-apply-
 * status`, 204 means it already fully finished (nothing more to wait
 * for).
 */
static int cmd_image_apply_recipe(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown image apply-recipe option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl image apply-recipe NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/images/%s/apply-recipe", name);
	if (kx_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	if (r.status == 202)
		printf("recipe apply started for '%s' (async artifact fetch) -- poll `image "
		       "recipe-apply-status`\n",
		       name);
	else
		printf("recipe applied for '%s'\n", name);
	kx_response_free(&r);
	return 0;
}

static void fmt_image_recipe_apply_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *image = json_str_field(v, "image");
	const char *error = json_str_field(v, "error");

	printf("state=%s image=%s error=%s\n", state != NULL ? state : "unknown",
	       image != NULL ? image : "-", error != NULL ? error : "-");
}

static int cmd_image_recipe_apply_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/images/recipe-apply-status", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_recipe_apply_status);
}

/* ---- ADR-0151: container recipes ---- */

static void fmt_container_recipe_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");

	printf("%s\n", name != NULL ? name : "");
}

static void fmt_container_recipe_list(const struct json_value *v)
{
	const struct json_value *recipes = json_object_get(v, "recipes");
	size_t i;

	if (recipes == NULL || recipes->type != JSON_ARRAY)
		return;
	for (i = 0; i < recipes->u.array.count; i++)
		fmt_container_recipe_line(recipes->u.array.items[i]);
}

static int cmd_container_recipe_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/containers/recipes", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_recipe_list);
}

/* --name= is the recipe's own name AND must match the "name" field
 * already inside --file='s own content (ADR-0151, mirroring
 * pkg_recipe_add()'s pkg_name= contract) -- unlike an image recipe,
 * a container recipe's content is a real POST /v1/containers body, so
 * it necessarily already carries its own name. */
static int cmd_container_recipe_add(const struct kx_client *c, int json_mode, int argc,
                                     char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown container recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: thincctl container recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "thincctl: could not read %s\n", file);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(content);

	if (kx_client_request(c, "POST", "/v1/containers/recipes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("container recipe '%s' added\n", name);
	kx_response_free(&r);
	return 0;
}

static void fmt_container_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

static int cmd_container_recipe_show(const struct kx_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown container recipe show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl container recipe show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/recipes/%s", name);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_recipe_show);
}

static int cmd_container_recipe_rm(const struct kx_client *c, int json_mode, int argc,
                                    char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown container recipe rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl container recipe rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/containers/recipes/%s", name);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_container_recipe(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl container recipe add --name=NAME --file=PATH\n"
		                "       thincctl container recipe show NAME\n"
		                "       thincctl container recipe rm NAME\n"
		                "       thincctl container recipe ls\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_container_recipe_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_container_recipe_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_container_recipe_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_container_recipe_ls(c, json_mode);

	fprintf(stderr, "thincctl: unknown container recipe subcommand '%s'\n", sub);
	return 2;
}

/*
 * Applies NAME's own already-stored recipe (ADR-0151): renders it
 * (substituting any {{SECRET:KEY}} token from --secret=KEY=VALUE
 * flags) and creates the container -- a real, synchronous
 * POST /v1/containers under the hood, so this always returns 201 or a
 * real error, never an async job to poll (unlike image recipes' own
 * artifact-fetch fast path, which containers have no equivalent of).
 */
static int cmd_container_apply_recipe(const struct kx_client *c, int json_mode, int argc,
                                       char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	int i;
	struct json_writer w;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "secrets");
	jw_obj_open(&w);
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--secret=", 9) == 0) {
			char *eq = strchr(argv[i] + 9, '=');

			if (eq == NULL) {
				fprintf(stderr,
				        "thincctl: invalid --secret= value '%s' (expected KEY=VALUE)\n",
				        argv[i] + 9);
				jw_free(&w);
				return 2;
			}
			*eq = '\0';
			jw_key(&w, argv[i] + 9);
			jw_str(&w, eq + 1);
			*eq = '=';
		} else if (name == NULL) {
			name = argv[i];
		} else {
			fprintf(stderr, "thincctl: unknown container apply-recipe option '%s'\n", argv[i]);
			jw_free(&w);
			return 2;
		}
	}
	jw_obj_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (name == NULL) {
		fprintf(stderr,
		        "usage: thincctl container apply-recipe NAME [--secret=KEY=VALUE ...]\n");
		jw_free(&w);
		return 2;
	}

	snprintf(path, sizeof(path), "/v1/containers/recipes/%s/apply", name);
	if (kx_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_image(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl image create --name=NAME\n"
		                "       thincctl image ls\n"
		                "       thincctl image show NAME\n"
		                "       thincctl image rm NAME\n"
		                "       thincctl image manifest set --image=NAME --package=NAME "
		                "--mode=pinned|rolling --version=VERSION\n"
		                "       thincctl image manifest rm --image=NAME --package=NAME\n"
		                "       thincctl image recipe add --name=NAME --file=PATH\n"
		                "       thincctl image recipe show|rm NAME\n"
		                "       thincctl image recipe ls\n"
		                "       thincctl image apply-recipe NAME\n"
		                "       thincctl image recipe-apply-status\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_image_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_image_ls(c, json_mode);
	if (strcmp(sub, "show") == 0)
		return cmd_image_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_image_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "manifest") == 0)
		return cmd_image_manifest(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "recipe") == 0)
		return cmd_image_recipe(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "apply-recipe") == 0)
		return cmd_image_apply_recipe(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "recipe-apply-status") == 0)
		return cmd_image_recipe_apply_status(c, json_mode);

	fprintf(stderr, "thincctl: unknown image subcommand '%s'\n", sub);
	return 2;
}

static int cmd_device_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/devices", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_device_list);
}

static int cmd_device(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl device ls\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_device_ls(c, json_mode);

	fprintf(stderr, "thincctl: unknown device subcommand '%s'\n", sub);
	return 2;
}

static int cmd_diskrole_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name = NULL;
	const char *role = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk_name = argv[i] + 7;
		else if (strncmp(argv[i], "--role=", 7) == 0)
			role = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown diskrole create option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (disk_name == NULL || role == NULL) {
		fprintf(stderr,
		        "usage: thincctl diskrole create --disk=NAME --role=container-storage|backup|state-storage|rebuildable-storage|log-storage\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "disk_name");
	jw_str(&w, disk_name);
	jw_key(&w, "role");
	jw_str(&w, role);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/diskroles", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_diskrole_line);
}

static int cmd_diskrole_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/diskroles", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_diskrole_list);
}

static int cmd_diskrole_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: diskrole rm requires a disk name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/diskroles/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_diskrole(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl diskrole create --disk=NAME --role=container-storage|backup|state-storage|rebuildable-storage|log-storage\n"
		                "       thincctl diskrole ls\n"
		                "       thincctl diskrole rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_diskrole_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_diskrole_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_diskrole_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown diskrole subcommand '%s'\n", sub);
	return 2;
}

static int cmd_devicemap_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *kind = NULL;
	const char *selector = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--kind=", 7) == 0)
			kind = argv[i] + 7;
		else if (strncmp(argv[i], "--selector=", 11) == 0)
			selector = argv[i] + 11;
		else {
			fprintf(stderr, "thincctl: unknown devicemap create option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || kind == NULL || selector == NULL) {
		fprintf(stderr,
		        "usage: thincctl devicemap create --name=NAME --kind=exact|vendor_model "
		        "--selector=SELECTOR\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "kind");
	jw_str(&w, kind);
	jw_key(&w, "selector");
	jw_str(&w, selector);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/devicemaps", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_devicemap_line);
}

static int cmd_devicemap_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/devicemaps", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_devicemap_list);
}

static int cmd_devicemap_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: devicemap rm requires a mapping name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/devicemaps/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_devicemap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl devicemap create --name=NAME --kind=exact|vendor_model "
		        "--selector=SELECTOR\n"
		        "       thincctl devicemap ls\n"
		        "       thincctl devicemap rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_devicemap_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_devicemap_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_devicemap_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown devicemap subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_record_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *ip = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else {
			fprintf(stderr, "thincctl: unknown dns record create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || ip == NULL) {
		fprintf(stderr, "usage: thincctl dns record create --name=NAME --ip=A.B.C.D\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "ip");
	jw_str(&w, ip);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/dns/records", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_record_line);
}

static int cmd_dns_record_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/dns/records", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_record_list);
}

static int cmd_dns_record_update(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *ip = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;
	char path[256];

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else {
			fprintf(stderr, "thincctl: unknown dns record update option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || ip == NULL) {
		fprintf(stderr, "usage: thincctl dns record update --name=NAME --ip=A.B.C.D\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ip");
	jw_str(&w, ip);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/dns/records/%s", name);
	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_record_line);
}

static int cmd_dns_record_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: dns record rm requires a record name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/dns/records/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_record(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl dns record create --name=NAME --ip=A.B.C.D\n"
		        "       thincctl dns record update --name=NAME --ip=A.B.C.D\n"
		        "       thincctl dns record ls\n"
		        "       thincctl dns record rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_dns_record_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "update") == 0)
		return cmd_dns_record_update(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_dns_record_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_dns_record_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown dns record subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_server_register(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	const char *hosts_path = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--hosts-path=", 13) == 0)
			hosts_path = argv[i] + 13;
		else {
			fprintf(stderr, "thincctl: unknown dns server register option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (container == NULL || hosts_path == NULL) {
		fprintf(stderr,
		        "usage: thincctl dns server register --container=NAME --hosts-path=PATH\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_key(&w, "hosts_path");
	jw_str(&w, hosts_path);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/dns/servers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_server_line);
}

static int cmd_dns_server_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/dns/servers", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_server_list);
}

static int cmd_dns_server_unregister(const struct kx_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: dns server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/dns/servers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_server(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl dns server register --container=NAME --hosts-path=PATH\n"
		        "       thincctl dns server ls\n"
		        "       thincctl dns server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_dns_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_dns_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_dns_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown dns server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl dns record ...\n"
		                "       thincctl dns server ...\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "record") == 0)
		return cmd_dns_record(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "server") == 0)
		return cmd_dns_server(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown dns subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_server_register(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	const char *config_path = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--config-path=", 14) == 0)
			config_path = argv[i] + 14;
		else {
			fprintf(stderr, "thincctl: unknown ldap server register option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (container == NULL || config_path == NULL) {
		fprintf(stderr,
		        "usage: thincctl ldap server register --container=NAME --config-path=PATH\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_key(&w, "config_path");
	jw_str(&w, config_path);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/ldap/servers", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_server_line);
}

static int cmd_ldap_server_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/ldap/servers", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_server_list);
}

static int cmd_ldap_server_unregister(const struct kx_client *c, int json_mode, int argc,
                                       char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: ldap server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/ldap/servers/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_server(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl ldap server register --container=NAME --config-path=PATH\n"
		        "       thincctl ldap server ls\n"
		        "       thincctl ldap server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_ldap_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ldap_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_ldap_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ldap server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_group_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *gidnumber = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--gidnumber=", 12) == 0)
			gidnumber = argv[i] + 12;
		else {
			fprintf(stderr, "thincctl: unknown ldap group add option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: thincctl ldap group add --name=NAME [--gidnumber=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	/* gidnumber omitted (task #748): server auto-allocates from the
	 * configurable start_gid pool -- see `ldap config`. */
	if (gidnumber != NULL) {
		jw_key(&w, "gidnumber");
		jw_int(&w, strtol(gidnumber, NULL, 10));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/ldap/groups", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_group_line);
}

static int cmd_ldap_group_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/ldap/groups", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_group_list);
}

static int cmd_ldap_group_update(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *gidnumber = NULL;
	const char *new_name = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;
	char path[256];

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--gidnumber=", 12) == 0)
			gidnumber = argv[i] + 12;
		else if (strncmp(argv[i], "--new-name=", 11) == 0)
			new_name = argv[i] + 11;
		else {
			fprintf(stderr, "thincctl: unknown ldap group update option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || gidnumber == NULL) {
		fprintf(stderr,
		        "usage: thincctl ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "gidnumber");
	jw_int(&w, strtol(gidnumber, NULL, 10));
	if (new_name != NULL) {
		/* ADR-0147: renames the group -- if it's currently an admin
		 * group (hostauth-config's own admin_groups), the daemon side
		 * keeps that list pointed at the new name automatically, so a
		 * rename can never silently drop a group out of write-gating. */
		jw_key(&w, "name");
		jw_str(&w, new_name);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/ldap/groups/%s", name);
	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_group_line);
}

static int cmd_ldap_group_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: ldap group rm requires a group name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/ldap/groups/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_group(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl ldap group add --name=NAME [--gidnumber=N]\n"
		                "       thincctl ldap group update --name=NAME --gidnumber=N "
		                "[--new-name=NEWNAME]\n"
		                "       thincctl ldap group ls\n"
		                "       thincctl ldap group rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_ldap_group_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "update") == 0)
		return cmd_ldap_group_update(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ldap_group_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_ldap_group_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ldap group subcommand '%s'\n", sub);
	return 2;
}

/* Shared by "ldap user add" and a future "ldap user update" -- parses
 * every optional field thincctl exposes into a JSON request body. */
/* is_update: for `add`, --name= both selects the outgoing "name" field
 * and is the record's own real name being created, so it's written into
 * the body as-is. For `update`, --name= only ever identifies WHICH user
 * (the URL path) -- ADR-0147's own rename support is a distinct
 * --new-name= flag, so the body's own "name" key is written only when
 * that's given (a real rename request), never a same-value echo of
 * --name= that PUT's full-field-replacement semantics would otherwise
 * harmlessly (but pointlessly) re-assert on every update call. */
static void build_ldap_user_body(struct json_writer *w, int argc, char **argv, int is_update,
                                  const char **out_name)
{
	int i;
	const char *name = NULL;

	jw_obj_open(w);
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0) {
			name = argv[i] + 7;
			if (!is_update) {
				jw_key(w, "name");
				jw_str(w, name);
			}
		} else if (is_update && strncmp(argv[i], "--new-name=", 11) == 0) {
			jw_key(w, "name");
			jw_str(w, argv[i] + 11);
		} else if (strncmp(argv[i], "--uidnumber=", 12) == 0) {
			jw_key(w, "uidnumber");
			jw_int(w, strtol(argv[i] + 12, NULL, 10));
		} else if (strncmp(argv[i], "--primarygroup=", 15) == 0) {
			jw_key(w, "primarygroup");
			jw_int(w, strtol(argv[i] + 15, NULL, 10));
		} else if (strncmp(argv[i], "--secondary-groups=", 19) == 0) {
			/* ADR-0144: comma-separated gidnumbers, e.g.
			 * --secondary-groups=1000,10000 -- real secondary/
			 * supplementary group membership, alongside primarygroup. */
			char buf[256];
			char *tok, *save = NULL;

			jw_key(w, "secondary_groups");
			jw_arr_open(w);
			snprintf(buf, sizeof(buf), "%s", argv[i] + 19);
			for (tok = strtok_r(buf, ",", &save); tok != NULL; tok = strtok_r(NULL, ",", &save))
				jw_int(w, strtol(tok, NULL, 10));
			jw_arr_close(w);
		} else if (strncmp(argv[i], "--givenname=", 12) == 0) {
			jw_key(w, "givenname");
			jw_str(w, argv[i] + 12);
		} else if (strncmp(argv[i], "--sn=", 5) == 0) {
			jw_key(w, "sn");
			jw_str(w, argv[i] + 5);
		} else if (strncmp(argv[i], "--mail=", 7) == 0) {
			jw_key(w, "mail");
			jw_str(w, argv[i] + 7);
		} else if (strncmp(argv[i], "--loginshell=", 13) == 0) {
			jw_key(w, "loginshell");
			jw_str(w, argv[i] + 13);
		} else if (strncmp(argv[i], "--homedirectory=", 16) == 0) {
			jw_key(w, "homedirectory");
			jw_str(w, argv[i] + 16);
		} else if (strncmp(argv[i], "--password=", 11) == 0) {
			jw_key(w, "password");
			jw_str(w, argv[i] + 11);
		} else if (strcmp(argv[i], "--disabled") == 0) {
			jw_key(w, "disabled");
			jw_bool(w, 1);
		} else if (strncmp(argv[i], "--ssh-key=", 10) == 0) {
			jw_key(w, "ssh_public_key");
			jw_str(w, argv[i] + 10);
		} else if (strcmp(argv[i], "--can-search") == 0) {
			jw_key(w, "can_search");
			jw_bool(w, 1);
		}
	}
	jw_obj_close(w);
	*out_name = name;
}

static int cmd_ldap_user_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct kx_response r;
	const char *name;

	jw_init(&w);
	build_ldap_user_body(&w, argc, argv, 0, &name);
	if (name == NULL) {
		jw_free(&w);
		fprintf(stderr,
		        "usage: thincctl ldap user add --name=NAME [--uidnumber=N] "
		        "--primarygroup=N [--givenname=S] [--sn=S] [--mail=S] "
		        "[--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] "
		        "[--ssh-key=S] [--can-search]\n");
		return 2;
	}
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/ldap/users", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_user_line);
}

/* PUT is full-field-replacement (see handle_ldap_user_update()'s own doc
 * comment) -- an omitted field here resets to empty/0 on the server, same
 * as "ldap group update". */
static int cmd_ldap_user_update(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct kx_response r;
	const char *name;
	char path[256];

	jw_init(&w);
	build_ldap_user_body(&w, argc, argv, 1, &name);
	if (name == NULL) {
		jw_free(&w);
		fprintf(stderr,
		        "usage: thincctl ldap user update --name=NAME [--new-name=NEWNAME] "
		        "[--uidnumber=N] [--primarygroup=N] [--givenname=S] [--sn=S] [--mail=S] "
		        "[--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] "
		        "[--ssh-key=S] [--can-search]\n");
		return 2;
	}
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), "/v1/ldap/users/%s", name);
	if (kx_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_user_line);
}

static int cmd_ldap_user_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/ldap/users", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_user_list);
}

static int cmd_ldap_user_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: ldap user rm requires a user name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/ldap/users/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_user(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl ldap user add --name=NAME --uidnumber=N "
		                "--primarygroup=N ...\n"
		                "       thincctl ldap user update --name=NAME ...\n"
		                "       thincctl ldap user ls\n"
		                "       thincctl ldap user rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_ldap_user_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "update") == 0)
		return cmd_ldap_user_update(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ldap_user_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_ldap_user_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ldap user subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/ldap/config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_config_line);
}

static int cmd_ldap_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	long start_uid = -1, start_gid = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--start-uid=", 12) == 0)
			start_uid = strtol(argv[i] + 12, NULL, 10);
		else if (strncmp(argv[i], "--start-gid=", 12) == 0)
			start_gid = strtol(argv[i] + 12, NULL, 10);
		else {
			fprintf(stderr, "thincctl: unknown ldap config set option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (start_uid < 0 || start_gid < 0) {
		fprintf(stderr, "usage: thincctl ldap config set --start-uid=N --start-gid=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "start_uid");
	jw_int(&w, start_uid);
	jw_key(&w, "start_gid");
	jw_int(&w, start_gid);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/ldap/config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_config_line);
}

static int cmd_ldap_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl ldap config show\n"
		                "       thincctl ldap config set --start-uid=N --start-gid=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_ldap_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_ldap_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ldap config subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl ldap server ...\n"
		                "       thincctl ldap config ...\n"
		                "       thincctl ldap group ...\n"
		                "       thincctl ldap user ...\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "server") == 0)
		return cmd_ldap_server(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "config") == 0)
		return cmd_ldap_config(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "group") == 0)
		return cmd_ldap_group(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "user") == 0)
		return cmd_ldap_user(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown ldap subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_ca_bootstrap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "thincctl: unknown pki ca bootstrap option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (common_name != NULL) {
		jw_key(&w, "common_name");
		jw_str(&w, common_name);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/ca", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/ca", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pki ca bootstrap [--common-name=NAME] [--days=N]\n"
		                "       thincctl pki ca show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_ca_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_ca_show(c, json_mode);

	fprintf(stderr, "thincctl: unknown pki ca subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_intermediate_bootstrap(const struct kx_client *c, int json_mode, int argc,
                                           char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "thincctl: unknown pki intermediate bootstrap option '%s'\n",
			        argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (common_name != NULL) {
		jw_key(&w, "common_name");
		jw_str(&w, common_name);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/intermediate", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/intermediate", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pki intermediate bootstrap [--common-name=NAME] [--days=N]\n"
		                "       thincctl pki intermediate show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_intermediate_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_intermediate_show(c, json_mode);

	fprintf(stderr, "thincctl: unknown pki intermediate subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_cert_create(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *sans = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--sans=", 7) == 0)
			sans = argv[i] + 7;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "thincctl: unknown pki cert create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: thincctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	if (sans != NULL) {
		char buf[1024];
		char *tok, *save = NULL;

		snprintf(buf, sizeof(buf), "%s", sans);
		jw_key(&w, "sans");
		jw_arr_open(&w);
		tok = strtok_r(buf, ",", &save);
		while (tok != NULL) {
			jw_str(&w, tok);
			tok = strtok_r(NULL, ",", &save);
		}
		jw_arr_close(&w);
	}
	if (days >= 0) {
		jw_key(&w, "days");
		jw_int(&w, days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/certs", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_cert_issued);
}

static int cmd_pki_cert_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pki/certs", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_cert_list);
}

static int cmd_pki_cert_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: pki cert rm requires a cert name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pki/certs/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pki_cert(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n"
		                "       thincctl pki cert ls\n"
		                "       thincctl pki cert rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_pki_cert_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_pki_cert_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_pki_cert_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown pki cert subcommand '%s'\n", sub);
	return 2;
}

/*
 * Destructive: wipes and regenerates the whole CA chain (root, plus
 * the intermediate if one exists), reissuing every currently-tracked
 * leaf under the new chain. No separate --yes confirmation flag --
 * matches every other destructive thincctl subcommand's own direct-
 * execution convention (pki cert rm, network rm, ...); the web
 * dashboard's own confirm dialog is where the "are you sure" prompt
 * lives for this project.
 */
static int cmd_pki_reset(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *root_cn = NULL;
	const char *intermediate_cn = NULL;
	long root_days = -1;
	long intermediate_days = -1;
	long leaf_days = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--root-common-name=", 19) == 0)
			root_cn = argv[i] + 19;
		else if (strncmp(argv[i], "--intermediate-common-name=", 27) == 0)
			intermediate_cn = argv[i] + 27;
		else if (strncmp(argv[i], "--root-days=", 12) == 0)
			root_days = atol(argv[i] + 12);
		else if (strncmp(argv[i], "--intermediate-days=", 20) == 0)
			intermediate_days = atol(argv[i] + 20);
		else if (strncmp(argv[i], "--leaf-days=", 12) == 0)
			leaf_days = atol(argv[i] + 12);
		else {
			fprintf(stderr, "thincctl: unknown pki reset option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (root_cn != NULL) {
		jw_key(&w, "root_common_name");
		jw_str(&w, root_cn);
	}
	if (intermediate_cn != NULL) {
		jw_key(&w, "intermediate_common_name");
		jw_str(&w, intermediate_cn);
	}
	if (root_days >= 0) {
		jw_key(&w, "root_days");
		jw_int(&w, root_days);
	}
	if (intermediate_days >= 0) {
		jw_key(&w, "intermediate_days");
		jw_int(&w, intermediate_days);
	}
	if (leaf_days >= 0) {
		jw_key(&w, "leaf_days");
		jw_int(&w, leaf_days);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pki/reset", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_reset);
}

static int cmd_pki(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pki ca ...\n"
		                "       thincctl pki intermediate ...\n"
		                "       thincctl pki cert ...\n"
		                "       thincctl pki reset [--root-common-name=NAME]\n"
		                "               [--intermediate-common-name=NAME] [--root-days=N]\n"
		                "               [--intermediate-days=N] [--leaf-days=N]  -- wipes and\n"
		                "               regenerates the whole CA chain, reissuing every leaf\n"
		                "               currently tracked; defaults name each tier after this\n"
		                "               install's own domain_suffix (thincctl site show)\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ca") == 0)
		return cmd_pki_ca(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "intermediate") == 0)
		return cmd_pki_intermediate(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "cert") == 0)
		return cmd_pki_cert(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "reset") == 0)
		return cmd_pki_reset(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown pki subcommand '%s'\n", sub);
	return 2;
}

static void fmt_bootstrapped(const struct json_value *v)
{
	(void)v;
	printf("bootstrapped\n");
}

static void fmt_bootstrap_fetch_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *error = json_str_field(v, "error");

	printf("state=%s", state != NULL ? state : "?");
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/* Polls GET /v1/pkg/bootstrap until state leaves "fetching" -- --wait's
 * own loop for the toolchain_url mode, the same shape poll_hostbuild()/
 * poll_iso() already established. */
static int poll_bootstrap_fetch(const struct kx_client *c, struct kx_response *out)
{
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", "/v1/pkg/bootstrap", NULL, out) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "fetching") != 0)
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

static void fmt_pkg_recipe_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *version = json_str_field(v, "version");
	const char *depends = json_str_field(v, "depends");

	printf("%-24s %-16s %s\n", name, version, depends != NULL && depends[0] != '\0' ? depends : "-");
}

static void fmt_pkg_recipe_list(const struct json_value *v)
{
	const struct json_value *recipes = json_object_get(v, "recipes");
	size_t i;

	if (recipes == NULL || recipes->type != JSON_ARRAY)
		return;
	for (i = 0; i < recipes->u.array.count; i++)
		fmt_pkg_recipe_line(recipes->u.array.items[i]);
}

static void fmt_pkg_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *image = json_str_field(v, "image");
	const char *version = json_str_field(v, "version");
	const char *state = json_str_field(v, "state");
	const char *error = json_str_field(v, "error");
	const char *available = json_str_field(v, "available_version");

	printf("%-24s %-16s %-12s %-10s %-14s %s\n", name,
	       image != NULL && image[0] != '\0' ? image : "-", version, state,
	       available != NULL && available[0] != '\0' ? available : "-",
	       error != NULL && error[0] != '\0' ? error : "-");
}

static void fmt_pkg_list(const struct json_value *v)
{
	const struct json_value *packages = json_object_get(v, "packages");
	size_t i;

	if (packages == NULL || packages->type != JSON_ARRAY)
		return;
	for (i = 0; i < packages->u.array.count; i++)
		fmt_pkg_line(packages->u.array.items[i]);
}

static int cmd_pkg_bootstrap_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/bootstrap", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_bootstrap_fetch_status);
}

static int cmd_pkg_bootstrap(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *toolchain = NULL;
	const char *toolchain_url = NULL;
	const char *toolchain_sha256 = NULL;
	int wait = 0;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--toolchain=", 12) == 0)
			toolchain = argv[i] + 12;
		else if (strncmp(argv[i], "--toolchain-url=", 16) == 0)
			toolchain_url = argv[i] + 16;
		else if (strncmp(argv[i], "--toolchain-sha256=", 19) == 0)
			toolchain_sha256 = argv[i] + 19;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else {
			fprintf(stderr, "thincctl: unknown pkg bootstrap option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (toolchain_url != NULL) {
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "toolchain_url");
		jw_str(&w, toolchain_url);
		if (toolchain_sha256 != NULL) {
			jw_key(&w, "toolchain_sha256");
			jw_str(&w, toolchain_sha256);
		}
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		if (kx_client_request(c, "POST", "/v1/pkg/bootstrap", w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		if (r.status < 200 || r.status >= 300 || !wait)
			return emit(&r, json_mode, fmt_bootstrap_fetch_status);
		kx_response_free(&r);

		if (poll_bootstrap_fetch(c, &r) != 0)
			return 1;
		return emit(&r, json_mode, fmt_bootstrap_fetch_status);
	}

	if (toolchain == NULL) {
		if (kx_client_request(c, "POST", "/v1/pkg/bootstrap", NULL, &r) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return 1;
		}
	} else {
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "toolchain_path");
		jw_str(&w, toolchain);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		if (kx_client_request(c, "POST", "/v1/pkg/bootstrap", w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
	}
	return emit(&r, json_mode, fmt_bootstrapped);
}

/* ADR-0121: pkg/ redesign Part 2 -- configurable repo + pkg sync. */
static void fmt_pkg_repo_config(const struct json_value *v)
{
	const char *url = json_str_field(v, "repo_url");
	const char *kind = json_str_field(v, "repo_kind");
	const char *ref = json_str_field(v, "ref");
	const struct json_value *token_set = json_object_get(v, "auth_token_set");
	long interval = (long)json_as_number(json_object_get(v, "sync_interval_seconds"));

	if (url == NULL || url[0] == '\0') {
		printf("(no repo configured)\n");
		return;
	}
	printf("repo_url=%s repo_kind=%s ref=%s auth_token=%s sync_interval_seconds=%ld\n", url,
	       kind != NULL ? kind : "?", ref != NULL ? ref : "?",
	       (token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean) ? "set"
	                                                                                    : "unset",
	       interval);
}

static int cmd_pkg_repo_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/repo-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_repo_config);
}

static int cmd_pkg_repo_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *url = NULL;
	const char *kind = NULL;
	const char *ref = NULL;
	const char *token = NULL;
	long interval = -1;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--url=", 6) == 0)
			url = argv[i] + 6;
		else if (strncmp(argv[i], "--kind=", 7) == 0)
			kind = argv[i] + 7;
		else if (strncmp(argv[i], "--ref=", 6) == 0)
			ref = argv[i] + 6;
		else if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strcmp(argv[i], "--clear-token") == 0)
			token = "";
		else if (strncmp(argv[i], "--sync-interval=", 16) == 0)
			interval = strtol(argv[i] + 16, NULL, 10);
		else {
			fprintf(stderr, "thincctl: unknown pkg repo-config set option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (url != NULL) {
		jw_key(&w, "repo_url");
		jw_str(&w, url);
	}
	if (kind != NULL) {
		jw_key(&w, "repo_kind");
		jw_str(&w, kind);
	}
	if (ref != NULL) {
		jw_key(&w, "ref");
		jw_str(&w, ref);
	}
	if (token != NULL) {
		jw_key(&w, "auth_token");
		jw_str(&w, token);
	}
	if (interval >= 0) {
		jw_key(&w, "sync_interval_seconds");
		jw_int(&w, interval);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/pkg/repo-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_pkg_repo_config);
}

static int cmd_pkg_repo_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg repo-config show\n"
		                "       thincctl pkg repo-config set [--url=URL] [--kind=gitea|github|gitlab] "
		                "[--ref=REF] [--token=TOKEN | --clear-token] [--sync-interval=SECONDS]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_repo_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_repo_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "thincctl: unknown pkg repo-config subcommand '%s'\n", sub);
	return 2;
}

static void fmt_pkg_sync_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const struct json_value *last_attempt = json_object_get(v, "last_attempt");
	long added = (long)json_as_number(json_object_get(v, "added"));
	long skipped = (long)json_as_number(json_object_get(v, "skipped"));
	const char *error = json_str_field(v, "error");

	printf("state=%s", state != NULL ? state : "?");
	if (last_attempt != NULL && last_attempt->type != JSON_NULL)
		printf(" last_attempt=%lld", (long long)json_as_number(last_attempt));
	printf(" added=%ld skipped=%ld", added, skipped);
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/* Polls GET /v1/pkg/sync until state leaves "running" -- --wait's own
 * loop, the same shape poll_hostbuild()/poll_iso() already establish. */
static int poll_pkg_sync(const struct kx_client *c, struct kx_response *out)
{
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", "/v1/pkg/sync", NULL, out) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "running") != 0)
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

static int cmd_pkg_sync(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	int wait = 0;
	int i;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else {
			fprintf(stderr, "thincctl: unknown pkg sync option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (kx_client_request(c, "POST", "/v1/pkg/sync", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202) {
		int rc = emit(&r, json_mode, fmt_pkg_sync_status);

		return rc != 0 ? rc : 1;
	}
	kx_response_free(&r);

	if (wait) {
		if (poll_pkg_sync(c, &r) != 0)
			return 1;
	} else if (kx_client_request(c, "GET", "/v1/pkg/sync", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_sync_status);
}

static int cmd_pkg_sync_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/sync", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_sync_status);
}

static void fmt_pkg_cache_status(const struct json_value *v)
{
	long max_bytes = (long)json_as_number(json_object_get(v, "max_bytes"));
	long current_bytes = (long)json_as_number(json_object_get(v, "current_bytes"));
	long entry_count = (long)json_as_number(json_object_get(v, "entry_count"));

	printf("max_bytes=%ld current_bytes=%ld entry_count=%ld\n", max_bytes, current_bytes,
	       entry_count);
}

static int cmd_pkg_cache_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/cache-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_config_set(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	long max_bytes = -1;
	int i;
	char body[128];
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-bytes=", 12) == 0)
			max_bytes = strtol(argv[i] + 12, NULL, 10);
		else {
			fprintf(stderr, "thincctl: unknown pkg cache-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (max_bytes <= 0) {
		fprintf(stderr, "usage: thincctl pkg cache-config set --max-bytes=N\n");
		return 2;
	}

	snprintf(body, sizeof(body), "{\"max_bytes\":%ld}", max_bytes);
	if (kx_client_request(c, "PUT", "/v1/pkg/cache-config", body, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg cache-config show\n"
		                "       thincctl pkg cache-config set --max-bytes=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_cache_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_cache_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "thincctl: unknown pkg cache-config subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_cache_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/cache", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_clear(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "DELETE", "/v1/pkg/cache", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static void fmt_pkg_artifact_config(const struct json_value *v)
{
	const char *base_url = json_str_field(v, "base_url");
	const struct json_value *token_set = json_object_get(v, "auth_token_set");

	if (base_url == NULL || base_url[0] == '\0') {
		printf("(no artifact server configured)\n");
		return;
	}
	printf("base_url=%s auth_token=%s\n", base_url,
	       (token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean) ? "set"
	                                                                                    : "unset");
}

static int cmd_pkg_artifact_config_show(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/artifact-config", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_artifact_config);
}

static int cmd_pkg_artifact_config_set(const struct kx_client *c, int json_mode, int argc,
                                        char **argv)
{
	const char *base_url = NULL;
	const char *token = NULL;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--url=", 6) == 0)
			base_url = argv[i] + 6;
		else if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strcmp(argv[i], "--clear-token") == 0)
			token = "";
		else {
			fprintf(stderr, "thincctl: unknown pkg artifact-config set option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (base_url != NULL) {
		jw_key(&w, "base_url");
		jw_str(&w, base_url);
	}
	if (token != NULL) {
		jw_key(&w, "auth_token");
		jw_str(&w, token);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "PUT", "/v1/pkg/artifact-config", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_pkg_artifact_config);
}

static int cmd_pkg_artifact_config(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg artifact-config show\n"
		                "       thincctl pkg artifact-config set [--url=URL] "
		                "[--token=TOKEN | --clear-token]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_artifact_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_artifact_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "thincctl: unknown pkg artifact-config subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_recipes(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg/recipes", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_recipe_list);
}

/* ADR-0040: add or update a recipe on an already-running system, no
 * ISO rebuild/reinstall needed -- the real, ongoing way recipes get
 * onto a system. --name= is the lookup key (must match the .recipe
 * content's own pkg_name= field, validated server-side); --file= is a
 * local path to the .recipe file's content, read and embedded the
 * same way `run --file=` already stages container config files. */
static int cmd_pkg_recipe_add(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "thincctl: unknown pkg recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: thincctl pkg recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "thincctl: could not read %s\n", file);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "content");
	jw_str(&w, content);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(content);

	if (kx_client_request(c, "POST", "/v1/pkg/recipes", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		kx_response_free(&r);
		return 1;
	}
	printf("recipe '%s' added\n", name);
	kx_response_free(&r);
	return 0;
}

static void fmt_pkg_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

/* ADR-0107: an optional trailing --version=X selects a specific
 * published recipe version; omitted resolves to the highest available
 * version for NAME, matching every other "no version given" caller. */
static int cmd_pkg_recipe_show(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	const char *version = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown pkg recipe show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl pkg recipe show NAME [--version=VERSION]\n");
		return 2;
	}
	if (version != NULL)
		snprintf(path, sizeof(path), "/v1/pkg/recipes/%s?version=%s", name, version);
	else
		snprintf(path, sizeof(path), "/v1/pkg/recipes/%s", name);
	if (kx_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_recipe_show);
}

/* No --version= removes every published version of NAME; a specific
 * version removes only that one (ADR-0107). */
static int cmd_pkg_recipe_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];
	const char *name = NULL;
	const char *version = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown pkg recipe rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: thincctl pkg recipe rm NAME [--version=VERSION]\n");
		return 2;
	}
	if (version != NULL)
		snprintf(path, sizeof(path), "/v1/pkg/recipes/%s?version=%s", name, version);
	else
		snprintf(path, sizeof(path), "/v1/pkg/recipes/%s", name);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pkg_recipe(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg recipe add --name=NAME --file=PATH\n"
		                "       thincctl pkg recipe show NAME [--version=VERSION]\n"
		                "       thincctl pkg recipe rm NAME [--version=VERSION]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_pkg_recipe_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_recipe_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_pkg_recipe_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown pkg recipe subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_install(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	const char *version = NULL;
	int upgrade = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else {
			fprintf(stderr, "thincctl: unknown pkg install option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr,
		        "usage: thincctl pkg install --name=NAME [--image=IMAGE] "
		        "[--version=VERSION] [--upgrade]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	if (image != NULL) {
		jw_key(&w, "image");
		jw_str(&w, image);
	}
	/* ADR-0107: omitted resolves to the highest available version. */
	if (version != NULL) {
		jw_key(&w, "version");
		jw_str(&w, version);
	}
	if (upgrade) {
		jw_key(&w, "upgrade");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pkg/install", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_line);
}

/* Polls GET /v1/pkg/hostbuild/name until state leaves "fetching"/
 * "building" (--wait's own loop, and --deploy's own prerequisite --
 * it needs the finished artifact_path, not the 202's own in-flight
 * snapshot). Prints nothing itself; *out is the final response,
 * caller-owned (kx_response_free()'d by the caller). Returns 0 on a
 * real terminal state (installed/failed), -1 if the daemon became
 * unreachable mid-poll. */
static int poll_hostbuild(const struct kx_client *c, const char *name, struct kx_response *out)
{
	char path[300];

	snprintf(path, sizeof(path), "/v1/pkg/hostbuild/%s", name);
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", path, NULL, out) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || (strcmp(state, "fetching") != 0 && strcmp(state, "building") != 0))
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

/*
 * Real GET /system/boot read for the thinc bootroot-assembly
 * generation counters (task #737, ADR-0058/ADR-0104-adjacent fix).
 * Returns 0 with *out_completed/*out_running filled, -1 if the daemon
 * became unreachable.
 */
static int get_bootroot_assembly_generation(const struct kx_client *c, long *out_completed, int *out_running)
{
	struct kx_response r;
	const struct json_value *jrunning;

	if (kx_client_request(c, "GET", "/v1/system/boot", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return -1;
	}
	*out_completed = (long)json_as_number(json_object_get(r.json, "bootroot_assembly_completed_generation"));
	jrunning = json_object_get(r.json, "bootroot_assembly_running");
	*out_running = jrunning != NULL && jrunning->type == JSON_BOOL && jrunning->u.boolean;
	kx_response_free(&r);
	return 0;
}

/*
 * The real fix for task #737: `thincd-root.squashfs` existing is not
 * the same as it being *this* hostbuild round's own fresh artifact --
 * it's a leftover from whichever server-side bootroot assembly
 * (ADR-0057) last succeeded, which may still be a round or more behind
 * the hostbuild round --deploy just watched finish. Waits for the real
 * completed generation to advance past baseline_completed (captured by
 * the caller before this hostbuild round was even started) rather than
 * trusting file-exists. Distinguishes "still assembling" (keep polling)
 * from "that attempt is over and never advanced past baseline" (a real,
 * reported failure, not an infinite spin) via bootroot_assembly_running.
 * Returns 0 once a genuinely fresh artifact is confirmed on disk, -1
 * otherwise (daemon unreachable, or the assembly failed).
 */
static int wait_for_fresh_bootroot_assembly(const struct kx_client *c, long baseline_completed)
{
	for (;;) {
		long completed;
		int running;

		if (get_bootroot_assembly_generation(c, &completed, &running) != 0)
			return -1;
		if (completed > baseline_completed)
			return 0;
		if (!running) {
			fprintf(stderr,
			        "thincctl: thinc bootroot assembly did not produce a fresh artifact "
			        "(see GET /system/logs for the real failure)\n");
			return -1;
		}
		usleep(500000);
	}
}

static int cmd_pkg_hostbuild(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *build_image = NULL;
	const char *version = NULL;
	int wait = 0, deploy = 0, upgrade = 0;
	int i;
	struct json_writer w;
	struct kx_response r;
	long bootroot_baseline_completed = 0;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--build-image=", 14) == 0)
			build_image = argv[i] + 14;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else if (strcmp(argv[i], "--deploy") == 0)
			deploy = 1; /* implies --wait -- a not-yet-finished artifact has no path to deploy */
		else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "thincctl: unknown pkg hostbuild option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || build_image == NULL) {
		fprintf(stderr,
		        "usage: thincctl pkg hostbuild NAME --build-image=IMAGE [--version=VERSION] "
		        "[--wait] [--deploy] [--upgrade]\n");
		return 2;
	}
	if (deploy)
		wait = 1;

	/*
	 * task #737: capture the real baseline *before* this hostbuild
	 * round even starts -- comparing against a snapshot taken any
	 * later (e.g. right before the deploy step) could itself already
	 * be racing a slow-but-real assembly from an EARLIER round that
	 * just hasn't finished yet, which would wrongly look like "the
	 * baseline" to a check done later. "thinc" only: every other
	 * hostbuild name deploys straight from its own artifact_path with
	 * no server-side follow-on assembly to wait for.
	 */
	if (deploy && strcmp(name, "thinc") == 0) {
		int running;

		if (get_bootroot_assembly_generation(c, &bootroot_baseline_completed, &running) != 0)
			return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_key(&w, "build_image");
	jw_str(&w, build_image);
	if (version != NULL) {
		jw_key(&w, "version");
		jw_str(&w, version);
	}
	jw_key(&w, "upgrade");
	jw_bool(&w, upgrade);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/pkg/hostbuild", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300)
		return emit(&r, json_mode, fmt_pkg_line);

	if (!wait)
		return emit(&r, json_mode, fmt_pkg_line);
	kx_response_free(&r);

	if (poll_hostbuild(c, name, &r) != 0)
		return 1;

	/* emit() frees r internally -- can't use it after, so only call it
	 * on the terminal "we're done, nothing more to read out of r" path
	 * (an error response, or no --deploy requested). --deploy still
	 * needs artifact_path out of r afterward, so that path prints
	 * manually instead and keeps r alive a little longer. */
	if (r.status < 200 || r.status >= 300 || !deploy)
		return emit(&r, json_mode, fmt_pkg_line);

	if (json_mode)
		print_raw_json(r.json);
	else
		fmt_pkg_line(r.json);

	{
		const char *state = json_str_field(r.json, "state");
		const char *artifact_path = json_str_field(r.json, "artifact_path");
		char deploy_arg[PATH_MAX + 16];
		char *deploy_argv[1];
		int rc;

		if (state == NULL || strcmp(state, "installed") != 0 || artifact_path == NULL) {
			kx_response_free(&r);
			fprintf(stderr, "thincctl: hostbuild did not produce an artifact to deploy\n");
			return 1;
		}
		/* Honest, explicit per-name handling -- only what this
		 * mechanism has real recipes for today gets a real deploy
		 * path, not a fake-generic dispatcher pretending to support
		 * every possible hostbuild recipe name. */
		if (strcmp(name, "kernel") == 0)
			snprintf(deploy_arg, sizeof(deploy_arg), "--kernel=%s/bzImage", artifact_path);
		else if (strcmp(name, "thinc") == 0) {
			/*
			 * thincd-root.squashfs is assembled server-side by the
			 * daemon itself (ADR-0057), asynchronously, once this
			 * hostbuild's own artifacts finish harvesting -- state
			 * =="installed" only means pkg_build_completed() ran, not
			 * that the follow-on mkbootroot child has finished, let
			 * alone that it's THIS round's own attempt rather than a
			 * stale file left by an earlier one (task #737, the real
			 * gap this closes: file-exists is not the same as fresh).
			 * This CLI still never invokes mkbootroot itself
			 * (API-First Mandate) -- it just waits for the daemon's own
			 * real completed-generation counter (GET /system/boot,
			 * ADR-0057-follow-on) to confirm a genuinely new success
			 * before trusting the path below at all.
			 */
			if (wait_for_fresh_bootroot_assembly(c, bootroot_baseline_completed) != 0) {
				kx_response_free(&r);
				return 1;
			}
			snprintf(deploy_arg, sizeof(deploy_arg), "--image=%s/thincd-root.squashfs",
			         artifact_path);
		} else {
			kx_response_free(&r);
			fprintf(stderr, "thincctl: --deploy has no rule for hostbuild '%s' yet\n", name);
			return 1;
		}
		kx_response_free(&r);
		deploy_argv[0] = deploy_arg;
		rc = cmd_update(c, json_mode, 1, deploy_argv);
		return rc;
	}
}

/* thincctl kmod-build --build-image=IMAGE [--version=VERSION]
 * [--symbol=CONFIG_FOO ...] [--upgrade] [--wait]  -- ADR-0159 Phase B:
 * an ordinary `pkg hostbuild kernel` under the hood, reusing
 * poll_hostbuild()/fmt_pkg_line() completely unmodified (this is the
 * exact same "kernel" hostbuild entry `pkg hostbuild kernel` itself
 * produces, GET /v1/pkg/hostbuild/kernel either way). */
#define CLI_KMOD_BUILD_MAX_SYMBOLS 16

static int cmd_kmod_build(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *build_image = NULL;
	const char *version = NULL;
	const char *symbols[CLI_KMOD_BUILD_MAX_SYMBOLS];
	int symbol_count = 0;
	int wait = 0, upgrade = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--build-image=", 14) == 0)
			build_image = argv[i] + 14;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strncmp(argv[i], "--symbol=", 9) == 0) {
			if (symbol_count >= CLI_KMOD_BUILD_MAX_SYMBOLS) {
				fprintf(stderr, "thincctl: too many --symbol= flags (max %d)\n",
				        CLI_KMOD_BUILD_MAX_SYMBOLS);
				return 2;
			}
			symbols[symbol_count++] = argv[i] + 9;
		} else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else {
			fprintf(stderr, "thincctl: unknown kmod-build option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (build_image == NULL) {
		fprintf(stderr, "usage: thincctl kmod-build --build-image=IMAGE [--version=VERSION] "
		                "[--symbol=CONFIG_FOO ...] [--upgrade] [--wait]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "build_image");
	jw_str(&w, build_image);
	if (version != NULL) {
		jw_key(&w, "version");
		jw_str(&w, version);
	}
	if (symbol_count > 0) {
		jw_key(&w, "config_symbols");
		jw_arr_open(&w);
		for (i = 0; i < symbol_count; i++)
			jw_str(&w, symbols[i]);
		jw_arr_close(&w);
	}
	jw_key(&w, "upgrade");
	jw_bool(&w, upgrade);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/kmod-build", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300)
		return emit(&r, json_mode, fmt_pkg_line);
	if (!wait)
		return emit(&r, json_mode, fmt_pkg_line);
	kx_response_free(&r);

	if (poll_hostbuild(c, "kernel", &r) != 0)
		return 1;
	return emit(&r, json_mode, fmt_pkg_line);
}

static int cmd_pkg_ls(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/pkg", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_list);
}

static int cmd_pkg_rm(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	struct kx_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "thincctl: pkg rm requires a package name\n");
		return 2;
	}
	snprintf(path, sizeof(path), "/v1/pkg/%s", argv[0]);
	if (kx_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

/* "status" is only present in the "nothing to update" response --
 * pkg_get_one()'s own JSON shape (name/image/version/state/...) has no
 * such field, so its presence alone unambiguously picks which shape
 * this response is. */
static void fmt_pkg_update_all(const struct json_value *v)
{
	const char *status = json_str_field(v, "status");

	if (status != NULL) {
		printf("%s\n", status);
		return;
	}
	fmt_pkg_line(v);
}

static int cmd_pkg_update_all(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "POST", "/v1/pkg/update-all", "{}", &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_update_all);
}

/* task #676: live-tail the currently in-flight build's own output --
 * a thin wrapper, all the real work is kx_pkg_build_log_run()'s own
 * WS relay (client/src/console.c), same "cmd_* just calls the client
 * library" shape every other pkg subcommand here already has. */
static int cmd_pkg_build_log(const struct kx_client *c)
{
	return kx_pkg_build_log_run(c) == 0 ? 0 : 1;
}

static int cmd_pkg(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: thincctl pkg bootstrap [--toolchain=PATH]\n"
		                "       thincctl pkg bootstrap --toolchain-url=URL --toolchain-sha256=SHA256 [--wait]\n"
		                "       thincctl pkg bootstrap-status\n"
		                "       thincctl pkg recipes\n"
		                "       thincctl pkg recipe add --name=NAME --file=PATH\n"
		                "       thincctl pkg recipe show NAME [--version=VERSION]\n"
		                "       thincctl pkg recipe rm NAME [--version=VERSION]\n"
		                "       thincctl pkg install --name=NAME [--image=IMAGE] [--version=VERSION] "
		                "[--upgrade]\n"
		                "       thincctl pkg hostbuild NAME --build-image=IMAGE [--version=VERSION] "
		                "[--wait] [--deploy] [--upgrade]\n"
		                "       thincctl pkg build-log\n"
		                "       thincctl pkg ls\n"
		                "       thincctl pkg rm NAME[@IMAGE]\n"
		                "       thincctl pkg update-all\n"
		                "       thincctl pkg repo-config show\n"
		                "       thincctl pkg repo-config set [--url=URL] [--kind=gitea|github|gitlab] "
		                "[--ref=REF] [--token=TOKEN | --clear-token] [--sync-interval=SECONDS]\n"
		                "       thincctl pkg sync [--wait]\n"
		                "       thincctl pkg sync-status\n"
		                "       thincctl pkg cache-config show\n"
		                "       thincctl pkg cache-config set --max-bytes=N\n"
		                "       thincctl pkg cache-status\n"
		                "       thincctl pkg cache-clear\n"
		                "       thincctl pkg artifact-config show\n"
		                "       thincctl pkg artifact-config set [--url=URL] "
		                "[--token=TOKEN | --clear-token]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pkg_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "bootstrap-status") == 0)
		return cmd_pkg_bootstrap_status(c, json_mode);
	if (strcmp(sub, "recipes") == 0)
		return cmd_pkg_recipes(c, json_mode);
	if (strcmp(sub, "recipe") == 0)
		return cmd_pkg_recipe(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "install") == 0)
		return cmd_pkg_install(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "hostbuild") == 0)
		return cmd_pkg_hostbuild(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "build-log") == 0)
		return cmd_pkg_build_log(c);
	if (strcmp(sub, "ls") == 0)
		return cmd_pkg_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_pkg_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "update-all") == 0)
		return cmd_pkg_update_all(c, json_mode);
	if (strcmp(sub, "repo-config") == 0)
		return cmd_pkg_repo_config(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "sync") == 0)
		return cmd_pkg_sync(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "sync-status") == 0)
		return cmd_pkg_sync_status(c, json_mode);
	if (strcmp(sub, "cache-config") == 0)
		return cmd_pkg_cache_config(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "cache-status") == 0)
		return cmd_pkg_cache_status(c, json_mode);
	if (strcmp(sub, "cache-clear") == 0)
		return cmd_pkg_cache_clear(c, json_mode);
	if (strcmp(sub, "artifact-config") == 0)
		return cmd_pkg_artifact_config(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "thincctl: unknown pkg subcommand '%s'\n", sub);
	return 2;
}

static void fmt_iso_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *iso_path = json_str_field(v, "iso_path");
	const char *error = json_str_field(v, "error");

	printf("state=%s", state != NULL ? state : "?");
	if (iso_path != NULL)
		printf(" iso_path=%s", iso_path);
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
}

/* Polls GET /v1/system/iso until state leaves "building" -- --wait's own
 * loop, the same shape poll_hostbuild() already established. */
static int poll_iso(const struct kx_client *c, struct kx_response *out)
{
	for (;;) {
		const char *state;

		if (kx_client_request(c, "GET", "/v1/system/iso", NULL, out) != 0) {
			fprintf(stderr, "thincctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "building") != 0)
			return 0;
		kx_response_free(out);
		usleep(500000);
	}
}

static int cmd_iso_status(const struct kx_client *c, int json_mode)
{
	struct kx_response r;

	if (kx_client_request(c, "GET", "/v1/system/iso", NULL, &r) != 0) {
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_iso_status);
}

static int cmd_iso_build(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *prefix = NULL;
	const char *gateway = NULL;
	const char *interface = NULL;
	int wait = 0;
	int i;
	struct json_writer w;
	struct kx_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = argv[i] + 9;
		else if (strncmp(argv[i], "--gateway=", 10) == 0)
			gateway = argv[i] + 10;
		else if (strncmp(argv[i], "--interface=", 12) == 0)
			interface = argv[i] + 12;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else {
			fprintf(stderr, "thincctl: unknown iso build option '%s'\n", argv[i]);
			return 2;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (disk != NULL) {
		jw_key(&w, "disk");
		jw_str(&w, disk);
	}
	if (ip != NULL) {
		jw_key(&w, "ip");
		jw_str(&w, ip);
	}
	if (prefix != NULL) {
		jw_key(&w, "prefix");
		jw_str(&w, prefix);
	}
	if (gateway != NULL) {
		jw_key(&w, "gateway");
		jw_str(&w, gateway);
	}
	if (interface != NULL) {
		jw_key(&w, "interface");
		jw_str(&w, interface);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (kx_client_request(c, "POST", "/v1/system/iso", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300 || !wait)
		return emit(&r, json_mode, fmt_iso_status);
	kx_response_free(&r);

	if (poll_iso(c, &r) != 0)
		return 1;
	return emit(&r, json_mode, fmt_iso_status);
}

static int cmd_iso(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: thincctl iso build [--disk=DEV --ip=A.B.C.D --prefix=N "
		        "--gateway=A.B.C.D --interface=IFNAME] [--wait]\n"
		        "       thincctl iso status\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "build") == 0)
		return cmd_iso_build(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_iso_status(c, json_mode);

	fprintf(stderr, "thincctl: unknown iso subcommand '%s'\n", sub);
	return 2;
}

/*
 * ADR-0144: thincctl's own persisted-session support -- a single
 * dotfile (~/.thincctl_token, mode 0600, since it holds a live bearer
 * credential), read once at startup (see main()) and written by
 * cmd_login() / removed by cmd_logout(). No prior precedent for local
 * state in this CLI (it has always been a pure, stateless REST
 * client, per this file's own top-of-file comment) -- a session token
 * is the one thing that genuinely can't work any other way without
 * forcing every single command invocation to re-authenticate.
 */
static int token_file_path(char *out, size_t out_size)
{
	const char *home = getenv("HOME");

	if (home == NULL || home[0] == '\0')
		return -1;
	if ((size_t)snprintf(out, out_size, "%s/.thincctl_token", home) >= out_size)
		return -1;
	return 0;
}

static int load_token_file(char *out, size_t out_size)
{
	char path[PATH_MAX];
	FILE *f;
	size_t n;

	if (token_file_path(path, sizeof(path)) != 0)
		return -1;
	f = fopen(path, "r");
	if (f == NULL)
		return -1;
	n = fread(out, 1, out_size - 1, f);
	fclose(f);
	if (n == 0)
		return -1;
	out[n] = '\0';
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return out[0] != '\0' ? 0 : -1;
}

static int save_token_file(const char *token)
{
	char path[PATH_MAX];
	int fd;
	size_t len = strlen(token);

	if (token_file_path(path, sizeof(path)) != 0)
		return -1;
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0)
		return -1;
	if (write(fd, token, len) != (ssize_t)len) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static void clear_token_file(void)
{
	char path[PATH_MAX];

	if (token_file_path(path, sizeof(path)) == 0)
		unlink(path);
}

/* Reads a line from stdin into out, with terminal echo turned off for
 * the duration when stdin is a real terminal (a piped/scripted
 * invocation has no terminal to control, so it's read as plain input
 * instead -- the caller's own --password= flag is the right tool for
 * genuinely non-interactive use anyway). Strips a trailing newline.
 * Returns 0 on success, -1 on EOF/read error. */
static int read_line_noecho(const char *prompt, char *out, size_t out_size)
{
	struct termios saved, raw;
	int is_tty = isatty(STDIN_FILENO);
	size_t n;

	printf("%s", prompt);
	fflush(stdout);

	if (is_tty) {
		if (tcgetattr(STDIN_FILENO, &saved) != 0)
			return -1;
		raw = saved;
		raw.c_lflag &= ~(tcflag_t)ECHO;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}

	if (fgets(out, (int)out_size, stdin) == NULL) {
		if (is_tty)
			tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		return -1;
	}

	if (is_tty) {
		tcsetattr(STDIN_FILENO, TCSANOW, &saved);
		printf("\n");
	}

	n = strlen(out);
	while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
		out[--n] = '\0';
	return 0;
}

static int cmd_login(const struct kx_client *c, int json_mode, int argc, char **argv)
{
	const char *username = NULL;
	const char *password = NULL;
	char username_buf[128];
	char password_buf[256];
	struct json_writer w;
	struct kx_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--username=", 11) == 0)
			username = argv[i] + 11;
		else if (strncmp(argv[i], "--password=", 11) == 0)
			password = argv[i] + 11;
		else {
			fprintf(stderr, "usage: thincctl login [--username=NAME] [--password=PASS]\n");
			return 2;
		}
	}

	if (username == NULL) {
		if (read_line_noecho("Username: ", username_buf, sizeof(username_buf)) != 0) {
			fprintf(stderr, "thincctl: no username given\n");
			return 2;
		}
		username = username_buf;
	}
	if (password == NULL) {
		if (read_line_noecho("Password: ", password_buf, sizeof(password_buf)) != 0) {
			fprintf(stderr, "thincctl: no password given\n");
			return 2;
		}
		password = password_buf;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "username");
	jw_str(&w, username);
	jw_key(&w, "password");
	jw_str(&w, password);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	memset(&r, 0, sizeof(r));
	if (kx_client_request(c, "POST", "/v1/login", w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "thincctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status != 200) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "thincctl: login failed: %s (HTTP %d)\n", msg != NULL ? msg : "?",
		        r.status);
		kx_response_free(&r);
		return 1;
	}

	{
		const char *token = json_str_field(r.json, "token");

		if (token == NULL || token[0] == '\0') {
			fprintf(stderr, "thincctl: login response missing a token\n");
			kx_response_free(&r);
			return 1;
		}
		if (save_token_file(token) != 0) {
			fprintf(stderr,
			        "thincctl: warning: could not persist session token (%s) -- you'll "
			        "need to log in again for the next command\n",
			        strerror(errno));
		}
		if (json_mode)
			print_raw_json(r.json);
		else
			printf("Logged in as %s.\n", username);
	}
	kx_response_free(&r);
	return 0;
}

static int cmd_logout(const struct kx_client *c, int json_mode)
{
	char token[64];
	struct kx_response r;

	if (load_token_file(token, sizeof(token)) == 0) {
		memset(&r, 0, sizeof(r));
		if (kx_client_request_with_auth(c, "POST", "/v1/logout", token, NULL, &r) == 0)
			kx_response_free(&r);
	}
	clear_token_file();

	if (json_mode)
		printf("{}\n");
	else
		printf("Logged out.\n");
	return 0;
}

/*
 * The one dispatch table, shared by main()'s own one-shot invocation
 * and run_shell()'s interactive loop below -- extracted so both call
 * exactly the same code per command instead of two copies of this
 * chain drifting apart over time. argc/argv here are already just the
 * command's own remaining arguments (the leading "--host="-style
 * global flags and the command name itself are stripped by the
 * caller before this is reached).
 */
static int dispatch_command(const struct kx_client *client, int json_mode, const char *cmd,
                             int argc, char **argv)
{
	if (strcmp(cmd, "health") == 0)
		return cmd_health(client, json_mode);
	if (strcmp(cmd, "login") == 0)
		return cmd_login(client, json_mode, argc, argv);
	if (strcmp(cmd, "logout") == 0)
		return cmd_logout(client, json_mode);
	if (strcmp(cmd, "boot") == 0)
		return cmd_boot(client, json_mode);
	if (strcmp(cmd, "shutdown") == 0)
		return cmd_shutdown(client, json_mode);
	if (strcmp(cmd, "reboot") == 0)
		return cmd_reboot(client, json_mode);
	if (strcmp(cmd, "update") == 0)
		return cmd_update(client, json_mode, argc, argv);
	if (strcmp(cmd, "backup") == 0)
		return cmd_backup(client, json_mode, argc, argv);
	if (strcmp(cmd, "restore") == 0)
		return cmd_restore(client, json_mode, argc, argv);
	if (strcmp(cmd, "site") == 0)
		return cmd_site(client, json_mode, argc, argv);
	if (strcmp(cmd, "daemon-config") == 0)
		return cmd_daemon_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "backup-config") == 0)
		return cmd_backup_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "rolling-config") == 0)
		return cmd_rolling_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "pkg-build-config") == 0)
		return cmd_pkg_build_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "tls-throttle") == 0)
		return cmd_tls_throttle(client, json_mode, argc, argv);
	if (strcmp(cmd, "hostauth-config") == 0)
		return cmd_hostauth_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "hostauth-sessions") == 0)
		return cmd_hostauth_sessions(client, json_mode, argc, argv);
	if (strcmp(cmd, "iso") == 0)
		return cmd_iso(client, json_mode, argc, argv);
	if (strcmp(cmd, "routes") == 0)
		return cmd_routes(client, json_mode, argc, argv);
	if (strcmp(cmd, "disks") == 0)
		return cmd_disks(client, json_mode, argc, argv);
	if (strcmp(cmd, "diskrole") == 0)
		return cmd_diskrole(client, json_mode, argc, argv);
	if (strcmp(cmd, "storage") == 0)
		return cmd_storage(client, json_mode, argc, argv);
	if (strcmp(cmd, "logs") == 0)
		return cmd_logs_top(client, json_mode, argc, argv);
	if (strcmp(cmd, "swap") == 0)
		return cmd_swap(client, json_mode, argc, argv);
	if (strcmp(cmd, "host-stats") == 0)
		return cmd_host_stats(client, json_mode);
	if (strcmp(cmd, "process") == 0)
		return cmd_process(client, json_mode, argc, argv);
	if (strcmp(cmd, "ping") == 0)
		return cmd_ping(client, json_mode, argc, argv);
	if (strcmp(cmd, "resolv") == 0)
		return cmd_resolv(client, json_mode, argc, argv);
	if (strcmp(cmd, "sysctl") == 0)
		return cmd_sysctl(client, json_mode, argc, argv);
	if (strcmp(cmd, "kmod") == 0)
		return cmd_kmod(client, json_mode, argc, argv);
	if (strcmp(cmd, "kmod-config") == 0)
		return cmd_kmodconfig(client, json_mode, argc, argv);
	if (strcmp(cmd, "kmod-build") == 0)
		return cmd_kmod_build(client, json_mode, argc, argv);
	if (strcmp(cmd, "ntp") == 0)
		return cmd_ntp(client, json_mode, argc, argv);
	if (strcmp(cmd, "syslog") == 0)
		return cmd_syslog(client, json_mode, argc, argv);
	if (strcmp(cmd, "time") == 0)
		return cmd_time(client, json_mode, argc, argv);
	if (strcmp(cmd, "ps") == 0)
		return cmd_ps(client, json_mode);
	if (strcmp(cmd, "container") == 0)
		return cmd_container(client, json_mode, argc, argv);
	if (strcmp(cmd, "run") == 0)
		return cmd_run(client, json_mode, argc, argv);
	if (strcmp(cmd, "inspect") == 0)
		return cmd_inspect(client, json_mode, argc, argv);
	if (strcmp(cmd, "stop") == 0)
		return cmd_stop(client, json_mode, argc, argv);
	if (strcmp(cmd, "start") == 0)
		return cmd_start(client, json_mode, argc, argv);
	if (strcmp(cmd, "pause") == 0)
		return cmd_pause(client, json_mode, argc, argv);
	if (strcmp(cmd, "unpause") == 0)
		return cmd_unpause(client, json_mode, argc, argv);
	if (strcmp(cmd, "stats") == 0)
		return cmd_container_stats(client, json_mode, argc, argv);
	if (strcmp(cmd, "migrate-storage") == 0)
		return cmd_migrate_storage(client, json_mode, argc, argv);
	if (strcmp(cmd, "migrate-storage-status") == 0)
		return cmd_migrate_storage_status(client, json_mode, argc, argv);
	if (strcmp(cmd, "console") == 0)
		return cmd_console(client, argc, argv);
	if (strcmp(cmd, "files") == 0)
		return cmd_files(client, argc, argv);
	if (strcmp(cmd, "rm") == 0)
		return cmd_rm(client, json_mode, argc, argv);
	if (strcmp(cmd, "network") == 0)
		return cmd_network(client, json_mode, argc, argv);
	if (strcmp(cmd, "image") == 0)
		return cmd_image(client, json_mode, argc, argv);
	if (strcmp(cmd, "device") == 0)
		return cmd_device(client, json_mode, argc, argv);
	if (strcmp(cmd, "devicemap") == 0)
		return cmd_devicemap(client, json_mode, argc, argv);
	if (strcmp(cmd, "dns") == 0)
		return cmd_dns(client, json_mode, argc, argv);
	if (strcmp(cmd, "ldap") == 0)
		return cmd_ldap(client, json_mode, argc, argv);
	if (strcmp(cmd, "pki") == 0)
		return cmd_pki(client, json_mode, argc, argv);
	if (strcmp(cmd, "pkg") == 0)
		return cmd_pkg(client, json_mode, argc, argv);

	fprintf(stderr, "thincctl: unknown command '%s'\n", cmd);
	print_usage(stderr);
	return 2;
}

#define SHELL_MAX_TOKENS 64

/*
 * Splits line (modified in place) into up to max_tokens whitespace-
 * separated tokens, treating a "..."/'...' span as one token with the
 * quotes stripped -- not a full shell grammar (no backslash-escapes
 * inside quotes), just enough for the one real case that needs it:
 * "run --name=X --image=Y -- /bin/sh -c \"sleep 1\"", where a CMD arg
 * needs an embedded space.
 */
static int tokenize_line(char *line, char **tokens, int max_tokens)
{
	int n = 0;
	char *p = line;

	while (*p != '\0' && n < max_tokens) {
		char quote = '\0';

		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		if (*p == '\0')
			break;
		if (*p == '"' || *p == '\'') {
			quote = *p;
			p++;
		}
		tokens[n++] = p;
		if (quote != '\0') {
			while (*p != '\0' && *p != quote)
				p++;
		} else {
			while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
				p++;
		}
		if (*p != '\0') {
			*p = '\0';
			p++;
		}
	}
	return n;
}

#define SHELL_LINE_MAX 4096
#define SHELL_HISTORY_MAX 100

/* The connected daemon's own instance_name (GET /v1/system/site,
 * ADR-0046), not a fixed "thinc> " -- fetched once at shell startup
 * by shell_prompt_init() below, so the prompt actually identifies
 * *which* box this session is talking to (useful the moment an
 * operator has more than one thinC install reachable). Falls back to
 * the literal string "thinc" (site config's own documented default)
 * if the fetch fails for any reason -- never leaves the prompt blank. */
#define SHELL_PROMPT_MAX 96
static char g_shell_prompt[SHELL_PROMPT_MAX] = "thinc> ";

static void shell_prompt_init(const struct kx_client *client)
{
	struct kx_response r;
	const char *instance_name = NULL;

	if (kx_client_request(client, "GET", "/v1/system/site", NULL, &r) == 0) {
		if (r.status == 200)
			instance_name = json_str_field(r.json, "instance_name");
		if (instance_name != NULL && instance_name[0] != '\0')
			snprintf(g_shell_prompt, sizeof(g_shell_prompt), "%s> ", instance_name);
		kx_response_free(&r);
	}
}

/* Every top-level command dispatch_command() recognizes, plus the
 * shell's own "help"/"exit"/"quit" builtins -- kept as one literal
 * array specifically so a newly added command elsewhere in this file
 * doesn't silently stay uncompletable; there's no way to derive this
 * list from dispatch_command()'s own if-chain without parsing C, so
 * it's a second, explicit copy -- the same tradeoff CATEGORY_VIEWS
 * makes on the web dashboard side (web/app.js) for the identical
 * reason (a route table that can't be enumerated by walking code). */
static const char *const SHELL_COMMANDS[] = {
	"backup", "backup-config", "boot",      "console",       "container",     "daemon-config", "device",   "devicemap", "diskrole",
	"disks",  "dns",       "exit",          "files",    "health",    "help",
	"host-stats", "hostauth-config", "hostauth-sessions", "image", "inspect",       "iso",      "kmod",      "kmod-build", "kmod-config", "ldap",      "login",     "logout",    "logs",      "migrate-storage", "migrate-storage-status", "network",
	"ntp",
	"pause",  "ping",      "pkg",           "pkg-build-config", "pki",      "process",   "ps",        "quit",      "reboot",
	"resolv",
	"restore", "rm",       "rolling-config", "routes",        "run",      "shutdown",  "site",
	"start",  "stats",     "stop",          "storage",  "swap",     "sysctl",   "syslog",    "time",      "tls-throttle", "unpause",   "update",
	NULL
};

static char g_shell_history[SHELL_HISTORY_MAX][SHELL_LINE_MAX];
static int g_shell_history_count;

static void shell_history_add(const char *line)
{
	if (line[0] == '\0')
		return;
	if (g_shell_history_count > 0 &&
	    strcmp(g_shell_history[g_shell_history_count - 1], line) == 0)
		return; /* no consecutive duplicates, matching real shells */
	if (g_shell_history_count == SHELL_HISTORY_MAX) {
		memmove(g_shell_history[0], g_shell_history[1],
		        (size_t)(SHELL_HISTORY_MAX - 1) * SHELL_LINE_MAX);
		g_shell_history_count--;
	}
	snprintf(g_shell_history[g_shell_history_count], SHELL_LINE_MAX, "%s", line);
	g_shell_history_count++;
}

/* hist_pos == -1 means "editing a fresh, not-yet-submitted line";
 * 0..count-1 means "currently showing g_shell_history[hist_pos]".
 * pending holds the fresh line being edited so Down-ing back past the
 * newest history entry restores exactly what was there before Up was
 * first pressed, the same behavior bash's own readline has. */
static void shell_history_up(int *hist_pos, char *pending, char *buf, size_t *len)
{
	if (g_shell_history_count == 0)
		return;
	if (*hist_pos == -1) {
		snprintf(pending, SHELL_LINE_MAX, "%.*s", (int)*len, buf);
		*hist_pos = g_shell_history_count - 1;
	} else if (*hist_pos > 0) {
		(*hist_pos)--;
	} else {
		return; /* already at the oldest entry */
	}
	snprintf(buf, SHELL_LINE_MAX, "%s", g_shell_history[*hist_pos]);
	*len = strlen(buf);
}

static void shell_history_down(int *hist_pos, const char *pending, char *buf, size_t *len)
{
	if (*hist_pos == -1)
		return;
	(*hist_pos)++;
	if (*hist_pos >= g_shell_history_count) {
		*hist_pos = -1;
		snprintf(buf, SHELL_LINE_MAX, "%s", pending);
	} else {
		snprintf(buf, SHELL_LINE_MAX, "%s", g_shell_history[*hist_pos]);
	}
	*len = strlen(buf);
}

static void shell_write_all(const char *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(STDOUT_FILENO, data + off, len - off);

		if (n <= 0)
			return;
		off += (size_t)n;
	}
}

/* Tab completion is scoped to the command name only (the first word) --
 * completing flags/args per-subcommand would need this shell to carry
 * a model of every subcommand's own flag shape, which is real, separate
 * scope (and each subcommand's own usage banner, printed on a parse
 * error, already documents those). A space anywhere in the line means
 * this isn't that word anymore, so completion is a no-op past that point. */
static void shell_complete(char *buf, size_t *len, size_t *cursor)
{
	const char *matches[64];
	int match_count = 0;
	size_t i, match_len;

	for (i = 0; i < *len; i++) {
		if (buf[i] == ' ')
			return;
	}

	for (i = 0; SHELL_COMMANDS[i] != NULL && match_count < 64; i++) {
		if (strncmp(SHELL_COMMANDS[i], buf, *len) == 0)
			matches[match_count++] = SHELL_COMMANDS[i];
	}
	if (match_count == 0)
		return;
	if (match_count == 1) {
		match_len = strlen(matches[0]);
		if (match_len < SHELL_LINE_MAX) {
			memcpy(buf, matches[0], match_len);
			*len = match_len;
			*cursor = match_len;
		}
		return;
	}

	shell_write_all("\r\n", 2);
	for (i = 0; i < (size_t)match_count; i++) {
		shell_write_all(matches[i], strlen(matches[i]));
		shell_write_all("  ", 2);
	}
	shell_write_all("\r\n", 2);
}

/* Full-line redraw on every edit -- simplest way to stay correct
 * regardless of where the cursor is (mid-line insert/delete, history
 * recall, completion), and command lines are short enough that
 * redrawing the whole thing per keystroke has no visible cost. Moves
 * the cursor back from the end of the freshly drawn line to its real
 * position via a raw cursor-left escape, same as \x1b[K (clear to end
 * of line) for erasing whatever a shorter new line left behind. */
static void shell_redraw(const char *buf, size_t len, size_t cursor)
{
	char tail[32];

	shell_write_all("\r", 1);
	shell_write_all(g_shell_prompt, strlen(g_shell_prompt));
	shell_write_all(buf, len);
	shell_write_all("\x1b[K", 3);
	if (cursor < len) {
		int n = snprintf(tail, sizeof(tail), "\x1b[%zuD", len - cursor);

		shell_write_all(tail, (size_t)n);
	}
}

enum shell_key {
	SHELL_KEY_UP = 256,
	SHELL_KEY_DOWN,
	SHELL_KEY_RIGHT,
	SHELL_KEY_LEFT
};

/* Reads one logical keypress: a plain byte, or one of the arrow keys
 * decoded from their real 3-byte terminal escape sequence
 * (ESC '[' <letter>, the VT100/xterm encoding every terminal this
 * project's own users run -- a real Linux/macOS terminal emulator or
 * PuTTY -- already sends). Any other escape sequence (function keys,
 * Home/End, ...) is swallowed silently and reported as a bare ESC --
 * out of scope, the same "command name completion only" scoping
 * shell_complete() already applies. Returns -1 on EOF/read error. */
static int shell_read_key(void)
{
	unsigned char c;
	unsigned char seq[2];

	if (read(STDIN_FILENO, &c, 1) <= 0)
		return -1;
	if (c != 0x1b)
		return c;
	if (read(STDIN_FILENO, &seq[0], 1) <= 0)
		return 0x1b;
	if (seq[0] != '[')
		return 0x1b;
	if (read(STDIN_FILENO, &seq[1], 1) <= 0)
		return 0x1b;
	switch (seq[1]) {
	case 'A':
		return SHELL_KEY_UP;
	case 'B':
		return SHELL_KEY_DOWN;
	case 'C':
		return SHELL_KEY_RIGHT;
	case 'D':
		return SHELL_KEY_LEFT;
	default:
		return 0; /* unrecognized escape -- ignore, not a bare ESC */
	}
}

/* Reads one line with real editing: history (Up/Down), cursor movement
 * (Left/Right), Tab completion of the command name, Backspace, Ctrl-C
 * (abort the current line, matching a real shell -- raw mode disables
 * ISIG, so this process never sees it as SIGINT), and Ctrl-D on an
 * empty line (EOF). Returns 0 with buf filled on Enter, -1 on EOF. The
 * caller's own terminal must already be in raw mode (run_shell() below
 * toggles it around this call, not this function itself, so normal
 * \n-only stdio output from command results doesn't need any \r
 * translation of its own in between lines). */
static int shell_read_line(char *buf, size_t buf_size)
{
	size_t len = 0, cursor = 0;
	int hist_pos = -1;
	char pending[SHELL_LINE_MAX];

	pending[0] = '\0';
	buf[0] = '\0';

	for (;;) {
		int key = shell_read_key();

		if (key < 0)
			return -1;
		if (key == '\r' || key == '\n') {
			shell_write_all("\r\n", 2);
			buf[len] = '\0';
			return 0;
		}
		if (key == 0x7f || key == 0x08) { /* Backspace */
			if (cursor > 0) {
				memmove(buf + cursor - 1, buf + cursor, len - cursor);
				cursor--;
				len--;
				shell_redraw(buf, len, cursor);
			}
			continue;
		}
		if (key == 0x03) { /* Ctrl-C: abort this line, start a fresh one */
			shell_write_all("^C\r\n", 4);
			len = 0;
			cursor = 0;
			hist_pos = -1;
			shell_write_all(g_shell_prompt, strlen(g_shell_prompt));
			continue;
		}
		if (key == 0x04) { /* Ctrl-D */
			if (len == 0)
				return -1;
			continue; /* mid-line: ignored, matching bash */
		}
		if (key == '\t') {
			shell_complete(buf, &len, &cursor);
			shell_redraw(buf, len, cursor);
			continue;
		}
		if (key == SHELL_KEY_LEFT) {
			if (cursor > 0) {
				cursor--;
				shell_redraw(buf, len, cursor);
			}
			continue;
		}
		if (key == SHELL_KEY_RIGHT) {
			if (cursor < len) {
				cursor++;
				shell_redraw(buf, len, cursor);
			}
			continue;
		}
		if (key == SHELL_KEY_UP) {
			shell_history_up(&hist_pos, pending, buf, &len);
			cursor = len;
			shell_redraw(buf, len, cursor);
			continue;
		}
		if (key == SHELL_KEY_DOWN) {
			shell_history_down(&hist_pos, pending, buf, &len);
			cursor = len;
			shell_redraw(buf, len, cursor);
			continue;
		}
		if (key >= 0x20 && key < 0x7f && len + 1 < buf_size) {
			memmove(buf + cursor + 1, buf + cursor, len - cursor);
			buf[cursor] = (char)key;
			cursor++;
			len++;
			shell_redraw(buf, len, cursor);
			continue;
		}
		/* Any other control byte or an unrecognized escape -- ignore. */
	}
}

static int shell_set_raw_mode(struct termios *saved)
{
	struct termios raw;

	if (tcgetattr(STDIN_FILENO, saved) != 0)
		return -1;
	raw = *saved;
	cfmakeraw(&raw);
	if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
		return -1;
	return 0;
}

/* Plain fgets(), no line editing -- only reached if this terminal's
 * own termios couldn't be put into raw mode (shell_set_raw_mode()
 * failed), a real but rare case (e.g. stdin is a tty by isatty()'s
 * own check yet the underlying device rejects termios calls). Keeps
 * the shell usable rather than failing outright. */
static int run_shell_fallback(const struct kx_client *client, int json_mode)
{
	char line[SHELL_LINE_MAX];
	char *tokens[SHELL_MAX_TOKENS];

	shell_prompt_init(client);
	for (;;) {
		int n;

		printf("%s", g_shell_prompt);
		fflush(stdout);
		if (fgets(line, sizeof(line), stdin) == NULL) {
			printf("\n");
			break;
		}
		n = tokenize_line(line, tokens, SHELL_MAX_TOKENS);
		if (n == 0)
			continue;
		if (strcmp(tokens[0], "exit") == 0 || strcmp(tokens[0], "quit") == 0)
			break;
		if (strcmp(tokens[0], "help") == 0) {
			print_usage(stdout);
			continue;
		}
		dispatch_command(client, json_mode, tokens[0], n - 1, tokens + 1);
	}
	return 0;
}

/*
 * Interactive shell: entered when thincctl is invoked with no command
 * and stdin is a real terminal (see main()) -- one persistent client,
 * one dispatch_command() call per typed line, no reconnect-per-command
 * ceremony. A real, if minimal, line editor: history (Up/Down),
 * cursor movement (Left/Right), and Tab completion of the command
 * name -- no external dependency (no GNU readline), matching how this
 * project already built its own PTY relay (client/src/console.c) and
 * web terminal renderer (web/app.js) by hand rather than reaching for
 * a library. A failed command prints its existing error and continues
 * the loop -- a broken command shouldn't end the session, the same
 * posture any real shell already has. "exit"/"quit" or EOF (Ctrl-D on
 * an empty line) end it; "help" reuses print_usage(), not a second
 * copy of it.
 */
static int run_shell(const struct kx_client *client, int json_mode)
{
	struct termios saved;
	char line[SHELL_LINE_MAX];
	char *tokens[SHELL_MAX_TOKENS];

	printf("thincctl interactive shell -- type a command (e.g. \"ps\"), \"help\", or \"exit\"\n");

	if (shell_set_raw_mode(&saved) != 0)
		return run_shell_fallback(client, json_mode);

	shell_prompt_init(client);
	for (;;) {
		int n, rc;
		struct termios raw = saved;

		/* Raw only while reading this one line -- dispatch_command()'s
		 * own printf("...\n") result output needs OPOST's \n -> \r\n
		 * translation, which cfmakeraw() disables; restoring to saved
		 * (cooked) mode before running the command, then re-entering
		 * raw mode here for the next prompt, keeps both halves correct
		 * without threading a "raw vs cooked" flag through every print
		 * call this shell's commands already make. */
		cfmakeraw(&raw);
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
		shell_write_all(g_shell_prompt, strlen(g_shell_prompt));
		rc = shell_read_line(line, sizeof(line));
		tcsetattr(STDIN_FILENO, TCSANOW, &saved); /* cooked mode for command output */

		if (rc != 0) {
			printf("\n");
			break;
		}
		shell_history_add(line);
		n = tokenize_line(line, tokens, SHELL_MAX_TOKENS);
		if (n == 0)
			continue;
		if (strcmp(tokens[0], "exit") == 0 || strcmp(tokens[0], "quit") == 0)
			break;
		if (strcmp(tokens[0], "help") == 0) {
			print_usage(stdout);
			continue;
		}
		dispatch_command(client, json_mode, tokens[0], n - 1, tokens + 1);
	}

	tcsetattr(STDIN_FILENO, TCSANOW, &saved);
	return 0;
}

int main(int argc, char **argv)
{
	const char *host = DEFAULT_HOST;
	int port = DEFAULT_PORT;
	int json_mode = 0;
	int i = 1;
	const char *cmd;
	struct kx_client client;

	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strncmp(argv[i], "--host=", 7) == 0)
			host = argv[i] + 7;
		else if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else {
			fprintf(stderr, "thincctl: unknown option '%s'\n", argv[i]);
			print_usage(stderr);
			return 2;
		}
		i++;
	}

	kx_client_init(&client, host, port);
	{
		char saved_token[64];

		if (load_token_file(saved_token, sizeof(saved_token)) == 0)
			kx_client_set_token(&client, saved_token);
	}

	if (i >= argc) {
		if (isatty(STDIN_FILENO))
			return run_shell(&client, json_mode);
		print_usage(stderr);
		return 2;
	}
	cmd = argv[i++];

	return dispatch_command(&client, json_mode, cmd, argc - i, argv + i);
}
