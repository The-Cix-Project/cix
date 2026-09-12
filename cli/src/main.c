/*
 * cixctl: a pure REST client for the Cix host API
 * (docs/api/openapi.yaml). Per the project's API-First Mandate
 * (CLAUDE.md, ADR-0005), this file holds no namespace/cgroup/mount
 * logic of its own -- every subcommand is exactly one HTTP call via
 * client/src/httpclient.c, the same library test_daemon.c uses to
 * verify the daemon.
 */
#include "console.h"
#include "httpclient.h"
/*
 * ADR-0218 layer 1: every API path this client uses comes from the
 * contract, regenerated each build. A hand-typed path compiles and
 * fails at runtime; a wrong CIX_API_ name does not build at all.
 */
#include "generated/cix_api.h"
#include "json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/stat.h> /* chmod(): the exported PKI bundle is 0600 */
#include <sys/wait.h>
#include <unistd.h>

#include "cmdtree.h"

#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 80

/* A range split more than eight ways gives each server a handful of
 * addresses at best; past that the split is the problem, not the limit. */
#define DHCP_CLI_MAX_SERVERS 8

static void print_usage(FILE *out)
{
	fprintf(out,
	        "usage: cixctl [--host=ADDR] [--port=N] [--json] <command> [args]\n"
	        "\n"
	        "commands:\n"
	        "  health\n"
	        "  boot      -- build version/time, A/B slot, kernel version (uname)\n"
	        "  shutdown  -- stop cixd; powers off the host too when it's running as\n"
	        "               real PID 1 (an installed system) -- a dev/interactive cixd\n"
	        "               just exits, same as it always has on SIGTERM\n"
	        "  reboot    -- stop cixd; restarts the host too when running as PID 1\n"
	        "  boot-next [a|b|clear]  -- boot the given slot ONCE on the next boot, then\n"
	        "               revert to normal selection; no argument reports what is armed.\n"
	        "               This is the rollback tool: pinning the loader default to a slot\n"
	        "               instead is sticky and breaks the NEXT update, which stages the\n"
	        "               other slot and would then match nothing\n"
	        "  update [--image=PATH | --image-url=URL --image-sha256=HEX] [--kernel=PATH]\n"
	        "            -- writes a fresh control-plane squashfs and/or a fresh kernel onto\n"
	        "               this daemon's own inactive A/B slot and stages a fresh loader\n"
	        "               entry for it. --image=/--kernel= are paths already on the box;\n"
	        "               --image-url= has the daemon fetch it instead, which is the only\n"
	        "               option on an installed host (no shell, no scp target, and the\n"
	        "               API caps a request body well below an image). --image-sha256= is\n"
	        "               required with it: this writes a boot slot, so an unverified\n"
	        "               image is refused. At least one of the three required; does NOT\n"
	        "               reboot -- call reboot separately once ready to cut over\n"
	        "  backup [--output=PATH]  -- bundles platform configuration state (container\n"
	        "               defs, networks, DNS records, pkg install state + recipes) --\n"
	        "               NOT workload data, NOT image content, NEVER PKI keys. Prints the\n"
	        "               bundle (or --json) by default; --output= saves it verbatim for\n"
	        "               use with restore --input=\n"
	        "  restore --input=PATH  -- writes a previously-saved backup bundle's fields\n"
	        "               back to their real state files; does NOT reboot or hot-reload --\n"
	        "               call reboot separately for it to take effect on the next boot\n"
	        "  backup-config show  -- which disk (if any) automatic backup snapshots write\n"
	        "               to and whether enabled (ADR-0141). WHEN they run is a schedule:\n"
	        "               see `cixctl schedule ls` (ADR-0257)\n"
	        "  backup-config set [--disk=NAME | --clear-disk] [--enable | --disable]\n"
	        "               -- only the fields given are changed; the target disk must carry\n"
	        "               the backup role\n"
	        "  backup-config status  -- state/last_attempt_unixtime/error of the most recent\n"
	        "               snapshot attempt (manual or automatic)\n"
	        "  backup-config snapshot-now  -- write the same bundle GET /system/backup\n"
	        "               produces to the configured disk right now, regardless of schedule\n"
	        "  container ls  -- every provisioned container and its current state\n"
	        "  container run --name=NAME --image=IMAGE [--memory-max=BYTES] [--pids-max=N]\n"
	        "      [--memory-swap-max=BYTES]  -- 0 means this container may not swap at all;\n"
	        "                                    omit it to leave swap unlimited\n"
	        "      [--cpu-max=\"Q P\"] [--cpuset=0-1,3] [--disk-quota=BYTES] [--network=NAME[:IP] ...]\n"
	        "      [--ip-forward] [--ksm] [--dns-register] [--userns] [--ldap-client] [--capture-output]\n"
	        "      [--ldap-allow-group=NAME ...]  -- with --ldap-client, restricts login to\n"
	        "                                        members of these LDAP groups\n"
	        "      [--pki-issue | --pki-cert=NAME] [--pki-cert-dir=PATH]\n"
	        "      [--pki-days=N] [--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME]\n"
	        "      [--ldap-uid=N] [--ldap-secret-dir=PATH]\n"
	        "      [--route=DEST/PREFIX:VIA ...] [--device=ID ...] [--optional-device=ID ...]\n"
	        "      [--volume=NAME:/path[:ro] ...] [--console=NAME=/path [args] ...]\n"
	        "      [--interface=IFNAME ...] [--cap-add=CAP_NAME ...] [--restart=always|on-failure|unless-stopped]\n"
	        "      [--restart-delay=N] [--follow-rolling] [--follow-rolling-jitter-seconds=N]\n"
	        "      [--depends-on=NAME ...] [--dns-server=A.B.C.D ...]\n"
	        "      --service=NAME=/path [args] ... [--oneshot=NAME=/path [args] ...]\n"
	        "      [--after=NAME:DEP[,DEP] ...] [--ready=NAME:tcp:PORT|socket:PATH|command:/path [args] ...]\n"
	        "      [--on-exit=NAME:restart|stop|fail-container ...]\n"
	        "               -- what the container runs (ADR-0260): its services, started in --after\n"
	        "               order by cix-init, pid 1 in every container. A container with one daemon\n"
	        "               declares one --service. No -- CMD: there is no single command any more.\n"
	        "  container service start|stop|restart NAME SERVICE  -- one declared service; a stop\n"
	        "               is held until the next container start, never an edit\n"
	        "  container inspect NAME\n"
	        "  container start NAME  -- bring a stopped container back up\n"
	        "  container stop NAME   -- stop it but keep it (start it again later); rm deletes\n"
	        "  container pause NAME / container unpause NAME  -- cgroup v2 freezer (real freeze)\n"
	        "  container stats NAME  -- real, host-side CPU/memory/disk/network usage\n"
	        "  container migrate-storage NAME [--disk=NAME]  -- move this container's storage to a\n"
	        "               disk carrying the container-storage role (ADR-0142); omit --disk= for\n"
	        "               the default OS-disk placement; briefly restarts for the cutover -- poll\n"
	        "               container migrate-storage-status NAME\n"
	        "  container console NAME [--console=NAME]  -- attach to one of the consoles a\n"
	        "               container DECLARES; --console picks which (default: the first\n"
	        "               declared). A container that declares none has no console and says\n"
	        "               so (#248). There is no free-text command: ADR-0261 removed both\n"
	        "               that and the exec endpoint, so a container is reachable only\n"
	        "               through what its recipe declares\n"
	        "  container files NAME --path=PATH  -- read a file from the container's rootfs\n"
	        "  container rm NAME  -- delete the container and its storage\n"
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
	        "  image apply-recipe NAME  -- bulk-declares the image's manifest from its\n"
	        "               recipe; packages still need a real install afterward\n"
	        "  image gc [--dry-run] [--measure]  -- reclaims image versions nothing\n"
	        "               references. Every install leaves an immutable version behind\n"
	        "               (ADR-0107/0108) and nothing else ever removes one. --measure\n"
	        "               sizes what it finds, which walks every collectable version and\n"
	        "               can block the daemon for minutes -- off by default\n"
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
	        "  ldap config show  -- uid/gid floor, the client login settings, and which\n"
	        "               listeners the registered servers serve\n"
	        "  ldap config set [--start-uid=N --start-gid=N] [--client-uri=URIS]\n"
	        "      [--base-dn=DN] [--bind-dn=DN] [--bind-password=PW]\n"
	        "      [--client-tls | --no-client-tls]\n"
	        "      [--server-plaintext | --no-server-plaintext] [--server-plaintext-port=N]\n"
	        "      [--server-tls | --no-server-tls] [--server-tls-port=N]\n"
	        "      [--restart-servers]\n"
	        "               -- only the flags given change. The --server-* flags (#419) are\n"
	        "               rendered into every registered server's own glauth config, and\n"
	        "               into what is staged for it on its next start. glauth binds its\n"
	        "               listeners at STARTUP and its config watcher reloads only records,\n"
	        "               so a listener change reaches a running server when it restarts:\n"
	        "               --restart-servers does that, one at a time, each back before the\n"
	        "               next is touched. Off by default -- this is the directory that\n"
	        "               authenticates the control plane. Until the first --server-* flag\n"
	        "               is set, `listeners_managed` is false and each server's own config\n"
	        "               still decides. --client-tls selects WHICH enabled listener\n"
	        "               clients are pointed at, and is refused if that listener is off;\n"
	        "               the daemon's own bind is a separate switch\n"
	        "               (`hostauth-config set --ldap-tls`, #416)\n"
	        "  login [--username=NAME] [--password=PASS]  -- ADR-0144: authenticates against\n"
	        "               the daemon's own host-auth backend (prompts for whichever of\n"
	        "               username/password isn't given as a flag, with echo off for the\n"
	        "               password); on success persists the session token to\n"
	        "               ~/.cixctl_token (mode 0600) so every subsequent cixctl\n"
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
	        "  pki export [--passphrase-file=PATH] [--out=PATH]  -- the whole PKI store\n"
	        "               (CA key+cert, intermediate, every leaf) as one encrypted\n"
	        "               line. The only thing that carries a CA across a reinstall\n"
	        "               or onto new hardware (ADR-0281); GET /system/backup\n"
	        "               deliberately holds no key material at all\n"
	        "  pki import --in=PATH [--passphrase-file=PATH]  -- restores that\n"
	        "               bundle. Refused with 409 when a CA already exists, since\n"
	        "               replacing a live trust root invalidates every certificate\n"
	        "               this install has issued\n"
	        "  schedule ls | actions | show N | set N | rm N | run N  -- everything this host\n"
	        "               does on a clock, in one place (ADR-0257)\n"
	        "  pipeline [--all]  -- where every package stands in the eleven-stage pipeline,\n"
	        "               and what is stopping it (ADR-0256). Hides ok/not-implemented\n"
	        "               rows unless --all\n"
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
	        "  pkg rebuilds  -- image rebuilds this host has queued but not started.\n"
	        "               Publishing a recipe queues one for every image tracking that\n"
	        "               package `rolling`, which is real work nothing else reports.\n"
	        "  pkg recipe add --name=NAME --file=PATH  -- publishes a new recipe version on\n"
	        "               this running system directly, no reinstall needed (ADR-0040);\n"
	        "               an already-published (name,version) is rejected, not overwritten\n"
	        "               (ADR-0107) -- bump pkg_version= to publish a fix\n"
	        "  pkg recipe show NAME [--version=VERSION]  -- print a recipe's own raw content,\n"
	        "               omitted version resolves to the highest available\n"
	        "  pkg recipe rm NAME [--version=VERSION]  -- omitted removes every version\n"
	        "  pkg install --name=NAME [--image=IMAGE] [--version=VERSION] [--upgrade]\n"
	        "               [--keep-on-failure]  -- on a build failure, leave the build\n"
	        "               container registered/mounted for GET .../files inspection\n"
	        "               instead of tearing it down (ADR-0175)\n"
	        "  pkg resume --name=NAME [--image=IMAGE] [--version=VERSION]\n"
	        "               [--keep-on-failure]  -- resumes a kept-on-failure build\n"
	        "               container in place (its already-extracted source tree\n"
	        "               kept, only the recipe + install destination refreshed),\n"
	        "               instead of a full fetch+extract+build restart (ADR-0177)\n"
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
	        "  daemon-config show  -- cixd's own listen port, HTTP/HTTPS exposure, and\n"
	        "               which network is currently its management one\n"
	        "  daemon-config set [--port=N] [--https-port=N] [--enable-http] [--disable-http]\n"
	        "               [--enable-https] [--disable-https] [--management-network=NAME]\n"
	        "               [--bind-ip=A.B.C.D | --clear-bind-ip]\n"
	        "               -- live, no-restart; only the fields given are changed. bind_ip\n"
	        "               (ADR-0068) is a second, dedicated address on the management\n"
	        "               network's own bridge -- cixd binds there instead of that\n"
	        "               network's own address; --clear-bind-ip reverts to it\n"
	        "  hostauth-config show  -- current admin_groups/idle_timeout_seconds/live-LDAP\n"
	        "               backend settings (ADR-0144)\n"
	        "  hostauth-config set [--admin-group=NAME ...] [--idle-timeout-seconds=N]\n"
	        "               [--ldap-enable | --ldap-disable] [--ldap-server=HOST ...]\n"
	        "               [--ldap-port=N] [--ldap-tls | --no-ldap-tls]\n"
	        "               [--ldap-base-dn=NAME]\n"
	        "               -- read-modify-write; only the flags given are changed, everything\n"
	        "               else keeps its current value. Write-gating activates the instant\n"
	        "               a real user is a member of one of admin_groups.\n"
	        "               --ldap-tls (#416) runs the DAEMON's own bind over LDAPS,\n"
	        "               verified against this host's own CA -- a different switch from\n"
	        "               `ldap config set --client-tls`, which configures the LDAP\n"
	        "               clients inside containers\n"
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
	        "               run at once, and the memory/CPU ceiling each one's own pkgbuild\n"
	        "               sandbox is capped to (ADR-0157/ADR-0165)\n"
	        "  pkg-build-config set [--max-concurrent-jobs=N] [--memory-max=BYTES]\n"
	        "               [--cpu-max=\"QUOTA PERIOD\"]  -- partial update, only the flags given\n"
	        "               are changed; max-concurrent-jobs is 1-10 and lowering it doesn't\n"
	        "               disrupt jobs already in flight, only future ones; memory-max=0 or\n"
	        "               cpu-max=\"\" means unlimited; cpu-max is raw cgroup v2 cpu.max syntax\n"
	        "               (microseconds quota/period, e.g. \"100000 100000\" for one full CPU),\n"
	        "               same format as run --cpu-max=\n"
	        "  iso build [--disk=DEV --ip=A.B.C.D --prefix=N --gateway=A.B.C.D\n"
	        "               --interface=IFNAME] [--wait]  -- assembles a fresh installer ISO\n"
	        "               server-side (ADR-0064), from the most recent \"cix\"/\"kernel\"/\n"
	        "               \"isotools\" hostbuild artifacts; every flag is optional, an empty\n"
	        "               call reproduces the original edit-at-the-GRUB-menu placeholder ISO\n"
	        "  iso status  -- state/iso_path/error of the most recent ISO build\n"
	        "  iso publish [--wait]  -- put the ISO + its signature in the artifact cache\n"
	        "  routes  -- the box's own real kernel IPv4 routing table (ADR-0066); the\n"
	        "               only way to see this on a real install, no SSH/general shell\n"
	        "  routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]  -- add a real\n"
	        "               kernel route (ADR-0067 Part 3); or --default --gateway=A.B.C.D\n"
	        "  routes rm --dest=A.B.C.D --prefix=N  -- remove one; or --default\n"
	        "  assembly status  -- what control-plane assembly is doing, and where the\n"
	        "               assembled root is (image_path, ready for update --image=)\n"
	        "  assembly start  -- assemble a fresh control-plane root now (#308). Until\n"
	        "               this existed, assembly happened only as a side effect of a\n"
	        "               hostbuild named cix completing, so re-assembling meant\n"
	        "               inventing a version number whose code had not changed.\n"
	        "               Returns immediately -- poll status for completed_generation\n"
	        "               to pass what it read before the call\n"
	        "  storage  -- real host block devices, including any partitions on them\n"
	        "               (task #844); which one is the fixed OS disk vs. assignable is\n"
	        "               flagged per entry, and a partition entry names its parent disk\n"
	        "  storage partition-table NAME  -- destructive: writes a fresh, empty GPT\n"
	        "               partition table to a non-OS whole disk with no role or\n"
	        "               partitions of its own currently in use\n"
	        "  storage add-partition NAME --name=PART_NAME [--size-mib=N]  -- appends one\n"
	        "               new partition to a disk's existing table (never touches an\n"
	        "               existing partition); omit --size-mib for \"rest of the disk\"\n"
	        "  storage rm-partition DISK_NAME PARTITION_NAME  -- removes one partition;\n"
	        "               refused (409) if it still has a role assigned (diskrole rm first)\n"
	        "  storage-role create --disk=NAME\n"
	        "               --role=container-storage|backup|rebuildable-storage|log-storage|swap\n"
	        "               -- assign a persisted role to a disk (never the OS disk)\n"
	        "  storage-role ls / diskrole rm NAME  -- list assigned roles (with whether each\n"
	        "               disk is currently present) / remove one; refused (409) if the\n"
	        "               disk is the active rebuildable/log-storage placement (storage migrate\n"
	        "               away first)\n"
	        "  storage format NAME [--fs-type=ext4|btrfs]  -- destructive: mkfs + mount an\n"
	        "               already role-assigned, non-OS disk (assign a role first via\n"
	        "               diskrole create); fs_type defaults to ext4; refused (409) if the\n"
	        "               disk is the active storage placement\n"
	        "  storage format-status NAME  -- state/mount_path/error of the most recent\n"
	        "               format job for this disk\n"
	        "  storage unmount NAME  -- a real, synchronous umount2(2) of an already-mounted,\n"
	        "               non-OS disk (issue #34); data on the disk is untouched, only its\n"
	        "               attachment to the running system is removed; refused (409) if\n"
	        "               it's the active placement for a storage singleton/backup-config/\n"
	        "               swap, or a live container has its own storage on it\n"
	        "  storage state [show]  -- which disk (if any) is the active placement for\n"
	        "               Cix's own state (ADR-0141); default (null) is the OS disk\n"
	        "  storage state migrate [--disk=NAME]  -- move Cix's own state to a disk\n"
	        "               already carrying the matching role and currently mounted;\n"
	        "               omit --disk= to migrate back to the default OS-disk placement;\n"
	        "               async, no pause -- poll storage state migrate-status\n"
	        "  storage state migrate-status  -- state/disk/error of the most recent (or\n"
	        "               running) storage migration\n"
	        "  storage logs [show|migrate [--disk=NAME]|migrate-status]  -- same shape as\n"
	        "               storage state, for where Cix's own consolidated log store\n"
	        "               (ADR-0070/ADR-0126) lives instead\n"
	        "  storage rebuildable [show|migrate [--disk=NAME]|migrate-status]  -- same shape\n"
	        "               again, for where images/packages/artifacts (regenerable from\n"
	        "               recipes/sources, never irreplaceable) live instead\n"
	        "  swap  -- show whether the host swap file is enabled (ADR-0069) and which disk\n"
	        "               (if any) it's placed on\n"
	        "  swap enable --size-mb=N [--disk=NAME]  -- create and activate a swap file of\n"
	        "               this size; --disk= places it on a disk carrying the \"swap\" role\n"
	        "               (diskrole create --role=swap, issue #28) instead of the default\n"
	        "               OS-disk location -- omit for the default\n"
	        "  swap disable  -- deactivate and remove it\n"
	        "  host-stats  -- host-wide load/CPU/memory/disk/network snapshot (ADR-0073)\n"
	        "  kmsg [--tail=N]  -- the kernel ring buffer (/dev/kmsg), dmesg over REST --\n"
	        "  server-health [ls] | drain KIND NAME | undrain KIND NAME  -- health of every\n"
	        "               registered LDAP/DNS/NTP/syslog server; drain takes one out of\n"
	        "               service deliberately (issue #81)\n"
	        "               the only kernel-log window a shell-less installed host has\n"
	        "  process ls  -- every real process on the box (a direct /proc scan), each\n"
	        "               correlated to a container by its own real host ppid chain, if any\n"
	        "               (ADR-0131)\n"
	        "  process kill PID  -- a real, immediate SIGKILL; refuses pid 1 and this\n"
	        "               daemon's own pid\n"
	        "  ping HOST  -- real ICMP echo against an IPv4 address, waits for the result\n"
	        "               (~2s max) and exits nonzero if unreachable\n"
	        "  resolv [show]  -- the host's own outbound DNS resolver config (ADR-0076)\n"
	        "  signing-keys [show]  -- the Secure Boot signing key pair `iso build` needs\n"
	        "  signing-keys set --key=PATH --cert=PATH  -- install it (ADR-0212)\n"
	        "  signing-keys clear  -- remove it from this host\n"
	        "  release-key [show]  -- the Ed25519 key that signs published artifacts\n"
	        "  release-key set --key=PATH  -- install it (ADR-0220)\n"
	        "  release-key clear  -- remove it from this host\n"
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
	        "               [--upgrade] [--wait] [--keep-on-failure]  -- an ordinary\n"
	        "               `pkg hostbuild kernel` under the hood, gaining extra =m module\n"
	        "               symbols merged into the same curated kernel config; needs a\n"
	        "               reboot onto the new bzImage (see kernel-build-and-ab-updates.md)\n"
	        "               to actually take effect. --keep-on-failure (ADR-0175) leaves a\n"
	        "               failed build container registered/mounted for real debugging\n"
	        "               instead of tearing it down\n"
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
	        "  logs [--source=kernel|cixd|audit|container] [--level=...] [--container=NAME]\n"
	        "               [--regex=PATTERN] [--tail=N] [--since=UNIXTS]\n"
	        "               -- the consolidated log (kernel dmesg + cixd's own\n"
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

/* A missing or null string renders as "-" rather than as "(null)" --
 * every table in this client already means "nothing here" by a dash. */
static const char *json_str_or(const struct json_value *obj, const char *key)
{
	const char *v = json_as_string(json_object_get(obj, key));

	return v != NULL && v[0] != '\0' ? v : "-";
}

static int json_bool_field(const struct json_value *obj, const char *key)
{
	const struct json_value *v = json_object_get(obj, key);

	return v != NULL && v->type == JSON_BOOL && v->u.boolean;
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
	const struct json_value *pd = json_object_get(v, "platform_devices");

	printf("build:   %s (%s)\n", json_str_field(v, "build_version"), json_str_field(v, "build_time"));
	printf("slot:    %s\n", (slot != NULL) ? slot : "(none)");
	printf("kernel:  %s\n", (kernel != NULL) ? kernel : "(unknown)");
	/*
	 * #305: which device each platform partition resolved to by GPT
	 * label this boot. Printed on one line per partition rather than
	 * folded away, because the whole point is that these change when a
	 * disk comes back under a different name, and this is the only
	 * place outside a serial console that says what they are.
	 */
	if (pd != NULL && pd->type == JSON_OBJECT) {
		static const char *const keys[] = { "esp", "root_a", "root_b", "config",
		                                     "containers" };
		const char *root_disk = json_str_field(pd, "root_disk");
		size_t i;

		/*
		 * The disk those five were resolved on (#427). First, because
		 * it is what makes the five below trustworthy: with two disks
		 * carrying the same GPT label, five plausible device paths
		 * off the wrong disk look exactly like five off the right one.
		 */
		printf("%-11s %s\n", "root disk",
		       (root_disk != NULL && root_disk[0] != '\0') ? root_disk : "(undetermined)");

		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
			const char *dev = json_str_field(pd, keys[i]);

			/* 11, not 8: "containers" is ten characters and an
			 * under-width field silently stops aligning rather than
			 * truncating, which is what shipped in v2.57.127. */
			printf("%-11s%s\n", keys[i], (dev != NULL && dev[0] != '\0') ? dev : "(not found)");
		}
	}
}

static void fmt_container_line(const struct json_value *v)
{
	const char *name = json_str_field(v, "name");
	const char *status = json_str_field(v, "status");
	long pid = (long)json_as_number(json_object_get(v, "pid"));
	const struct json_value *exitv = json_object_get(v, "exit_status");
	const struct json_value *termsig = json_object_get(v, "term_signal");
	const struct json_value *networks = json_object_get(v, "networks");
	const struct json_value *ip_forward = json_object_get(v, "ip_forward");
	const char *restart = json_str_field(v, "restart");
	const struct json_value *restart_delay = json_object_get(v, "restart_delay_seconds");
	const struct json_value *stopped = json_object_get(v, "stopped");
	const struct json_value *follow_rolling = json_object_get(v, "follow_rolling");
	const struct json_value *follow_rolling_jitter =
	    json_object_get(v, "follow_rolling_jitter_seconds");
	const struct json_value *ready = json_object_get(v, "ready");
	const struct json_value *files = json_object_get(v, "files");
	const struct json_value *sysctls = json_object_get(v, "sysctls");
	const struct json_value *env = json_object_get(v, "env");
	const struct json_value *services = json_object_get(v, "services");
	const struct json_value *memory_max = json_object_get(v, "memory_max");
	const struct json_value *cpu_max = json_object_get(v, "cpu_max");
	const struct json_value *pids_max = json_object_get(v, "pids_max");
	/* Issues #48/#49: cpuset/quota/caps used to be completely invisible
	 * client-side even once the daemon learned to report them. */
	const struct json_value *cpuset = json_object_get(v, "cpuset_cpus");
	const struct json_value *disk_quota = json_object_get(v, "disk_quota_bytes");
	const struct json_value *cap_add = json_object_get(v, "cap_add");
	char exit_buf[16];
	char net_buf[256];
	char delay_buf[16];
	char jitter_buf[16];
	char cmd_buf[256];
	char quota_buf[24];
	char mem_buf[24];
	char pids_buf[16];
	size_t off = 0;
	size_t cmd_off = 0;
	size_t i;

	/*
	 * Issue #78/#77: exit_status alone is ambiguous -- a container killed
	 * by a signal reports that signal's NUMBER here, indistinguishable
	 * from a real exit code of the same value (a stop/delete SIGKILL is
	 * exit_status 9, not "exit code 9"). term_signal disambiguates it, so
	 * render a signal death as "sig9" rather than a bare "9" instead of
	 * spending another column on it.
	 */
	if (termsig != NULL && termsig->type == JSON_NUMBER &&
	    (long)json_as_number(termsig) != 0)
		snprintf(exit_buf, sizeof(exit_buf), "sig%ld", (long)json_as_number(termsig));
	else if (exitv != NULL && exitv->type == JSON_NUMBER)
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

	if (restart_delay != NULL && restart_delay->type == JSON_NUMBER)
		snprintf(delay_buf, sizeof(delay_buf), "%ld", (long)json_as_number(restart_delay));
	else
		snprintf(delay_buf, sizeof(delay_buf), "-");

	if (follow_rolling_jitter != NULL && follow_rolling_jitter->type == JSON_NUMBER)
		snprintf(jitter_buf, sizeof(jitter_buf), "%ld", (long)json_as_number(follow_rolling_jitter));
	else
		snprintf(jitter_buf, sizeof(jitter_buf), "-");

	/* ADR-0260: each declared service and its live state, in cix-init's order. */
	cmd_buf[0] = '\0';
	if (services != NULL && services->type == JSON_ARRAY) {
		for (i = 0; i < services->u.array.count && cmd_off < sizeof(cmd_buf) - 1; i++) {
			const struct json_value *item = services->u.array.items[i];
			const char *sname = json_str_field(item, "name");
			const char *state = json_str_field(item, "state");
			int written = snprintf(cmd_buf + cmd_off, sizeof(cmd_buf) - cmd_off, "%s%s:%s",
			                        i > 0 ? "," : "", sname != NULL ? sname : "?",
			                        state != NULL ? state : "?");

			if (written > 0)
				cmd_off += (size_t)written;
		}
	}

	if (memory_max != NULL && memory_max->type == JSON_NUMBER)
		snprintf(mem_buf, sizeof(mem_buf), "%lld", (long long)json_as_number(memory_max));
	else
		snprintf(mem_buf, sizeof(mem_buf), "-");

	if (pids_max != NULL && pids_max->type == JSON_NUMBER)
		snprintf(pids_buf, sizeof(pids_buf), "%lld", (long long)json_as_number(pids_max));
	else
		snprintf(pids_buf, sizeof(pids_buf), "-");

	if (disk_quota != NULL && disk_quota->type == JSON_NUMBER)
		snprintf(quota_buf, sizeof(quota_buf), "%lld", (long long)json_as_number(disk_quota));
	else
		snprintf(quota_buf, sizeof(quota_buf), "-");

	printf("%-20s %-8s pid=%-8ld exit_status=%-6s networks=%-20s fwd=%-4s restart=%-15s "
	       "delay=%-4s roll=%-4s jitter=%-4s stopped=%-5s ready=%-5s files=%-3zu sysctls=%-3zu "
	       "env=%-3zu memory_max=%-12s cpu_max=%-14s pids_max=%-5s cpuset=%-8s disk_quota=%-12s "
	       "caps=%-3zu services=%s\n",
	       name, status, pid, exit_buf, net_buf[0] != '\0' ? net_buf : "-",
	       (ip_forward != NULL && ip_forward->type == JSON_BOOL && ip_forward->u.boolean) ? "yes"
	                                                                                        : "no",
	       restart != NULL ? restart : "no", delay_buf,
	       (follow_rolling != NULL && follow_rolling->type == JSON_BOOL && follow_rolling->u.boolean)
	           ? "yes"
	           : "no",
	       jitter_buf,
	       (stopped != NULL && stopped->type == JSON_BOOL && stopped->u.boolean) ? "yes" : "no",
	       (ready != NULL && ready->type == JSON_BOOL && ready->u.boolean) ? "yes" : "no",
	       files != NULL && files->type == JSON_ARRAY ? files->u.array.count : 0,
	       sysctls != NULL && sysctls->type == JSON_OBJECT ? sysctls->u.object.count : 0,
	       env != NULL && env->type == JSON_OBJECT ? env->u.object.count : 0, mem_buf,
	       cpu_max != NULL && cpu_max->type == JSON_STRING ? cpu_max->u.string : "-", pids_buf,
	       cpuset != NULL && cpuset->type == JSON_STRING ? cpuset->u.string : "-", quota_buf,
	       cap_add != NULL && cap_add->type == JSON_ARRAY ? cap_add->u.array.count : 0,
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
	/* issue #28: the configured disk (or its absence -- the default
	 * OS-disk location) is real, meaningful state even while swap is
	 * currently disabled -- shown either way, not only when enabled. */
	const char *disk = json_str_field(v, "disk");

	if (!enabled) {
		printf("swap: disabled disk=%s\n", disk != NULL ? disk : "(default)");
		return;
	}
	printf("swap: enabled size_mb=%ld path=%s disk=%s\n", size_mb, path != NULL ? path : "",
	       disk != NULL ? disk : "(default)");
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

	const struct json_value *as = json_object_get(v, "alloc_start_host");
	const struct json_value *ae = json_object_get(v, "alloc_end_host");

	printf("%-20s %s/%-3ld address=%s", name, subnet, prefix_len,
	       address != NULL ? address : "none");
	/*
	 * Show the auto-allocation window (issue #137). Worth a column
	 * because its ABSENCE is meaningful on a bridged network -- that is
	 * the state where auto-allocation is refused outright -- and a
	 * `set-pool` that printed nothing about the pool would leave an
	 * operator with no confirmation of what they just changed.
	 */
	if ((as != NULL && as->type == JSON_NUMBER) || (ae != NULL && ae->type == JSON_NUMBER)) {
		printf(" pool=");
		if (as != NULL && as->type == JSON_NUMBER)
			printf(".%ld", (long)json_as_number(as));
		else
			printf("(low)");
		printf("-");
		if (ae != NULL && ae->type == JSON_NUMBER)
			printf(".%ld", (long)json_as_number(ae));
		else
			printf("(high)");
	} else {
		printf(" pool=none");
	}
	printf("\n");
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
	const struct json_value *jprot = json_object_get(v, "protected");
	const char *parent_disk = json_str_field(v, "parent_disk");
	const char *mount_path = json_str_field(v, "mount_path");
	int removable = jremovable != NULL && jremovable->type == JSON_BOOL && jremovable->u.boolean;
	int is_os_disk = jos != NULL && jos->type == JSON_BOOL && jos->u.boolean;
	int mounted = jmounted != NULL && jmounted->type == JSON_BOOL && jmounted->u.boolean;
	int is_partition = jpart != NULL && jpart->type == JSON_BOOL && jpart->u.boolean;
	/*
	 * Protection is its own column, not a suffix on another one.
	 *
	 * It answers a different question from "what kind of device is
	 * this": whether the platform will let you act on it at all. The
	 * daemon computes it (GET /disks' `protected`), and reading it here
	 * rather than re-deriving it from is_os_disk is what keeps the CLI
	 * from disagreeing with the daemon -- the web dashboard did exactly
	 * that and refused to manage a perfectly assignable partition.
	 */
	int protected_dev = jprot != NULL && jprot->type == JSON_BOOL && jprot->u.boolean;
	double size_gib = (double)size_bytes / (1024.0 * 1024.0 * 1024.0);
	char mount_col[288];
	char kind_col[32];

	if (mounted)
		snprintf(mount_col, sizeof(mount_col), "mounted@%s",
		         mount_path != NULL ? mount_path : "?");
	else
		snprintf(mount_col, sizeof(mount_col), "not-mounted");

	/* "os-disk" is the whole OS disk only. Its partitions are ordinary
	 * partitions -- several of them are assignable, and saying
	 * "os-disk" for those is what made them look untouchable. */
	if (is_os_disk && !is_partition)
		snprintf(kind_col, sizeof(kind_col), "os-disk");
	else if (is_partition)
		snprintf(kind_col, sizeof(kind_col), "partition");
	else
		snprintf(kind_col, sizeof(kind_col), "assignable");

	printf("%-12s %-16s %8.1f GiB  %-32s %-9s %-10s %-9s %s", dev_path, name, size_gib,
	       model != NULL && model[0] != '\0' ? model : "-", removable ? "removable" : "fixed",
	       kind_col, protected_dev ? "protected" : "-", mount_col);
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
	const struct json_value *disks = json_object_get(v, "storage");
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
	const struct json_value *roles = json_object_get(v, "storage_roles");
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
	const char *client_uri = json_as_string(json_object_get(v, "client_uri"));
	const char *base_dn = json_as_string(json_object_get(v, "base_dn"));
	const char *bind_dn = json_as_string(json_object_get(v, "bind_dn"));
	const struct json_value *pwset = json_object_get(v, "bind_password_set");

	printf("start_uid=%ld start_gid=%ld\n", start_uid, start_gid);
	printf("client_uri=%s\n", client_uri != NULL ? client_uri : "(unset)");
	/* Issue #84: what clients are actually handed, after derivation from
	 * registered servers and after dropping any that are drained or
	 * unhealthy -- a different question from the line above it. */
	printf("effective_client_uri=%s\n",
	       json_as_string(json_object_get(v, "effective_client_uri")) != NULL
	           ? json_as_string(json_object_get(v, "effective_client_uri"))
	           : "(none)");
	printf("base_dn=%s\n", base_dn != NULL ? base_dn : "(unset)");
	printf("bind_dn=%s\n", bind_dn != NULL ? bind_dn : "(unset)");
	printf("bind_password=%s\n",
	       (pwset != NULL && pwset->type == JSON_BOOL && pwset->u.boolean) ? "(set)" : "(unset)");
	/* #419: what the servers actually serve, and whether the daemon is
	 * the one saying so. "unmanaged" is reported rather than hidden --
	 * it means each server's own config still decides, and the values
	 * printed are only what a first PUT would apply. */
	{
		const struct json_value *managed = json_object_get(v, "listeners_managed");
		const struct json_value *splain = json_object_get(v, "server_plaintext");
		const struct json_value *stls = json_object_get(v, "server_tls");
		const struct json_value *ctls = json_object_get(v, "client_tls");
		int is_managed = managed != NULL && managed->type == JSON_BOOL && managed->u.boolean;

		printf("listeners=%s ldap=%s:%ld ldaps=%s:%ld clients_use=%s\n",
		       is_managed ? "managed" : "unmanaged (each server's own config decides)",
		       (splain != NULL && splain->type == JSON_BOOL && splain->u.boolean) ? "on" :
		                                                                            "off",
		       (long)json_as_number(json_object_get(v, "server_plaintext_port")),
		       (stls != NULL && stls->type == JSON_BOOL && stls->u.boolean) ? "on" : "off",
		       (long)json_as_number(json_object_get(v, "server_tls_port")),
		       (ctls != NULL && ctls->type == JSON_BOOL && ctls->u.boolean) ? "ldaps" : "ldap");
	}
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
 * view; cixctl pki cert ls / a saved --json capture is how an
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
static int emit(struct cix_response *r, int json_mode, void (*fmt)(const struct json_value *))
{
	int rc;

	if (r->status < 200 || r->status >= 300) {
		const char *msg = json_str_field(r->json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r->status);
		rc = 1;
	} else if (json_mode || fmt == NULL) {
		print_raw_json(r->json);
		rc = 0;
	} else {
		fmt(r->json);
		rc = 0;
	}
	cix_response_free(r);
	return rc;
}

static int cmd_health(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getHealth_METHOD, CIX_API_getHealth, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_boot(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemBoot_METHOD, CIX_API_getSystemBoot, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_boot);
}

static int cmd_routes_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemRoutes_METHOD, CIX_API_getSystemRoutes, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_route_list);
}

static int cmd_routes_add(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *dest = NULL;
	const char *gateway = NULL;
	const char *prefix = NULL;
	int is_default = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown routes add option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (!is_default && (dest == NULL || prefix == NULL)) {
		fprintf(stderr,
		        "usage: cixctl routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]\n"
		        "       cixctl routes add --default --gateway=A.B.C.D\n");
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

	if (cix_client_request(c, CIX_API_postSystemRoute_METHOD, CIX_API_postSystemRoute, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_added);
}

static int cmd_routes_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *dest = NULL;
	const char *prefix = NULL;
	int is_default = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--dest=", 7) == 0)
			dest = argv[i] + 7;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix = argv[i] + 9;
		else if (strcmp(argv[i], "--default") == 0)
			is_default = 1;
		else {
			fprintf(stderr, "cixctl: unknown routes rm option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (!is_default && (dest == NULL || prefix == NULL)) {
		fprintf(stderr,
		        "usage: cixctl routes rm --dest=A.B.C.D --prefix=N\n"
		        "       cixctl routes rm --default\n");
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

	if (cix_client_request(c, CIX_API_deleteSystemRoute_METHOD, CIX_API_deleteSystemRoute, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_removed);
}

static int cmd_routes(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	        "usage: cixctl routes [ls]\n"
	        "       cixctl routes add --dest=A.B.C.D --prefix=N [--gateway=A.B.C.D]\n"
	        "       cixctl routes add --default --gateway=A.B.C.D\n"
	        "       cixctl routes rm --dest=A.B.C.D --prefix=N\n"
	        "       cixctl routes rm --default\n");
	return 2;
}

static int cmd_disks_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listStorage_METHOD, CIX_API_listStorage, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
 * already-role-assigned disk (POST /v1/storage-roles first). Destructive
 * and deliberately explicit -- disk_name is sent as the request body's
 * own confirm_disk_name (matching what the operator already typed once
 * in the URL path), the same double-confirmation the REST layer itself
 * requires (see main.c's handle_disk_format_post()). No further
 * interactive "are you sure" prompt here, matching this CLI's existing
 * "pki reset" precedent for destructive operations -- the request body
 * confirmation IS the safety gate.
 */
static int cmd_disks_format(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	const char *fs_type = NULL;
	char path[256];
	struct json_writer w;
	struct cix_response r;
	int i;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage format NAME [--fs-type=ext4|btrfs]\n");
		return 2;
	}
	disk_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--fs-type=", 10) == 0)
			fs_type = argv[i] + 10;
	}
	snprintf(path, sizeof(path), CIX_API_formatStorage, disk_name);

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

	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_diskformat_status);
}

static int cmd_disks_format_status(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	char path[256];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage format-status NAME\n");
		return 2;
	}
	disk_name = argv[0];
	snprintf(path, sizeof(path), CIX_API_getStorageFormatStatus, disk_name);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_diskformat_status);
}

/*
 * Found live, issue #34: a disk with its role already removed had no
 * way to actually be let go of -- it just stayed mounted forever, and a
 * real, currently-mounted-but-role-less disk turned out to correlate
 * with a genuine mountns_pivot() EXDEV failure in every subsequent
 * container creation. Synchronous (a real umount2(2) is fast, unlike
 * mkfs) -- same double-confirmation shape as "disks format": disk_name
 * sent as the request body's own confirm_disk_name, no further
 * interactive prompt, the request body confirmation IS the safety gate.
 */
static int cmd_disks_unmount(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	char path[256];
	struct json_writer w;
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage unmount NAME\n");
		return 2;
	}
	disk_name = argv[0];
	snprintf(path, sizeof(path), CIX_API_unmountStorage, disk_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "confirm_disk_name");
	jw_str(&w, disk_name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_line);
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

/*
 * Issue #95: what will actually fit. `disks ls` reports each partition's
 * size and the disk's size, so it looks like you can subtract -- but
 * that misses alignment, the GPT's reserved areas, and gaps left by an
 * earlier delete. This asks sfdisk.
 */
static void fmt_disk_free_space(const struct json_value *v)
{
	const struct json_value *has_table = json_object_get(v, "has_partition_table");
	const struct json_value *extents = json_object_get(v, "extents");
	double total = json_as_number(json_object_get(v, "total_free_bytes"));
	double largest = json_as_number(json_object_get(v, "largest_free_bytes"));
	int i;

	if (has_table == NULL || has_table->type != JSON_BOOL || !has_table->u.boolean) {
		printf("no partition table -- create one first (disks partition-table NAME)\n");
		return;
	}
	printf("total free:   %.0f bytes (%.0f MiB)\n", total, total / (1024 * 1024));
	printf("largest gap:  %.0f bytes (%.0f MiB)  <- the most one new partition can take\n", largest,
	       largest / (1024 * 1024));
	if (extents != NULL && extents->type == JSON_ARRAY && extents->u.array.count > 1) {
		printf("free space is split across %d gaps:\n", extents->u.array.count);
		for (i = 0; i < extents->u.array.count; i++) {
			const struct json_value *e = extents->u.array.items[i];

			printf("  start sector %-12.0f %.0f MiB\n",
			       json_as_number(json_object_get(e, "start_sector")),
			       json_as_number(json_object_get(e, "bytes")) / (1024 * 1024));
		}
	}
}

/*
 * Issue #94: grow a partition, and the filesystem inside it. Grow only
 * -- shrinking needs the filesystem shrunk first, and cutting the table
 * entry before that destroys the tail of a live filesystem.
 */
static int cmd_disks_grow_partition(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name = NULL;
	const char *part_name = NULL;
	long size_mib = 0;
	char path[300];
	struct json_writer w;
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--size-mib=", 11) == 0)
			size_mib = atol(argv[i] + 11);
		else if (disk_name == NULL)
			disk_name = argv[i];
		else if (part_name == NULL)
			part_name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown grow-partition option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (disk_name == NULL || part_name == NULL) {
		fprintf(stderr, "usage: cixctl storage grow-partition DISK_NAME PARTITION_NAME "
		                "[--size-mib=N]\n"
		                "  Omit --size-mib to take all free space immediately after it.\n"
		                "  The partition must be unmounted, and ext4 or unformatted.\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_resizeStoragePartition, disk_name, part_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "size_mib");
	jw_int(&w, size_mib);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_or_partitions);
}

static int cmd_disks_free_space(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[256];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage free-space NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getStorageFreeSpace, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_disk_free_space);
}

static int cmd_disks_partition_table(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	char path[256];
	struct json_writer w;
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage partition-table NAME\n");
		return 2;
	}
	disk_name = argv[0];
	snprintf(path, sizeof(path), CIX_API_createStoragePartitionTable, disk_name);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "confirm_disk_name");
	jw_str(&w, disk_name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_or_partitions);
}

static int cmd_disks_add_partition(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name;
	const char *part_name = NULL;
	unsigned long long size_mib = 0;
	char path[256];
	struct json_writer w;
	struct cix_response r;
	int i;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: cixctl storage add-partition NAME --name=PART_NAME [--size-mib=N]\n"
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
		fprintf(stderr, "cixctl: disks add-partition requires --name=PART_NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_addStoragePartition, disk_name);

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

	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_disk_or_partitions);
}

static int cmd_disks_rm_partition(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[256];
	struct cix_response r;

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl storage rm-partition DISK_NAME PARTITION_NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteStoragePartition, argv[0], argv[1]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		cix_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("partition removed\n");
	else
		printf("error: status %d\n", r.status);
	cix_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_disks(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	if (strcmp(sub, "unmount") == 0)
		return cmd_disks_unmount(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "free-space") == 0)
		return cmd_disks_free_space(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "partition-table") == 0)
		return cmd_disks_partition_table(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "add-partition") == 0)
		return cmd_disks_add_partition(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "grow-partition") == 0)
		return cmd_disks_grow_partition(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm-partition") == 0)
		return cmd_disks_rm_partition(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "usage: cixctl storage [ls]\n"
	                "       cixctl storage format NAME [--fs-type=ext4|btrfs]\n"
	                "       cixctl storage format-status NAME\n"
	                "       cixctl storage unmount NAME\n"
	                "       cixctl storage free-space NAME\n"
	                "       cixctl storage partition-table NAME\n"
	                "       cixctl storage add-partition NAME --name=PART_NAME [--size-mib=N]\n"
	                "       cixctl storage grow-partition DISK_NAME PARTITION_NAME [--size-mib=N]\n"
	                "       cixctl storage rm-partition DISK_NAME PARTITION_NAME\n");
	return 2;
}

/*
 * ADR-0141 Phase 2: only "state" is wired to a real REST resource yet
 * (GET/POST /v1/system/rebuildable-storage(/migrate)) -- "logs"
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

/*
 * cixctl storage {state,logs,rebuildable} all share this shape; only
 * which storage kind is acted on differs.
 *
 * That used to be expressed by interpolating a URL segment --
 * snprintf(path, "/v1/system/%s", endpoint) with endpoint being
 * "log-storage" / "rebuildable-storage". Convenient,
 * and exactly the drift ADR-0218 removes: a path assembled from a
 * variable is a path no generated constant covers, so a renamed
 * endpoint would compile here and 404 at runtime. The three paths now
 * come from the contract and are passed in, so the shared shape is
 * kept without any client-side path synthesis.
 */
static int cmd_storage_kind_show(const struct cix_client *c, int json_mode, const char *path_fmt)
{
	char path[64];
	struct cix_response r;

	snprintf(path, sizeof(path), "%s", path_fmt);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_placement);
}

static int cmd_storage_kind_migrate(const struct cix_client *c, int json_mode, const char *path_fmt,
                                     int argc, char **argv)
{
	const char *disk = NULL;
	char path[64];
	struct json_writer w;
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown storage migrate option '%s'\n", argv[i]);
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

	snprintf(path, sizeof(path), "%s", path_fmt);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_storage_kind_migrate_status(const struct cix_client *c, int json_mode,
                                            const char *status_path)
{
	char path[64];
	struct cix_response r;

	snprintf(path, sizeof(path), "%s", status_path);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

/* Three paths, not two: the migrate POST and its status GET share a URL
 * but are different operations, and each call site names its own -- the
 * coverage check (ADR-0218 layer 2) can only mean something if an
 * operation is credited to a channel that genuinely invokes it. */
static int cmd_storage_kind(const struct cix_client *c, int json_mode, const char *name,
                             const char *show_path, const char *migrate_path,
                             const char *status_path, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_storage_kind_show(c, json_mode, show_path);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_storage_kind_show(c, json_mode, show_path);
	if (strcmp(sub, "migrate") == 0)
		return cmd_storage_kind_migrate(c, json_mode, migrate_path, argc - 1, argv + 1);
	if (strcmp(sub, "migrate-status") == 0)
		return cmd_storage_kind_migrate_status(c, json_mode, status_path);

	fprintf(stderr,
	        "usage: cixctl storage %s [show]\n"
	        "       cixctl storage %s migrate [--disk=NAME]  -- omit for the default "
	        "OS-disk placement\n"
	        "       cixctl storage %s migrate-status\n",
	        name, name, name);
	return 2;
}

static int cmd_storage(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage logs|rebuildable [show|migrate|migrate-status]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "logs") == 0)
		return cmd_storage_kind(c, json_mode, "logs", CIX_API_getLogStorage,
		                         CIX_API_migrateLogStorage, CIX_API_getLogStorageMigrateStatus, argc - 1, argv + 1);
	if (strcmp(sub, "rebuildable") == 0)
		return cmd_storage_kind(c, json_mode, "rebuildable", CIX_API_getRebuildableStorage,
		                         CIX_API_migrateRebuildableStorage, CIX_API_getRebuildableStorageMigrateStatus, argc - 1, argv + 1);

	fprintf(stderr, "usage: cixctl storage logs|rebuildable [show|migrate|migrate-status]\n");
	return 2;
}

/* ADR-0160: cixctl sysctl show|get|set|rm -- host-level /proc/sys,
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

static int cmd_sysctl_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listSystemSysctl_METHOD, CIX_API_listSystemSysctl, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_sysctl_list);
}

static int cmd_sysctl_get(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl sysctl get KEY\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getSystemSysctl, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_sysctl_one);
}

/* cixctl sysctl set KEY --value=V [--value=V ...] [--no-persist]
 * -- repeatable --value= (matching resolv set's own --nameserver=
 * repeatable-flag convention) builds a JSON array when given more
 * than once, a bare JSON string when given exactly once -- either
 * shape the daemon already accepts. */
#define CLI_SYSCTL_MAX_VALUES 8

static int cmd_sysctl_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *key;
	const char *values[CLI_SYSCTL_MAX_VALUES];
	int value_count = 0;
	int persist = 1;
	int i;
	char path[192];
	struct json_writer w;
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl sysctl set KEY --value=V [--value=V ...] [--no-persist]\n");
		return 2;
	}
	key = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--value=", 8) == 0) {
			if (value_count >= CLI_SYSCTL_MAX_VALUES) {
				fprintf(stderr, "cixctl: too many --value= flags (max %d)\n",
				        CLI_SYSCTL_MAX_VALUES);
				return 2;
			}
			values[value_count++] = argv[i] + 8;
		} else if (strcmp(argv[i], "--no-persist") == 0) {
			persist = 0;
		} else {
			fprintf(stderr, "cixctl: unknown sysctl set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (value_count == 0) {
		fprintf(stderr, "cixctl: sysctl set requires at least one --value=\n");
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

	snprintf(path, sizeof(path), CIX_API_putSystemSysctl, key);
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_sysctl_one);
}

static int cmd_sysctl_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl sysctl rm KEY\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteSystemSysctl, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		cix_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("removed from persisted config (live value untouched)\n");
	else
		printf("error: status %d\n", r.status);
	cix_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_sysctl(const struct cix_client *c, int json_mode, int argc, char **argv)
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

	fprintf(stderr, "usage: cixctl sysctl [show]  -- every persisted (daemon-managed) key\n"
	                "       cixctl sysctl get KEY  -- live current value, persisted or not\n"
	                "       cixctl sysctl set KEY --value=V [--value=V ...] [--no-persist]\n"
	                "       cixctl sysctl rm KEY  -- stop reapplying at boot (live value untouched)\n");
	return 2;
}

/* ADR-0159 Phase A: `cixctl kmod ls|show|load|unload` (management)
 * and `cixctl kmod-config set|ls|rm` (persisted default options +
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

static int cmd_kmod_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listKmod_METHOD, CIX_API_listKmod, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmod_list);
}

static int cmd_kmod_show(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl kmod show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getKmodInfo, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmod_info);
}

#define CLI_KMOD_MAX_OPTIONS 8

static int cmd_kmod_load(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name;
	const char *options[CLI_KMOD_MAX_OPTIONS];
	int option_count = 0;
	int i;
	char path[192];
	struct json_writer w;
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl kmod load NAME [--option=KEY=VALUE ...]\n");
		return 2;
	}
	name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--option=", 9) == 0) {
			if (option_count >= CLI_KMOD_MAX_OPTIONS) {
				fprintf(stderr, "cixctl: too many --option= flags (max %d)\n",
				        CLI_KMOD_MAX_OPTIONS);
				return 2;
			}
			options[option_count++] = argv[i] + 9;
		} else {
			fprintf(stderr, "cixctl: unknown kmod load option '%s'\n", argv[i]);
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
			fprintf(stderr, "cixctl: --option= must be KEY=VALUE, got '%s'\n", options[i]);
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

	snprintf(path, sizeof(path), CIX_API_postKmodLoad, name);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		cix_response_free(&r);
		return r.status == 200 ? 0 : 1;
	}
	if (r.status == 200)
		printf("loaded %s\n", name);
	else
		printf("error: status %d\n", r.status);
	cix_response_free(&r);
	return r.status == 200 ? 0 : 1;
}

static int cmd_kmod_unload(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl kmod unload NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteKmod, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		cix_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("unloaded %s\n", argv[0]);
	else
		printf("error: status %d\n", r.status);
	cix_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_kmod(const struct cix_client *c, int json_mode, int argc, char **argv)
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

	fprintf(stderr, "usage: cixctl kmod [ls]  -- every currently-loaded module (/proc/modules)\n"
	                "       cixctl kmod show NAME  -- modinfo: description, params, depends\n"
	                "       cixctl kmod load NAME [--option=KEY=VALUE ...]  -- real modprobe;\n"
	                "               no --option= falls back to this module's own persisted\n"
	                "               kmod-config default_options, if any\n"
	                "       cixctl kmod unload NAME  -- real modprobe -r\n");
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

static int cmd_kmodconfig_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listKmodConfig_METHOD, CIX_API_listKmodConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmodconfig_list);
}

static int cmd_kmodconfig_set(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl kmod-config set NAME [--option=KEY=VALUE ...] "
		                "[--autoload|--no-autoload]\n");
		return 2;
	}
	name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--option=", 9) == 0) {
			if (option_count >= CLI_KMOD_MAX_OPTIONS) {
				fprintf(stderr, "cixctl: too many --option= flags (max %d)\n",
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
			fprintf(stderr, "cixctl: unknown kmod-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (!have_options && !have_autoload) {
		fprintf(stderr, "cixctl: kmod-config set requires --option= and/or "
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
				fprintf(stderr, "cixctl: --option= must be KEY=VALUE, got '%s'\n", options[i]);
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

	snprintf(path, sizeof(path), CIX_API_putKmodConfig, name);
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_kmodconfig_one);
}

static int cmd_kmodconfig_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[192];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl kmod-config rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteKmodConfig, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (json_mode) {
		printf("{\"status\":%d}\n", r.status);
		cix_response_free(&r);
		return r.status == 204 ? 0 : 1;
	}
	if (r.status == 204)
		printf("removed kmod-config for %s\n", argv[0]);
	else
		printf("error: status %d\n", r.status);
	cix_response_free(&r);
	return r.status == 204 ? 0 : 1;
}

static int cmd_kmodconfig(const struct cix_client *c, int json_mode, int argc, char **argv)
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

	fprintf(stderr, "usage: cixctl kmod-config [ls]  -- every module with a persisted default\n"
	                "       cixctl kmod-config set NAME [--option=KEY=VALUE ...] "
	                "[--autoload|--no-autoload]\n"
	                "       cixctl kmod-config rm NAME  -- clears both fields\n");
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

static int cmd_resolv_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemResolv_METHOD, CIX_API_getSystemResolv, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_resolv);
}

/* cixctl resolv set --nameserver=A.B.C.D [--nameserver=A.B.C.D ...]
 * -- repeatable, same convention run's own --network=/--device=/
 * --interface= already use. No flags at all means an empty list --
 * clears the host's own resolver config entirely, same "the absence
 * of the flag is a real, valid choice" precedent --clear-bind-ip
 * established for daemon-config. */
static int cmd_resolv_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *nameservers[CLI_RESOLV_MAX_NAMESERVERS];
	int count = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--nameserver=", 13) == 0) {
			if (count >= CLI_RESOLV_MAX_NAMESERVERS) {
				fprintf(stderr, "cixctl: too many --nameserver= flags (max %d)\n",
				        CLI_RESOLV_MAX_NAMESERVERS);
				return 2;
			}
			nameservers[count++] = argv[i] + 13;
		} else {
			fprintf(stderr, "cixctl: unknown resolv set option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_putSystemResolv_METHOD, CIX_API_putSystemResolv, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_resolv);
}

static int cmd_resolv(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	        "usage: cixctl resolv [show]\n"
	        "       cixctl resolv set [--nameserver=A.B.C.D ...]\n");
	return 2;
}

/* cixctl ntp config show/set, ntp status, ntp server register/ls/
 * unregister, and the separate cixctl time show/set -- task
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

static int cmd_ntp_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemNtp_METHOD, CIX_API_getSystemNtp, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_config);
}

static int cmd_ntp_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *upstream[CLI_NTP_MAX_UPSTREAM];
	int count = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--server=", 9) == 0) {
			if (count >= CLI_NTP_MAX_UPSTREAM) {
				fprintf(stderr, "cixctl: too many --server= flags (max %d)\n",
				        CLI_NTP_MAX_UPSTREAM);
				return 2;
			}
			upstream[count++] = argv[i] + 9;
		} else {
			fprintf(stderr, "cixctl: unknown ntp config set option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_putSystemNtp_METHOD, CIX_API_putSystemNtp, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_ntp_config);
}

static int cmd_ntp_config(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	        "usage: cixctl ntp config [show]\n"
	        "       cixctl ntp config set [--server=A.B.C.D ...]\n");
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

static int cmd_ntp_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemNtpStatus_METHOD, CIX_API_getSystemNtpStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_status);
}

/* Triggers one sync attempt on demand rather than waiting for the
 * next hourly automatic fire -- 202 on success, no body; check
 * `ntp status` afterward for the outcome. */
static int cmd_ntp_sync(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_postSystemNtpSync_METHOD, CIX_API_postSystemNtpSync, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "sync failed to start",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	if (!json_mode)
		printf("sync started\n");
	else
		print_raw_json(r.json);
	cix_response_free(&r);
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

static int cmd_ntp_server_register(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown ntp server register option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (container == NULL) {
		fprintf(stderr, "usage: cixctl ntp server register --container=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createNtpServer_METHOD, CIX_API_createNtpServer, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_ntp_server_line);
}

static int cmd_ntp_server_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listNtpServers_METHOD, CIX_API_listNtpServers, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ntp_server_list);
}

static int cmd_ntp_server_unregister(const struct cix_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: ntp server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteNtpServer, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ntp_server(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl ntp server register --container=NAME\n"
		        "       cixctl ntp server ls\n"
		        "       cixctl ntp server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_ntp_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ntp_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_ntp_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown ntp server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ntp(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl ntp config [show]\n"
		        "       cixctl ntp config set [--server=A.B.C.D ...]\n"
		        "       cixctl ntp status\n"
		        "       cixctl ntp sync  -- trigger a sync attempt now, don't wait for the\n"
		        "               next hourly automatic one\n"
		        "       cixctl ntp server register --container=NAME\n"
		        "       cixctl ntp server ls\n"
		        "       cixctl ntp server unregister CONTAINER\n");
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

	fprintf(stderr, "cixctl: unknown ntp subcommand '%s'\n", sub);
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

static int cmd_syslog_target_register(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown syslog target register option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (container == NULL) {
		fprintf(stderr, "usage: cixctl syslog target register --container=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "container");
	jw_str(&w, container);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createSyslogTarget_METHOD, CIX_API_createSyslogTarget, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_syslog_target_line);
}

static int cmd_syslog_target_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listSyslogTargets_METHOD, CIX_API_listSyslogTargets, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_syslog_target_list);
}

static int cmd_syslog_target_unregister(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: syslog target unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteSyslogTarget, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_syslog_target(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl syslog target register --container=NAME\n"
		        "       cixctl syslog target ls\n"
		        "       cixctl syslog target unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_syslog_target_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_syslog_target_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_syslog_target_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown syslog target subcommand '%s'\n", sub);
	return 2;
}

static int cmd_syslog(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl syslog target register --container=NAME  -- forward every\n"
		        "               container-sourced log line to this running container as a real\n"
		        "               RFC 3164 UDP syslog datagram (e.g. syslog-1/syslog-2 running\n"
		        "               sysklogd), alongside (never instead of) the consolidated log\n"
		        "               store every other 'cixctl logs' call already reads from\n"
		        "               (ADR-0127)\n"
		        "       cixctl syslog target ls\n"
		        "       cixctl syslog target unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "target") == 0)
		return cmd_syslog_target(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown syslog subcommand '%s'\n", sub);
	return 2;
}

static void fmt_time(const struct json_value *v)
{
	const struct json_value *unixtime = json_object_get(v, "unixtime");

	if (unixtime != NULL && unixtime->type == JSON_NUMBER)
		printf("unixtime=%lld\n", (long long)unixtime->u.number);
}

static int cmd_time_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemTime_METHOD, CIX_API_getSystemTime, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_time);
}

static int cmd_time_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *unixtime = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--unixtime=", 11) == 0)
			unixtime = argv[i] + 11;
		else {
			fprintf(stderr, "cixctl: unknown time set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (unixtime == NULL) {
		fprintf(stderr, "usage: cixctl time set --unixtime=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "unixtime");
	jw_num(&w, atof(unixtime));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putSystemTime_METHOD, CIX_API_putSystemTime, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_time);
}

static int cmd_time(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	        "usage: cixctl time [show]\n"
	        "       cixctl time set --unixtime=N\n");
	return 2;
}

/*
 * DHCP (ADR-0197). Served by the same dnsmasq that serves DNS, which is
 * why enabling it names a registered DNS server rather than a host and
 * port -- a lease resolves the moment it is handed out.
 */
static void fmt_dhcp(const struct json_value *v)
{
	const struct json_value *nets = json_object_get(v, "networks");
	const struct json_value *statics = json_object_get(v, "static");
	size_t i;
	int any = 0;

	if (nets != NULL && nets->type == JSON_ARRAY) {
		for (i = 0; i < nets->u.array.count; i++) {
			const struct json_value *n = nets->u.array.items[i];
			const struct json_value *en = json_object_get(n, "enabled");
			int on = en != NULL && en->type == JSON_BOOL && en->u.boolean;

			const struct json_value *slices = json_object_get(n, "slices");

			if (!any) {
				printf("%-16s %-9s %-34s %s\n", "NETWORK", "STATE", "RANGE", "LEASE");
				any = 1;
			}
			{
				char range[80];

				if (json_str_field(n, "range_start") != NULL)
					snprintf(range, sizeof(range), "%s - %s", json_str_field(n, "range_start"),
					         json_str_field(n, "range_end"));
				else
					snprintf(range, sizeof(range), "%s", "-");
				printf("%-16s %-9s %-34s %lds\n",
				       json_str_field(n, "network") != NULL ? json_str_field(n, "network") : "-",
				       on ? "enabled" : "disabled", range,
				       (long)json_as_number(json_object_get(n, "lease_seconds")));
			}
			/*
			 * Which server holds which part. dnsmasq has no failover
			 * protocol, so redundancy here is disjoint pools -- and the
			 * split is the thing worth seeing, since it is what makes
			 * two servers safe rather than merely two.
			 */
			if (slices != NULL && slices->type == JSON_ARRAY) {
				size_t k;

				for (k = 0; k < slices->u.array.count; k++) {
					const struct json_value *sl = slices->u.array.items[k];

					printf("    served by %-14s %s - %s\n", json_str_field(sl, "server"),
					       json_str_field(sl, "range_start"), json_str_field(sl, "range_end"));
				}
			}
		}
	}
	if (!any)
		printf("no network has DHCP configured\n");
	if (statics != NULL && statics->type == JSON_ARRAY && statics->u.array.count > 0) {
		printf("\n%-20s %-16s %s\n", "MAC", "IP", "HOSTNAME");
		for (i = 0; i < statics->u.array.count; i++) {
			const struct json_value *e = statics->u.array.items[i];

			printf("%-20s %-16s %s\n", json_str_field(e, "mac"), json_str_field(e, "ip"),
			       json_str_field(e, "hostname") != NULL ? json_str_field(e, "hostname") : "-");
		}
	}
}

/* Whether a DHCP server is also a DNS server is REPORTED, not required:
 * dnsmasq answers for the names of clients it leased addresses to, so a
 * server that is both makes leases resolve the moment they are issued.
 * Saying which is true beats one service quietly requiring the other. */
static void fmt_dhcp_servers(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	size_t i;

	if (servers == NULL || servers->type != JSON_ARRAY || servers->u.array.count == 0) {
		printf("no DHCP server registered\n");
		return;
	}
	printf("%-24s %-12s %s\n", "CONTAINER", "STATE", "RESOLVES LEASES");
	for (i = 0; i < servers->u.array.count; i++) {
		const struct json_value *e = servers->u.array.items[i];
		const struct json_value *run = json_object_get(e, "running");
		const struct json_value *res = json_object_get(e, "resolves_leases");

		printf("%-24s %-12s %s\n", json_str_field(e, "container"),
		       run != NULL && run->type == JSON_BOOL && run->u.boolean ? "running" : "stopped",
		       res != NULL && res->type == JSON_BOOL && res->u.boolean
		           ? "yes"
		           : "no -- not a DNS server");
	}
}

static void fmt_dhcp_leases(const struct json_value *v)
{
	const struct json_value *leases = json_object_get(v, "leases");
	size_t i;

	if (leases == NULL || leases->type != JSON_ARRAY || leases->u.array.count == 0) {
		printf("no leases -- nothing has asked for an address yet\n");
		return;
	}
	printf("%-20s %-16s %-20s %-12s %s\n", "MAC", "IP", "HOSTNAME", "EXPIRES(UNIX)", "SERVER");
	for (i = 0; i < leases->u.array.count; i++) {
		const struct json_value *e = leases->u.array.items[i];

		printf("%-20s %-16s %-20s %-12lld %s\n", json_str_field(e, "mac"),
		       json_str_field(e, "ip"),
		       json_str_field(e, "hostname") != NULL ? json_str_field(e, "hostname") : "-",
		       (long long)json_as_number(json_object_get(e, "expires_at")),
		       json_str_field(e, "server"));
	}
}

static int cmd_dhcp(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	const char *network = NULL, *range = NULL, *router = NULL;
	const char *servers[DHCP_CLI_MAX_SERVERS];
	int server_count = 0;
	const char *mac = NULL, *ip = NULL, *hostname = NULL;
	long lease_seconds = -1;
	char path[256];
	struct json_writer w;
	int i;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getDhcp_METHOD, CIX_API_getDhcp, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_dhcp);
	}
	if (strcmp(sub, "leases") == 0) {
		if (cix_client_request(c, CIX_API_getDhcpLeases_METHOD, CIX_API_getDhcpLeases, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_dhcp_leases);
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--network=", 10) == 0)
			network = argv[i] + 10;
		else if (strncmp(argv[i], "--range=", 8) == 0)
			range = argv[i] + 8;
		else if (strncmp(argv[i], "--router=", 9) == 0)
			router = argv[i] + 9;
		else if (strncmp(argv[i], "--server=", 9) == 0) {
			if (server_count >= DHCP_CLI_MAX_SERVERS) {
				fprintf(stderr, "cixctl: too many --server= flags (max %d)\n",
				        DHCP_CLI_MAX_SERVERS);
				return 2;
			}
			servers[server_count++] = argv[i] + 9;
		}
		else if (strncmp(argv[i], "--lease-seconds=", 16) == 0)
			lease_seconds = atol(argv[i] + 16);
		else if (strncmp(argv[i], "--mac=", 6) == 0)
			mac = argv[i] + 6;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (strncmp(argv[i], "--hostname=", 11) == 0)
			hostname = argv[i] + 11;
		else if (argv[i][0] != '-' && mac == NULL)
			mac = argv[i];
	}

	/*
	 * `remove` deletes a network's DHCP configuration outright, which
	 * is not what `disable` does -- that keeps the range and stops
	 * serving it. Without this the config could be created and edited
	 * from here but never taken back, and DELETE /dhcp/networks/{n}
	 * had no interface on either channel at all (ADR-0218 layer 2
	 * turned that from an accident into a visible gap).
	 */
	if (strcmp(sub, "remove") == 0) {
		if (network == NULL) {
			fprintf(stderr,
			        "usage: cixctl dhcp remove --network=NAME\n"
			        "  Deletes this network's DHCP configuration entirely. To stop serving\n"
			        "  while keeping the range, use `cixctl dhcp disable --network=NAME`.\n");
			return 2;
		}
		snprintf(path, sizeof(path), CIX_API_deleteDhcpNetwork, network);
		if (cix_client_request(c, CIX_API_deleteDhcpNetwork_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, NULL);
	}

	if (strcmp(sub, "enable") == 0 || strcmp(sub, "disable") == 0) {
		int enable = strcmp(sub, "enable") == 0;

		if (network == NULL || (enable && (range == NULL || server_count == 0))) {
			fprintf(stderr,
			        "usage: cixctl dhcp enable --network=NAME --range=START-END\n"
			        "                            --server=CONTAINER [--server=CONTAINER ...]\n"
			        "                            [--lease-seconds=N] [--router=IP]\n"
			        "       cixctl dhcp disable --network=NAME\n"
			        "       cixctl dhcp remove --network=NAME   -- delete the config entirely\n"
			        "  --server= is repeatable. dnsmasq has no failover protocol, so a range\n"
			        "  named to more than one server is split into disjoint slices: all of\n"
			        "  them answer, and no two hold the same address. Register servers first\n"
			        "  with `cixctl dhcp server add`.\n");
			return 2;
		}
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "enabled");
		jw_bool(&w, enable);
		if (range != NULL) {
			const char *dash = strchr(range, '-');

			if (dash == NULL) {
				jw_free(&w);
				fprintf(stderr, "cixctl: --range= must be START-END, e.g. "
				                "172.30.0.100-172.30.0.200\n");
				return 2;
			}
			{
				char start[64];
				size_t n = (size_t)(dash - range);

				if (n >= sizeof(start)) {
					jw_free(&w);
					fprintf(stderr, "cixctl: --range= start is too long\n");
					return 2;
				}
				memcpy(start, range, n);
				start[n] = '\0';
				jw_key(&w, "range_start");
				jw_str(&w, start);
				jw_key(&w, "range_end");
				jw_str(&w, dash + 1);
			}
		}
		if (router != NULL) {
			jw_key(&w, "router");
			jw_str(&w, router);
		}
		if (server_count > 0) {
			jw_key(&w, "servers");
			jw_arr_open(&w);
			for (i = 0; i < server_count; i++)
				jw_str(&w, servers[i]);
			jw_arr_close(&w);
		}
		if (lease_seconds >= 0) {
			jw_key(&w, "lease_seconds");
			jw_int(&w, lease_seconds);
		}
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		snprintf(path, sizeof(path), CIX_API_setDhcpNetwork, network);
		if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		return emit(&r, json_mode, NULL);
	}

	if (strcmp(sub, "server") == 0) {
		const char *op = argc > 1 ? argv[1] : "";

		if (strcmp(op, "add") == 0 && argc > 2) {
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "container");
			jw_str(&w, argv[2]);
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			if (cix_client_request(c, CIX_API_registerDhcpServer_METHOD, CIX_API_registerDhcpServer, w.buf, &r) != 0) {
				jw_free(&w);
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
			jw_free(&w);
			return emit(&r, json_mode, fmt_dhcp_servers);
		}
		if (strcmp(op, "rm") == 0 && argc > 2) {
			snprintf(path, sizeof(path), CIX_API_unregisterDhcpServer, argv[2]);
			if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
			return emit(&r, json_mode, NULL);
		}
		if (strcmp(op, "ls") == 0 || op[0] == '\0') {
			if (cix_client_request(c, CIX_API_getDhcpServers_METHOD, CIX_API_getDhcpServers, NULL, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
			return emit(&r, json_mode, fmt_dhcp_servers);
		}
		fprintf(stderr, "usage: cixctl dhcp server ls | add CONTAINER | rm CONTAINER\n");
		return 2;
	}
	if (strcmp(sub, "static") == 0) {
		const char *op = argc > 1 ? argv[1] : "";

		if (strcmp(op, "add") == 0) {
			if (mac == NULL || ip == NULL) {
				fprintf(stderr, "usage: cixctl dhcp static add --mac=M --ip=IP "
				                "[--hostname=NAME]\n");
				return 2;
			}
			jw_init(&w);
			jw_obj_open(&w);
			jw_key(&w, "mac");
			jw_str(&w, mac);
			jw_key(&w, "ip");
			jw_str(&w, ip);
			if (hostname != NULL) {
				jw_key(&w, "hostname");
				jw_str(&w, hostname);
			}
			jw_obj_close(&w);
			w.buf[w.len] = '\0';
			if (cix_client_request(c, CIX_API_addDhcpReservation_METHOD, CIX_API_addDhcpReservation, w.buf, &r) != 0) {
				jw_free(&w);
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
			jw_free(&w);
			return emit(&r, json_mode, fmt_dhcp);
		}
		if (strcmp(op, "rm") == 0) {
			if (argc < 3) {
				fprintf(stderr, "usage: cixctl dhcp static rm MAC\n");
				return 2;
			}
			snprintf(path, sizeof(path), CIX_API_deleteDhcpReservation, argv[2]);
			if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
			return emit(&r, json_mode, NULL);
		}
		fprintf(stderr, "usage: cixctl dhcp static add --mac=M --ip=IP [--hostname=NAME]\n"
		                "       cixctl dhcp static rm MAC\n");
		return 2;
	}

	fprintf(stderr, "usage: cixctl dhcp show | leases\n"
	                "       cixctl dhcp server ls | add CONTAINER | rm CONTAINER\n"
	                "       cixctl dhcp enable --network=NAME --range=START-END\n"
	                "       cixctl dhcp disable --network=NAME\n"
	                "       cixctl dhcp static add --mac=M --ip=IP [--hostname=NAME]\n"
	                "       cixctl dhcp static rm MAC\n");
	return 2;
}

/*
 * Issue #51: cixctl zswap show|set. Prints the configured intent and
 * what the kernel actually reports side by side -- they are different
 * questions, and the only interesting case is when they differ.
 */
static void fmt_zswap(const struct json_value *v)
{
	const struct json_value *supported = json_object_get(v, "supported");
	const struct json_value *enabled = json_object_get(v, "enabled");
	const struct json_value *pct = json_object_get(v, "max_pool_percent");
	const struct json_value *kernel = json_object_get(v, "kernel");
	const struct json_value *avail = json_object_get(v, "available_compressors");
	const char *comp = json_str_field(v, "compressor");
	int cfg_on = enabled != NULL && enabled->type == JSON_BOOL && enabled->u.boolean;

	if (supported != NULL && supported->type == JSON_BOOL && !supported->u.boolean) {
		printf("this kernel has no zswap at all -- nothing here can take effect\n");
		return;
	}
	printf("configured: %s, max_pool_percent=%d, compressor=%s\n", cfg_on ? "enabled" : "disabled",
	       (int)json_as_number(pct), comp != NULL ? comp : "-");
	if (kernel != NULL) {
		const struct json_value *ken = json_object_get(kernel, "enabled");
		const char *kcomp = json_str_field(kernel, "compressor");
		int k_on = ken != NULL && ken->type == JSON_BOOL && ken->u.boolean;

		printf("kernel:     %s, max_pool_percent=%d, compressor=%s\n",
		       ken == NULL || ken->type == JSON_NULL ? "unknown" : k_on ? "enabled" : "disabled",
		       (int)json_as_number(json_object_get(kernel, "max_pool_percent")),
		       kcomp != NULL ? kcomp : "-");
		if (ken != NULL && ken->type == JSON_BOOL && k_on != cfg_on)
			printf("  ^ these disagree: the kernel did not take what was configured\n");
	}
	if (avail != NULL && avail->type == JSON_ARRAY) {
		size_t i;

		printf("compressors this kernel has:");
		for (i = 0; i < avail->u.array.count; i++)
			printf(" %s", json_as_string(avail->u.array.items[i]));
		printf("%s\n", avail->u.array.count == 0 ? " (none)" : "");
	}
}

/*
 * Issue #50: cixctl ksm show|set. Same two-column shape as zswap --
 * what was asked for, and what the kernel actually reports -- for the
 * same reason: they can differ, and only the second one is true.
 */
static long long ksm_num(const struct json_value *v, const char *key, long long dflt)
{
	const struct json_value *n = json_object_get(v, key);

	return (n != NULL && n->type == JSON_NUMBER) ? (long long)n->u.number : dflt;
}

static int ksm_flag(const struct json_value *v, const char *key)
{
	const struct json_value *b = json_object_get(v, key);

	return b != NULL && b->type == JSON_BOOL && b->u.boolean;
}

static void fmt_ksm(const struct json_value *v)
{
	const struct json_value *supported = json_object_get(v, "supported");
	long long run;

	if (supported != NULL && supported->type == JSON_BOOL && !supported->u.boolean) {
		printf("this kernel has no KSM at all -- nothing here can take effect\n");
		return;
	}
	printf("configured : %s  pages_to_scan=%lld  sleep_millisecs=%lld\n",
	       ksm_flag(v, "enabled") ? "on" : "off",
	       ksm_num(v, "pages_to_scan", 0), ksm_num(v, "sleep_millisecs", 0));

	run = ksm_num(v, "kernel_run", -1);
	printf("kernel     : %s\n",
	       run == 1 ? "running" : run == 0 ? "stopped" :
	       run == 2 ? "unmerging (someone wrote 2 by hand)" : "unknown");

	/*
	 * pages_sharing answers "is this worth its CPU", so it is spelled
	 * out rather than left as one counter among several: it counts
	 * pages that were eliminated. Near zero means the scanner is
	 * running for nothing.
	 */
	printf("saved      : %lld pages merged away (%lld physical pages hold the shared content)\n",
	       ksm_num(v, "pages_sharing", 0), ksm_num(v, "pages_shared", 0));
	printf("scanning   : %lld unshared, %lld volatile, %lld full scans\n",
	       ksm_num(v, "pages_unshared", 0), ksm_num(v, "pages_volatile", 0),
	       ksm_num(v, "full_scans", 0));
}

static int cmd_ksm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	long pages_to_scan = -1, sleep_millisecs = -1;
	int enabled = -1;
	int i;
	struct json_writer w;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getKsm_METHOD, CIX_API_getKsm, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_ksm);
	}
	if (strcmp(sub, "set") != 0) {
		fprintf(stderr, "usage: cixctl ksm show\n"
		                "       cixctl ksm set [--enable | --disable] "
		                "[--pages-to-scan=N] [--sleep-millisecs=N]\n"
		                "  KSM merges identical memory pages across containers. It merges\n"
		                "  nothing until a container opts in, so enabling the scanner alone\n"
		                "  costs CPU and saves nothing\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--enable") == 0)
			enabled = 1;
		else if (strcmp(argv[i], "--disable") == 0)
			enabled = 0;
		else if (strncmp(argv[i], "--pages-to-scan=", 16) == 0)
			pages_to_scan = atol(argv[i] + 16);
		else if (strncmp(argv[i], "--sleep-millisecs=", 18) == 0)
			sleep_millisecs = atol(argv[i] + 18);
		else {
			fprintf(stderr, "cixctl: unknown ksm set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (enabled < 0 && pages_to_scan < 0 && sleep_millisecs < 0) {
		fprintf(stderr, "usage: cixctl ksm set [--enable | --disable] "
		                "[--pages-to-scan=N] [--sleep-millisecs=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (enabled >= 0) {
		jw_key(&w, "enabled");
		jw_bool(&w, enabled);
	}
	if (pages_to_scan >= 0) {
		jw_key(&w, "pages_to_scan");
		jw_int(&w, pages_to_scan);
	}
	if (sleep_millisecs >= 0) {
		jw_key(&w, "sleep_millisecs");
		jw_int(&w, sleep_millisecs);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putKsm_METHOD, CIX_API_putKsm, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_ksm);
}

static int cmd_zswap(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	const char *compressor = NULL;
	long max_pool_percent = -1;
	int enabled = -1;
	int i;
	struct json_writer w;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getZswap_METHOD, CIX_API_getZswap, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_zswap);
	}
	if (strcmp(sub, "set") != 0) {
		fprintf(stderr, "usage: cixctl zswap show\n"
		                "       cixctl zswap set [--enable | --disable] "
		                "[--max-pool-percent=N] [--compressor=NAME]\n"
		                "  zswap compresses pages in RAM before they would be written to the\n"
		                "  real swap device -- memory pressure costs CPU instead of disk I/O\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--enable") == 0)
			enabled = 1;
		else if (strcmp(argv[i], "--disable") == 0)
			enabled = 0;
		else if (strncmp(argv[i], "--max-pool-percent=", 19) == 0)
			max_pool_percent = atol(argv[i] + 19);
		else if (strncmp(argv[i], "--compressor=", 13) == 0)
			compressor = argv[i] + 13;
		else {
			fprintf(stderr, "cixctl: unknown zswap set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (enabled < 0 && max_pool_percent < 0 && compressor == NULL) {
		fprintf(stderr, "usage: cixctl zswap set [--enable | --disable] "
		                "[--max-pool-percent=N] [--compressor=NAME]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (enabled >= 0) {
		jw_key(&w, "enabled");
		jw_bool(&w, enabled);
	}
	if (max_pool_percent >= 0) {
		jw_key(&w, "max_pool_percent");
		jw_int(&w, max_pool_percent);
	}
	if (compressor != NULL) {
		jw_key(&w, "compressor");
		jw_str(&w, compressor);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_setZswap_METHOD, CIX_API_setZswap, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_zswap);
}

static int cmd_swap_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemSwap_METHOD, CIX_API_getSystemSwap, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_swap);
}

static int cmd_swap_enable(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *size_mb = NULL;
	const char *disk = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--size-mb=", 10) == 0)
			size_mb = argv[i] + 10;
		else if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown swap enable option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (size_mb == NULL) {
		fprintf(stderr, "usage: cixctl swap enable --size-mb=N [--disk=NAME]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "size_mb");
	jw_int(&w, atol(size_mb));
	/* issue #28: omitted means the default OS-disk location, same
	 * "give it to change, leave it out to keep the default" convention
	 * every other opt-in PUT/POST field in this codebase already uses. */
	if (disk != NULL) {
		jw_key(&w, "disk");
		jw_str(&w, disk);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_postSystemSwap_METHOD, CIX_API_postSystemSwap, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_swap);
}

static int cmd_swap_disable(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_deleteSystemSwap_METHOD, CIX_API_deleteSystemSwap, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_logs(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown logs option '%s'\n", argv[i]);
			return 2;
		}
	}

	o = (size_t)snprintf(path, sizeof(path), CIX_API_getSystemLogs);
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

	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status == 400) {
		fprintf(stderr, "cixctl: %s\n",
		        json_str_field(r.json, "error") != NULL ? json_str_field(r.json, "error") :
		                                                   "bad request");
		cix_response_free(&r);
		return 1;
	}
	return emit(&r, json_mode, fmt_logs);
}

static int cmd_logs_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *max_bytes = NULL;
	const char *min_level = NULL;
	int i;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-bytes=", 12) == 0)
			max_bytes = argv[i] + 12;
		else if (strncmp(argv[i], "--min-level=", 12) == 0)
			min_level = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown logs config option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (max_bytes == NULL && min_level == NULL) {
		if (cix_client_request(c, CIX_API_getSystemLogsConfig_METHOD, CIX_API_getSystemLogsConfig, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
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

		if (cix_client_request(c, CIX_API_putSystemLogsConfig_METHOD, CIX_API_putSystemLogsConfig, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
	}
	return emit(&r, json_mode, fmt_logs_config);
}

static int cmd_logs_top(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc > 0 && strcmp(argv[0], "config") == 0)
		return cmd_logs_config(c, json_mode, argc - 1, argv + 1);
	return cmd_logs(c, json_mode, argc, argv);
}

static int cmd_swap(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	        "usage: cixctl swap [status]\n"
	        "       cixctl swap enable --size-mb=N [--disk=NAME]\n"
	        "       cixctl swap disable\n");
	return 2;
}

static int cmd_shutdown(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_shutdownSystem_METHOD, CIX_API_shutdownSystem, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_reboot(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_rebootSystem_METHOD, CIX_API_rebootSystem, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_update(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *image_url = NULL;
	const char *image_sha256 = NULL;
	const char *kernel = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--image-url=", 12) == 0)
			image_url = argv[i] + 12;
		else if (strncmp(argv[i], "--image-sha256=", 15) == 0)
			image_sha256 = argv[i] + 15;
		else if (strncmp(argv[i], "--kernel=", 9) == 0)
			kernel = argv[i] + 9;
		else {
			fprintf(stderr, "cixctl: unknown update option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL && image_url == NULL && kernel == NULL) {
		fprintf(stderr, "usage: cixctl update [--image=PATH | --image-url=URL "
		                "--image-sha256=HEX] [--kernel=PATH]\n");
		return 2;
	}
	/*
	 * Caught here as well as server-side, because the two failures read
	 * very differently to whoever typed the command: a 400 arriving
	 * after a multi-megabyte download has already run is far more
	 * annoying than being told up front which flag is missing.
	 */
	if (image != NULL && image_url != NULL) {
		fprintf(stderr, "cixctl: --image= and --image-url= are alternatives, give one\n");
		return 2;
	}
	if (image_url != NULL && image_sha256 == NULL) {
		fprintf(stderr, "cixctl: --image-url= requires --image-sha256= -- this writes a boot "
		                "slot, so an unverified image is not accepted\n");
		return 2;
	}
	if (image_url == NULL && image_sha256 != NULL) {
		fprintf(stderr, "cixctl: --image-sha256= only applies to --image-url=\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (image_url != NULL) {
		jw_key(&w, "image_url");
		jw_str(&w, image_url);
		jw_key(&w, "image_sha256");
		jw_str(&w, image_sha256);
	}
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

	if (cix_client_request(c, CIX_API_updateSystem_METHOD, CIX_API_updateSystem, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_update);
}

static int cmd_ps(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listContainers_METHOD, CIX_API_listContainers, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
/*
 * `container drift` -- is this box in the state it is supposed to be
 * in? (#268)
 *
 * A deploy can succeed, the daemon can come up healthy, and the box
 * can still be broken, because containers that should be running are
 * not and nothing asks. That really happened: a deploy took dns-1 and
 * dns-2 down, every signal said it worked, and the host had silently
 * lost the ability to resolve the name it fetches its own source from.
 *
 * The rule here is not invented, it is containerdef_autostart_all()'s
 * own, applied to what the daemon already reports:
 *
 *   restart "no"                  -- never started automatically
 *   "unless-stopped" and stopped  -- an operator's own choice, not a fault
 *   anything else not running     -- drift
 *
 * That second line is the whole reason this can live in a client at
 * all. It needs to tell a crash apart from a deliberate stop, and
 * until #268 the "stopped" field was written as a constant true for
 * every container that was not running -- so the two were
 * indistinguishable and no client could ask the question.
 *
 * Exits 1 when anything has drifted, so a deploy script can gate on
 * it rather than an operator having to read the output. Exit 2 stays
 * reserved for "could not ask" -- an unreachable daemon must never
 * look like a clean box.
 */
static int container_should_be_running(const struct json_value *e)
{
	const char *restart = json_str_field(e, "restart");
	const struct json_value *stopped = json_object_get(e, "stopped");
	int is_stopped = stopped != NULL && stopped->type == JSON_BOOL && stopped->u.boolean;

	if (restart == NULL || strcmp(restart, "no") == 0)
		return 0;
	if (strcmp(restart, "unless-stopped") == 0 && is_stopped)
		return 0;
	return 1;
}

static int cmd_container_drift(const struct cix_client *c, int json_mode)
{
	struct cix_response r;
	const struct json_value *containers;
	size_t i;
	int drifted = 0;
	int stopped_note = 0;

	if (cix_client_request(c, CIX_API_listContainers_METHOD, CIX_API_listContainers, NULL, &r) !=
	    0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 2;
	}
	if (r.status != 200 || r.json == NULL) {
		fprintf(stderr, "cixctl: could not list containers (HTTP %d)\n", r.status);
		cix_response_free(&r);
		return 2;
	}
	if (json_mode) {
		printf("%s\n", r.body != NULL ? r.body : "");
		cix_response_free(&r);
		return 0;
	}

	containers = json_object_get(r.json, "containers");
	if (containers == NULL || containers->type != JSON_ARRAY) {
		fprintf(stderr, "cixctl: unexpected response shape\n");
		cix_response_free(&r);
		return 2;
	}
	/*
	 * Two passes, because "not running" has two causes and only one of
	 * them is a fault.
	 *
	 * A container an operator stopped is not broken, even when its
	 * policy would start it again at the next boot -- "always" and
	 * "on-failure" only honour an explicit stop until then (ADR-0027/
	 * ADR-0045), so it really would come back, but calling that a
	 * fault would report the operator's own action to them as a
	 * problem. It is still worth SAYING, because it means the box will
	 * not look like this after a reboot.
	 *
	 * Only the first group sets the exit status. An "unless-stopped"
	 * container that was stopped is not listed at all: its policy
	 * honours the stop permanently, so there is nothing to say.
	 */
	for (i = 0; i < containers->u.array.count; i++) {
		const struct json_value *e = containers->u.array.items[i];
		const char *name = json_str_field(e, "name");
		const char *status = json_str_field(e, "status");
		const struct json_value *st;

		if (name == NULL || status == NULL || !container_should_be_running(e))
			continue;
		if (strcmp(status, "running") == 0)
			continue;
		st = json_object_get(e, "stopped");
		if (st != NULL && st->type == JSON_BOOL && st->u.boolean)
			continue; /* second pass */
		if (!drifted)
			printf("containers that should be running and are not:\n");
		drifted++;
		printf("  %-24s %-10s restart=%s\n", name, status, json_str_field(e, "restart"));
	}
	for (i = 0; i < containers->u.array.count; i++) {
		const struct json_value *e = containers->u.array.items[i];
		const char *name = json_str_field(e, "name");
		const char *status = json_str_field(e, "status");
		const struct json_value *st;

		if (name == NULL || status == NULL || !container_should_be_running(e))
			continue;
		if (strcmp(status, "running") == 0)
			continue;
		st = json_object_get(e, "stopped");
		if (st == NULL || st->type != JSON_BOOL || !st->u.boolean)
			continue;
		if (!stopped_note)
			printf("stopped by an operator, and their policy will start them at the next "
			       "boot:\n");
		stopped_note++;
		printf("  %-24s %-10s restart=%s\n", name, status, json_str_field(e, "restart"));
	}
	if (!drifted && !stopped_note)
		printf("no drift: every container that should be running is running\n");
	else if (!drifted)
		printf("no drift: nothing has failed\n");
	return drifted ? 1 : 0;
}

static int cmd_container_recipe(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_container_apply_recipe(const struct cix_client *c, int json_mode, int argc,
                                       char **argv);

/*
 * ADR-0156/task #861: attach/detach a network on an already-running
 * container, live -- no recreate. See daemon/src/main.c's own doc
 * comment on handle_container_network_attach() for the full
 * live/ephemeral design reasoning.
 */
static int cmd_container_network_attach(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *name = NULL;
	const char *network = NULL;
	const char *ip = NULL;
	const char *ifname = NULL;
	struct json_writer w;
	char path[300];
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--network=", 10) == 0)
			network = argv[i] + 10;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else if (strncmp(argv[i], "--ifname=", 9) == 0)
			ifname = argv[i] + 9;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown container network attach option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || network == NULL) {
		fprintf(stderr,
		        "usage: cixctl container network attach NAME --network=NETWORK [--ip=A.B.C.D] "
		        "[--ifname=NAME]\n");
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
	if (ifname != NULL) {
		jw_key(&w, "ifname");
		jw_str(&w, ifname);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_attachContainerNetwork, name);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_network_detach(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl container network detach NAME NETWORK\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_detachContainerNetwork, argv[0], argv[1]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_network(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "attach") == 0)
		return cmd_container_network_attach(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "detach") == 0)
		return cmd_container_network_detach(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "usage: cixctl container network attach NAME --network=NETWORK [--ip=A.B.C.D] [--ifname=NAME]\n"
	                "       cixctl container network detach NAME NETWORK\n");
	return 2;
}

/*
 * Issue #92: `cixctl container volume attach|detach` -- edits the
 * container's persisted definition, applying on its next start. Same
 * command shape as `container network attach|detach` above, deliberately
 * different semantics: that one is live and ephemeral, this one is
 * durable and deferred (see handle_container_volume_attach()'s own
 * comment for why a live volume attach needs a primitive this daemon
 * does not have yet).
 */
static void fmt_container_volumes(const struct json_value *v)
{
	const struct json_value *vols = json_object_get(v, "volumes");
	const char *applies = json_as_string(json_object_get(v, "applies"));
	int i;

	if (vols == NULL || vols->type != JSON_ARRAY || vols->u.array.count == 0) {
		printf("no volumes\n");
	} else {
		for (i = 0; i < vols->u.array.count; i++) {
			const struct json_value *e = vols->u.array.items[i];
			const struct json_value *ro = json_object_get(e, "read_only");

			printf("%-20s %-28s %s\n", json_as_string(json_object_get(e, "name")),
			       json_as_string(json_object_get(e, "path")),
			       (ro != NULL && ro->type == JSON_BOOL && ro->u.boolean) ? "read-only"
			                                                              : "read-write");
		}
	}
	if (applies != NULL)
		printf("(applies %s -- the running container is unchanged)\n", applies);
}

static int cmd_container_volume_attach(const struct cix_client *c, int json_mode, int argc,
                                        char **argv)
{
	const char *name = NULL;
	const char *volume = NULL;
	const char *path_in = NULL;
	int read_only = 0;
	struct json_writer w;
	char path[300];
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--volume=", 9) == 0)
			volume = argv[i] + 9;
		else if (strncmp(argv[i], "--path=", 7) == 0)
			path_in = argv[i] + 7;
		else if (strcmp(argv[i], "--read-only") == 0)
			read_only = 1;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown container volume attach option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || volume == NULL || path_in == NULL) {
		fprintf(stderr, "usage: cixctl container volume attach NAME --volume=VOLUME "
		                "--path=/mount/point [--read-only]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, volume);
	jw_key(&w, "path");
	jw_str(&w, path_in);
	if (read_only) {
		jw_key(&w, "read_only");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_attachContainerVolume, name);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_container_volumes);
}

static int cmd_container_volume_detach(const struct cix_client *c, int json_mode, int argc,
                                        char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl container volume detach NAME VOLUME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_detachContainerVolume, argv[0], argv[1]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_volumes);
}

static int cmd_container_volume(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "attach") == 0)
		return cmd_container_volume_attach(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "detach") == 0)
		return cmd_container_volume_detach(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "usage: cixctl container volume attach NAME --volume=VOLUME "
	                "--path=/mount/point [--read-only]\n"
	                "       cixctl container volume detach NAME VOLUME\n"
	                "  Both edit the container's definition and take effect on its next start.\n");
	return 2;
}

/* ADR-0161 Phase D: `cixctl container device attach|detach` -- the
 * manual REST primitive POST/DELETE /v1/containers/{name}/devices,
 * same shape as container network attach/detach above (a real,
 * callable-by-hand primitive first; Phase C's own hotplug listener is
 * just another, internal caller of the identical daemon-side
 * mechanism). */
static int cmd_container_device_attach(const struct cix_client *c, int json_mode, int argc,
                                        char **argv)
{
	struct cix_response r;
	char path[300];
	struct json_writer w;

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl container device attach NAME ID\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "id");
	jw_str(&w, argv[1]);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_attachContainerDevice, argv[0]);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_container_device_detach(const struct cix_client *c, int json_mode, int argc,
                                        char **argv)
{
	struct cix_response r;
	char path[400];

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl container device detach NAME ID\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_detachContainerDevice, argv[0], argv[1]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

/*
 * ADR-0260: cixctl container service start|stop|restart NAME SERVICE --
 * the control plane reaching one declared service inside a container.
 */
static int cmd_container_service_op(const struct cix_client *c, int json_mode, int argc,
                                    char **argv, const char *op_path, const char *verb)
{
	struct cix_response r;
	char path[512];

	if (argc < 2 || argv[0][0] == '-' || argv[1][0] == '-') {
		fprintf(stderr, "cixctl: container service %s requires a container name and a service name\n",
		        verb);
		return 2;
	}
	snprintf(path, sizeof(path), op_path, argv[0], argv[1]);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_container_service(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "start") == 0)
		return cmd_container_service_op(c, json_mode, argc - 1, argv + 1, CIX_API_startContainerService,
		                                "start");
	if (argc >= 1 && strcmp(argv[0], "stop") == 0)
		return cmd_container_service_op(c, json_mode, argc - 1, argv + 1, CIX_API_stopContainerService,
		                                "stop");
	if (argc >= 1 && strcmp(argv[0], "restart") == 0)
		return cmd_container_service_op(c, json_mode, argc - 1, argv + 1,
		                                CIX_API_restartContainerService, "restart");
	fprintf(stderr, "usage: cixctl container service start NAME SERVICE    -- start a stopped, exited or failed service\n"
	                "       cixctl container service stop NAME SERVICE     -- stop it and hold it stopped until the next container start\n"
	                "       cixctl container service restart NAME SERVICE  -- stop then start it; the rest of the container is untouched\n");
	return 2;
}

static int cmd_container_device(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "attach") == 0)
		return cmd_container_device_attach(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "detach") == 0)
		return cmd_container_device_detach(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "usage: cixctl container device attach NAME ID  -- a real device id or "
	                "devicemap name, resolved live\n"
	                "       cixctl container device detach NAME ID  -- refuses a device "
	                "granted at container creation (409)\n");
	return 2;
}

/* Forward declarations: cmd_container() dispatches to these container verbs,
 * which are defined below it (issue #74 -- all container ops under `container`). */
static int cmd_run(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_inspect(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_start(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_stop(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_pause(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_unpause(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_rm(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_container_stats(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_console(const struct cix_client *c, int argc, char **argv);
static int cmd_files(const struct cix_client *c, int argc, char **argv);
static int cmd_migrate_storage(const struct cix_client *c, int json_mode, int argc, char **argv);
static int cmd_migrate_storage_status(const struct cix_client *c, int json_mode, int argc, char **argv);

static int cmd_container_edit(const struct cix_client *c, int json_mode, int argc,
                               char **argv); /* issue #11 -- defined below */

static int cmd_container(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	if (argc >= 1 && strcmp(argv[0], "volume") == 0)
		return cmd_container_volume(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "network") == 0)
		return cmd_container_network(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "device") == 0)
		return cmd_container_device(c, json_mode, argc - 1, argv + 1);
	/*
	 * All container operations live under this one `container` namespace,
	 * consistent with every other resource (network/image/dns/ldap/pki/pkg/
	 * device). There are no top-level container verbs -- see docs/guides/
	 * cli-reference.md and issue #74.
	 */
	if (argc >= 1 && strcmp(argv[0], "ls") == 0)
		return cmd_ps(c, json_mode);
	if (argc >= 1 && strcmp(argv[0], "drift") == 0)
		return cmd_container_drift(c, json_mode);
	if (argc >= 1 && strcmp(argv[0], "run") == 0)
		return cmd_run(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "inspect") == 0)
		return cmd_inspect(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "edit") == 0)
		return cmd_container_edit(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "start") == 0)
		return cmd_start(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "stop") == 0)
		return cmd_stop(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "service") == 0)
		return cmd_container_service(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "pause") == 0)
		return cmd_pause(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "unpause") == 0)
		return cmd_unpause(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "rm") == 0)
		return cmd_rm(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "stats") == 0)
		return cmd_container_stats(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "console") == 0)
		return cmd_console(c, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "files") == 0)
		return cmd_files(c, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "migrate-storage") == 0)
		return cmd_migrate_storage(c, json_mode, argc - 1, argv + 1);
	if (argc >= 1 && strcmp(argv[0], "migrate-storage-status") == 0)
		return cmd_migrate_storage_status(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr,
	        "usage: cixctl container ls | drift | run ... | start NAME | stop NAME |\n"
	        "         pause NAME |\n"
	        "         unpause NAME | rm NAME | inspect NAME | stats NAME |\n"
	        "         console NAME [--console=NAME] |\n"
	        "         files NAME --path=PATH | migrate-storage NAME --disk=ID | migrate-storage-status NAME\n"
	        "       cixctl deployment add --name=NAME --file=PATH\n"
	        "       cixctl deployment show|rm NAME / deployment ls\n"
	        "       cixctl deployment apply NAME [--secret=KEY=VALUE ...]\n"
	        "       cixctl container network attach NAME --network=NETWORK [--ip=A.B.C.D]\n"
	        "       cixctl container network detach NAME NETWORK\n"
	        "       cixctl container device attach NAME ID / device detach NAME ID\n");
	return 2;
}

static int cmd_inspect(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: inspect requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getContainer, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: rm requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteContainer, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_stop(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "cixctl: stop requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_stopContainer, argv[0]);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
static int cmd_migrate_storage(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	char path[300];
	struct json_writer w;
	struct cix_response r;
	int i;

	if (argc < 1) {
		fprintf(stderr, "cixctl: migrate-storage requires a container name\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown migrate-storage option '%s'\n", argv[i]);
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

	snprintf(path, sizeof(path), CIX_API_migrateContainerStorage, argv[0]);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_migrate_storage_status(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;

	if (argc < 1) {
		fprintf(stderr, "cixctl: migrate-storage-status requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getContainerStorageMigrateStatus, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_storage_migrate_status);
}

static int cmd_start(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "cixctl: start requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_startContainer, argv[0]);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_pause(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "cixctl: pause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_pauseContainer, argv[0]);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_health);
}

static int cmd_unpause(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "cixctl: unpause requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_unpauseContainer, argv[0]);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
 * cixctl host-stats -- the host-wide counterpart to `stats NAME`,
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

	{
		const struct json_value *up = json_object_get(v, "uptime");

		printf("uptime.host=%lld uptime.daemon=%lld\n",
		       (long long)json_as_number(json_object_get(up, "host_seconds")),
		       (long long)json_as_number(json_object_get(up, "daemon_seconds")));
	}

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

/*
 * GET /v1/system/kmsg (issue #77 surface parity) -- the kernel ring
 * buffer, the only kernel-log window a shell-less installed host has.
 * priority is the standard syslog level (0 emerg .. 7 debug); ts_usec is
 * the kernel's own monotonic timestamp since boot, printed as seconds
 * the way dmesg itself does rather than reformatted as a wall clock it
 * genuinely isn't.
 */
static const char *kmsg_priority_name(long long prio)
{
	switch (prio) {
	case 0: return "emerg";
	case 1: return "alert";
	case 2: return "crit";
	case 3: return "err";
	case 4: return "warn";
	case 5: return "notice";
	case 6: return "info";
	case 7: return "debug";
	default: return "?";
	}
}

static void fmt_kmsg(const struct json_value *v)
{
	const struct json_value *entries = json_object_get(v, "entries");
	size_t i;

	if (entries == NULL || entries->type != JSON_ARRAY || entries->u.array.count == 0) {
		printf("no kernel log entries\n");
		return;
	}
	for (i = 0; i < entries->u.array.count; i++) {
		const struct json_value *e = entries->u.array.items[i];
		long long prio = (long long)json_as_number(json_object_get(e, "priority"));
		double ts = json_as_number(json_object_get(e, "ts_usec")) / 1000000.0;
		const char *msg = json_as_string(json_object_get(e, "message"));

		printf("[%12.6f] %-6s %s\n", ts, kmsg_priority_name(prio), msg != NULL ? msg : "");
	}
}

static int cmd_kmsg(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[64];
	long tail = -1;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--tail=", 7) == 0) {
			tail = atol(argv[i] + 7);
		} else {
			fprintf(stderr, "usage: cixctl kmsg [--tail=N]\n");
			return 2;
		}
	}
	if (tail > 0)
		snprintf(path, sizeof(path), CIX_API_getSystemKmsg "?tail=%ld", tail);
	else
		snprintf(path, sizeof(path), CIX_API_getSystemKmsg);

	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_kmsg);
}

/*
 * Issue #81: registered-server health. One command for all four kinds
 * (ldap/dns/ntp/syslog) because it is one mechanism -- see
 * daemon/include/serverhealth.h.
 */
static void fmt_server_health(const struct json_value *v)
{
	const struct json_value *servers = json_object_get(v, "servers");
	const struct json_value *warnings = json_object_get(v, "warnings");
	size_t i;

	/* Issue #83: printed FIRST and unmissably -- a warning here means
	 * Cix is managing state it has nowhere to deliver, which is a
	 * silent, total failure of that subsystem (records that resolve
	 * nothing), not a degradation. */
	if (warnings != NULL && warnings->type == JSON_ARRAY) {
		for (i = 0; i < warnings->u.array.count; i++) {
			const char *msg = json_as_string(warnings->u.array.items[i]);

			if (msg != NULL)
				printf("WARNING: %s\n", msg);
		}
		if (warnings->u.array.count > 0)
			printf("\n");
	}

	if (servers == NULL || servers->type != JSON_ARRAY || servers->u.array.count == 0) {
		printf("no registered servers are being health-tracked yet\n");
		return;
	}
	for (i = 0; i < servers->u.array.count; i++) {
		const struct json_value *e = servers->u.array.items[i];
		const char *kind = json_as_string(json_object_get(e, "kind"));
		const char *container = json_as_string(json_object_get(e, "container"));
		const char *state = json_as_string(json_object_get(e, "state"));
		const char *probe = json_as_string(json_object_get(e, "probe"));
		const char *err = json_as_string(json_object_get(e, "last_error"));
		const struct json_value *drained = json_object_get(e, "drained");
		const struct json_value *in_service = json_object_get(e, "in_service");
		const struct json_value *fails = json_object_get(e, "consecutive_failures");

		printf("%-8s %-20s %-10s in_service=%-4s drained=%-4s probe=%-10s fails=%-3ld%s%s\n",
		       kind != NULL ? kind : "?", container != NULL ? container : "?",
		       state != NULL ? state : "?",
		       (in_service != NULL && in_service->type == JSON_BOOL && in_service->u.boolean)
		           ? "yes" : "no",
		       (drained != NULL && drained->type == JSON_BOOL && drained->u.boolean) ? "yes" : "no",
		       probe != NULL ? probe : "-",
		       fails != NULL ? (long)json_as_number(fails) : 0,
		       err != NULL ? "  " : "", err != NULL ? err : "");
	}
}

static int cmd_server_health(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;

	if (argc == 0 || strcmp(argv[0], "ls") == 0) {
		if (cix_client_request(c, CIX_API_getServerHealth_METHOD, CIX_API_getServerHealth, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_server_health);
	}
	if ((strcmp(argv[0], "drain") == 0 || strcmp(argv[0], "undrain") == 0) && argc == 3) {
		char path[256];
		struct json_writer w;
		int rc;

		snprintf(path, sizeof(path), CIX_API_setServerHealthDrain, argv[1], argv[2]);
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "drained");
		jw_bool(&w, strcmp(argv[0], "drain") == 0);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		rc = cix_client_request(c, "PUT", path, w.buf, &r);
		jw_free(&w);
		if (rc != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_server_health);
	}
	fprintf(stderr, "usage: cixctl server-health [ls]\n"
	                "       cixctl server-health drain KIND CONTAINER\n"
	                "       cixctl server-health undrain KIND CONTAINER\n"
	                "       (KIND is one of: ldap dns ntp syslog)\n");
	return 2;
}

/*
 * Issue #88: persistent volumes -- storage whose lifetime is independent
 * of any container using it.
 */
static void fmt_volumes(const struct json_value *v)
{
	const struct json_value *vols = json_object_get(v, "volumes");
	size_t i;

	if (vols == NULL || vols->type != JSON_ARRAY || vols->u.array.count == 0) {
		printf("no volumes\n");
		return;
	}
	for (i = 0; i < vols->u.array.count; i++) {
		const struct json_value *e = vols->u.array.items[i];
		const char *name = json_as_string(json_object_get(e, "name"));
		const char *disk = json_as_string(json_object_get(e, "disk"));
		const char *path = json_as_string(json_object_get(e, "host_path"));

		printf("%-20s disk=%-12s path=%s\n", name != NULL ? name : "?",
		       disk != NULL ? disk : "(default)", path != NULL ? path : "?");
	}
}

static void fmt_volume_one(const struct json_value *v)
{
	const char *name = json_as_string(json_object_get(v, "name"));
	const char *disk = json_as_string(json_object_get(v, "disk"));
	const char *path = json_as_string(json_object_get(v, "host_path"));
	double quota = json_as_number(json_object_get(v, "quota_bytes"));

	printf("%-20s disk=%-12s path=%s\n", name != NULL ? name : "?",
	       disk != NULL ? disk : "(default)", path != NULL ? path : "?");
	/* Said explicitly rather than omitted when absent: "no limit" is a
	 * real state worth seeing on a volume, not a blank. */
	if (quota > 0)
		printf("%-20s limit=%.0f bytes (%.1f GiB)\n", "", quota, quota / (1024 * 1024 * 1024));
	else
		printf("%-20s limit=none -- can grow until the disk is full\n", "");
	/* Issue #102: said either way, because "root owns it" is the state
	 * that stops a workload writing to its own volume and it should
	 * never have to be inferred from silence. */
	{
		const struct json_value *ou = json_object_get(v, "owner_uid");
		const struct json_value *og = json_object_get(v, "owner_gid");

		if (ou != NULL && ou->type == JSON_NUMBER)
			printf("%-20s owner=%d:%d\n", "", (int)json_as_number(ou), (int)json_as_number(og));
		else
			printf("%-20s owner=root (0:0) -- a non-root workload cannot write to it\n", "");
	}
}

/*
 * Issue #96: a volume's own content snapshots. Distinct from `backup`
 * (the platform configuration bundle, ADR-0033) -- that one is config
 * and never workload data; this is the workload data itself, opt-in per
 * volume.
 */
static void fmt_volume_backups(const struct json_value *v)
{
	const struct json_value *snaps = json_object_get(v, "snapshots");
	const struct json_value *en = json_object_get(v, "enabled");
	const struct json_value *status = json_object_get(v, "status");
	int i;

	printf("backups:  %s\n", (en != NULL && en->type == JSON_BOOL && en->u.boolean)
	                             ? "enabled"
	                             : "disabled (opt in with: volume backups NAME --enable)");
	printf("retain:   %.0f\n", json_as_number(json_object_get(v, "retain")));
	{
		const char *wr = json_as_string(json_object_get(v, "while_running"));

		if (wr == NULL)
			wr = "refuse";
		printf("while running: %s%s\n", wr,
		       strcmp(wr, "refuse") == 0
		           ? "  -- an always-on container means this is never backed up"
		           : strcmp(wr, "pause") == 0 ? "  (containers frozen for the copy)"
		                                      : "  (crash-consistent)");
	}
	if (status != NULL) {
		const char *err = json_as_string(json_object_get(status, "last_error"));

		if (err != NULL && err[0] != '\0')
			printf("last try: FAILED -- %s\n", err);
	}
	if (snaps == NULL || snaps->type != JSON_ARRAY || snaps->u.array.count == 0) {
		printf("no snapshots\n");
		return;
	}
	printf("snapshots (newest first):\n");
	for (i = 0; i < snaps->u.array.count; i++)
		printf("  %s\n", json_as_string(json_object_get(snaps->u.array.items[i], "stamp")));
}

/*
 * Issue #97: what is declared against what is actually installed. The
 * state worth seeing is "installed, no recipe" -- it cannot be rebuilt
 * from source control, and nothing else surfaces it.
 */
static void fmt_software(const struct json_value *v)
{
	const struct json_value *list = json_object_get(v, "software");
	int i, drift = 0;

	if (list == NULL || list->type != JSON_ARRAY || list->u.array.count == 0) {
		printf("no software\n");
		return;
	}
	printf("%-28s %-10s %s\n", "NAME", "KIND", "STATE");
	for (i = 0; i < list->u.array.count; i++) {
		const struct json_value *e = list->u.array.items[i];
		const struct json_value *de = json_object_get(e, "declared");
		const struct json_value *in = json_object_get(e, "installed");
		int d = de != NULL && de->type == JSON_BOOL && de->u.boolean;
		int inst = in != NULL && in->type == JSON_BOOL && in->u.boolean;
		const char *state;

		if (d && inst)
			state = "ok";
		else if (inst) {
			state = "NO RECIPE -- cannot be rebuilt from source";
			drift++;
		} else
			state = "declared, not installed";
		printf("%-28s %-10s %s\n", json_as_string(json_object_get(e, "name")),
		       json_as_string(json_object_get(e, "kind")), state);
	}
	if (drift > 0)
		printf("\n%d item(s) installed with no recipe -- either capture one, or they are debris.\n",
		       drift);
}

/*
 * Issue #63: the most destructive thing this platform can do, so the
 * CLI asks for the instance name typed back as well -- the daemon
 * requires it regardless, and a client that sends it without the
 * operator having seen what it destroys would defeat the point of the
 * guard rather than honour it.
 */
static int cmd_factory_reset(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	struct json_writer w;
	const char *confirm = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--confirm=", 10) == 0)
			confirm = argv[i] + 10;
	}
	if (confirm == NULL) {
		fprintf(stderr,
		        "usage: cixctl factory-reset --confirm=<instance name>\n"
		        "\n"
		        "  Returns this box to its just-installed state and REBOOTS. Destroys every\n"
		        "  container, image, network, DNS/PKI/LDAP/NTP registration, package state,\n"
		        "  the log store, and every VOLUME AND ALL DATA IN THEM. Cannot be undone.\n"
		        "\n"
		        "  Keeps the installed OS itself, and does not reformat any disk -- disk role\n"
		        "  assignments are forgotten, the filesystems on them are left alone.\n"
		        "\n"
		        "  The instance name is this install's own (cixctl site).\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "confirm");
	jw_str(&w, confirm);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, CIX_API_factoryReset_METHOD, CIX_API_factoryReset, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, NULL);
}

static int cmd_software(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listSoftware_METHOD, CIX_API_listSoftware, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_software);
}

static int cmd_volume_quota(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;
	struct json_writer w;
	long long bytes;

	if (argc < 3) {
		fprintf(stderr, "usage: cixctl volume quota NAME BYTES\n"
		                "  0 removes the limit. A volume with no limit is an unbounded way to\n"
		                "  fill the disk it sits on.\n");
		return 2;
	}
	bytes = atoll(argv[2]);
	snprintf(path, sizeof(path), CIX_API_setVolumeQuota, argv[1]);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "quota_bytes");
	jw_int(&w, bytes);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_volume_one);
}

/*
 * #365: how much a volume actually holds.
 *
 * Its own command rather than a column in `volume ls`, because the
 * daemon measures it with a full tree walk -- listing volumes must not
 * cost a walk of every one of them.
 */
static int cmd_volume_usage(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: cixctl volume usage NAME\n"
		        "  Measured on demand, never a stored figure. 'source' says how: 'qgroup'\n"
		        "  reads btrfs's own counter (exclusive allocated extents, settles at the\n"
		        "  next commit, and is what a limit is enforced against), 'walk' walks the\n"
		        "  tree (apparent file content, instant). df inside a container cannot\n"
		        "  answer this at all: a volume is a bind of a directory, so df reports the\n"
		        "  whole backing filesystem.\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getVolumeUsage, argv[1]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, NULL);
}

static int cmd_volume_backups(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;
	struct json_writer w;
	int enable = -1;
	long retain = 0;
	const char *while_running = NULL;
	int i;

	if (argc < 2) {
		fprintf(stderr,
		        "usage: cixctl volume backups NAME [--enable|--disable] [--retain=N]\n"
		        "                                    [--while-running=refuse|pause|allow]\n"
		        "  What to do when a container using the volume is running:\n"
		        "    refuse  (default) skip it -- and an always-on container means never\n"
		        "    pause   freeze every container using it for the copy, then thaw:\n"
		        "            a genuinely consistent snapshot, at the cost of real downtime\n"
		        "    allow   copy it live and accept a crash-consistent snapshot\n");
		return 2;
	}
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--enable") == 0)
			enable = 1;
		else if (strcmp(argv[i], "--disable") == 0)
			enable = 0;
		else if (strncmp(argv[i], "--retain=", 9) == 0)
			retain = atol(argv[i] + 9);
		else if (strncmp(argv[i], "--while-running=", 16) == 0)
			while_running = argv[i] + 16;
	}
	snprintf(path, sizeof(path), CIX_API_getVolumeBackups, argv[1]);

	if (enable < 0 && retain == 0 && while_running == NULL) {
		if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_volume_backups);
	}
	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "enabled");
	jw_bool(&w, enable > 0);
	if (retain > 0) {
		jw_key(&w, "retain");
		jw_int(&w, retain);
	}
	if (while_running != NULL) {
		jw_key(&w, "while_running");
		jw_str(&w, while_running);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_volume_backups);
}

static int cmd_volume_backup_now(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;

	if (argc < 2) {
		fprintf(stderr, "usage: cixctl volume backup NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_takeVolumeBackup, argv[1]);
	if (cix_client_request(c, "POST", path, "{}", &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_volume_backups);
}

static int cmd_volume_restore(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[300];
	struct cix_response r;
	struct json_writer w;

	if (argc < 3) {
		fprintf(stderr, "usage: cixctl volume restore NAME SNAPSHOT\n"
		                "  Replaces everything currently in the volume with that snapshot.\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_restoreVolume, argv[1]);

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "snapshot");
	jw_str(&w, argv[2]);
	jw_key(&w, "confirm_volume_name");
	jw_str(&w, argv[1]);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_volume_backups);
}

static int cmd_volume(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "ls";

	if (strcmp(sub, "ls") == 0) {
		if (cix_client_request(c, CIX_API_listVolumes_METHOD, CIX_API_listVolumes, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_volumes);
	}
	if (strcmp(sub, "create") == 0) {
		struct json_writer w;
		const char *name = NULL, *disk = NULL;
		/* Issue #102: who the volume belongs to, since a volume created
		 * as root is one its own workload cannot write to. */
		const char *owner_uid = NULL, *owner_gid = NULL;
		int i, rc;

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--name=", 7) == 0)
				name = argv[i] + 7;
			else if (strncmp(argv[i], "--disk=", 7) == 0)
				disk = argv[i] + 7;
			else if (strncmp(argv[i], "--owner-uid=", 12) == 0)
				owner_uid = argv[i] + 12;
			else if (strncmp(argv[i], "--owner-gid=", 12) == 0)
				owner_gid = argv[i] + 12;
		}
		if (name == NULL || (owner_uid != NULL) != (owner_gid != NULL)) {
			fprintf(stderr, "usage: cixctl volume create --name=NAME [--disk=DISK] "
			                "[--owner-uid=N --owner-gid=N]\n");
			return 2;
		}
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, name);
		if (disk != NULL) {
			jw_key(&w, "disk");
			jw_str(&w, disk);
		}
		if (owner_uid != NULL) {
			jw_key(&w, "owner_uid");
			jw_int(&w, atol(owner_uid));
			jw_key(&w, "owner_gid");
			jw_int(&w, atol(owner_gid));
		}
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		rc = cix_client_request(c, CIX_API_createVolume_METHOD, CIX_API_createVolume, w.buf, &r);
		jw_free(&w);
		if (rc != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_volume_one);
	}
	/*
	 * Issue #102: hand a volume to the account that will actually use
	 * it. --recursive is explicit because a volume in use holds files
	 * whose ownership someone may have chosen deliberately.
	 */
	if (strcmp(sub, "owner") == 0 && argc >= 2) {
		struct json_writer w;
		const char *uid = NULL, *gid = NULL;
		int recursive = 0, clear = 0, i, rc;
		char path[160];

		for (i = 2; i < argc; i++) {
			if (strncmp(argv[i], "--uid=", 6) == 0)
				uid = argv[i] + 6;
			else if (strncmp(argv[i], "--gid=", 6) == 0)
				gid = argv[i] + 6;
			else if (strcmp(argv[i], "--recursive") == 0)
				recursive = 1;
			else if (strcmp(argv[i], "--root") == 0)
				clear = 1;
			else {
				fprintf(stderr, "cixctl: unknown volume owner option '%s'\n", argv[i]);
				return 2;
			}
		}
		if (!clear && (uid == NULL || gid == NULL)) {
			fprintf(stderr, "usage: cixctl volume owner NAME --uid=N --gid=N [--recursive]\n"
			                "       cixctl volume owner NAME --root   (hand it back to root)\n");
			return 2;
		}

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "uid");
		if (clear)
			jw_null(&w);
		else
			jw_int(&w, atol(uid));
		jw_key(&w, "gid");
		if (clear)
			jw_null(&w);
		else
			jw_int(&w, atol(gid));
		jw_key(&w, "recursive");
		jw_bool(&w, recursive);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		snprintf(path, sizeof(path), CIX_API_setVolumeOwner, argv[1]);
		rc = cix_client_request(c, "PUT", path, w.buf, &r);
		jw_free(&w);
		if (rc != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_volume_one);
	}
	if (strcmp(sub, "show") == 0 && argc == 2) {
		char path[128];

		snprintf(path, sizeof(path), CIX_API_getVolume, argv[1]);
		if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_volume_one);
	}
	if (strcmp(sub, "rm") == 0 && argc == 2) {
		char path[128];

		snprintf(path, sizeof(path), CIX_API_deleteVolume, argv[1]);
		if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, NULL);
	}
	if (strcmp(argv[0], "quota") == 0)
		return cmd_volume_quota(c, json_mode, argc, argv);
	if (strcmp(argv[0], "usage") == 0)
		return cmd_volume_usage(c, json_mode, argc, argv);
	if (strcmp(argv[0], "backups") == 0)
		return cmd_volume_backups(c, json_mode, argc, argv);
	if (strcmp(argv[0], "backup") == 0)
		return cmd_volume_backup_now(c, json_mode, argc, argv);
	if (strcmp(argv[0], "restore") == 0)
		return cmd_volume_restore(c, json_mode, argc, argv);
	if (strcmp(argv[0], "migrate") == 0) {
		const char *disk = "";
		struct json_writer w;
		struct cix_response r;
		char path[256];
		int i;

		if (argc < 2) {
			fprintf(stderr, "usage: cixctl volume migrate NAME [--disk=DISK]\n"
			                "  Omit --disk to move it back to the default OS-disk placement.\n");
			return 2;
		}
		for (i = 2; i < argc; i++) {
			if (strncmp(argv[i], "--disk=", 7) == 0)
				disk = argv[i] + 7;
		}
		snprintf(path, sizeof(path), CIX_API_migrateVolume, argv[1]);

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "disk");
		jw_str(&w, disk);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';

		if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		return emit(&r, json_mode, fmt_volume_one);
	}
	fprintf(stderr, "usage: cixctl volume ls\n"
	                "       cixctl volume create --name=NAME [--disk=DISK]\n"
	                "       cixctl volume show NAME\n"
	                "       cixctl volume rm NAME\n"
	                "       cixctl volume migrate NAME [--disk=DISK]\n"
	                "       cixctl volume quota NAME BYTES   (0 removes the limit)\n"
	                "       cixctl volume usage NAME         (measured now, not stored)\n"
	                "       cixctl volume backups NAME [--enable|--disable] [--retain=N]\n"
	                "       cixctl volume backup NAME\n"
	                "       cixctl volume restore NAME SNAPSHOT\n"
	                "  A volume outlives the containers using it: deleting a container never\n"
	                "  removes its volumes, and `volume rm` refuses while any container\n"
	                "  definition still references one.\n");
	return 2;
}

static int cmd_host_stats(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemStats_METHOD, CIX_API_getSystemStats, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_process_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listSystemProcesses_METHOD, CIX_API_listSystemProcesses, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_process_list);
}

static int cmd_process_kill(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[64];

	if (argc < 1) {
		fprintf(stderr, "cixctl: process kill requires a pid\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_killSystemProcess, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_process(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl process ls  -- every real process on the box, correlated to a\n"
		        "               container by its own real host ppid chain (ADR-0131)\n"
		        "       cixctl process kill PID  -- a real, immediate SIGKILL; refuses pid 1\n"
		        "               and this daemon's own pid\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_process_ls(c, json_mode);
	if (strcmp(sub, "kill") == 0)
		return cmd_process_kill(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown process subcommand '%s'\n", sub);
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
static int poll_ping(const struct cix_client *c, struct cix_response *out)
{
	for (;;) {
		const char *state;

		if (cix_client_request(c, CIX_API_getSystemPing_METHOD, CIX_API_getSystemPing, NULL, out) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "pending") != 0)
			return 0;
		cix_response_free(out);
		usleep(100000);
	}
}

static int cmd_ping(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct cix_response r;
	int rc;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl ping HOST\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "host");
	jw_str(&w, argv[0]);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_postSystemPing_METHOD, CIX_API_postSystemPing, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status == 409) {
		cix_response_free(&r);
		fprintf(stderr, "cixctl: another ping is already in flight\n");
		return 1;
	}
	if (r.status != 202) {
		return emit(&r, json_mode, fmt_ping);
	}
	cix_response_free(&r);

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
 * cixctl container stats <name> -- one-shot fetch-and-print, plain
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

static int cmd_container_stats(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[300];

	if (argc < 1) {
		fprintf(stderr, "cixctl: stats requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getContainerStats, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_stats);
}

static int cmd_console(const struct cix_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *console_name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--console=", 10) == 0)
			console_name = argv[i] + 10;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown console option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl console NAME [--console=NAME]\n");
		return 2;
	}

	return cix_console_run(c, name, console_name) == 0 ? 0 : 1;
}

/* Matches daemon's CONTAINER_MAX_NETWORKS -- see include/container.h. */
#define CLI_MAX_NETWORKS 64
#define CLI_MAX_ROUTES 8
/* Matches daemon's CONTAINER_MAX_DEVICES -- see include/container.h. */
#define CLI_MAX_DEVICES 16
/* Issue #88: matches CONTAINER_MAX_VOLUMES in include/container.h. */
#define CLI_MAX_VOLUMES 8
/* Issue #248: matches REGISTRY_MAX_CONSOLES / REGISTRY_CONSOLE_ARGC_MAX. */
/*
 * ADR-0260: one declared service, from --service=/--oneshot= plus the
 * per-service attribute flags that name it. Mirrors --console=NAME=/path
 * args, which is the CLI's existing answer to "how is a named argv
 * given"; the attributes follow the same NAME:value shape.
 */
#define CLI_MAX_SERVICES 16
#define CLI_SERVICE_ARGC_MAX 32

struct cli_service_decl {
	char name[32];
	char argv[CLI_SERVICE_ARGC_MAX][128];
	int argc;
	int oneshot;
	char after[256];   /* comma-separated names, or "" */
	char ready[256];   /* "tcp:PORT" | "socket:PATH" | "command:/path args" | "" */
	char on_exit[24];  /* "" = the API's default for the type */
};

static int parse_service_flag(const char *arg, struct cli_service_decl *out)
{
	const char *eq = strchr(arg, '=');
	const char *p;
	size_t nlen;

	if (eq == NULL || eq == arg)
		return -1;
	nlen = (size_t)(eq - arg);
	if (nlen >= sizeof(out->name))
		return -1;
	memset(out, 0, sizeof(*out));
	memcpy(out->name, arg, nlen);
	out->name[nlen] = '\0';
	p = eq + 1;
	while (*p != '\0') {
		size_t len = 0;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		if (out->argc >= CLI_SERVICE_ARGC_MAX)
			return -1;
		while (p[len] != '\0' && p[len] != ' ')
			len++;
		if (len >= sizeof(out->argv[0]))
			return -1;
		memcpy(out->argv[out->argc], p, len);
		out->argv[out->argc][len] = '\0';
		out->argc++;
		p += len;
	}
	if (out->argc == 0 || out->argv[0][0] != '/')
		return -1;
	return 0;
}

/* NAME:value -- the service the attribute flag names, and the value. */
static struct cli_service_decl *service_attr_target(const char *arg, struct cli_service_decl *svcs,
                                                    int count, const char **value_out)
{
	const char *colon = strchr(arg, ':');
	size_t nlen;
	int i;

	if (colon == NULL || colon == arg)
		return NULL;
	nlen = (size_t)(colon - arg);
	for (i = 0; i < count; i++) {
		if (strlen(svcs[i].name) == nlen && strncmp(svcs[i].name, arg, nlen) == 0) {
			*value_out = colon + 1;
			return &svcs[i];
		}
	}
	return NULL;
}

/* Writes one comma-separated list as a JSON array of strings. */
static void jw_csv_array(struct json_writer *w, const char *csv)
{
	const char *p = csv;

	jw_arr_open(w);
	while (*p != '\0') {
		const char *end = strchr(p, ',');
		size_t len = end != NULL ? (size_t)(end - p) : strlen(p);
		char item[64];

		if (len > 0 && len < sizeof(item)) {
			memcpy(item, p, len);
			item[len] = '\0';
			jw_str(w, item);
		}
		p += len;
		if (*p == ',')
			p++;
	}
	jw_arr_close(w);
}

/* Writes a space-separated argv string as a JSON array of strings. */
static void jw_words_array(struct json_writer *w, const char *words)
{
	const char *p = words;

	jw_arr_open(w);
	while (*p != '\0') {
		size_t len = 0;
		char item[128];

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		while (p[len] != '\0' && p[len] != ' ')
			len++;
		if (len < sizeof(item)) {
			memcpy(item, p, len);
			item[len] = '\0';
			jw_str(w, item);
		}
		p += len;
	}
	jw_arr_close(w);
}

static void jw_service_decl(struct json_writer *w, const struct cli_service_decl *s)
{
	int a;

	jw_obj_open(w);
	jw_key(w, "name");
	jw_str(w, s->name);
	if (s->oneshot) {
		jw_key(w, "type");
		jw_str(w, "oneshot");
	}
	jw_key(w, "cmd");
	jw_arr_open(w);
	for (a = 0; a < s->argc; a++)
		jw_str(w, s->argv[a]);
	jw_arr_close(w);
	if (s->after[0] != '\0') {
		jw_key(w, "after");
		jw_csv_array(w, s->after);
	}
	if (s->ready[0] != '\0') {
		jw_key(w, "ready");
		jw_obj_open(w);
		if (strncmp(s->ready, "tcp:", 4) == 0) {
			jw_key(w, "tcp_port");
			jw_int(w, atol(s->ready + 4));
		} else if (strncmp(s->ready, "socket:", 7) == 0) {
			jw_key(w, "socket");
			jw_str(w, s->ready + 7);
		} else if (strncmp(s->ready, "command:", 8) == 0) {
			jw_key(w, "command");
			jw_words_array(w, s->ready + 8);
		}
		jw_obj_close(w);
	}
	if (s->on_exit[0] != '\0') {
		jw_key(w, "on_exit");
		jw_str(w, s->on_exit);
	}
	jw_obj_close(w);
}

static int ready_spec_ok(const char *v)
{
	if (strncmp(v, "tcp:", 4) == 0)
		return atol(v + 4) >= 1 && atol(v + 4) <= 65535;
	if (strncmp(v, "socket:", 7) == 0)
		return v[7] == '/';
	if (strncmp(v, "command:", 8) == 0)
		return v[8] == '/';
	return 0;
}

#define CLI_MAX_CONSOLES 4
#define CLI_CONSOLE_ARGC_MAX 8

/*
 * One --console=NAME=/path/to/cmd arg arg flag, parsed.
 *
 * Name up to the first '=', then the command split on spaces. Spaces
 * because that is how an operator types a command and how it reads back
 * in help; an argument containing a literal space cannot be expressed
 * this way, which is a real limit and the reason a deployment --
 * a real JSON array, no quoting to lose -- is the better place to
 * declare anything non-trivial.
 */
struct cli_console_decl {
	char name[32];
	char argv[CLI_CONSOLE_ARGC_MAX][128];
	int argc;
};

static int parse_console_flag(const char *arg, struct cli_console_decl *out)
{
	const char *eq = strchr(arg, '=');
	const char *p;
	size_t nlen;

	if (eq == NULL || eq == arg)
		return -1;
	nlen = (size_t)(eq - arg);
	if (nlen >= sizeof(out->name))
		return -1;
	memcpy(out->name, arg, nlen);
	out->name[nlen] = '\0';

	out->argc = 0;
	p = eq + 1;
	while (*p != '\0') {
		size_t len = 0;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		if (out->argc >= CLI_CONSOLE_ARGC_MAX)
			return -1;
		while (p[len] != '\0' && p[len] != ' ')
			len++;
		if (len >= sizeof(out->argv[0]))
			return -1;
		memcpy(out->argv[out->argc], p, len);
		out->argv[out->argc][len] = '\0';
		out->argc++;
		p += len;
	}
	if (out->argc == 0 || out->argv[0][0] != '/')
		return -1;
	return 0;
}

/*
 * One --volume=NAME:/path[:ro] flag, parsed. Two colon-separated
 * fields plus an optional trailing ":ro" -- deliberately the shape
 * people already know from other runtimes, so nobody has to learn a
 * Cix-specific spelling for the most ordinary thing you can ask of a
 * volume.
 */
struct cli_volume_mount {
	char name[64];
	char path[256];
	int read_only;
};

static int parse_volume_flag(const char *arg, struct cli_volume_mount *out)
{
	const char *colon = strchr(arg, ':');
	const char *path;
	const char *ro;
	size_t name_len;
	size_t path_len;

	if (colon == NULL)
		return -1;
	name_len = (size_t)(colon - arg);
	if (name_len == 0 || name_len >= sizeof(out->name))
		return -1;
	memcpy(out->name, arg, name_len);
	out->name[name_len] = '\0';

	path = colon + 1;
	if (path[0] != '/')
		return -1;
	out->read_only = 0;
	ro = strrchr(path, ':');
	if (ro != NULL) {
		if (strcmp(ro, ":ro") == 0)
			out->read_only = 1;
		else if (strcmp(ro, ":rw") != 0)
			return -1;
		path_len = (size_t)(ro - path);
	} else {
		path_len = strlen(path);
	}
	if (path_len == 0 || path_len >= sizeof(out->path))
		return -1;
	memcpy(out->path, path, path_len);
	out->path[path_len] = '\0';

	return 0;
}
/* Matches daemon's CONTAINER_MAX_INTERFACES -- see include/container.h. */
#define CLI_MAX_INTERFACES 16
#define CLI_MAX_CAP_ADD 8
#define CLI_MAX_LDAP_ALLOW_GROUPS 8
/* Matches daemon's CONTAINERDEF_MAX_DEPENDS -- see daemon/include/containerdef.h. */
#define CLI_MAX_DEPENDS 16
/* Matches daemon's CONTAINER_MAX_FILES -- see include/container.h. */
#define CLI_MAX_FILES 16
/* Matches daemon's CONTAINER_MAX_SYSCTLS -- see include/container.h. */
#define CLI_MAX_SYSCTLS 32
/* Matches daemon's CONTAINER_MAX_ENV -- see include/container.h. */
#define CLI_MAX_ENV 32
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
 * cixctl signing-keys [show] / set --key=PATH --cert=PATH / clear
 * (ADR-0212) -- the Secure Boot signing key pair `cixctl iso build`
 * needs on the host that cuts installer media.
 *
 * Takes file paths rather than the PEM text itself: the key is
 * already a file wherever it is kept, and a private key on a command
 * line would land in shell history and in this host's own process
 * list. The web dashboard offers the paste box instead, where the
 * material never becomes an argv entry.
 *
 * There is no "get" of the private key, here or anywhere -- see
 * signingkeys.h.
 */
static void fmt_signing_keys(const struct json_value *v)
{
	const struct json_value *key_set = json_object_get(v, "key_set");
	const struct json_value *cert_set = json_object_get(v, "cert_set");
	const char *subject = json_as_string(json_object_get(v, "subject"));
	const char *not_after = json_as_string(json_object_get(v, "not_after"));
	const char *fp = json_as_string(json_object_get(v, "fingerprint_sha256"));
	int have_key = key_set != NULL && key_set->type == JSON_BOOL && key_set->u.boolean;
	int have_cert = cert_set != NULL && cert_set->type == JSON_BOOL && cert_set->u.boolean;

	if (!have_key && !have_cert) {
		printf("no signing key pair installed -- `cixctl iso build` cannot run on this host\n");
		return;
	}
	printf("private key   %s\n", have_key ? "installed" : "MISSING");
	printf("certificate   %s\n", have_cert ? "installed" : "MISSING");
	if (subject != NULL)
		printf("subject       %s\n", subject);
	if (not_after != NULL)
		printf("expires       %s\n", not_after);
	if (fp != NULL)
		printf("fingerprint   %s\n", fp);
	if (have_key != have_cert)
		printf("\nOnly half the pair is present -- POST /v1/system/iso needs both.\n");
}

static int cmd_signing_keys_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemSigningKeys_METHOD, CIX_API_getSystemSigningKeys, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_signing_keys);
}

static int cmd_signing_keys_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *key_path = NULL;
	const char *cert_path = NULL;
	char *key_buf = NULL;
	char *cert_buf = NULL;
	size_t key_len = 0;
	size_t cert_len = 0;
	struct json_writer w;
	struct cix_response r;
	int i;
	int rc;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--key=", 6) == 0)
			key_path = argv[i] + 6;
		else if (strncmp(argv[i], "--cert=", 7) == 0)
			cert_path = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown signing-keys set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (key_path == NULL || cert_path == NULL) {
		fprintf(stderr, "cixctl: signing-keys set needs both --key=PATH and --cert=PATH\n");
		return 2;
	}
	if (read_local_file(key_path, &key_buf, &key_len) != 0)
		return 1;
	if (read_local_file(cert_path, &cert_buf, &cert_len) != 0) {
		free(key_buf);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "key");
	jw_str(&w, key_buf);
	jw_key(&w, "cert");
	jw_str(&w, cert_buf);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(key_buf);
	free(cert_buf);

	if (cix_client_request(c, CIX_API_putSystemSigningKeys_METHOD, CIX_API_putSystemSigningKeys, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	rc = emit(&r, json_mode, fmt_signing_keys);
	return rc;
}

static int cmd_signing_keys_clear(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_deleteSystemSigningKeys_METHOD, CIX_API_deleteSystemSigningKeys, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_signing_keys);
}

static int cmd_signing_keys(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_signing_keys_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_signing_keys_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_signing_keys_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "clear") == 0)
		return cmd_signing_keys_clear(c, json_mode);

	fprintf(stderr,
	        "usage: cixctl signing-keys [show]\n"
	        "       cixctl signing-keys set --key=PATH --cert=PATH\n"
	        "       cixctl signing-keys clear\n");
	return 2;
}


/*
 * cixctl release-key [show] / set --key=PATH / clear (ADR-0220) -- the
 * Ed25519 key that signs published artifacts, distinct from the RSA
 * Secure Boot pair above.
 *
 * Same argument as signing-keys for taking a path rather than the PEM
 * text: a private key on a command line lands in shell history and in
 * this host's process list.
 *
 * `show` prints the public key in full, because that is the whole
 * point of holding it -- an operator publishes that exact text and
 * whoever downloads a Cix ISO verifies against it with stock minisign.
 */
static void fmt_release_key(const struct json_value *v)
{
	const struct json_value *key_set = json_object_get(v, "key_set");
	const char *pub = json_as_string(json_object_get(v, "public_key"));
	const char *key_id = json_as_string(json_object_get(v, "key_id"));
	int have = key_set != NULL && key_set->type == JSON_BOOL && key_set->u.boolean;

	if (!have) {
		printf("no release key installed -- ISOs built here will be unsigned\n");
		return;
	}
	printf("release key   installed\n");
	if (key_id != NULL)
		printf("key id        %s\n", key_id);
	if (pub != NULL)
		printf("\npublic key (publish this -- verifiers need it):\n%s", pub);
}

static int cmd_release_key_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemReleaseKey_METHOD, CIX_API_getSystemReleaseKey,
	                       NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_release_key);
}

static int cmd_release_key_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *key_path = NULL;
	char *key_buf = NULL;
	size_t key_len = 0;
	struct json_writer w;
	struct cix_response r;
	int i;
	int rc;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--key=", 6) == 0)
			key_path = argv[i] + 6;
		else {
			fprintf(stderr, "cixctl: unknown release-key set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (key_path == NULL) {
		fprintf(stderr, "cixctl: release-key set needs --key=PATH\n");
		return 2;
	}
	if (read_local_file(key_path, &key_buf, &key_len) != 0)
		return 1;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "key");
	jw_str(&w, key_buf);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(key_buf);

	if (cix_client_request(c, CIX_API_putSystemReleaseKey_METHOD, CIX_API_putSystemReleaseKey,
	                       w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	rc = emit(&r, json_mode, fmt_release_key);
	return rc;
}

static int cmd_release_key_clear(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_deleteSystemReleaseKey_METHOD,
	                       CIX_API_deleteSystemReleaseKey, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_release_key);
}

static int cmd_release_key(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1)
		return cmd_release_key_show(c, json_mode);

	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_release_key_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_release_key_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "clear") == 0)
		return cmd_release_key_clear(c, json_mode);

	fprintf(stderr,
	        "usage: cixctl release-key [show]\n"
	        "       cixctl release-key set --key=PATH\n"
	        "       cixctl release-key clear\n");
	return 2;
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

static int cmd_files_get(const struct cix_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *path_arg = NULL;
	const char *output = NULL;
	char encoded_path[256 * 3]; /* 256 matches daemon's CONTAINER_FILE_PATH_MAX */
	char path[400];
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--path=", 7) == 0)
			path_arg = argv[i] + 7;
		else if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown files get option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || path_arg == NULL) {
		fprintf(stderr, "usage: cixctl files get NAME --path=/some/path [--output=PATH]\n");
		return 2;
	}

	url_encode_query_value(path_arg, encoded_path, sizeof(encoded_path));
	snprintf(path, sizeof(path), CIX_API_getContainerFile "?path=%s", name, encoded_path);

	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed", r.status);
		cix_response_free(&r);
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
			cix_response_free(&r);
			return 1;
		}
		fclose(f);
	} else if (r.body != NULL) {
		fwrite(r.body, 1, r.body_len, stdout);
	}

	cix_response_free(&r);
	return 0;
}

/*
 * PUT /v1/containers/{name}/files?path=... (ADR-0153, task #861): a
 * live, immediate hotfix to one file inside a running (or stopped-
 * but-defined) container -- deliberately not persisted into the
 * container's own definition (a future restart replays the original
 * persisted body unchanged). See the daemon's own doc comment on
 * handle_container_file_write() for why: the durable "this should
 * survive a recreate" path is a deployment (`deployment
 * add`/`deployment apply`), not this command.
 */
static int cmd_files_put(const struct cix_client *c, int argc, char **argv)
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
	struct cix_response r;
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
			fprintf(stderr, "cixctl: unknown files put option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || path_arg == NULL || file_arg == NULL) {
		fprintf(stderr,
		        "usage: cixctl files put NAME --path=/some/path --file=LOCAL_PATH "
		        "[--mode=0644]\n");
		return 2;
	}
	if (read_local_file(file_arg, &content, &content_len) != 0) {
		fprintf(stderr, "cixctl: could not read %s\n", file_arg);
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
	snprintf(path, sizeof(path), CIX_API_putContainerFile "?path=%s", name, encoded_path);

	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed", r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("wrote %s on %s\n", path_arg, name);
	cix_response_free(&r);
	return 0;
}

static int cmd_files(const struct cix_client *c, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl files get NAME --path=/some/path [--output=PATH]\n"
		                "       cixctl files put NAME --path=/some/path --file=LOCAL_PATH "
		                "[--mode=0644]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "get") == 0)
		return cmd_files_get(c, argc - 1, argv + 1);
	if (strcmp(sub, "put") == 0)
		return cmd_files_put(c, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown files subcommand '%s'\n", sub);
	return 2;
}

static int cmd_backup(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *output = NULL;
	int i;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--output=", 9) == 0)
			output = argv[i] + 9;
		else {
			fprintf(stderr, "cixctl: unknown backup option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (cix_client_request(c, CIX_API_getSystemBackup_METHOD, CIX_API_getSystemBackup, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
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
			cix_response_free(&r);
			return 1;
		}
		fclose(f);
		printf("backup written to %s (%zu bytes)\n", output, r.body_len);
		cix_response_free(&r);
		return 0;
	}

	return emit(&r, json_mode, NULL);
}

static int cmd_restore(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *input = NULL;
	int i;
	char *buf;
	size_t len;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--input=", 8) == 0)
			input = argv[i] + 8;
		else {
			fprintf(stderr, "cixctl: unknown restore option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (input == NULL) {
		fprintf(stderr, "usage: cixctl restore --input=PATH\n");
		return 2;
	}
	if (read_local_file(input, &buf, &len) != 0) {
		fprintf(stderr, "cixctl: could not read %s\n", input);
		return 1;
	}

	if (cix_client_request(c, CIX_API_postSystemRestore_METHOD, CIX_API_postSystemRestore, buf, &r) != 0) {
		free(buf);
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_site_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemSite_METHOD, CIX_API_getSystemSite, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
static int cmd_site_set(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--instance-name=", 16) == 0)
			new_instance_name = argv[i] + 16;
		else if (strncmp(argv[i], "--site-name=", 12) == 0)
			new_site_name = argv[i] + 12;
		else if (strncmp(argv[i], "--domain-suffix=", 16) == 0)
			new_domain_suffix = argv[i] + 16;
		else {
			fprintf(stderr, "cixctl: unknown site set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (new_instance_name == NULL && new_site_name == NULL && new_domain_suffix == NULL) {
		fprintf(stderr, "usage: cixctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}

	if (cix_client_request(c, CIX_API_getSystemSite_METHOD, CIX_API_getSystemSite, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		fprintf(stderr, "cixctl: could not read current site config (HTTP %d)\n", r.status);
		cix_response_free(&r);
		return 1;
	}
	cur = json_str_field(r.json, "instance_name");
	snprintf(instance_name, sizeof(instance_name), "%s", cur != NULL ? cur : "cix");
	cur = json_str_field(r.json, "site_name");
	snprintf(site_name, sizeof(site_name), "%s", cur != NULL ? cur : "");
	cur = json_str_field(r.json, "domain_suffix");
	snprintf(domain_suffix, sizeof(domain_suffix), "%s", cur != NULL ? cur : "internal");
	cix_response_free(&r);

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

	if (cix_client_request(c, CIX_API_putSystemSite_METHOD, CIX_API_putSystemSite, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_site_config);
}

static int cmd_site(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl site show\n"
		                "       cixctl site set [--instance-name=NAME] [--site-name=NAME] "
		                "[--domain-suffix=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_site_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_site_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown site subcommand '%s'\n", sub);
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

static int cmd_daemon_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getDaemonConfig_METHOD, CIX_API_getDaemonConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
static int cmd_daemon_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown daemon-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (bind_ip != NULL && clear_bind_ip) {
		fprintf(stderr, "cixctl: --bind-ip= and --clear-bind-ip are mutually exclusive\n");
		return 2;
	}
	if (port == NULL && https_port == NULL && management_network == NULL && bind_ip == NULL &&
	    !clear_bind_ip && want_http == -1 && want_https == -1) {
		fprintf(stderr,
		        "usage: cixctl daemon-config set [--port=N] [--https-port=N] "
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

	if (cix_client_request(c, CIX_API_putDaemonConfig_METHOD, CIX_API_putDaemonConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_daemon_config);
}

static int cmd_daemon_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl daemon-config show\n"
		                "       cixctl daemon-config set [--port=N] [--https-port=N] "
		                "[--enable-http] [--disable-http] [--enable-https] [--disable-https] "
		                "[--management-network=NAME] [--bind-ip=A.B.C.D | --clear-bind-ip]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_daemon_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_daemon_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown daemon-config subcommand '%s'\n", sub);
	return 2;
}

/* ADR-0141 Phase 5: cixctl backup-config show|set|status|snapshot-now
 * -- turns the `backup` disk role from a pure inert label into a real,
 * scheduled snapshot mechanism. */
static void fmt_backup_config(const struct json_value *v)
{
	const char *disk = json_str_field(v, "disk");
	const struct json_value *jenabled = json_object_get(v, "enabled");

	/* ADR-0257: when it runs is `cixctl schedule ls`, not here. */
	printf("disk=%s enabled=%s  (schedule: cixctl schedule ls)\n",
	       disk != NULL ? disk : "(none)",
	       (jenabled != NULL && jenabled->type == JSON_BOOL && jenabled->u.boolean) ? "true"
	                                                                                : "false");
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

static int cmd_backup_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getBackupConfig_METHOD, CIX_API_getBackupConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_config);
}

static int cmd_backup_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	int clear_disk = 0;
	int want_enabled = -1; /* -1: not given */
	struct json_writer w;
	struct cix_response r;
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
		else if (strncmp(argv[i], "--interval-hours=", 17) == 0) {
			fprintf(stderr, "cixctl: --interval-hours is gone (ADR-0257) -- use\n"
			                "  cixctl schedule set system-backup --action=system.backup "
			                "--every-hours=N\n");
			return 2;
		}
		else {
			fprintf(stderr, "cixctl: unknown backup-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (disk != NULL && clear_disk) {
		fprintf(stderr, "cixctl: --disk= and --clear-disk are mutually exclusive\n");
		return 2;
	}
	if (disk == NULL && !clear_disk && want_enabled == -1) {
		fprintf(stderr,
		        "usage: cixctl backup-config set [--disk=NAME | --clear-disk] "
		        "[--enable | --disable]\n"
		        "  the schedule lives in `cixctl schedule` since ADR-0257\n");
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
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putBackupConfig_METHOD, CIX_API_putBackupConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_backup_config);
}

static int cmd_backup_config_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getBackupConfigStatus_METHOD, CIX_API_getBackupConfigStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_status);
}

static int cmd_backup_config_snapshot_now(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_postBackupConfigSnapshotNow_METHOD, CIX_API_postBackupConfigSnapshotNow, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_backup_status);
}

static int cmd_backup_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl backup-config show|set|status|snapshot-now\n");
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

	fprintf(stderr, "cixctl: unknown backup-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * Part 5 (ADR-0124): cixctl rolling-config show|set -- mirrors
 * cmd_daemon_config's own shape exactly, one field instead of several.
 */
static void fmt_rolling_config(const struct json_value *v)
{
	printf("jitter_window_seconds=%ld\n",
	       (long)json_as_number(json_object_get(v, "jitter_window_seconds")));
}

static int cmd_rolling_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemRollingConfig_METHOD, CIX_API_getSystemRollingConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_rolling_config);
}

static int cmd_rolling_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *window = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--jitter-window-seconds=", 24) == 0)
			window = argv[i] + 24;
		else {
			fprintf(stderr, "cixctl: unknown rolling-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (window == NULL) {
		fprintf(stderr, "usage: cixctl rolling-config set --jitter-window-seconds=N\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "jitter_window_seconds");
	jw_int(&w, atol(window));
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putSystemRollingConfig_METHOD, CIX_API_putSystemRollingConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_rolling_config);
}

static int cmd_rolling_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl rolling-config show\n"
		                "       cixctl rolling-config set --jitter-window-seconds=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_rolling_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_rolling_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown rolling-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * ADR-0157 Phase 3 / ADR-0165: cixctl pkg-build-config show|set --
 * mirrors cmd_rolling_config's own shape, now three independently
 * settable fields (partial update, only the flags given are changed)
 * instead of one.
 */
static void fmt_pkg_build_config(const struct json_value *v)
{
	const struct json_value *jcpu = json_object_get(v, "cpu_max");
	const char *cpu_max = jcpu != NULL ? json_as_string(jcpu) : NULL;

	printf("max_concurrent_jobs=%ld\n",
	       (long)json_as_number(json_object_get(v, "max_concurrent_jobs")));
	printf("memory_max=%.0f\n", json_as_number(json_object_get(v, "memory_max")));
	printf("cpu_max=%s\n", cpu_max != NULL ? cpu_max : "(none)");
}

static int cmd_pkg_build_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemPkgBuildConfig_METHOD, CIX_API_getSystemPkgBuildConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_build_config);
}

static int cmd_pkg_build_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *max_jobs = NULL;
	const char *memory_max = NULL;
	const char *cpu_max = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-concurrent-jobs=", 23) == 0)
			max_jobs = argv[i] + 23;
		else if (strncmp(argv[i], "--memory-max=", 13) == 0)
			memory_max = argv[i] + 13;
		else if (strncmp(argv[i], "--cpu-max=", 10) == 0)
			cpu_max = argv[i] + 10;
		else {
			fprintf(stderr, "cixctl: unknown pkg-build-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (max_jobs == NULL && memory_max == NULL && cpu_max == NULL) {
		fprintf(stderr, "usage: cixctl pkg-build-config set [--max-concurrent-jobs=N] "
		                "[--memory-max=BYTES] [--cpu-max=\"QUOTA PERIOD\"]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (max_jobs != NULL) {
		jw_key(&w, "max_concurrent_jobs");
		jw_int(&w, atol(max_jobs));
	}
	if (memory_max != NULL) {
		jw_key(&w, "memory_max");
		jw_int(&w, atoll(memory_max));
	}
	if (cpu_max != NULL) {
		jw_key(&w, "cpu_max");
		jw_str(&w, cpu_max);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putSystemPkgBuildConfig_METHOD, CIX_API_putSystemPkgBuildConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_build_config);
}

static int cmd_pkg_build_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg-build-config show\n"
		                "       cixctl pkg-build-config set --max-concurrent-jobs=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_build_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_build_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown pkg-build-config subcommand '%s'\n", sub);
	return 2;
}


/*
 * Issue #100: cixctl stalls -- times the control plane stopped going
 * round its own loop, recorded by a watchdog process because the loop
 * cannot report its own silence.
 */
static void fmt_stalls(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "stalls");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("no control-plane stalls recorded\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *s = arr->u.array.items[i];
		const char *wchan = json_str_field(s, "wchan");
		const char *activity = json_str_field(s, "activity");

		printf("%-12lld %-16s %4llds state=%s wchan=%-24s %s\n",
		       (long long)json_as_number(json_object_get(s, "ts")),
		       json_str_field(s, "event") != NULL ? json_str_field(s, "event") : "-",
		       (long long)json_as_number(json_object_get(s, "seconds")),
		       json_str_field(s, "state") != NULL ? json_str_field(s, "state") : "?",
		       wchan != NULL && wchan[0] != '\0' ? wchan : "(running)",
		       activity != NULL && activity[0] != '\0' ? activity : "(idle)");
	}
}

static int cmd_stalls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getStalls_METHOD, CIX_API_getStalls, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_stalls);
}


/*
 * Issue #308: cixctl assembly status|start.
 *
 * Assembly had status and no trigger -- it happened only as a side
 * effect of a hostbuild named "cix" completing, and `pkg hostbuild`
 * answers an already-installed version with 409, so re-assembling a
 * root meant inventing a version number whose code had not changed.
 *
 * `status` reads the same endpoint --deploy already polls; `start`
 * asks for one. Both print image_path, because the next thing an
 * operator does with a fresh root is `cixctl update --image=<that>`.
 */
static void fmt_assembly(const struct json_value *v)
{
	const struct json_value *running = json_object_get(v, "running");
	const char *image_path = json_str_field(v, "image_path");
	int is_running = running != NULL && running->type == JSON_BOOL && running->u.boolean;

	printf("running:              %s\n", is_running ? "yes" : "no");
	printf("started_generation:   %ld\n",
	       (long)json_as_number(json_object_get(v, "started_generation")));
	printf("completed_generation: %ld\n",
	       (long)json_as_number(json_object_get(v, "completed_generation")));
	printf("image_path:           %s\n", image_path != NULL ? image_path : "-");
}

static int cmd_assembly(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "status";

	if (strcmp(sub, "status") == 0) {
		if (cix_client_request(c, CIX_API_getSystemAssembly_METHOD, CIX_API_getSystemAssembly,
		                       NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_assembly);
	}
	if (strcmp(sub, "start") == 0) {
		if (cix_client_request(c, CIX_API_postSystemAssembly_METHOD, CIX_API_postSystemAssembly,
		                       NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		/*
		 * Deliberately does not wait. An assembly takes tens of
		 * seconds and `--deploy` already owns the wait-and-then-deploy
		 * flow; this is the primitive underneath it, and a client that
		 * wants to block polls `assembly status` for
		 * completed_generation to pass what it saw before the call.
		 */
		return emit(&r, json_mode, fmt_assembly);
	}
	fprintf(stderr, "cixctl: unknown assembly subcommand \"%s\" (status, start)\n", sub);
	return 2;
}


/*
 * Issue #128: cixctl esp show|set|rm-entry -- the EFI System
 * Partition's boot configuration.
 *
 * The show output leads with the answer to the question that actually
 * matters, "what will this machine boot next", because working that out
 * by hand from a glob pattern and a directory listing is exactly the
 * step that once cost a very long investigation. An entry the default
 * pattern does not select is marked, so a staged update being silently
 * outranked is visible at a glance rather than inferred.
 */
static void fmt_esp(const struct json_value *v)
{
	const struct json_value *entries = json_object_get(v, "entries");
	const struct json_value *present = json_object_get(v, "present");
	const struct json_value *writable = json_object_get(v, "writable");
	const char *def = json_str_field(v, "default");
	const char *selected = json_str_field(v, "selected_entry");
	const char *boot_next = json_str_field(v, "boot_next");
	const char *running = json_str_field(v, "running_slot");
	const struct json_value *timeout = json_object_get(v, "timeout");
	size_t i;

	if (present == NULL || present->type != JSON_BOOL || !present->u.boolean) {
		printf("no EFI System Partition visible from here (not an installed host)\n");
		return;
	}
	printf("default:  %s\n", def != NULL ? def : "(unset)");
	printf("timeout:  ");
	if (timeout != NULL && timeout->type == JSON_NUMBER)
		printf("%d\n", (int)json_as_number(timeout));
	else
		printf("(unset)\n");
	printf("running:  slot %s\n", running != NULL ? running : "(unknown)");
	printf("will boot: %s\n", selected != NULL ? selected : "(nothing matches the default)");
	/* Say WHY it differs from the default pattern, and say that it
	 * differs only once -- an operator seeing an unexpected entry here
	 * needs both facts, not just the name (#154). */
	if (boot_next != NULL)
		printf("           (one-shot armed -- this boot only, then normal selection)\n");
	if (writable != NULL && writable->type == JSON_BOOL && !writable->u.boolean)
		printf("NOTE: the ESP is mounted read-only -- changes are not possible\n");

	if (entries != NULL && entries->type == JSON_ARRAY) {
		printf("\nentries:\n");
		for (i = 0; i < entries->u.array.count; i++) {
			const struct json_value *e = entries->u.array.items[i];
			const struct json_value *m = json_object_get(e, "matches_default");
			const struct json_value *rs = json_object_get(e, "is_running_slot");
			int matches = (m != NULL && m->type == JSON_BOOL && m->u.boolean);
			int is_running = (rs != NULL && rs->type == JSON_BOOL && rs->u.boolean);
			const char *nm = json_str_field(e, "name");

			printf("  %-20s %-14s %s\n", nm != NULL ? nm : "-",
			       matches ? "[default]" : "[not matched]", is_running ? "(running slot)" : "");
		}
	}
}

static int cmd_esp(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getEsp_METHOD, CIX_API_getEsp, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_esp);
	}
	if (strcmp(sub, "set") == 0) {
		const char *def = NULL;
		const char *timeout = NULL;
		struct json_writer w;
		int i;

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--default=", 10) == 0)
				def = argv[i] + 10;
			else if (strncmp(argv[i], "--timeout=", 10) == 0)
				timeout = argv[i] + 10;
			else {
				fprintf(stderr, "cixctl: unknown esp set option '%s'\n", argv[i]);
				return 2;
			}
		}
		if (def == NULL && timeout == NULL) {
			fprintf(stderr, "usage: cixctl esp set [--default=PATTERN] [--timeout=N]\n");
			return 2;
		}
		jw_init(&w);
		jw_obj_open(&w);
		if (def != NULL) {
			jw_key(&w, "default");
			jw_str(&w, def);
		}
		if (timeout != NULL) {
			jw_key(&w, "timeout");
			jw_int(&w, atoi(timeout));
		}
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		if (cix_client_request(c, CIX_API_putEsp_METHOD, CIX_API_putEsp, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		return emit(&r, json_mode, fmt_esp);
	}
	if (strcmp(sub, "rm-entry") == 0) {
		char path[256];

		if (argc < 2) {
			fprintf(stderr, "usage: cixctl esp rm-entry NAME\n");
			return 2;
		}
		snprintf(path, sizeof(path), CIX_API_deleteEspEntry, argv[1]);
		if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status == 204) {
			printf("removed %s\n", argv[1]);
			cix_response_free(&r);
			return 0;
		}
		return emit(&r, json_mode, NULL);
	}
	fprintf(stderr, "usage: cixctl esp show\n"
	                "       cixctl esp set [--default=PATTERN] [--timeout=N]\n"
	                "       cixctl esp rm-entry NAME\n");
	return 2;
}

/*
 * Issue #24: cixctl boot-console show|set -- what the installed
 * system's own boot line says about consoles. Applied to the loader
 * entries already on the ESP, so it takes effect at the next boot
 * rather than at the next A/B update.
 */
static void fmt_boot_console(const struct json_value *v)
{
	const struct json_value *cfg = json_object_get(v, "config");
	const struct json_value *entries = json_object_get(v, "loader_entries");
	const struct json_value *updated = json_object_get(v, "loader_entries_updated");
	size_t i;

	printf("rendered: %s\n",
	       cfg != NULL && json_str_field(cfg, "rendered") != NULL ? json_str_field(cfg, "rendered")
	                                                              : "(none)");
	if (updated != NULL)
		printf("loader entries rewritten: %d (takes effect at the next boot)\n",
		       (int)json_as_number(updated));
	if (entries != NULL && entries->type == JSON_ARRAY) {
		if (entries->u.array.count == 0)
			printf("no loader entries visible from here (not an installed host)\n");
		for (i = 0; i < entries->u.array.count; i++)
			printf("%-20s %s\n",
			       json_str_field(entries->u.array.items[i], "entry") != NULL
			           ? json_str_field(entries->u.array.items[i], "entry")
			           : "-",
			       json_str_field(entries->u.array.items[i], "options") != NULL
			           ? json_str_field(entries->u.array.items[i], "options")
			           : "-");
	}
}

static int cmd_boot_console(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	const char *consoles[8];
	int console_count = 0;
	const char *extra = NULL;
	int i;
	struct json_writer w;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getBootConsole_METHOD, CIX_API_getBootConsole, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_boot_console);
	}
	if (strcmp(sub, "set") != 0) {
		fprintf(stderr, "usage: cixctl boot-console show\n"
		                "       cixctl boot-console set [--console=NAME ...] [--extra=\"...\"]\n"
		                "  --console is repeatable and ordered (e.g. --console=tty0 "
		                "--console=ttyS0,115200n8)\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--console=", 10) == 0) {
			if (console_count >= (int)(sizeof(consoles) / sizeof(consoles[0]))) {
				fprintf(stderr, "cixctl: too many --console= values\n");
				return 2;
			}
			consoles[console_count++] = argv[i] + 10;
		} else if (strncmp(argv[i], "--extra=", 8) == 0) {
			extra = argv[i] + 8;
		} else {
			fprintf(stderr, "cixctl: unknown boot-console set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (console_count == 0 && extra == NULL) {
		fprintf(stderr, "usage: cixctl boot-console set [--console=NAME ...] [--extra=\"...\"]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (console_count > 0) {
		jw_key(&w, "consoles");
		jw_arr_open(&w);
		for (i = 0; i < console_count; i++)
			jw_str(&w, consoles[i]);
		jw_arr_close(&w);
	}
	if (extra != NULL) {
		jw_key(&w, "extra");
		jw_str(&w, extra);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_setBootConsole_METHOD, CIX_API_setBootConsole, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_boot_console);
}


/*
 * Issue #65: cixctl kernel-policy show|set|refresh -- which kernel
 * line this box tracks. Prints the gap rather than just the two
 * versions: "you are on X, your channel is at Y" is the question, and
 * leaving the reader to compare two version strings themselves is
 * leaving them to make the mistake.
 */
static void fmt_kernel_policy(const struct json_value *v)
{
	const struct json_value *behind = json_object_get(v, "behind");
	const struct json_value *maintained = json_object_get(v, "running_series_maintained");
	const char *channel = json_str_field(v, "channel");
	const char *running = json_str_field(v, "running_version");
	const char *series = json_str_field(v, "running_series");
	const char *resolved = json_str_field(v, "resolved_version");
	const char *newest_lt = json_str_field(v, "newest_longterm");
	const char *err = json_str_field(v, "last_refresh_error");
	const struct json_value *fetched = json_object_get(v, "releases_fetched_at");
	const struct json_value *refreshing = json_object_get(v, "refreshing");

	printf("channel:  %s\n", channel != NULL ? channel : "-");
	printf("running:  %s (line %s)\n", running != NULL ? running : "-",
	       series != NULL && series[0] != '\0' ? series : "?");
	if (resolved != NULL)
		printf("channel is at: %s%s\n", resolved,
		       behind != NULL && behind->type == JSON_BOOL && behind->u.boolean
		           ? "  -- you are behind it"
		           : "  -- you are on it");
	else if (channel != NULL && strcmp(channel, "pinned") == 0)
		printf("channel is at: (nothing -- pinned means no version is proposed)\n");
	else
		printf("channel is at: (unknown -- run `cixctl kernel-policy refresh` first)\n");
	/* Only says anything on the longterm channel: on stable or mainline
	 * whether your line is a longterm one is not a fact you asked
	 * about, and printing it there is noise dressed as a warning. */
	if (channel != NULL && strcmp(channel, "longterm") == 0 && maintained != NULL &&
	    maintained->type == JSON_BOOL && !maintained->u.boolean && newest_lt != NULL)
		printf("note:     %s is not a longterm line kernel.org still lists; the newest is %s\n",
		       series != NULL ? series : "your line", newest_lt);
	if (fetched != NULL && fetched->type == JSON_NUMBER)
		printf("resolved from %s at unix %lld\n",
		       json_str_field(v, "source") != NULL ? json_str_field(v, "source") : "kernel.org",
		       (long long)json_as_number(fetched));
	else
		printf("never resolved -- no kernel.org release data fetched yet\n");
	if (refreshing != NULL && refreshing->type == JSON_BOOL && refreshing->u.boolean)
		printf("a refresh is running right now\n");
	if (err != NULL)
		printf("last refresh error: %s\n", err);
}

static int cmd_kernel_policy(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	const char *channel = NULL;
	int i;
	struct json_writer w;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getKernelPolicy_METHOD, CIX_API_getKernelPolicy, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_kernel_policy);
	}
	if (strcmp(sub, "refresh") == 0) {
		if (cix_client_request(c, CIX_API_refreshKernelPolicy_METHOD, CIX_API_refreshKernelPolicy, "{}", &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_kernel_policy);
	}
	if (strcmp(sub, "set") != 0) {
		fprintf(stderr, "usage: cixctl kernel-policy show\n"
		                "       cixctl kernel-policy set --channel=pinned|longterm|stable|mainline\n"
		                "       cixctl kernel-policy refresh\n"
		                "  channels are kernel.org's own monikers; pinned (the default) means\n"
		                "  the version stays where it is, in the kernel recipe\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--channel=", 10) == 0)
			channel = argv[i] + 10;
		else {
			fprintf(stderr, "cixctl: unknown kernel-policy set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (channel == NULL) {
		fprintf(stderr, "usage: cixctl kernel-policy set "
		                "--channel=pinned|longterm|stable|mainline\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "channel");
	jw_str(&w, channel);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_setKernelPolicy_METHOD, CIX_API_setKernelPolicy, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_kernel_policy);
}

/*
 * Issue #86: cixctl control-plane-reservation show|set -- how much of
 * the machine is held back for the daemon itself. Reported with the
 * derived workload ceiling, because a percentage on its own is not
 * something an operator can act on.
 */
static void fmt_cpreserve(const struct json_value *v)
{
	const struct json_value *en = json_object_get(v, "enabled");
	const char *cpu_max = json_str_field(v, "workload_cpu_max");
	const struct json_value *mem_max = json_object_get(v, "workload_memory_max");

	printf("enabled=%s\n",
	       (en != NULL && en->type == JSON_BOOL && en->u.boolean) ? "true" : "false");
	printf("reserved cpu_percent=%d memory_bytes=%lld\n",
	       (int)json_as_number(json_object_get(v, "cpu_percent")),
	       (long long)json_as_number(json_object_get(v, "memory_bytes")));
	printf("host cpus=%d memory_bytes=%lld\n",
	       (int)json_as_number(json_object_get(v, "host_cpus")),
	       (long long)json_as_number(json_object_get(v, "host_memory_bytes")));
	{
		char mem_str[32];

		if (mem_max != NULL && mem_max->type == JSON_NUMBER)
			snprintf(mem_str, sizeof(mem_str), "%lld", (long long)json_as_number(mem_max));
		else
			snprintf(mem_str, sizeof(mem_str), "(unlimited)");
		printf("workload cgroup=%s cpu.max=%s memory.max=%s\n",
		       json_str_field(v, "cgroup") != NULL ? json_str_field(v, "cgroup") : "-",
		       cpu_max != NULL ? cpu_max : "(unlimited)", mem_str);
	}
}

static int cmd_cpreserve(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";
	const char *cpu = NULL, *mem = NULL;
	int want_enabled = -1;
	int i;
	struct json_writer w;

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getControlPlaneReservation_METHOD, CIX_API_getControlPlaneReservation, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_cpreserve);
	}
	if (strcmp(sub, "set") != 0) {
		fprintf(stderr, "usage: cixctl control-plane-reservation show\n"
		                "       cixctl control-plane-reservation set [--enabled | --disabled] "
		                "[--cpu-percent=N] [--memory-bytes=N]\n");
		return 2;
	}
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--enabled") == 0)
			want_enabled = 1;
		else if (strcmp(argv[i], "--disabled") == 0)
			want_enabled = 0;
		else if (strncmp(argv[i], "--cpu-percent=", 14) == 0)
			cpu = argv[i] + 14;
		else if (strncmp(argv[i], "--memory-bytes=", 15) == 0)
			mem = argv[i] + 15;
		else {
			fprintf(stderr, "cixctl: unknown control-plane-reservation set option '%s'\n",
			        argv[i]);
			return 2;
		}
	}
	if (want_enabled == -1 && cpu == NULL && mem == NULL) {
		fprintf(stderr, "usage: cixctl control-plane-reservation set [--enabled | --disabled] "
		                "[--cpu-percent=N] [--memory-bytes=N]\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (want_enabled != -1) {
		jw_key(&w, "enabled");
		jw_bool(&w, want_enabled);
	}
	if (cpu != NULL) {
		jw_key(&w, "cpu_percent");
		jw_int(&w, atol(cpu));
	}
	if (mem != NULL) {
		jw_key(&w, "memory_bytes");
		jw_int(&w, atoll(mem));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_setControlPlaneReservation_METHOD, CIX_API_setControlPlaneReservation, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_cpreserve);
}


/*
 * ADR-0134: cixctl tls-throttle show|set|status -- per-source-IP
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

static int cmd_tls_throttle_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemTlsThrottle_METHOD, CIX_API_getSystemTlsThrottle, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_tls_throttle);
}

static int cmd_tls_throttle_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *threshold = NULL;
	const char *window = NULL;
	const char *block = NULL;
	const char *log_interval = NULL;
	int want_enabled = -1; /* -1: untouched, 0: disable, 1: enable */
	int i;
	struct json_writer w;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown tls-throttle set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (want_enabled == -1 && threshold == NULL && window == NULL && block == NULL && log_interval == NULL) {
		fprintf(stderr, "usage: cixctl tls-throttle set [--enabled | --disabled] "
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

	if (cix_client_request(c, CIX_API_putSystemTlsThrottle_METHOD, CIX_API_putSystemTlsThrottle, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_tls_throttle_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemTlsThrottleStatus_METHOD, CIX_API_getSystemTlsThrottleStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_tls_throttle_status);
}

static int cmd_tls_throttle(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl tls-throttle show\n"
		                "       cixctl tls-throttle set [--enabled | --disabled] "
		                "[--threshold=N] [--window-seconds=N] [--block-seconds=N] "
		                "[--log-interval-seconds=N]\n"
		                "       cixctl tls-throttle status\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_tls_throttle_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_tls_throttle_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_tls_throttle_status(c, json_mode);

	fprintf(stderr, "cixctl: unknown tls-throttle subcommand '%s'\n", sub);
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
	const struct json_value *jldap_tls = json_object_get(v, "ldap_tls");
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
	printf(" ldap_port=%ld ldap_tls=%s ldap_base_dn=%s\n",
	       (long)json_as_number(json_object_get(v, "ldap_port")),
	       (jldap_tls != NULL && jldap_tls->type == JSON_BOOL && jldap_tls->u.boolean) ? "true"
	                                                                                   : "false",
	       base_dn != NULL && base_dn[0] != '\0' ? base_dn : "-");
}

static int cmd_hostauth_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getHostauthConfig_METHOD, CIX_API_getHostauthConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_hostauth_config);
}

static int cmd_hostauth_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *admin_groups[8];
	int admin_group_count = -1; /* -1: not given, keep current */
	const char *idle_timeout = NULL;
	int want_ldap_enabled = -1; /* -1: untouched, 0: disable, 1: enable */
	int want_ldap_tls = -1;     /* same tri-state, for #416's own TLS switch */
	const char *ldap_servers[3];
	int ldap_server_count = -1;
	const char *ldap_port = NULL;
	const char *ldap_base_dn = NULL;
	int i;
	struct json_value *current;
	struct cix_response r;
	struct json_writer w;

	admin_group_count = 0;
	ldap_server_count = 0;
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--admin-group=", 14) == 0) {
			if (admin_group_count >= 8) {
				fprintf(stderr, "cixctl: too many --admin-group= flags (max 8)\n");
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
				fprintf(stderr, "cixctl: too many --ldap-server= flags (max 3)\n");
				return 2;
			}
			ldap_servers[ldap_server_count++] = argv[i] + 14;
		} else if (strncmp(argv[i], "--ldap-port=", 12) == 0) {
			ldap_port = argv[i] + 12;
		} else if (strcmp(argv[i], "--ldap-tls") == 0) {
			want_ldap_tls = 1;
		} else if (strcmp(argv[i], "--no-ldap-tls") == 0) {
			want_ldap_tls = 0;
		} else if (strncmp(argv[i], "--ldap-base-dn=", 15) == 0) {
			ldap_base_dn = argv[i] + 15;
		} else {
			fprintf(stderr, "cixctl: unknown hostauth-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (admin_group_count == 0)
		admin_group_count = -1;
	if (ldap_server_count == 0)
		ldap_server_count = -1;
	if (admin_group_count == -1 && idle_timeout == NULL && want_ldap_enabled == -1 &&
	    ldap_server_count == -1 && ldap_port == NULL && want_ldap_tls == -1 &&
	    ldap_base_dn == NULL) {
		fprintf(stderr,
		        "usage: cixctl hostauth-config set [--admin-group=NAME ...] "
		        "[--idle-timeout-seconds=N] [--ldap-enable | --ldap-disable] "
		        "[--ldap-server=HOST ...] [--ldap-port=N] "
		        "[--ldap-tls | --no-ldap-tls] [--ldap-base-dn=NAME]\n");
		return 2;
	}

	/* Read-modify-write: fetch the current config so any flag NOT
	 * given here is preserved exactly, not reset by the server's own
	 * full-replacement PUT semantics. */
	if (cix_client_request(c, CIX_API_getHostauthConfig_METHOD, CIX_API_getHostauthConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
		const struct json_value *jcur_tls = json_object_get(current, "ldap_tls");
		int cur_tls = jcur_tls != NULL && jcur_tls->type == JSON_BOOL && jcur_tls->u.boolean;

		jw_key(&w, "ldap_tls");
		jw_bool(&w, want_ldap_tls != -1 ? want_ldap_tls : cur_tls);
	}
	{
		const char *cur_base_dn = json_str_field(current, "ldap_base_dn");

		jw_key(&w, "ldap_base_dn");
		jw_str(&w, ldap_base_dn != NULL ? ldap_base_dn : (cur_base_dn != NULL ? cur_base_dn : ""));
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	cix_response_free(&r);

	if (cix_client_request(c, CIX_API_putHostauthConfig_METHOD, CIX_API_putHostauthConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_hostauth_config);
}

static int cmd_hostauth_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl hostauth-config show\n"
		        "       cixctl hostauth-config set [--admin-group=NAME ...] "
		        "[--idle-timeout-seconds=N] [--ldap-enable | --ldap-disable] "
		        "[--ldap-server=HOST ...] [--ldap-port=N] [--ldap-base-dn=NAME]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_hostauth_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_hostauth_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown hostauth-config subcommand '%s'\n", sub);
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

static int cmd_hostauth_sessions_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listHostauthSessions_METHOD, CIX_API_listHostauthSessions, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_hostauth_sessions_list);
}

static int cmd_hostauth_sessions_revoke(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl hostauth-sessions revoke USERNAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_revokeHostauthSessions, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_hostauth_sessions(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl hostauth-sessions ls\n"
		                "       cixctl hostauth-sessions revoke USERNAME  -- log out everywhere\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_hostauth_sessions_ls(c, json_mode);
	if (strcmp(sub, "revoke") == 0)
		return cmd_hostauth_sessions_revoke(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown hostauth-sessions subcommand '%s'\n", sub);
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

struct cli_env {
	char key[128]; /* matches daemon's CONTAINER_ENV_KEY_MAX */
	char value[384]; /* matches daemon's CONTAINER_ENV_VALUE_MAX */
};

/* Parses "KEY=VALUE" (e.g. "DEBUG=1") -- whether KEY is a POSIX-safe
 * environment variable name is the daemon's job to validate, not
 * duplicated here, same "presentation-syntax split only" precedent
 * parse_sysctl_flag() above already has. */
static int parse_env_flag(const char *s, struct cli_env *out)
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
	if (value_len >= sizeof(out->value))
		return -1;
	strcpy(out->value, eq + 1);

	return 0;
}

static int cmd_run(const struct cix_client *c, int json_mode, int argc, char **argv)
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
	/* Issue #88: persistent volumes to bind-mount into this container. */
	struct cli_volume_mount volumes[CLI_MAX_VOLUMES];
	struct cli_console_decl consoles[CLI_MAX_CONSOLES];
	int console_count = 0;
	int volume_count = 0;
	const char *interfaces[CLI_MAX_INTERFACES];
	int interface_count = 0;
	const char *cap_add[CLI_MAX_CAP_ADD];
	int cap_add_count = 0;
	const char *ldap_allow_groups[CLI_MAX_LDAP_ALLOW_GROUPS];
	int ldap_allow_group_count = 0;
	const char *restart = NULL;
	long restart_delay = -1;
	int follow_rolling = 0;
	long follow_rolling_jitter = -1;
	const char *depends_on[CLI_MAX_DEPENDS];
	int depends_on_count = 0;
	struct cli_service_decl services[CLI_MAX_SERVICES];
	int service_count = 0;
	int ip_forward = 0;
	int ksm = 0;
	int dns_register = 0;
	int userns = 0;
	int ldap_client = 0;
	int capture_output = 0;
	int pki_issue = 0;
	const char *pki_cert = NULL;
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
	struct cli_env envs[CLI_MAX_ENV];
	int env_count = 0;
	long memory_max = -1;
	/*
	 * Issue #52: -2 is "flag not given", because -1 cannot be -- 0 is
	 * a real value here (this container may not swap) and a negative
	 * one is refused by the daemon, so the sentinel has to sit outside
	 * both.
	 */
	long memory_swap_max = -2;
	long pids_max = -1;
	const char *cpu_max = NULL;
	const char *cpuset_cpus = NULL;
	long long disk_quota_bytes = -1;
	const char *disk = NULL;
	const char *dns_servers[CLI_MAX_DNS_SERVERS];
	int dns_server_count = 0;
	int i = 0;
	struct json_writer w;
	struct cix_response r;

	while (i < argc) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--memory-max=", 13) == 0)
			memory_max = atol(argv[i] + 13);
		else if (strncmp(argv[i], "--memory-swap-max=", 18) == 0)
			memory_swap_max = atol(argv[i] + 18);
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
				fprintf(stderr, "cixctl: too many --network= flags (max %d)\n",
				        CLI_MAX_NETWORKS);
				return 2;
			}
			if (parse_network_flag(argv[i] + 10, &networks[network_count]) != 0) {
				fprintf(stderr, "cixctl: invalid --network= value '%s'\n", argv[i] + 10);
				return 2;
			}
			network_count++;
		} else if (strncmp(argv[i], "--device=", 9) == 0) {
			if (device_count >= CLI_MAX_DEVICES) {
				fprintf(stderr, "cixctl: too many --device= flags (max %d)\n",
				        CLI_MAX_DEVICES);
				return 2;
			}
			devices[device_count++] = argv[i] + 9;
		} else if (strncmp(argv[i], "--optional-device=", 18) == 0) {
			if (optional_device_count >= CLI_MAX_DEVICES) {
				fprintf(stderr, "cixctl: too many --optional-device= flags (max %d)\n",
				        CLI_MAX_DEVICES);
				return 2;
			}
			optional_devices[optional_device_count++] = argv[i] + 18;
		} else if (strncmp(argv[i], "--console=", 10) == 0) {
			if (console_count >= CLI_MAX_CONSOLES) {
				fprintf(stderr, "cixctl: too many --console= flags (max %d)\n",
				        CLI_MAX_CONSOLES);
				return 2;
			}
			if (parse_console_flag(argv[i] + 10, &consoles[console_count]) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --console= value '%s' "
				        "(expected NAME=/absolute/path [args...])\n",
				        argv[i] + 10);
				return 2;
			}
			console_count++;
		} else if (strncmp(argv[i], "--volume=", 9) == 0) {
			if (volume_count >= CLI_MAX_VOLUMES) {
				fprintf(stderr, "cixctl: too many --volume= flags (max %d)\n",
				        CLI_MAX_VOLUMES);
				return 2;
			}
			if (parse_volume_flag(argv[i] + 9, &volumes[volume_count]) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --volume= value '%s' "
				        "(expected NAME:/path or NAME:/path:ro)\n",
				        argv[i] + 9);
				return 2;
			}
			volume_count++;
		} else if (strncmp(argv[i], "--interface=", 12) == 0) {
			if (interface_count >= CLI_MAX_INTERFACES) {
				fprintf(stderr, "cixctl: too many --interface= flags (max %d)\n",
				        CLI_MAX_INTERFACES);
				return 2;
			}
			interfaces[interface_count++] = argv[i] + 12;
		} else if (strncmp(argv[i], "--cap-add=", 10) == 0) {
			if (cap_add_count >= CLI_MAX_CAP_ADD) {
				fprintf(stderr, "cixctl: too many --cap-add= flags (max %d)\n",
				        CLI_MAX_CAP_ADD);
				return 2;
			}
			cap_add[cap_add_count++] = argv[i] + 10;
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
				fprintf(stderr, "cixctl: too many --depends-on= flags (max %d)\n",
				        CLI_MAX_DEPENDS);
				return 2;
			}
			depends_on[depends_on_count++] = argv[i] + 13;
		} else if (strncmp(argv[i], "--service=", 10) == 0 || strncmp(argv[i], "--oneshot=", 10) == 0) {
			if (service_count >= CLI_MAX_SERVICES) {
				fprintf(stderr, "cixctl: too many services (max %d)\n", CLI_MAX_SERVICES);
				return 2;
			}
			if (parse_service_flag(argv[i] + 10, &services[service_count]) != 0) {
				fprintf(stderr, "cixctl: invalid %.9s value '%s' (expected NAME=/absolute/path [args...])\n",
				        argv[i], argv[i] + 10);
				return 2;
			}
			services[service_count].oneshot = strncmp(argv[i], "--oneshot=", 10) == 0;
			service_count++;
		} else if (strncmp(argv[i], "--after=", 8) == 0 || strncmp(argv[i], "--ready=", 8) == 0 ||
		           strncmp(argv[i], "--on-exit=", 10) == 0) {
			/* Attribute flags name the service they belong to; it must be declared first. */
			int is_after = strncmp(argv[i], "--after=", 8) == 0;
			int is_ready = strncmp(argv[i], "--ready=", 8) == 0;
			const char *val = NULL;
			struct cli_service_decl *t = service_attr_target(argv[i] + (is_after || is_ready ? 8 : 10),
			                                                 services, service_count, &val);

			if (t == NULL) {
				fprintf(stderr, "cixctl: %s names a service not declared by an earlier --service=/--oneshot=\n",
				        argv[i]);
				return 2;
			}
			if (is_after) {
				snprintf(t->after, sizeof(t->after), "%s", val);
			} else if (is_ready) {
				if (!ready_spec_ok(val)) {
					fprintf(stderr, "cixctl: --ready= wants tcp:PORT, socket:/path or command:/path [args]\n");
					return 2;
				}
				snprintf(t->ready, sizeof(t->ready), "%s", val);
			} else {
				if (strcmp(val, "restart") != 0 && strcmp(val, "stop") != 0 && strcmp(val, "fail-container") != 0) {
					fprintf(stderr, "cixctl: --on-exit= wants restart, stop or fail-container\n");
					return 2;
				}
				snprintf(t->on_exit, sizeof(t->on_exit), "%s", val);
			}
		} else if (strcmp(argv[i], "--ksm") == 0) {
			/* #260: volunteer this container's memory for samepage
			 * merging. Does nothing unless the host scanner is on --
			 * see cixctl ksm. */
			ksm = 1;
		} else if (strcmp(argv[i], "--ip-forward") == 0) {
			ip_forward = 1;
		} else if (strcmp(argv[i], "--dns-register") == 0) {
			dns_register = 1;
		} else if (strcmp(argv[i], "--userns") == 0) {
			userns = 1;
		} else if (strcmp(argv[i], "--ldap-client") == 0) {
			ldap_client = 1;
		} else if (strncmp(argv[i], "--ldap-allow-group=", 19) == 0) {
			if (ldap_allow_group_count >= CLI_MAX_LDAP_ALLOW_GROUPS) {
				fprintf(stderr, "cixctl: too many --ldap-allow-group= flags (max %d)\n",
				        CLI_MAX_LDAP_ALLOW_GROUPS);
				return 2;
			}
			ldap_allow_groups[ldap_allow_group_count++] = argv[i] + 19;
		} else if (strcmp(argv[i], "--capture-output") == 0) {
			capture_output = 1;
		} else if (strcmp(argv[i], "--pki-issue") == 0) {
			pki_issue = 1;
		} else if (strncmp(argv[i], "--pki-cert-dir=", 15) == 0) {
			pki_cert_dir = argv[i] + 15;
		} else if (strncmp(argv[i], "--pki-cert=", 11) == 0) {
			/* Checked after --pki-cert-dir= purely for readability -- the
			 * two cannot be confused, since they differ at offset 10
			 * ('=' vs '-') and both prefixes are compared in full. */
			pki_cert = argv[i] + 11;
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
				fprintf(stderr, "cixctl: too many --route= flags (max %d)\n",
				        CLI_MAX_ROUTES);
				return 2;
			}
			if (parse_route_flag(argv[i] + 8, &routes[route_count]) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --route= value '%s' (expected DEST/PREFIX:VIA)\n",
				        argv[i] + 8);
				return 2;
			}
			route_count++;
		} else if (strncmp(argv[i], "--file=", 7) == 0) {
			if (file_count >= CLI_MAX_FILES) {
				fprintf(stderr, "cixctl: too many --file= flags (max %d)\n", CLI_MAX_FILES);
				return 2;
			}
			if (parse_file_flag(argv[i] + 7, &files[file_count]) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --file= value '%s' (expected "
				        "CONTAINER_PATH=LOCAL_PATH[:MODE])\n",
				        argv[i] + 7);
				return 2;
			}
			file_count++;
		} else if (strncmp(argv[i], "--file-owner=", 13) == 0) {
			if (file_owner_count >= CLI_MAX_FILES) {
				fprintf(stderr, "cixctl: too many --file-owner= flags (max %d)\n",
				        CLI_MAX_FILES);
				return 2;
			}
			if (parse_file_owner_flag(argv[i] + 13, file_owners[file_owner_count].path,
			                          sizeof(file_owners[file_owner_count].path),
			                          &file_owners[file_owner_count].uid,
			                          &file_owners[file_owner_count].gid) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --file-owner= value '%s' (expected "
				        "CONTAINER_PATH:UID:GID)\n",
				        argv[i] + 13);
				return 2;
			}
			file_owner_count++;
		} else if (strncmp(argv[i], "--sysctl=", 9) == 0) {
			if (sysctl_count >= CLI_MAX_SYSCTLS) {
				fprintf(stderr, "cixctl: too many --sysctl= flags (max %d)\n",
				        CLI_MAX_SYSCTLS);
				return 2;
			}
			if (parse_sysctl_flag(argv[i] + 9, &sysctls[sysctl_count]) != 0) {
				fprintf(stderr,
				        "cixctl: invalid --sysctl= value '%s' (expected KEY=VALUE)\n",
				        argv[i] + 9);
				return 2;
			}
			sysctl_count++;
		} else if (strncmp(argv[i], "--env=", 6) == 0) {
			if (env_count >= CLI_MAX_ENV) {
				fprintf(stderr, "cixctl: too many --env= flags (max %d)\n", CLI_MAX_ENV);
				return 2;
			}
			if (parse_env_flag(argv[i] + 6, &envs[env_count]) != 0) {
				fprintf(stderr, "cixctl: invalid --env= value '%s' (expected KEY=VALUE)\n",
				        argv[i] + 6);
				return 2;
			}
			env_count++;
		} else if (strncmp(argv[i], "--dns-server=", 13) == 0) {
			if (dns_server_count >= CLI_MAX_DNS_SERVERS) {
				fprintf(stderr, "cixctl: too many --dns-server= flags (max %d)\n",
				        CLI_MAX_DNS_SERVERS);
				return 2;
			}
			dns_servers[dns_server_count++] = argv[i] + 13;
		} else {
			fprintf(stderr, "cixctl: unknown run option '%s'\n", argv[i]);
			return 2;
		}
		i++;
	}

	if (name == NULL || image == NULL || service_count == 0) {
		fprintf(stderr,
		        "usage: cixctl run --name=NAME --image=IMAGE [--memory-max=N] "
		        "[--pids-max=N] [--cpu-max=\"QUOTA PERIOD\"] [--cpuset=0-1,3] "
		        "[--disk-quota=BYTES] [--disk=NAME] "
		        "[--network=NAME[:IP] ...] [--ip-forward] [--dns-register] "
		        "[--pki-issue | --pki-cert=NAME] [--pki-cert-dir=PATH] [--pki-days=N] "
		        "[--ldap-provision] [--ldap-user=NAME] [--ldap-group=NAME] "
		        "[--ldap-uid=N] [--ldap-secret-dir=PATH] "
		        "[--route=DEST/PREFIX:VIA ...] [--device=ID ...] [--optional-device=ID ...] "
	        "[--volume=NAME:/path[:ro] ...] [--console=NAME=/path [args] ...] "
		        "[--interface=IFNAME ...] "
		        "[--restart=always|on-failure|unless-stopped] [--restart-delay=N] "
		        "[--follow-rolling] [--follow-rolling-jitter-seconds=N] "
		        "[--depends-on=NAME ...] "
		        "--service=NAME=/path [args] ... [--oneshot=NAME=/path [args] ...] "
		        "[--after=NAME:DEP,DEP ...] [--ready=NAME:tcp:PORT|socket:PATH|command:/path ...] "
		        "[--on-exit=NAME:restart|stop|fail-container ...] "
		        "[--file=CONTAINER_PATH=LOCAL_PATH[:MODE] ...] "
		        "[--file-owner=CONTAINER_PATH:UID:GID ...] [--sysctl=KEY=VALUE ...] "
		        "[--env=KEY=VALUE ...] "
		        "[--dns-server=A.B.C.D ...]\n");
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
			        "cixctl: --file-owner=%s:... has no matching --file=%s=... entry\n",
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
	jw_key(&w, "services");
	jw_arr_open(&w);
	for (i = 0; i < service_count; i++)
		jw_service_decl(&w, &services[i]);
	jw_arr_close(&w);
	if (memory_max >= 0) {
		jw_key(&w, "memory_max");
		jw_int(&w, memory_max);
	}
	if (memory_swap_max >= 0) {
		jw_key(&w, "memory_swap_max");
		jw_int(&w, memory_swap_max);
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
	if (ksm) {
		jw_key(&w, "ksm");
		jw_bool(&w, 1);
	}
	if (ip_forward) {
		jw_key(&w, "ip_forward");
		jw_bool(&w, 1);
	}
	if (dns_register) {
		jw_key(&w, "dns_register");
		jw_bool(&w, 1);
	}
	if (userns) {
		jw_key(&w, "userns");
		jw_bool(&w, 1);
	}
	if (ldap_client) {
		jw_key(&w, "ldap_client");
		jw_bool(&w, 1);
	}
	if (capture_output) {
		jw_key(&w, "capture_output");
		jw_bool(&w, 1);
	}
	if (pki_issue) {
		jw_key(&w, "pki_issue");
		jw_bool(&w, 1);
		if (pki_days >= 0) {
			jw_key(&w, "pki_days");
			jw_int(&w, pki_days);
		}
	}
	/* ADR-0280/#397: deliver an existing, unowned cert instead of issuing
	 * one. Mutually exclusive with --pki-issue, rejected by the daemon
	 * with a 400 -- not re-checked here, so the contract has one place
	 * that decides rather than two that can disagree. */
	if (pki_cert != NULL) {
		jw_key(&w, "pki_cert");
		jw_str(&w, pki_cert);
	}
	/* Applies to whichever of the two is set; pki_days is pki_issue's
	 * alone, since an unowned cert's validity was chosen when the
	 * operator created it. */
	if ((pki_issue || pki_cert != NULL) && pki_cert_dir != NULL) {
		jw_key(&w, "pki_cert_dir");
		jw_str(&w, pki_cert_dir);
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
	if (console_count > 0) {
		jw_key(&w, "consoles");
		jw_arr_open(&w);
		for (i = 0; i < console_count; i++) {
			int a;

			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, consoles[i].name);
			jw_key(&w, "cmd");
			jw_arr_open(&w);
			for (a = 0; a < consoles[i].argc; a++)
				jw_str(&w, consoles[i].argv[a]);
			jw_arr_close(&w);
			jw_obj_close(&w);
		}
		jw_arr_close(&w);
	}
	if (volume_count > 0) {
		jw_key(&w, "volumes");
		jw_arr_open(&w);
		for (i = 0; i < volume_count; i++) {
			jw_obj_open(&w);
			jw_key(&w, "name");
			jw_str(&w, volumes[i].name);
			jw_key(&w, "path");
			jw_str(&w, volumes[i].path);
			if (volumes[i].read_only) {
				jw_key(&w, "read_only");
				jw_bool(&w, 1);
			}
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
	if (cap_add_count > 0) {
		jw_key(&w, "cap_add");
		jw_arr_open(&w);
		for (i = 0; i < cap_add_count; i++)
			jw_str(&w, cap_add[i]);
		jw_arr_close(&w);
	}
	if (ldap_allow_group_count > 0) {
		jw_key(&w, "ldap_allow_groups");
		jw_arr_open(&w);
		for (i = 0; i < ldap_allow_group_count; i++)
			jw_str(&w, ldap_allow_groups[i]);
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
	if (env_count > 0) {
		jw_key(&w, "env");
		jw_obj_open(&w);
		for (i = 0; i < env_count; i++) {
			jw_key(&w, envs[i].key);
			jw_str(&w, envs[i].value);
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
	 * cix_client_request() needs a NUL-terminated C string; w.buf isn't
	 * one, but jw_ensure()'s growth policy always keeps at least one
	 * spare byte of capacity beyond w.len, so writing the NUL directly
	 * here is safe without a further allocation.
	 */
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createContainer_METHOD, CIX_API_createContainer, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_container_line);
}

static int cmd_network_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *subnet = NULL;
	const char *address = NULL;
	const char *alloc_start = NULL, *alloc_end = NULL;
	long prefix_len = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--subnet=", 9) == 0)
			subnet = argv[i] + 9;
		else if (strncmp(argv[i], "--prefix=", 9) == 0)
			prefix_len = atol(argv[i] + 9);
		else if (strncmp(argv[i], "--address=", 10) == 0)
			address = argv[i] + 10;
		else if (strncmp(argv[i], "--alloc-start=", 14) == 0)
			alloc_start = argv[i] + 14;
		else if (strncmp(argv[i], "--alloc-end=", 12) == 0)
			alloc_end = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown network create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || subnet == NULL || prefix_len < 0) {
		fprintf(stderr,
		        "usage: cixctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
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
	if (alloc_start != NULL) {
		jw_key(&w, "alloc_start");
		jw_str(&w, alloc_start);
	}
	if (alloc_end != NULL) {
		jw_key(&w, "alloc_end");
		jw_str(&w, alloc_end);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createNetwork_METHOD, CIX_API_createNetwork, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listNetworks_METHOD, CIX_API_listNetworks, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_network_list);
}

static int cmd_network_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: network rm requires a network name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteNetwork, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_network_attach_interface(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	long vlan_id = 0;
	int i;
	struct json_writer w;
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: network attach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else if (strncmp(argv[i], "--vlan=", 7) == 0)
			vlan_id = atol(argv[i] + 7);
		else {
			fprintf(stderr, "cixctl: unknown network attach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr,
		        "usage: cixctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n");
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

	snprintf(path, sizeof(path), CIX_API_attachNetworkInterface, net_name);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_network_line);
}

static int cmd_network_detach_interface(const struct cix_client *c, int json_mode, int argc,
                                         char **argv)
{
	const char *net_name;
	const char *ifname = NULL;
	int i;
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: network detach-interface requires a network name\n");
		return 2;
	}
	net_name = argv[0];
	for (i = 1; i < argc; i++) {
		if (strncmp(argv[i], "--interface=", 12) == 0)
			ifname = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown network detach-interface option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (ifname == NULL) {
		fprintf(stderr, "usage: cixctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}

	snprintf(path, sizeof(path), CIX_API_detachNetworkInterface, net_name, ifname);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

/*
 * Issue #26: a network's ports, the way a switch panel shows them.
 * The list is the kernel's -- everything actually on the bridge -- so a
 * port nothing can account for prints as "unattributed" rather than
 * being quietly left out, which is the one line here worth noticing.
 */
static void fmt_network_ports(const struct json_value *v)
{
	const struct json_value *ports = json_object_get(v, "ports");
	const struct json_value *present = json_object_get(v, "bridge_present");
	size_t i;

	if (present != NULL && present->type == JSON_BOOL && !present->u.boolean) {
		printf("no bridge for this network on this host -- nothing is plugged in\n");
		return;
	}
	if (ports == NULL || ports->type != JSON_ARRAY || ports->u.array.count == 0) {
		printf("no ports -- the bridge exists but nothing is on it\n");
		return;
	}
	printf("%-3s %-16s %-14s %-18s %-6s %14s %14s\n", "#", "PORT", "KIND", "ATTACHED", "LINK",
	       "RX BYTES", "TX BYTES");
	for (i = 0; i < ports->u.array.count; i++) {
		const struct json_value *p = ports->u.array.items[i];
		const char *container = json_str_field(p, "container");
		const char *ip = json_str_field(p, "ip");
		const struct json_value *vlan = json_object_get(p, "vlan_id");
		char attached[64];

		if (container != NULL)
			snprintf(attached, sizeof(attached), "%s %s", container, ip != NULL ? ip : "");
		else if (vlan != NULL && vlan->type == JSON_NUMBER && (int)json_as_number(vlan) != 0)
			snprintf(attached, sizeof(attached), "vlan %d", (int)json_as_number(vlan));
		else
			snprintf(attached, sizeof(attached), "%s", "-");
		/*
		 * The number is this listing's own, positional: ports come and
		 * go with containers, so nothing here is a stable port
		 * identity. The interface name is.
		 */
		printf("%-3d %-16s %-14s %-18s %-6s %14lld %14lld\n", (int)i + 1,
		       json_str_field(p, "ifname") != NULL ? json_str_field(p, "ifname") : "-",
		       json_str_field(p, "kind") != NULL ? json_str_field(p, "kind") : "-", attached,
		       json_str_field(p, "link") != NULL ? json_str_field(p, "link") : "?",
		       (long long)json_as_number(json_object_get(p, "rx_bytes")),
		       (long long)json_as_number(json_object_get(p, "tx_bytes")));
	}
	printf("\nrx/tx are from the port's own side: rx is what reached the switch from\n"
	       "whatever is plugged in, which is the inverse of what a container sees.\n");
}

static int cmd_network_ports(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl network ports NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getNetworkPorts, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_network_ports);
}

/*
 * cixctl network set-pool NAME [--alloc-start=IP] [--alloc-end=IP]
 * (issue #137)
 *
 * The window used to be settable only at `network create`, which put
 * it out of reach on a bridged management network -- that one cannot
 * be deleted while it is the management network, so there was no
 * recreate-with-a-pool route.
 *
 * "none" clears a bound. Clearing both on a bridged network returns it
 * to refusing auto-allocation, which is the safe state rather than a
 * regression.
 */
static int cmd_network_set_pool(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL, *start = NULL, *end = NULL;
	struct json_writer w;
	struct cix_response r;
	char path[256];
	int i, rc, any = 0;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--alloc-start=", 14) == 0) {
			start = argv[i] + 14;
			any = 1;
		} else if (strncmp(argv[i], "--alloc-end=", 12) == 0) {
			end = argv[i] + 12;
			any = 1;
		} else if (name == NULL) {
			name = argv[i];
		} else {
			fprintf(stderr, "cixctl: unknown network set-pool option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || !any) {
		fprintf(stderr,
		        "usage: cixctl network set-pool NAME [--alloc-start=IP] [--alloc-end=IP]\n"
		        "       use 'none' to clear a bound\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (start != NULL) {
		jw_key(&w, "alloc_start");
		if (strcmp(start, "none") == 0)
			jw_null(&w);
		else
			jw_str(&w, start);
	}
	if (end != NULL) {
		jw_key(&w, "alloc_end");
		if (strcmp(end, "none") == 0)
			jw_null(&w);
		else
			jw_str(&w, end);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_updateNetwork, name);
	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, CIX_API_updateNetwork_METHOD, path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	rc = emit(&r, json_mode, fmt_network_line);
	return rc;
}

/*
 * cixctl pkg buildenv ls | rm NAME (ADR-0221 / issue #112)
 *
 * Reported apart from `pkg cache-status` deliberately: pruning the
 * cache makes a build slower, reclaiming an environment makes a build
 * recompose one. Those look identical in a size figure and are not the
 * same thing.
 */
static void fmt_buildenvs(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "buildenvs");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("no composed build environments\n");
		return;
	}
	printf("%-28s %-12s %s\n", "NAME", "IDLE", "STATE");
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *name = json_str_field(e, "name");
		long long idle = (long long)json_as_number(json_object_get(e, "idle_seconds"));
		long long keep = (long long)json_as_number(json_object_get(e, "retention_seconds"));
		const struct json_value *iu = json_object_get(e, "in_use");
		int in_use = iu != NULL && iu->type == JSON_BOOL && iu->u.boolean;
		char idle_str[32];

		if (idle >= 86400)
			snprintf(idle_str, sizeof(idle_str), "%lldd", idle / 86400);
		else if (idle >= 3600)
			snprintf(idle_str, sizeof(idle_str), "%lldh", idle / 3600);
		else
			snprintf(idle_str, sizeof(idle_str), "%llds", idle);

		printf("%-28s %-12s %s\n", name != NULL ? name : "?", idle_str,
		       in_use ? "in use (never reclaimed while building)"
		              : (keep > 0 && idle >= keep) ? "due for reclamation at next daemon start"
		                                           : "idle");
	}
}

static int cmd_pkg_buildenv(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc >= 1 && strcmp(argv[0], "rm") == 0) {
		if (argc < 2) {
			fprintf(stderr, "usage: cixctl pkg buildenv rm NAME\n");
			return 2;
		}
		snprintf(path, sizeof(path), CIX_API_deleteBuildEnvironment, argv[1]);
		memset(&r, 0, sizeof(r));
		if (cix_client_request(c, CIX_API_deleteBuildEnvironment_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status == 204) {
			printf("reclaimed %s -- it will be recomposed automatically if a build wants it\n",
			       argv[1]);
			cix_response_free(&r);
			return 0;
		}
		return emit(&r, json_mode, NULL);
	}
	if (argc >= 1 && strcmp(argv[0], "ls") != 0) {
		fprintf(stderr, "usage: cixctl pkg buildenv [ls]\n"
		                "       cixctl pkg buildenv rm NAME\n");
		return 2;
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, CIX_API_listBuildEnvironments_METHOD, CIX_API_listBuildEnvironments,
	                       NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_buildenvs);
}

static int cmd_network(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl network create --name=NAME --subnet=A.B.C.D --prefix=N "
		        "[--address=A.B.C.D]\n"
		        "       cixctl network ls\n"
		        "       cixctl network set-pool NAME [--alloc-start=IP] [--alloc-end=IP]\n"
		        "       cixctl network rm NAME\n"
	        "       cixctl network ports NAME  -- what is plugged into this network's\n"
	        "                                       bridge right now, and each port's traffic\n"
		        "       cixctl network attach-interface NAME --interface=IFNAME [--vlan=N]\n"
		        "       cixctl network detach-interface NAME --interface=IFNAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_network_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_network_ls(c, json_mode);
	if (strcmp(sub, "set-pool") == 0)
		return cmd_network_set_pool(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_network_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ports") == 0)
		return cmd_network_ports(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "attach-interface") == 0)
		return cmd_network_attach_interface(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "detach-interface") == 0)
		return cmd_network_detach_interface(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown network subcommand '%s'\n", sub);
	return 2;
}

static int cmd_image_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown image create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: cixctl image create --name=NAME\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createImage_METHOD, CIX_API_createImage, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_image_line);
}

static int cmd_image_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listImages_METHOD, CIX_API_listImages, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_list);
}

static int cmd_image_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: image rm requires an image name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteImage, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image_show(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl image show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getImage, argv[0]);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_detail);
}

/* ADR-0107: declares package intent on an image -- does not itself
 * trigger a rebuild (task #720's own job). */
static int cmd_image_manifest_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *package = NULL;
	const char *mode = NULL;
	const char *version = NULL;
	int i;
	char path[256];
	struct json_writer w;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown image manifest set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL || package == NULL || mode == NULL || version == NULL) {
		fprintf(stderr,
		        "usage: cixctl image manifest set --image=NAME --package=NAME "
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

	snprintf(path, sizeof(path), CIX_API_setImageManifestEntry, image);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("%s@%s (%s) set on image '%s'\n", package, version, mode, image);
	cix_response_free(&r);
	return 0;
}

static int cmd_image_manifest_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *image = NULL;
	const char *package = NULL;
	int i;
	char path[300];
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--package=", 10) == 0)
			package = argv[i] + 10;
		else {
			fprintf(stderr, "cixctl: unknown image manifest rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL || package == NULL) {
		fprintf(stderr, "usage: cixctl image manifest rm --image=NAME --package=NAME\n");
		return 2;
	}

	snprintf(path, sizeof(path), CIX_API_unsetImageManifestEntry, image, package);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static void fmt_version_manifest(const struct json_value *v)
{
	const struct json_value *manifest = json_object_get(v, "manifest");
	size_t i;

	printf("%s @ %s\n", json_str_field(v, "name"), json_str_field(v, "version"));
	if (manifest == NULL || manifest->type != JSON_ARRAY || manifest->u.array.count == 0) {
		printf("  (this version holds no packages)\n");
		return;
	}
	for (i = 0; i < manifest->u.array.count; i++) {
		const struct json_value *e = manifest->u.array.items[i];

		printf("  %-24s %s\n", json_str_field(e, "package"), json_str_field(e, "version"));
	}
}

/*
 * #398: what one specific image VERSION holds -- the name@version pairs
 * installed into it, snapshotted when it was produced. A container runs
 * a pinned version and keeps running it, so this is the only honest
 * answer to "what is in that container". `image show` reports the
 * declared manifest, which is live intent and whose version field is a
 * floor for a rolling entry rather than a fact.
 */
static int cmd_image_manifest_show(const struct cix_client *c, int json_mode, int argc,
                                    char **argv)
{
	const char *image = NULL;
	const char *version = NULL;
	int i;
	char path[300];
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else {
			fprintf(stderr, "cixctl: unknown image manifest show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image == NULL || version == NULL) {
		fprintf(stderr, "usage: cixctl image manifest show --image=NAME --version=VERSION\n");
		return 2;
	}

	snprintf(path, sizeof(path), CIX_API_getImageVersionManifest, image, version);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_version_manifest);
}

static int cmd_image_manifest(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl image manifest set --image=NAME --package=NAME "
		        "--mode=pinned|rolling --version=VERSION\n"
		        "       cixctl image manifest rm --image=NAME --package=NAME\n"
		        "       cixctl image manifest show --image=NAME --version=VERSION\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "set") == 0)
		return cmd_image_manifest_set(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_image_manifest_rm(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_image_manifest_show(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown image manifest subcommand '%s'\n", sub);
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

static int cmd_image_recipe_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listImageRecipes_METHOD, CIX_API_listImageRecipes, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_recipe_list);
}

/* Same shape as `pkg recipe add` -- --name= is the image name this
 * recipe declares intent for (recipe name == image name, a 1:1
 * relationship, ADR-0123), --file= a local path to the recipe text. */
static int cmd_image_recipe_add(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown image recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: cixctl image recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "cixctl: could not read %s\n", file);
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

	if (cix_client_request(c, CIX_API_addImageRecipe_METHOD, CIX_API_addImageRecipe, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("image recipe '%s' added\n", name);
	cix_response_free(&r);
	return 0;
}

static void fmt_image_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

static int cmd_image_recipe_show(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown image recipe show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl image recipe show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getImageRecipe, name);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_recipe_show);
}

static int cmd_image_recipe_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown image recipe rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl image recipe rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteImageRecipe, name);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_image_recipe(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl image recipe add --name=NAME --file=PATH\n"
		                "       cixctl image recipe show NAME\n"
		                "       cixctl image recipe rm NAME\n"
		                "       cixctl image recipe ls\n");
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

	fprintf(stderr, "cixctl: unknown image recipe subcommand '%s'\n", sub);
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
static void fmt_image_gc(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "reclaimed");
	long long total = json_as_number(json_object_get(v, "apparent_bytes_total"));
	long long kept = json_as_number(json_object_get(v, "kept"));
	long long failed = json_as_number(json_object_get(v, "failed"));
	int dry = 0;
	size_t i;

	{
		const struct json_value *d = json_object_get(v, "dry_run");

		dry = d != NULL && d->type == JSON_BOOL && d->u.boolean;
	}
	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count; i++) {
			const struct json_value *e = arr->u.array.items[i];
			const char *img = json_str_field(e, "image");
			const char *ver = json_str_field(e, "version");
			/* Bound to a long long before printing: json_as_number()
			 * returns a double, and handing that straight to %lld is
			 * undefined -- it printed garbage per entry while the
			 * total (which went through a variable) was correct. */
			const struct json_value *b = json_object_get(e, "apparent_bytes");

			if (b != NULL && b->type == JSON_NUMBER)
				printf("%s %s@%.12s  %lld bytes\n", dry ? "would remove" : "removed",
				       img != NULL ? img : "?", ver != NULL ? ver : "?",
				       (long long)json_as_number(b));
			else
				printf("%s %s@%.12s\n", dry ? "would remove" : "removed",
				       img != NULL ? img : "?", ver != NULL ? ver : "?");
		}
	}
	/* "apparent" is not hedging: on btrfs a version is a snapshot
	 * sharing extents with its neighbours, so the space actually
	 * returned is typically well below this. Saying so beats printing
	 * a number the operator will later find was wrong. */
	printf("%s %lld version(s)", dry ? "would reclaim" : "reclaimed",
	       arr != NULL && arr->type == JSON_ARRAY ? (long long)arr->u.array.count : 0);
	{
		const struct json_value *t = json_object_get(v, "apparent_bytes_total");

		if (t != NULL && t->type == JSON_NUMBER)
			printf(", %lld apparent bytes", total);
	}
	printf("; kept %lld", kept);
	if (failed > 0)
		printf(", %lld could not be removed (see the log store)", failed);
	printf("\n");
}

static int cmd_image_gc(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char body[64];
	int dry_run = 0;
	int measure = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--dry-run") == 0)
			dry_run = 1;
		else if (strcmp(argv[i], "--measure") == 0)
			measure = 1;
		else {
			fprintf(stderr, "cixctl: unknown image gc option '%s'\n", argv[i]);
			return 2;
		}
	}
	/* --measure walks every collectable version to size it, and the
	 * daemon is single-threaded -- on a box with 80 of them that walk
	 * took nearly two minutes, blocking every other request. Off by
	 * default for that reason, not to hide the number. */
	snprintf(body, sizeof(body), "{\"dry_run\":%s,\"measure\":%s}",
	         dry_run ? "true" : "false", measure ? "true" : "false");
	if (cix_client_request(c, CIX_API_imagesGc_METHOD, CIX_API_imagesGc, body, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_image_gc);
}

static int cmd_image_apply_recipe(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown image apply-recipe option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl image apply-recipe NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_applyImageRecipe, name);
	if (cix_client_request(c, "POST", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("recipe applied for '%s'\n", name);
	cix_response_free(&r);
	return 0;
}

/* ---- ADR-0151: deployments (#371 -- were "container recipes") ---- */

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

static int cmd_container_recipe_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listDeployments_METHOD, CIX_API_listDeployments, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_recipe_list);
}

/* --name= is the recipe's own name AND must match the "name" field
 * already inside --file='s own content (ADR-0151, mirroring
 * pkg_recipe_add()'s pkg_name= contract) -- unlike an image recipe,
 * a deployment's content is a real POST /v1/containers body, so
 * it necessarily already carries its own name. */
static int cmd_container_recipe_add(const struct cix_client *c, int json_mode, int argc,
                                     char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown deployment add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: cixctl deployment add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "cixctl: could not read %s\n", file);
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

	if (cix_client_request(c, CIX_API_addDeployment_METHOD, CIX_API_addDeployment, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("deployment '%s' added\n", name);
	cix_response_free(&r);
	return 0;
}

static void fmt_container_recipe_show(const struct json_value *v)
{
	const char *content = json_str_field(v, "content");

	printf("%s", content != NULL ? content : "");
}

static int cmd_container_recipe_show(const struct cix_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct cix_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown deployment show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl deployment show NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_getDeployment, name);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_recipe_show);
}

static int cmd_container_recipe_rm(const struct cix_client *c, int json_mode, int argc,
                                    char **argv)
{
	struct cix_response r;
	char path[256];
	const char *name = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown deployment rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl deployment rm NAME\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteDeployment, name);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_container_recipe(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl deployment add --name=NAME --file=PATH\n"
		                "       cixctl deployment show NAME\n"
		                "       cixctl deployment rm NAME\n"
		                "       cixctl deployment ls\n");
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

	fprintf(stderr, "cixctl: unknown deployment subcommand '%s'\n", sub);
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
static int cmd_container_apply_recipe(const struct cix_client *c, int json_mode, int argc,
                                       char **argv)
{
	struct cix_response r;
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
				        "cixctl: invalid --secret= value '%s' (expected KEY=VALUE)\n",
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
			fprintf(stderr, "cixctl: unknown deployment apply option '%s'\n", argv[i]);
			jw_free(&w);
			return 2;
		}
	}
	jw_obj_close(&w);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (name == NULL) {
		fprintf(stderr,
		        "usage: cixctl deployment apply NAME [--secret=KEY=VALUE ...]\n");
		jw_free(&w);
		return 2;
	}

	snprintf(path, sizeof(path), CIX_API_applyDeployment, name);
	if (cix_client_request(c, "POST", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	return emit(&r, json_mode, fmt_container_line);
}

/*
 * image materialize -- turn an image recipe into a real, populated image
 * in one operator action (#141).
 *
 * A freshly installed box cannot build anything until it has a build
 * image, and until now the only ways to get one were to hand the daemon
 * a 1.4 GB squashfs by local path (impossible: a real install has no
 * shell and no SSH) or by URL (an operator standing up an HTTP server),
 * or to let `pkg bootstrap` copy the host's own /usr -- which is how a
 * developer workstation's rustup, cargo, chromium and node ended up
 * inside cix-builder (#168, ADR-0225).
 *
 * None of that is necessary any more, and the reason is in pkg.c: an
 * artifact-tier install verifies the tarball against the recipe's own
 * checksum and stages it directly, WITHOUT composing a build sandbox.
 * A host with no compiler can therefore install any approved package.
 * Measured on a fresh image: 26 of the toolchain's 27 packages
 * installed with no compilation at all, gcc included, in under thirty
 * seconds.
 *
 * So the capability already existed and only the shape was missing: it
 * took twenty-seven separate calls. This is deliberately CLIENT-side --
 * every endpoint it uses already exists, so it needs no daemon change,
 * no A/B slot write and no reboot, and it keeps the API-First rule
 * exactly as it stands (the CLI is a pure REST client and holds no
 * logic of its own beyond sequencing).
 */
static int materialize_one(const struct cix_client *c, const char *image, const char *pkg,
                           int *out_compiled)
{
	struct cix_response r;
	struct json_writer w;
	int settled = 0;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, pkg);
	jw_key(&w, "image");
	jw_str(&w, image);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, CIX_API_pkgInstall_METHOD, CIX_API_pkgInstall, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return -1;
	}
	jw_free(&w);
	if (r.status != 200 && r.status != 202) {
		const char *msg = json_str_field(r.json, "error");

		/*
		 * Already installed is success, not refusal. Packages arrive
		 * as each other's build dependencies -- installing gcc brings
		 * binutils, m4, flex and zlib with it -- so by the time this
		 * loop reaches them the daemon rightly says 409. Reporting
		 * that as a failure made a run that worked perfectly announce
		 * "5 of 27 package(s) did not install".
		 */
		if (r.status == 409 && msg != NULL && strstr(msg, "already installed") != NULL) {
			cix_response_free(&r);
			if (out_compiled != NULL)
				*out_compiled = 0;
			return 1; /* present, nothing to wait for */
		}
		fprintf(stderr, "  %-18s REFUSED (HTTP %d)%s%s\n", pkg, r.status,
		        msg != NULL ? " -- " : "", msg != NULL ? msg : "");
		cix_response_free(&r);
		return -1;
	}
	cix_response_free(&r);

	/*
	 * Polled through the package LIST, not GET /pkg/{name}.
	 *
	 * Two gates shaped this, in order. The first version built
	 * "/v1/pkg/%s@%s" by hand, which test_api_surfaces refused
	 * (ADR-0218): a hand-typed path compiles, works, and breaks
	 * silently the day the contract moves. Reaching for the generated
	 * CIX_API_getPkg constant instead was then refused too, for a
	 * better reason -- that operation declares `x-cix-expose: []`, so
	 * the contract exposes it to no channel at all, and "the API
	 * decides which channel offers a capability, not the channel".
	 *
	 * listPkg is exposed to cli and returns every package with its
	 * state, so the answer is here; it just has to be looked up rather
	 * than addressed. Slightly more data per poll, and the right shape.
	 */
	for (;;) {
		const struct json_value *arr;
		const struct json_value *entry = NULL;
		const char *state = NULL;
		size_t k;

		usleep(500000);
		if (cix_client_request(c, CIX_API_listPkg_METHOD, CIX_API_listPkg, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return -1;
		}
		arr = json_object_get(r.json, "packages");
		if (arr != NULL && arr->type == JSON_ARRAY) {
			for (k = 0; k < arr->u.array.count; k++) {
				const struct json_value *it = arr->u.array.items[k];
				const char *n = json_str_field(it, "name");
				const char *im = json_str_field(it, "image");

				if (n != NULL && im != NULL && strcmp(n, pkg) == 0 &&
				    strcmp(im, image) == 0) {
					entry = it;
					break;
				}
			}
		}
		if (entry != NULL)
			state = json_str_field(entry, "state");
		if (state != NULL && strcmp(state, "installed") == 0) {
			settled = 1;
			break;
		}
		if (state != NULL && strcmp(state, "failed") == 0) {
			const char *err = json_str_field(entry, "error");

			fprintf(stderr, "  %-18s FAILED%s%s\n", pkg, err != NULL ? " -- " : "",
			        err != NULL ? err : "");
			cix_response_free(&r);
			return -1;
		}
		/* A package that COMPILED wrote a build log; one that came
		 * from an artifact did not. Reported because "did this box
		 * need a compiler" is the whole question a fresh install is
		 * asking, and artifact_cached does not answer it -- that
		 * field means "these bytes are in the local cache", which is
		 * true of anything built here too. */
		cix_response_free(&r);
	}
	(void)settled;
	(void)out_compiled;
	return 0;
}

/* image_packages="name:mode:version name2:..." -- the only line this
 * needs from the recipe. Written into names[], one bare package name
 * per entry; mode and version are the daemon's business, not this
 * loop's. */
static int parse_image_packages(const char *content, char names[][128], int max)
{
	const char *p = strstr(content, "image_packages=\"");
	const char *end;
	int n = 0;

	if (p == NULL)
		return 0;
	p += strlen("image_packages=\"");
	end = strchr(p, '"');
	if (end == NULL)
		return 0;
	while (p < end && n < max) {
		const char *tok;
		size_t len;

		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\\'))
			p++;
		if (p >= end)
			break;
		tok = p;
		while (p < end && *p != ' ' && *p != '\t' && *p != '\n')
			p++;
		len = (size_t)(p - tok);
		{
			const char *colon = memchr(tok, ':', len);

			if (colon != NULL)
				len = (size_t)(colon - tok);
		}
		if (len == 0 || len >= 128)
			continue;
		memcpy(names[n], tok, len);
		names[n][len] = '\0';
		n++;
	}
	return n;
}

static int cmd_image_materialize(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	char path[512];
	char names[128][128];
	struct cix_response r;
	const char *image;
	const char *content;
	int count, i, failed = 0;

	(void)json_mode;
	if (argc < 1) {
		fprintf(stderr, "usage: cixctl image materialize NAME\n"
		                "  Creates the image if absent, declares its manifest from the\n"
		                "  image recipe of the same name, and installs every package in\n"
		                "  it -- the one action a freshly installed box needs to reach a\n"
		                "  working build image (#141).\n");
		return 2;
	}
	image = argv[0];

	snprintf(path, sizeof(path), CIX_API_getImageRecipe, image);
	if (cix_client_request(c, CIX_API_getImageRecipe_METHOD, path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 200) {
		fprintf(stderr, "cixctl: no image recipe named '%s' (HTTP %d) -- add one with "
		                "`cixctl image recipe add --name=%s --file=...`\n",
		        image, r.status, image);
		cix_response_free(&r);
		return 1;
	}
	content = json_str_field(r.json, "content");
	count = content != NULL ? parse_image_packages(content, names, 128) : 0;
	cix_response_free(&r);
	if (count == 0) {
		fprintf(stderr, "cixctl: recipe '%s' declares no image_packages\n", image);
		return 1;
	}

	/* Creating the image is allowed to fail: it may already exist, and
	 * this whole command is meant to be safe to re-run. */
	{
		struct json_writer w;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "name");
		jw_str(&w, image);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		if (cix_client_request(c, CIX_API_createImage_METHOD, CIX_API_createImage,
		                       w.buf, &r) == 0)
			cix_response_free(&r);
		jw_free(&w);
	}

	snprintf(path, sizeof(path), CIX_API_applyImageRecipe, image);
	if (cix_client_request(c, CIX_API_applyImageRecipe_METHOD, path, "{}", &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 200 && r.status != 204) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: could not apply recipe '%s' (HTTP %d)%s%s\n", image,
		        r.status, msg != NULL ? " -- " : "", msg != NULL ? msg : "");
		cix_response_free(&r);
		return 1;
	}
	cix_response_free(&r);

	printf("%s: %d package%s declared, installing\n", image, count, count == 1 ? "" : "s");
	for (i = 0; i < count; i++) {
		int rc = materialize_one(c, image, names[i], NULL);

		if (rc < 0) {
			failed++;
			continue;
		}
		printf("  %-18s %s\n", names[i],
		       rc == 1 ? "already present (pulled in as a dependency)" : "installed");
	}
	if (failed > 0) {
		fprintf(stderr, "%s: %d of %d package(s) did not install\n", image, failed, count);
		return 1;
	}
	printf("%s: ready (%d packages)\n", image, count);
	return 0;
}

static int cmd_image(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl image create --name=NAME\n"
		                "       cixctl image ls\n"
		                "       cixctl image show NAME\n"
		                "       cixctl image rm NAME\n"
		                "       cixctl image manifest set --image=NAME --package=NAME "
		                "--mode=pinned|rolling --version=VERSION\n"
		                "       cixctl image manifest rm --image=NAME --package=NAME\n"
		                "       cixctl image recipe add --name=NAME --file=PATH\n"
		                "       cixctl image recipe show|rm NAME\n"
		                "       cixctl image recipe ls\n"
		                "       cixctl image apply-recipe NAME\n"
		                "       cixctl image materialize NAME\n"
		                "       cixctl image gc [--dry-run] [--measure]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "materialize") == 0)
		return cmd_image_materialize(c, json_mode, argc - 1, argv + 1);
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
	if (strcmp(sub, "gc") == 0)
		return cmd_image_gc(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown image subcommand '%s'\n", sub);
	return 2;
}

static int cmd_device_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listDevices_METHOD, CIX_API_listDevices, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_device_list);
}

static int cmd_device(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl device ls\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "ls") == 0)
		return cmd_device_ls(c, json_mode);

	fprintf(stderr, "cixctl: unknown device subcommand '%s'\n", sub);
	return 2;
}

static int cmd_diskrole_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk_name = NULL;
	const char *role = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--disk=", 7) == 0)
			disk_name = argv[i] + 7;
		else if (strncmp(argv[i], "--role=", 7) == 0)
			role = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown diskrole create option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (disk_name == NULL || role == NULL) {
		fprintf(stderr,
		        "usage: cixctl storage-role create --disk=NAME --role=container-storage|backup|state-storage|rebuildable-storage|log-storage|swap\n");
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

	if (cix_client_request(c, CIX_API_createStorageRole_METHOD, CIX_API_createStorageRole, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_diskrole_line);
}

static int cmd_diskrole_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listStorageRoles_METHOD, CIX_API_listStorageRoles, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_diskrole_list);
}

static int cmd_diskrole_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: diskrole rm requires a disk name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteStorageRole, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_diskrole(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl storage-role create --disk=NAME --role=container-storage|backup|state-storage|rebuildable-storage|log-storage|swap\n"
		                "       cixctl storage-role ls\n"
		                "       cixctl storage-role rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_diskrole_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_diskrole_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_diskrole_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown diskrole subcommand '%s'\n", sub);
	return 2;
}

static int cmd_devicemap_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *kind = NULL;
	const char *selector = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--kind=", 7) == 0)
			kind = argv[i] + 7;
		else if (strncmp(argv[i], "--selector=", 11) == 0)
			selector = argv[i] + 11;
		else {
			fprintf(stderr, "cixctl: unknown devicemap create option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || kind == NULL || selector == NULL) {
		fprintf(stderr,
		        "usage: cixctl devicemap create --name=NAME --kind=exact|vendor_model "
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

	if (cix_client_request(c, CIX_API_createDeviceMap_METHOD, CIX_API_createDeviceMap, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_devicemap_line);
}

static int cmd_devicemap_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listDeviceMaps_METHOD, CIX_API_listDeviceMaps, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_devicemap_list);
}

static int cmd_devicemap_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: devicemap rm requires a mapping name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteDeviceMap, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_devicemap(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl devicemap create --name=NAME --kind=exact|vendor_model "
		        "--selector=SELECTOR\n"
		        "       cixctl devicemap ls\n"
		        "       cixctl devicemap rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_devicemap_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_devicemap_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_devicemap_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown devicemap subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_record_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *ip = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else {
			fprintf(stderr, "cixctl: unknown dns record create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || ip == NULL) {
		fprintf(stderr, "usage: cixctl dns record create --name=NAME --ip=A.B.C.D\n");
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

	if (cix_client_request(c, CIX_API_createDnsRecord_METHOD, CIX_API_createDnsRecord, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_record_line);
}

static int cmd_dns_record_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listDnsRecords_METHOD, CIX_API_listDnsRecords, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_record_list);
}

static int cmd_dns_record_update(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *ip = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;
	char path[256];

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--ip=", 5) == 0)
			ip = argv[i] + 5;
		else {
			fprintf(stderr, "cixctl: unknown dns record update option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || ip == NULL) {
		fprintf(stderr, "usage: cixctl dns record update --name=NAME --ip=A.B.C.D\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "ip");
	jw_str(&w, ip);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_updateDnsRecord, name);
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_record_line);
}

static int cmd_dns_record_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: dns record rm requires a record name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteDnsRecord, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_record(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl dns record create --name=NAME --ip=A.B.C.D\n"
		        "       cixctl dns record update --name=NAME --ip=A.B.C.D\n"
		        "       cixctl dns record ls\n"
		        "       cixctl dns record rm NAME\n");
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

	fprintf(stderr, "cixctl: unknown dns record subcommand '%s'\n", sub);
	return 2;
}

static int cmd_dns_server_register(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	const char *hosts_path = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--hosts-path=", 13) == 0)
			hosts_path = argv[i] + 13;
		else {
			fprintf(stderr, "cixctl: unknown dns server register option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (container == NULL || hosts_path == NULL) {
		fprintf(stderr,
		        "usage: cixctl dns server register --container=NAME --hosts-path=PATH\n");
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

	if (cix_client_request(c, CIX_API_createDnsServer_METHOD, CIX_API_createDnsServer, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_dns_server_line);
}

static int cmd_dns_server_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listDnsServers_METHOD, CIX_API_listDnsServers, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_dns_server_list);
}

static int cmd_dns_server_unregister(const struct cix_client *c, int json_mode, int argc,
                                      char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: dns server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteDnsServer, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_dns_server(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl dns server register --container=NAME --hosts-path=PATH\n"
		        "       cixctl dns server ls\n"
		        "       cixctl dns server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_dns_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_dns_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_dns_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown dns server subcommand '%s'\n", sub);
	return 2;
}

/*
 * Issue #134: cixctl dns forwarders show|set --forwarder=IP ...
 * What this platform resolves upstream through -- daemon-owned and
 * applied live to every registered DNS server, rather than frozen into
 * each container's command line where the two replicas could disagree
 * and nothing could report the answer.
 */
static void fmt_dns_forwarders(const struct json_value *v)
{
	const struct json_value *a = json_object_get(v, "forwarders");
	size_t i;

	if (a == NULL || a->type != JSON_ARRAY || a->u.array.count == 0) {
		printf("no upstream forwarders configured (DNS is authoritative-only)\n");
		return;
	}
	for (i = 0; i < a->u.array.count; i++)
		printf("%s\n", json_as_string(a->u.array.items[i]));
}

static int cmd_dns_forwarders(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "show";

	if (strcmp(sub, "show") == 0) {
		if (cix_client_request(c, CIX_API_getDnsForwarders_METHOD, CIX_API_getDnsForwarders, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_dns_forwarders);
	}
	if (strcmp(sub, "set") == 0) {
		struct json_writer w;
		int i;

		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "forwarders");
		jw_arr_open(&w);
		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--forwarder=", 12) == 0) {
				jw_str(&w, argv[i] + 12);
			} else {
				fprintf(stderr, "cixctl: unknown dns forwarders set option '%s'\n", argv[i]);
				jw_free(&w);
				return 2;
			}
		}
		jw_arr_close(&w);
		jw_obj_close(&w);
		w.buf[w.len] = '\0';
		if (cix_client_request(c, CIX_API_setDnsForwarders_METHOD, CIX_API_setDnsForwarders, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		return emit(&r, json_mode, fmt_dns_forwarders);
	}
	fprintf(stderr, "usage: cixctl dns forwarders show\n"
	                "       cixctl dns forwarders set [--forwarder=IP ...]  (no flags clears them)\n");
	return 2;
}

/*
 * Issue #136: DNS in one action.
 *
 * Standing this up by hand takes seven steps, one of which is a
 * twelve-argument dnsmasq command line whose every flag encodes a past
 * failure (-u root because a minimal image has no /etc/passwd; -R/-h
 * because it has no resolv.conf or hosts; --pid-file= because it has no
 * /var/run, and without that dnsmasq crash-loops silently). Expecting
 * an operator to know all of that -- or to copy it correctly out of
 * prose documentation -- is not a product.
 *
 * The knowledge belongs in the deployments, which already carry
 * it (recipes/deployment/dns-1, dns-2), pinned addresses included. This
 * command is the missing ACTION that applies them: it does not
 * reimplement any of it, it drives the recipes and then registers the
 * result, reporting each step as it goes.
 *
 * Idempotent: re-running it against an already-provisioned box
 * re-applies the recipes and re-registers, which is exactly what an
 * operator wants after editing one.
 */
/*
 * Thin client over POST /v1/dns/provision.
 *
 * This used to orchestrate the whole workflow here -- apply each
 * recipe, register each server, read back the addresses, set the host
 * resolver -- four REST calls driven from the CLI. That is exactly what
 * the API-First Mandate forbids: a CLI feature with no endpoint behind
 * it, which the dashboard could only match by writing the same sequence
 * again in JavaScript. The sequence is the product, so it belongs to
 * the daemon; what is left here is argument parsing and printing.
 */
static void fmt_dns_provision(const struct cix_response *r)
{
	const struct json_value *reps;
	const struct json_value *resolver;
	long problems;
	size_t i;

	if (r->json == NULL) {
		printf("dns provision: no response body\n");
		return;
	}
	reps = json_object_get(r->json, "replicas");
	if (reps != NULL && reps->type == JSON_ARRAY) {
		for (i = 0; i < reps->u.array.count; i++) {
			const struct json_value *rep = reps->u.array.items[i];
			const char *name = json_str_field(rep, "name");
			const char *created = json_str_field(rep, "created");
			const char *err = json_str_field(rep, "error");
			const struct json_value *regd = json_object_get(rep, "registered");
			const char *ip = json_str_field(rep, "ip");

			printf("%-8s %s", name != NULL ? name : "?", created != NULL ? created : "?");
			if (regd != NULL && regd->type == JSON_BOOL && regd->u.boolean)
				printf(", registered");
			if (ip != NULL && ip[0] != '\0')
				printf(", %s", ip);
			if (err != NULL && err[0] != '\0')
				printf(" -- %s", err);
			printf("\n");
		}
	}
	resolver = json_object_get(r->json, "resolver");
	if (resolver != NULL && resolver->type == JSON_ARRAY) {
		printf("host resolver -> ");
		for (i = 0; i < resolver->u.array.count; i++)
			printf("%s%s", i ? ", " : "", json_as_string(resolver->u.array.items[i]));
		printf("\n");
	}
	problems = (long)json_as_number(json_object_get(r->json, "problems"));
	if (problems > 0) {
		/* The per-replica lines above went to stdout, which is block-
		 * buffered when piped -- without this the summary reaches the
		 * terminal first and reads as if nothing was reported. */
		fflush(stdout);
		fprintf(stderr, "\ndns provision finished with %ld problem(s) above.\n", problems);
	}
}

/*
 * boot-next: arm/read/disarm systemd-boot's one-shot entry (#154).
 *
 * One-shot rather than repointing the loader default, because
 * `default` is sticky: forcing a slot that way means the NEXT update,
 * which stages the other slot, matches nothing and silently never
 * boots. A one-shot self-clears, so a machine that fails to come back
 * falls back to normal selection instead of staying pinned to a broken
 * slot -- the property that makes this usable for rollback.
 */
static void fmt_boot_next(const struct json_value *v)
{
	const char *entry = (v != NULL) ? json_str_field(v, "entry") : NULL;

	if (entry != NULL && entry[0] != '\0')
		printf("next boot: %s (once, then back to normal selection)\n", entry);
	else
		printf("next boot: (nothing armed -- normal selection applies)\n");
}

static int cmd_boot_next(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char body[64];

	if (argc == 0) {
		if (cix_client_request(c, CIX_API_getBootNext_METHOD, CIX_API_getBootNext, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_boot_next);
	}
	if (strcmp(argv[0], "clear") == 0) {
		if (cix_client_request(c, CIX_API_clearBootNext_METHOD, CIX_API_clearBootNext, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status == 204) {
			/* A 204 has no body, so emit() would print "null" -- say
			 * what the machine will now do instead. */
			if (!json_mode)
				printf("next boot: (cleared -- normal selection applies)\n");
			cix_response_free(&r);
			return 0;
		}
		return emit(&r, json_mode, NULL);
	}
	if (strcmp(argv[0], "a") != 0 && strcmp(argv[0], "b") != 0) {
		fprintf(stderr, "usage: cixctl boot-next [a|b|clear]\n");
		return 2;
	}
	snprintf(body, sizeof(body), "{\"slot\":\"%s\"}", argv[0]);
	if (cix_client_request(c, CIX_API_setBootNext_METHOD, CIX_API_setBootNext, body, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_boot_next);
}

static int cmd_dns_provision(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct cix_response r;
	int set_resolver = 1;
	int replica_count = 0;
	int i;

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "replicas");
	jw_arr_open(&w);
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--replica=", 10) == 0) {
			jw_str(&w, argv[i] + 10);
			replica_count++;
		} else if (strcmp(argv[i], "--no-resolver") == 0) {
			set_resolver = 0;
		} else {
			fprintf(stderr, "cixctl: unknown dns provision option '%s'\n", argv[i]);
			jw_free(&w);
			return 2;
		}
	}
	jw_arr_close(&w);
	jw_key(&w, "set_resolver");
	jw_bool(&w, set_resolver);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	/* An empty replicas array means "use the platform default", which
	 * the daemon decides -- not restated here, so the two cannot
	 * disagree about what the standard topology is. */
	(void)replica_count;

	if (cix_client_request(c, CIX_API_provisionDns_METHOD, CIX_API_provisionDns, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	/* 207 means some replicas had problems; the body says which. */
	if (r.status != 200 && r.status != 207)
		return emit(&r, json_mode, NULL);
	if (json_mode)
		return emit(&r, json_mode, NULL);
	fmt_dns_provision(&r);
	{
		long problems = (r.json != NULL) ? (long)json_as_number(json_object_get(r.json, "problems")) : 0;

		cix_response_free(&r);
		return problems > 0 ? 1 : 0;
	}
}

static int cmd_dns(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl dns record ...\n"
		                "       cixctl dns server ...\n"
		                "       cixctl dns provision [--replica=NAME ...] [--no-resolver]\n"
		                "       cixctl dns forwarders show | set [--forwarder=IP ...]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "record") == 0)
		return cmd_dns_record(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "server") == 0)
		return cmd_dns_server(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "provision") == 0)
		return cmd_dns_provision(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "forwarders") == 0)
		return cmd_dns_forwarders(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown dns subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_server_register(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *container = NULL;
	const char *config_path = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--container=", 12) == 0)
			container = argv[i] + 12;
		else if (strncmp(argv[i], "--config-path=", 14) == 0)
			config_path = argv[i] + 14;
		else {
			fprintf(stderr, "cixctl: unknown ldap server register option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (container == NULL || config_path == NULL) {
		fprintf(stderr,
		        "usage: cixctl ldap server register --container=NAME --config-path=PATH\n");
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

	if (cix_client_request(c, CIX_API_createLdapServer_METHOD, CIX_API_createLdapServer, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_server_line);
}

static int cmd_ldap_server_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listLdapServers_METHOD, CIX_API_listLdapServers, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_server_list);
}

static int cmd_ldap_server_unregister(const struct cix_client *c, int json_mode, int argc,
                                       char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: ldap server unregister requires a container name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteLdapServer, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_server(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl ldap server register --container=NAME --config-path=PATH\n"
		        "       cixctl ldap server ls\n"
		        "       cixctl ldap server unregister CONTAINER\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "register") == 0)
		return cmd_ldap_server_register(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_ldap_server_ls(c, json_mode);
	if (strcmp(sub, "unregister") == 0)
		return cmd_ldap_server_unregister(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown ldap server subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_group_add(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *gidnumber = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--gidnumber=", 12) == 0)
			gidnumber = argv[i] + 12;
		else {
			fprintf(stderr, "cixctl: unknown ldap group add option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: cixctl ldap group add --name=NAME [--gidnumber=N]\n");
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

	if (cix_client_request(c, CIX_API_createLdapGroup_METHOD, CIX_API_createLdapGroup, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_group_line);
}

static int cmd_ldap_group_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listLdapGroups_METHOD, CIX_API_listLdapGroups, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_group_list);
}

static int cmd_ldap_group_update(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *gidnumber = NULL;
	const char *new_name = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;
	char path[256];

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--gidnumber=", 12) == 0)
			gidnumber = argv[i] + 12;
		else if (strncmp(argv[i], "--new-name=", 11) == 0)
			new_name = argv[i] + 11;
		else {
			fprintf(stderr, "cixctl: unknown ldap group update option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL || gidnumber == NULL) {
		fprintf(stderr,
		        "usage: cixctl ldap group update --name=NAME --gidnumber=N [--new-name=NEWNAME]\n");
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

	snprintf(path, sizeof(path), CIX_API_updateLdapGroup, name);
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_group_line);
}

static int cmd_ldap_group_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: ldap group rm requires a group name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteLdapGroup, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_group(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl ldap group add --name=NAME [--gidnumber=N]\n"
		                "       cixctl ldap group update --name=NAME --gidnumber=N "
		                "[--new-name=NEWNAME]\n"
		                "       cixctl ldap group ls\n"
		                "       cixctl ldap group rm NAME\n");
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

	fprintf(stderr, "cixctl: unknown ldap group subcommand '%s'\n", sub);
	return 2;
}

/* Shared by "ldap user add" and a future "ldap user update" -- parses
 * every optional field cixctl exposes into a JSON request body. */
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

static int cmd_ldap_user_add(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct cix_response r;
	const char *name;

	jw_init(&w);
	build_ldap_user_body(&w, argc, argv, 0, &name);
	if (name == NULL) {
		jw_free(&w);
		fprintf(stderr,
		        "usage: cixctl ldap user add --name=NAME [--uidnumber=N] "
		        "--primarygroup=N [--givenname=S] [--sn=S] [--mail=S] "
		        "[--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] "
		        "[--ssh-key=S] [--can-search]\n");
		return 2;
	}
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_createLdapUser_METHOD, CIX_API_createLdapUser, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_user_line);
}

/* PUT is full-field-replacement (see handle_ldap_user_update()'s own doc
 * comment) -- an omitted field here resets to empty/0 on the server, same
 * as "ldap group update". */
static int cmd_ldap_user_update(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct json_writer w;
	struct cix_response r;
	const char *name;
	char path[256];

	jw_init(&w);
	build_ldap_user_body(&w, argc, argv, 1, &name);
	if (name == NULL) {
		jw_free(&w);
		fprintf(stderr,
		        "usage: cixctl ldap user update --name=NAME [--new-name=NEWNAME] "
		        "[--uidnumber=N] [--primarygroup=N] [--givenname=S] [--sn=S] [--mail=S] "
		        "[--loginshell=S] [--homedirectory=S] [--password=S] [--disabled] "
		        "[--ssh-key=S] [--can-search]\n");
		return 2;
	}
	w.buf[w.len] = '\0';

	snprintf(path, sizeof(path), CIX_API_updateLdapUser, name);
	if (cix_client_request(c, "PUT", path, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_ldap_user_line);
}

static int cmd_ldap_user_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listLdapUsers_METHOD, CIX_API_listLdapUsers, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_user_list);
}

static int cmd_ldap_user_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: ldap user rm requires a user name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deleteLdapUser, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_ldap_user(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl ldap user add --name=NAME --uidnumber=N "
		                "--primarygroup=N ...\n"
		                "       cixctl ldap user update --name=NAME ...\n"
		                "       cixctl ldap user ls\n"
		                "       cixctl ldap user rm NAME\n");
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

	fprintf(stderr, "cixctl: unknown ldap user subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getLdapConfig_METHOD, CIX_API_getLdapConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_ldap_config_line);
}

static int cmd_ldap_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	long start_uid = -1, start_gid = -1;
	/* Issue #66: the client-login fields containers reference via
	 * {{LDAP:URI}}/{{LDAP:BASE_DN}}/{{LDAP:BIND_DN}}/
	 * {{LDAP:BIND_PASSWORD}} recipe tokens -- each individually
	 * optional here (absent = unchanged, "" = clear), same contract
	 * as the PUT itself. */
	const char *client_uri = NULL, *base_dn = NULL, *bind_dn = NULL, *bind_password = NULL;
	/* #414: -1 == "leave it", so a body that does not mention TLS cannot
	 * turn it off as a side effect of setting something else. */
	int client_tls = -1;
	/* #419: which listeners the SERVERS run. Same -1 sentinel. */
	int server_plaintext = -1, server_tls = -1;
	long server_plaintext_port = -1, server_tls_port = -1;
	int restart_servers = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--start-uid=", 12) == 0)
			start_uid = strtol(argv[i] + 12, NULL, 10);
		else if (strncmp(argv[i], "--start-gid=", 12) == 0)
			start_gid = strtol(argv[i] + 12, NULL, 10);
		else if (strncmp(argv[i], "--client-uri=", 13) == 0)
			client_uri = argv[i] + 13;
		else if (strncmp(argv[i], "--base-dn=", 10) == 0)
			base_dn = argv[i] + 10;
		else if (strncmp(argv[i], "--bind-dn=", 10) == 0)
			bind_dn = argv[i] + 10;
		else if (strncmp(argv[i], "--bind-password=", 16) == 0)
			bind_password = argv[i] + 16;
		else if (strcmp(argv[i], "--client-tls") == 0)
			client_tls = 1;
		else if (strcmp(argv[i], "--no-client-tls") == 0)
			client_tls = 0;
		else if (strcmp(argv[i], "--server-plaintext") == 0)
			server_plaintext = 1;
		else if (strcmp(argv[i], "--no-server-plaintext") == 0)
			server_plaintext = 0;
		else if (strncmp(argv[i], "--server-plaintext-port=", 24) == 0)
			server_plaintext_port = strtol(argv[i] + 24, NULL, 10);
		else if (strcmp(argv[i], "--server-tls") == 0)
			server_tls = 1;
		else if (strcmp(argv[i], "--no-server-tls") == 0)
			server_tls = 0;
		else if (strncmp(argv[i], "--server-tls-port=", 18) == 0)
			server_tls_port = strtol(argv[i] + 18, NULL, 10);
		else if (strcmp(argv[i], "--restart-servers") == 0)
			restart_servers = 1;
		else {
			fprintf(stderr, "cixctl: unknown ldap config set option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (start_uid < 0 && start_gid < 0 && client_uri == NULL && base_dn == NULL &&
	    bind_dn == NULL && bind_password == NULL && client_tls < 0 && server_plaintext < 0 &&
	    server_tls < 0 && server_plaintext_port < 0 && server_tls_port < 0) {
		fprintf(stderr,
		        "usage: cixctl ldap config set [--start-uid=N --start-gid=N] "
		        "[--client-uri=URIS] [--base-dn=DN] [--bind-dn=DN] [--bind-password=PW]\n"
		        "       [--client-tls | --no-client-tls]\n"
		        "       [--server-plaintext | --no-server-plaintext] "
		        "[--server-plaintext-port=N]\n"
		        "       [--server-tls | --no-server-tls] [--server-tls-port=N]\n"
		        "       [--restart-servers]\n");
		return 2;
	}
	if ((start_uid < 0) != (start_gid < 0)) {
		fprintf(stderr, "cixctl: --start-uid and --start-gid must be given together\n");
		return 2;
	}

	jw_init(&w);
	jw_obj_open(&w);
	if (start_uid >= 0) {
		jw_key(&w, "start_uid");
		jw_int(&w, start_uid);
		jw_key(&w, "start_gid");
		jw_int(&w, start_gid);
	}
	if (client_uri != NULL) {
		jw_key(&w, "client_uri");
		jw_str(&w, client_uri);
	}
	if (base_dn != NULL) {
		jw_key(&w, "base_dn");
		jw_str(&w, base_dn);
	}
	if (bind_dn != NULL) {
		jw_key(&w, "bind_dn");
		jw_str(&w, bind_dn);
	}
	if (bind_password != NULL) {
		jw_key(&w, "bind_password");
		jw_str(&w, bind_password);
	}
	if (client_tls >= 0) {
		jw_key(&w, "client_tls");
		jw_bool(&w, client_tls);
	}
	if (server_plaintext >= 0) {
		jw_key(&w, "server_plaintext");
		jw_bool(&w, server_plaintext);
	}
	if (server_plaintext_port >= 0) {
		jw_key(&w, "server_plaintext_port");
		jw_int(&w, server_plaintext_port);
	}
	if (server_tls >= 0) {
		jw_key(&w, "server_tls");
		jw_bool(&w, server_tls);
	}
	if (server_tls_port >= 0) {
		jw_key(&w, "server_tls_port");
		jw_int(&w, server_tls_port);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_updateLdapConfig_METHOD, CIX_API_updateLdapConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	/*
	 * #419: glauth binds its listeners at startup, and its config
	 * watcher reloads only the record datastore -- so a listener change
	 * reaches a RUNNING server only when that server restarts and
	 * re-reads the (now managed) config staged for it. Opt-in and off
	 * by default, because this is the directory that authenticates the
	 * control plane and a plain config PUT should never disrupt it.
	 *
	 * Sequenced deliberately: one server at a time, each brought back
	 * before the next is touched, so a healthy replica is always
	 * serving. Composed from the stop/start endpoints rather than a new
	 * one -- the capability already exists over REST, and this is the
	 * client sequencing it, the same way `hostauth-config set` composes
	 * a GET and a PUT.
	 */
	if (restart_servers && r.status == 200) {
		struct cix_response sr;
		char names[8][128];
		int count = 0, k;

		cix_response_free(&r);
		memset(&sr, 0, sizeof(sr));
		if (cix_client_request(c, CIX_API_listLdapServers_METHOD, CIX_API_listLdapServers,
		                        NULL, &sr) != 0 ||
		    sr.status != 200) {
			fprintf(stderr, "cixctl: config saved, but listing LDAP servers failed (HTTP %d) "
			                 "-- nothing was restarted\n",
			        sr.status);
			cix_response_free(&sr);
			return 1;
		}
		{
			const struct json_value *arr = json_object_get(sr.json, "servers");
			size_t j;

			if (arr != NULL && arr->type == JSON_ARRAY) {
				for (j = 0; j < arr->u.array.count && count < 8; j++) {
					const char *n =
					    json_str_field(arr->u.array.items[j], "container");

					if (n != NULL)
						snprintf(names[count++], sizeof(names[0]), "%s", n);
				}
			}
		}
		cix_response_free(&sr);

		for (k = 0; k < count; k++) {
			char path[256];
			int tries;

			snprintf(path, sizeof(path), CIX_API_stopContainer, names[k]);
			memset(&sr, 0, sizeof(sr));
			if (cix_client_request(c, CIX_API_stopContainer_METHOD, path, NULL, &sr) != 0 ||
			    sr.status / 100 != 2) {
				fprintf(stderr, "cixctl: stopping %s failed (HTTP %d) -- stopping here, "
				                 "later servers left running\n",
				        names[k], sr.status);
				cix_response_free(&sr);
				return 1;
			}
			cix_response_free(&sr);

			/* A stop releases the container's IP asynchronously, so the
			 * start can legitimately 409 for a moment ("ip is held by
			 * ... still shutting down"). Retried rather than reported,
			 * because it is the expected path, not a failure. */
			snprintf(path, sizeof(path), CIX_API_startContainer, names[k]);
			for (tries = 0; tries < 40; tries++) {
				memset(&sr, 0, sizeof(sr));
				if (cix_client_request(c, CIX_API_startContainer_METHOD, path, NULL, &sr) ==
				        0 &&
				    sr.status / 100 == 2) {
					cix_response_free(&sr);
					break;
				}
				cix_response_free(&sr);
				usleep(500000);
			}
			if (tries == 40) {
				fprintf(stderr, "cixctl: %s did not come back after its stop -- start it "
				                 "manually before touching the remaining servers\n",
				        names[k]);
				return 1;
			}
			printf("restarted %s\n", names[k]);
		}
		return 0;
	}

	return emit(&r, json_mode, fmt_ldap_config_line);
}

static int cmd_ldap_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl ldap config show\n"
		                "       cixctl ldap config set --start-uid=N --start-gid=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_ldap_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_ldap_config_set(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown ldap config subcommand '%s'\n", sub);
	return 2;
}

static int cmd_ldap(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl ldap server ...\n"
		                "       cixctl ldap config ...\n"
		                "       cixctl ldap group ...\n"
		                "       cixctl ldap user ...\n");
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

	fprintf(stderr, "cixctl: unknown ldap subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_ca_bootstrap(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "cixctl: unknown pki ca bootstrap option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_createPkiCa_METHOD, CIX_API_createPkiCa, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkiCa_METHOD, CIX_API_getPkiCa, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_ca(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pki ca bootstrap [--common-name=NAME] [--days=N]\n"
		                "       cixctl pki ca show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_ca_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_ca_show(c, json_mode);

	fprintf(stderr, "cixctl: unknown pki ca subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_intermediate_bootstrap(const struct cix_client *c, int json_mode, int argc,
                                           char **argv)
{
	const char *common_name = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--common-name=", 14) == 0)
			common_name = argv[i] + 14;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "cixctl: unknown pki intermediate bootstrap option '%s'\n",
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

	if (cix_client_request(c, CIX_API_createPkiIntermediate_METHOD, CIX_API_createPkiIntermediate, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkiIntermediate_METHOD, CIX_API_getPkiIntermediate, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_ca);
}

static int cmd_pki_intermediate(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pki intermediate bootstrap [--common-name=NAME] [--days=N]\n"
		                "       cixctl pki intermediate show\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "bootstrap") == 0)
		return cmd_pki_intermediate_bootstrap(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pki_intermediate_show(c, json_mode);

	fprintf(stderr, "cixctl: unknown pki intermediate subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pki_cert_create(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *sans = NULL;
	long days = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--sans=", 7) == 0)
			sans = argv[i] + 7;
		else if (strncmp(argv[i], "--days=", 7) == 0)
			days = atol(argv[i] + 7);
		else {
			fprintf(stderr, "cixctl: unknown pki cert create option '%s'\n", argv[i]);
			return 2;
		}
	}

	if (name == NULL) {
		fprintf(stderr, "usage: cixctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n");
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

	if (cix_client_request(c, CIX_API_createPkiCert_METHOD, CIX_API_createPkiCert, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_cert_issued);
}

static int cmd_pki_cert_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listPkiCerts_METHOD, CIX_API_listPkiCerts, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pki_cert_list);
}

static int cmd_pki_cert_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: pki cert rm requires a cert name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deletePkiCert, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pki_cert(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pki cert create --name=NAME [--sans=a,b,c] [--days=N]\n"
		                "       cixctl pki cert ls\n"
		                "       cixctl pki cert rm NAME\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "create") == 0)
		return cmd_pki_cert_create(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_pki_cert_ls(c, json_mode);
	if (strcmp(sub, "rm") == 0)
		return cmd_pki_cert_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown pki cert subcommand '%s'\n", sub);
	return 2;
}

/*
 * Destructive: wipes and regenerates the whole CA chain (root, plus
 * the intermediate if one exists), reissuing every currently-tracked
 * leaf under the new chain. No separate --yes confirmation flag --
 * matches every other destructive cixctl subcommand's own direct-
 * execution convention (pki cert rm, network rm, ...); the web
 * dashboard's own confirm dialog is where the "are you sure" prompt
 * lives for this project.
 */
static int cmd_pki_reset(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *root_cn = NULL;
	const char *intermediate_cn = NULL;
	long root_days = -1;
	long intermediate_days = -1;
	long leaf_days = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown pki reset option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_resetPki_METHOD, CIX_API_resetPki, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pki_reset);
}

/* Defined further down, beside `login`'s own prompt; declared here
 * because pki export/import need the same no-echo read and this CLI is
 * one translation unit in source order. */
static int read_line_noecho(const char *prompt, char *out, size_t out_size);

/*
 * #415/ADR-0281. The passphrase is NEVER accepted on argv: argv is
 * world-readable through /proc/<pid>/cmdline for as long as the process
 * lives, and this one opens the trust root. Prompted interactively, or
 * read from a file for automation -- the same two routes `cixctl login`
 * already offers for a password.
 */
static int pki_passphrase_read(const char *file, char *out, size_t out_size, const char *prompt)
{
	if (file != NULL) {
		FILE *f = fopen(file, "r");
		size_t n;

		if (f == NULL) {
			fprintf(stderr, "cixctl: cannot read passphrase file '%s'\n", file);
			return -1;
		}
		if (fgets(out, (int)out_size, f) == NULL) {
			fclose(f);
			fprintf(stderr, "cixctl: passphrase file '%s' is empty\n", file);
			return -1;
		}
		fclose(f);
		n = strlen(out);
		while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
			out[--n] = '\0';
	} else if (read_line_noecho(prompt, out, out_size) != 0) {
		fprintf(stderr, "cixctl: could not read passphrase\n");
		return -1;
	}
	if (out[0] == '\0') {
		fprintf(stderr, "cixctl: passphrase may not be empty\n");
		return -1;
	}
	return 0;
}

static int cmd_pki_export(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *pass_file = NULL, *out_file = NULL;
	char passphrase[512];
	char confirm[512];
	struct json_writer w;
	struct cix_response r;
	int i, rc = 0;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--passphrase-file=", 18) == 0)
			pass_file = argv[i] + 18;
		else if (strncmp(argv[i], "--out=", 6) == 0)
			out_file = argv[i] + 6;
		else {
			fprintf(stderr, "cixctl: unknown pki export option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (pki_passphrase_read(pass_file, passphrase, sizeof(passphrase),
	                         "Passphrase to encrypt the bundle: ") != 0)
		return 1;
	/* Confirmed when typed, not when read from a file: a mistyped
	 * passphrase produces a bundle nobody can ever open, and the
	 * operator would not find out until the reinstall they needed it
	 * for. */
	if (pass_file == NULL) {
		if (read_line_noecho("Repeat passphrase: ", confirm, sizeof(confirm)) != 0)
			return 1;
		if (strcmp(passphrase, confirm) != 0) {
			fprintf(stderr, "cixctl: passphrases do not match\n");
			return 1;
		}
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "passphrase");
	jw_str(&w, passphrase);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	if (cix_client_request(c, CIX_API_exportPki_METHOD, CIX_API_exportPki, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status != 200) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	if (json_mode) {
		printf("%s\n", r.body);
	} else {
		const char *bundle = json_str_field(r.json, "bundle");
		const char *cipher = json_str_field(r.json, "cipher");

		if (bundle == NULL) {
			fprintf(stderr, "cixctl: response carried no bundle\n");
			rc = 1;
		} else if (out_file != NULL) {
			FILE *f = fopen(out_file, "w");

			if (f == NULL) {
				fprintf(stderr, "cixctl: cannot write '%s'\n", out_file);
				rc = 1;
			} else {
				fprintf(f, "%s\n", bundle);
				fclose(f);
				/* 0600 after close: the bundle holds the CA
				 * private key, and a default-umask file in an
				 * operator's home is the wrong resting place
				 * for it. */
				chmod(out_file, 0600);
				printf("wrote %s (%s)\n", out_file, cipher != NULL ? cipher : "encrypted");
				printf("Keep this off the box. Without the passphrase it cannot be restored.\n");
			}
		} else {
			printf("%s\n", bundle);
		}
	}
	cix_response_free(&r);
	return rc;
}

static int cmd_pki_import(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *pass_file = NULL, *in_file = NULL;
	char passphrase[512];
	char *bundle = NULL;
	size_t cap = 0, len = 0;
	struct json_writer w;
	struct cix_response r;
	FILE *f;
	int i, ch;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--passphrase-file=", 18) == 0)
			pass_file = argv[i] + 18;
		else if (strncmp(argv[i], "--in=", 5) == 0)
			in_file = argv[i] + 5;
		else {
			fprintf(stderr, "cixctl: unknown pki import option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (in_file == NULL) {
		fprintf(stderr, "usage: cixctl pki import --in=PATH [--passphrase-file=PATH]\n");
		return 2;
	}
	f = fopen(in_file, "r");
	if (f == NULL) {
		fprintf(stderr, "cixctl: cannot read '%s'\n", in_file);
		return 1;
	}
	/* Grown rather than a fixed buffer: the bundle is one base64 line
	 * whose length scales with the number of certs in the store, and a
	 * silently truncated trust root is the worst possible failure here. */
	while ((ch = fgetc(f)) != EOF) {
		if (ch == '\n' || ch == '\r')
			continue;
		if (len + 1 >= cap) {
			size_t ncap = cap == 0 ? 8192 : cap * 2;
			char *nb = realloc(bundle, ncap);

			if (nb == NULL) {
				free(bundle);
				fclose(f);
				fprintf(stderr, "cixctl: out of memory reading bundle\n");
				return 1;
			}
			bundle = nb;
			cap = ncap;
		}
		bundle[len++] = (char)ch;
	}
	fclose(f);
	if (bundle == NULL || len == 0) {
		free(bundle);
		fprintf(stderr, "cixctl: '%s' is empty\n", in_file);
		return 1;
	}
	bundle[len] = '\0';

	if (pki_passphrase_read(pass_file, passphrase, sizeof(passphrase),
	                         "Passphrase for the bundle: ") != 0) {
		free(bundle);
		return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "passphrase");
	jw_str(&w, passphrase);
	jw_key(&w, "bundle");
	jw_str(&w, bundle);
	jw_obj_close(&w);
	w.buf[w.len] = '\0';
	free(bundle);
	if (cix_client_request(c, CIX_API_importPki_METHOD, CIX_API_importPki, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status != 200) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	if (json_mode) {
		printf("%s\n", r.body);
	} else {
		printf("restored %d certificate(s); intermediate: %s\n",
		       (int)json_as_number(json_object_get(r.json, "certs_restored")),
		       json_as_number(json_object_get(r.json, "intermediate_restored")) != 0 ? "yes" :
		                                                                                "no");
		printf("Containers holding certs from the previous store are not restaged -- recreate them, or reboot.\n");
	}
	cix_response_free(&r);
	return 0;
}

static int cmd_pki(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pki ca ...\n"
		                "       cixctl pki intermediate ...\n"
		                "       cixctl pki cert ...\n"
		                "       cixctl pki reset [--root-common-name=NAME]\n"
		                "               [--intermediate-common-name=NAME] [--root-days=N]\n"
		                "               [--intermediate-days=N] [--leaf-days=N]  -- wipes and\n"
		                "               regenerates the whole CA chain, reissuing every leaf\n"
		                "               currently tracked; defaults name each tier after this\n"
		                "               install's own domain_suffix (cixctl site show)\n"
		                "       cixctl pki export [--out=PATH] [--passphrase-file=PATH]\n"
		                "               -- the whole store, encrypted, to carry across a reinstall\n"
		                "       cixctl pki import --in=PATH [--passphrase-file=PATH]\n"
		                "               -- restore it into an install that has no CA yet\n");
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
	if (strcmp(sub, "export") == 0)
		return cmd_pki_export(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "import") == 0)
		return cmd_pki_import(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown pki subcommand '%s'\n", sub);
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
	const char *scope = json_str_field(v, "scope");

	printf("state=%s", state != NULL ? state : "?");
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
	/* Issue #17: "none" reads as "no bootstrap ever happened" but only
	 * means this daemon never handled a toolchain_url POST -- surface
	 * the daemon's own scope note so that misreading doesn't recur. */
	if (state != NULL && strcmp(state, "none") == 0 && scope != NULL)
		printf("(%s)\n", scope);
}

/* Polls GET /v1/pkg/bootstrap until state leaves "fetching" -- --wait's
 * own loop for the toolchain_url mode, the same shape poll_hostbuild()/
 * poll_iso() already established. */
static int poll_bootstrap_fetch(const struct cix_client *c, struct cix_response *out)
{
	for (;;) {
		const char *state;

		if (cix_client_request(c, CIX_API_getPkgBootstrapStatus_METHOD, CIX_API_getPkgBootstrapStatus, NULL, out) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "fetching") != 0)
			return 0;
		cix_response_free(out);
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
	/* ADR-0256: a package that could not be REACHED and one that failed
	 * to BUILD both read "failed", and they call for opposite responses
	 * -- retry the first, fix the second. The pipeline stage says which
	 * without anyone having to read the message, and the status
	 * distinguishes a build that was killed from one that did not work.
	 */
	const char *stage = json_str_field(v, "stage");
	const char *status = json_str_field(v, "status");
	char state_shown[40];

	if (stage != NULL && stage[0] != '\0' && status != NULL && status[0] != '\0')
		snprintf(state_shown, sizeof(state_shown), "%s:%s/%s", state, stage, status);
	else
		snprintf(state_shown, sizeof(state_shown), "%s", state != NULL ? state : "-");

	printf("%-24s %-16s %-12s %-16s %-14s %s\n", name,
	       image != NULL && image[0] != '\0' ? image : "-", version, state_shown,
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

static int cmd_pkg_bootstrap_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgBootstrapStatus_METHOD, CIX_API_getPkgBootstrapStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_bootstrap_fetch_status);
}

static int cmd_pkg_bootstrap(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *toolchain = NULL;
	const char *toolchain_url = NULL;
	const char *toolchain_sha256 = NULL;
	int wait = 0;
	int i;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown pkg bootstrap option '%s'\n", argv[i]);
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

		if (cix_client_request(c, CIX_API_pkgBootstrap_METHOD, CIX_API_pkgBootstrap, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		jw_free(&w);
		if (r.status < 200 || r.status >= 300 || !wait)
			return emit(&r, json_mode, fmt_bootstrap_fetch_status);
		cix_response_free(&r);

		if (poll_bootstrap_fetch(c, &r) != 0)
			return 1;
		return emit(&r, json_mode, fmt_bootstrap_fetch_status);
	}

	if (toolchain == NULL) {
		if (cix_client_request(c, CIX_API_pkgBootstrap_METHOD, CIX_API_pkgBootstrap, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
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

		if (cix_client_request(c, CIX_API_pkgBootstrap_METHOD, CIX_API_pkgBootstrap, w.buf, &r) != 0) {
			jw_free(&w);
			fprintf(stderr, "cixctl: could not reach daemon\n");
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

	if (url == NULL || url[0] == '\0') {
		printf("(no repo configured)\n");
		return;
	}
	/* ADR-0257: when it syncs is `cixctl schedule ls`, not here. */
	printf("repo_url=%s repo_kind=%s ref=%s auth_token=%s  (schedule: cixctl schedule ls)\n",
	       url, kind != NULL ? kind : "?", ref != NULL ? ref : "?",
	       (token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean)
	           ? "set"
	           : "unset");
}

static int cmd_pkg_repo_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgRepoConfig_METHOD, CIX_API_getPkgRepoConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_repo_config);
}

static int cmd_pkg_repo_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *url = NULL;
	const char *kind = NULL;
	const char *ref = NULL;
	const char *token = NULL;
	long interval = -1;
	int i;
	struct json_writer w;
	struct cix_response r;

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
		else if (strncmp(argv[i], "--sync-interval=", 16) == 0) {
			fprintf(stderr, "cixctl: --sync-interval is gone (ADR-0257) -- use\n"
			                "  cixctl schedule set recipe-sync --action=pkg.sync "
			                "--every-minutes=N\n");
			return 2;
		}
		else {
			fprintf(stderr, "cixctl: unknown pkg repo-config set option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_putPkgRepoConfig_METHOD, CIX_API_putPkgRepoConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_pkg_repo_config);
}

static int cmd_pkg_repo_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg repo-config show\n"
		                "       cixctl pkg repo-config set [--url=URL] [--kind=gitea|github|gitlab] "
		                "[--ref=REF] [--token=TOKEN | --clear-token]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_repo_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_repo_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "cixctl: unknown pkg repo-config subcommand '%s'\n", sub);
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
static int poll_pkg_sync(const struct cix_client *c, struct cix_response *out)
{
	for (;;) {
		const char *state;

		if (cix_client_request(c, CIX_API_getPkgSyncStatus_METHOD, CIX_API_getPkgSyncStatus, NULL, out) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "running") != 0)
			return 0;
		cix_response_free(out);
		usleep(500000);
	}
}

static int cmd_pkg_sync(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	int wait = 0;
	int i;
	struct cix_response r;
	const char *refetch = NULL;
	char body[256];

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else if (strncmp(argv[i], "--refetch=", 10) == 0)
			refetch = argv[i] + 10;
		else {
			fprintf(stderr, "cixctl: unknown pkg sync option '%s'\n", argv[i]);
			return 2;
		}
	}

	/* Issue #59: --refetch=name@version lets this ONE sync replace that
	 * one recipe version instead of skipping it as already-seen. */
	if (refetch != NULL)
		snprintf(body, sizeof(body), "{\"refetch\":\"%s\"}", refetch);

	if (cix_client_request(c, CIX_API_pkgSync_METHOD, CIX_API_pkgSync, refetch != NULL ? body : NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202) {
		int rc = emit(&r, json_mode, fmt_pkg_sync_status);

		return rc != 0 ? rc : 1;
	}
	cix_response_free(&r);

	if (wait) {
		if (poll_pkg_sync(c, &r) != 0)
			return 1;
	} else if (cix_client_request(c, CIX_API_getPkgSyncStatus_METHOD, CIX_API_getPkgSyncStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_sync_status);
}

static int cmd_pkg_sync_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgSyncStatus_METHOD, CIX_API_getPkgSyncStatus, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

/*
 * `cixctl pkg drift` -- issue #217.
 *
 * Prints the denominator, not just the list. "12 behind" reads very
 * differently against 30 packages than against 300, and a bare list
 * gives an operator no way to tell which they are looking at.
 */
static void fmt_pkg_drift(const struct json_value *v)
{
	long installed = (long)json_as_number(json_object_get(v, "installed"));
	long behind = (long)json_as_number(json_object_get(v, "behind"));
	const struct json_value *arr = json_object_get(v, "packages");
	size_t i;

	if (behind == 0) {
		printf("%ld installed, all current\n", installed);
		return;
	}

	printf("%-24s %-16s %-16s %s\n", "PACKAGE", "IMAGE", "INSTALLED", "AVAILABLE");
	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *name = json_str_field(e, "name");
		const char *image = json_str_field(e, "image");
		const char *iv = json_str_field(e, "installed_version");
		const char *av = json_str_field(e, "available_version");

		printf("%-24s %-16s %-16s %s\n", name ? name : "?", image ? image : "?",
		       iv ? iv : "?", av ? av : "?");
	}
	printf("\n%ld of %ld installed packages are behind their recipes\n", behind, installed);
	printf("run `cixctl pkg update-all` to upgrade one, repeatedly to drain the rest\n");
}

/*
 * Issue #281: packages this host reports as installed whose files are
 * not in their image. The healthy answer is the boring one, and it is
 * printed rather than left as an empty table -- "nothing to report" and
 * "the check did not run" must not look the same.
 */
static void fmt_pkg_verify(const struct json_value *v)
{
	long checked = (long)json_as_number(json_object_get(v, "checked"));
	long incomplete = (long)json_as_number(json_object_get(v, "incomplete"));
	const struct json_value *arr = json_object_get(v, "packages");
	size_t i;

	if (incomplete == 0) {
		printf("%ld installed packages checked, all present in their images with resolvable "
		       "headers\n", checked);
		return;
	}

	if (arr == NULL || arr->type != JSON_ARRAY)
		return;
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *name = json_str_field(e, "name");
		const char *image = json_str_field(e, "image");
		const char *ver = json_str_field(e, "version");
		long miss = (long)json_as_number(json_object_get(e, "missing_count"));
		long total = (long)json_as_number(json_object_get(e, "file_count"));
		long inc = (long)json_as_number(json_object_get(e, "unresolved_includes"));

		printf("%s@%s (%s)\n", name ? name : "?", image ? image : "?", ver ? ver : "?");
		/*
		 * The two findings are printed as separate lines rather than
		 * squeezed into shared columns: they are different failures
		 * with different repairs, and a package can have either, both
		 * or -- for the header one -- files that are all present.
		 */
		if (miss > 0)
			printf("    %ld of %ld installed file(s) are not in the image: %s\n", miss, total,
			       json_str_field(e, "first_missing"));
		if (inc > 0)
			printf("    %ld unresolved header include(s): %s\n", inc,
			       json_str_field(e, "first_unresolved_include"));
	}
	printf("\n%ld of %ld installed packages have a problem\n", incomplete, checked);
	printf("missing files: an image version is a hash of the installed package set, so\n");
	printf("  reinstalling the same name@version cannot produce a new one -- bump the package\n");
	printf("  revision, or delete and recreate the image\n");
	printf("unresolved includes: the package ships a header that cannot be included -- check\n");
	printf("  whether its recipe builds every directory whose headers it installs\n");
}

static int cmd_pkg_drift(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgDrift_METHOD, CIX_API_getPkgDrift, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_drift);
}

static int cmd_pkg_verify(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_verifyPkgFiles_METHOD, CIX_API_verifyPkgFiles, NULL, &r) !=
	    0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_verify);
}

static int cmd_pkg_cache_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgCacheConfig_METHOD, CIX_API_getPkgCacheConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_config_set(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	long max_bytes = -1;
	int i;
	char body[128];
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--max-bytes=", 12) == 0)
			max_bytes = strtol(argv[i] + 12, NULL, 10);
		else {
			fprintf(stderr, "cixctl: unknown pkg cache-config set option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (max_bytes <= 0) {
		fprintf(stderr, "usage: cixctl pkg cache-config set --max-bytes=N\n");
		return 2;
	}

	snprintf(body, sizeof(body), "{\"max_bytes\":%ld}", max_bytes);
	if (cix_client_request(c, CIX_API_putPkgCacheConfig_METHOD, CIX_API_putPkgCacheConfig, body, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg cache-config show\n"
		                "       cixctl pkg cache-config set --max-bytes=N\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_cache_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_cache_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "cixctl: unknown pkg cache-config subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_cache_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgCache_METHOD, CIX_API_getPkgCache, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_cache_status);
}

static int cmd_pkg_cache_clear(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_deletePkgCache_METHOD, CIX_API_deletePkgCache, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
	{
		const struct json_value *push = json_object_get(v, "push_enabled");

		printf("base_url=%s auth_token=%s push=%s\n", base_url,
		       (token_set != NULL && token_set->type == JSON_BOOL && token_set->u.boolean)
		           ? "set"
		           : "unset",
		       (push != NULL && push->type == JSON_BOOL && push->u.boolean) ? "enabled"
		                                                                    : "disabled");
	}
}

static int cmd_pkg_artifact_config_show(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getPkgArtifactConfig_METHOD, CIX_API_getPkgArtifactConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_artifact_config);
}

static int cmd_pkg_artifact_config_set(const struct cix_client *c, int json_mode, int argc,
                                        char **argv)
{
	const char *base_url = NULL;
	const char *token = NULL;
	int push = -1; /* -1 = not mentioned, so the daemon leaves it alone */
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--url=", 6) == 0)
			base_url = argv[i] + 6;
		else if (strncmp(argv[i], "--token=", 8) == 0)
			token = argv[i] + 8;
		else if (strcmp(argv[i], "--clear-token") == 0)
			token = "";
		else if (strcmp(argv[i], "--push") == 0)
			push = 1;
		else if (strcmp(argv[i], "--no-push") == 0)
			push = 0;
		else {
			fprintf(stderr, "cixctl: unknown pkg artifact-config set option '%s'\n", argv[i]);
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
	if (push >= 0) {
		jw_key(&w, "push_enabled");
		jw_bool(&w, push);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_putPkgArtifactConfig_METHOD, CIX_API_putPkgArtifactConfig, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	return emit(&r, json_mode, fmt_pkg_artifact_config);
}

static int cmd_pkg_artifact_config(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg artifact-config show\n"
		                "       cixctl pkg artifact-config set [--url=URL] "
		                "[--token=TOKEN | --clear-token] [--push | --no-push]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_artifact_config_show(c, json_mode);
	if (strcmp(sub, "set") == 0)
		return cmd_pkg_artifact_config_set(c, json_mode, argc - 1, argv + 1);
	fprintf(stderr, "cixctl: unknown pkg artifact-config subcommand '%s'\n", sub);
	return 2;
}

/*
 * Issue #129: exports a hostbuild package's artifact and writes it to a
 * local file. The whole point is getting expensive build output OFF the
 * box that made it, so this drives the full async sequence rather than
 * leaving the operator to poll and reassemble chunks by hand: POST,
 * poll until ready, then loop the bounded download.
 *
 * The chunk size matches the daemon's own 8 MiB clamp -- asking for
 * more just gets silently trimmed, and asking for much less makes a
 * large artifact take needlessly many round trips.
 */
#define ARTIFACT_EXPORT_CHUNK (8 * 1024 * 1024)

/*
 * `cixctl pkg artifact-publish NAME` -- issue #171's endpoint, which
 * had no interface on either channel until now. It was reachable only
 * with a hand-written curl, which is exactly how this session's own
 * artifact backfill had to be done: `cix` sat five releases behind in
 * the cache, and the one command that could fix it did not exist.
 *
 * Publishing an artifact that already exists matters because the local
 * cache does not survive a reinstall, and a rebuild of `cix` or
 * `kernel` is the most expensive thing this platform does. A push that
 * failed, or happened before the artifact server was configured, must
 * not cost that.
 */
static int cmd_pkg_artifact_publish(const struct cix_client *c, int json_mode, int argc,
                                     char **argv)
{
	char path[512];
	struct cix_response r;

	if (argc < 1 || argv[0][0] == '-') {
		fprintf(stderr,
		        "usage: cixctl pkg artifact-publish NAME\n"
		        "  Publishes an already-built artifact to the configured artifact server\n"
		        "  without rebuilding it. For a hostbuild whose tarball does not exist yet\n"
		        "  the tarball is built from the installed tree first, and the push follows\n"
		        "  when that completes -- so a 202 here can mean either \"queued\" or\n"
		        "  \"building artifact tarball\"; the response says which.\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_publishPkgArtifact, argv[0]);
	if (cix_client_request(c, CIX_API_publishPkgArtifact_METHOD, path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, NULL);
}

static int cmd_pkg_artifact_export(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *out_path = NULL;
	char path[512];
	char out_default[512];
	struct cix_response r;
	long long size = -1;
	long long got = 0;
	FILE *out;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--out=", 6) == 0)
			out_path = argv[i] + 6;
		else if (argv[i][0] == '-') {
			fprintf(stderr, "cixctl: unknown pkg artifact-export option '%s'\n", argv[i]);
			return 2;
		} else if (name == NULL) {
			name = argv[i];
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl pkg artifact-export NAME [--out=FILE]\n");
		return 2;
	}

	snprintf(path, sizeof(path), CIX_API_exportPkgArtifact, name);
	if (cix_client_request(c, "POST", path, "", &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202) {
		int rc = emit(&r, json_mode, NULL);

		return rc == 0 ? 1 : rc;
	}
	cix_response_free(&r);

	/* Poll for readiness. A large tree genuinely takes a while to tar,
	 * so this waits rather than reporting "not ready" and giving up. */
	for (i = 0; i < 1800; i++) {
		const char *state;

		if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		state = (r.json != NULL) ? json_str_field(r.json, "state") : NULL;
		if (state != NULL && strcmp(state, "ready") == 0) {
			const struct json_value *sz = json_object_get(r.json, "size_bytes");
			const char *aname = json_str_field(r.json, "artifact_name");

			if (sz != NULL)
				size = (long long)json_as_number(sz);
			if (out_path == NULL && aname != NULL) {
				snprintf(out_default, sizeof(out_default), "%s", aname);
				out_path = out_default;
			}
			cix_response_free(&r);
			break;
		}
		if (state != NULL && strcmp(state, "failed") == 0) {
			const char *err = json_str_field(r.json, "error");

			fprintf(stderr, "cixctl: export failed: %s\n", err != NULL ? err : "(no detail)");
			cix_response_free(&r);
			return 1;
		}
		cix_response_free(&r);
		usleep(500000);
	}
	if (size <= 0) {
		fprintf(stderr, "cixctl: export did not become ready\n");
		return 1;
	}
	if (out_path == NULL) {
		fprintf(stderr, "cixctl: daemon did not report an artifact name\n");
		return 1;
	}

	out = fopen(out_path, "wb");
	if (out == NULL) {
		fprintf(stderr, "cixctl: cannot write %s: %s\n", out_path, strerror(errno));
		return 1;
	}
	while (got < size) {
		long long want = size - got;

		if (want > ARTIFACT_EXPORT_CHUNK)
			want = ARTIFACT_EXPORT_CHUNK;
		snprintf(path, sizeof(path),
		         CIX_API_downloadPkgArtifactExport "?offset=%lld&length=%lld", name, got, want);
		if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			fclose(out);
			return 1;
		}
		if (r.status != 200) {
			fprintf(stderr, "cixctl: download failed at offset %lld (HTTP %d)\n", got,
			        r.status);
			cix_response_free(&r);
			fclose(out);
			return 1;
		}
		if (r.body_len == 0) {
			cix_response_free(&r);
			break;
		}
		fwrite(r.body, 1, r.body_len, out);
		got += (long long)r.body_len;
		cix_response_free(&r);
	}
	fclose(out);
	if (got != size) {
		fprintf(stderr, "cixctl: wrote %lld of %lld bytes to %s\n", got, size, out_path);
		return 1;
	}
	printf("%s (%lld bytes)\n", out_path, got);
	return 0;
}

static int cmd_pkg_recipes(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listPkgRecipes_METHOD, CIX_API_listPkgRecipes, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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
static int cmd_pkg_recipe_add(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *file = NULL;
	char *content;
	size_t content_len;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else {
			fprintf(stderr, "cixctl: unknown pkg recipe add option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL || file == NULL) {
		fprintf(stderr, "usage: cixctl pkg recipe add --name=NAME --file=PATH\n");
		return 2;
	}
	if (read_local_file(file, &content, &content_len) != 0) {
		fprintf(stderr, "cixctl: could not read %s\n", file);
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

	if (cix_client_request(c, CIX_API_addPkgRecipe_METHOD, CIX_API_addPkgRecipe, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status < 200 || r.status >= 300) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: %s (HTTP %d)\n", msg != NULL ? msg : "request failed",
		        r.status);
		cix_response_free(&r);
		return 1;
	}
	printf("recipe '%s' added\n", name);
	cix_response_free(&r);
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
static int cmd_pkg_recipe_show(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
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
			fprintf(stderr, "cixctl: unknown pkg recipe show option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl pkg recipe show NAME [--version=VERSION]\n");
		return 2;
	}
	if (version != NULL)
		snprintf(path, sizeof(path), CIX_API_getPkgRecipe "?version=%s", name, version);
	else
		snprintf(path, sizeof(path), CIX_API_getPkgRecipe, name);
	if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_recipe_show);
}

/* No --version= removes every published version of NAME; a specific
 * version removes only that one (ADR-0107). */
static int cmd_pkg_recipe_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
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
			fprintf(stderr, "cixctl: unknown pkg recipe rm option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl pkg recipe rm NAME [--version=VERSION]\n");
		return 2;
	}
	if (version != NULL)
		snprintf(path, sizeof(path), CIX_API_deletePkgRecipe "?version=%s", name, version);
	else
		snprintf(path, sizeof(path), CIX_API_deletePkgRecipe, name);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_removed);
}

static int cmd_pkg_recipe(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg recipe add --name=NAME --file=PATH\n"
		                "       cixctl pkg recipe show NAME [--version=VERSION]\n"
		                "       cixctl pkg recipe rm NAME [--version=VERSION]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "add") == 0)
		return cmd_pkg_recipe_add(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "show") == 0)
		return cmd_pkg_recipe_show(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "rm") == 0)
		return cmd_pkg_recipe_rm(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown pkg recipe subcommand '%s'\n", sub);
	return 2;
}

static int cmd_pkg_install(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	const char *version = NULL;
	int upgrade = 0;
	int keep_on_failure = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else if (strcmp(argv[i], "--keep-on-failure") == 0)
			keep_on_failure = 1;
		else {
			fprintf(stderr, "cixctl: unknown pkg install option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr,
		        "usage: cixctl pkg install --name=NAME [--image=IMAGE] "
		        "[--version=VERSION] [--upgrade] [--keep-on-failure]\n");
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
	/* ADR-0175/issue #35: a failed build's own container is left
	 * registered/mounted for inspection instead of torn down -- see
	 * GET .../files and DELETE .../containers/{kept_build_container}. */
	if (keep_on_failure) {
		jw_key(&w, "keep_on_failure");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_pkgInstall_METHOD, CIX_API_pkgInstall, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_line);
}

/*
 * Issue #213: stop an in-flight build.
 *
 * The signal to act on is GET /v1/pkg's own last_output_seconds_ago --
 * a build silent for an hour is either gcc linking or something waiting
 * on stdin, and a person can tell those apart where a timeout cannot.
 * That is why this is a verb and not a daemon-side watchdog.
 */
static int cmd_pkg_cancel(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else {
			fprintf(stderr, "cixctl: unknown pkg cancel option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr, "usage: cixctl pkg cancel --name=NAME [--image=IMAGE]\n");
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
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_pkgCancel_METHOD, CIX_API_pkgCancel, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_line);
}

/*
 * ADR-0177/issue #46: resumes a build container a prior `--keep-on-
 * failure` attempt (either pkg install or pkg hostbuild) left preserved
 * instead of paying for a full fetch+extract+build restart -- the real,
 * repeated cost this exists to eliminate on a long bootstrap build
 * (e.g. gcc.recipe). --image addresses an ordinary install's entry the
 * same way pkg install's own --image does; omit it (or pass the
 * reserved "__hostbuild" image) to resume a hostbuild entry instead.
 * The entry must currently be in the "failed" state with a real kept
 * build container (GET /v1/pkg or /v1/pkg/hostbuild/NAME shows this) --
 * there is nothing to resume otherwise.
 */
static int cmd_pkg_resume(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	const char *version = NULL;
	int keep_on_failure = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strcmp(argv[i], "--keep-on-failure") == 0)
			keep_on_failure = 1;
		else {
			fprintf(stderr, "cixctl: unknown pkg resume option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (name == NULL) {
		fprintf(stderr,
		        "usage: cixctl pkg resume --name=NAME [--image=IMAGE] "
		        "[--version=VERSION] [--keep-on-failure]\n");
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
	/* ADR-0107: omitted resolves to the highest available version --
	 * lets a resume pick up a newly-published, fixed recipe version
	 * without needing to reuse the failed attempt's own exact one. */
	if (version != NULL) {
		jw_key(&w, "version");
		jw_str(&w, version);
	}
	if (keep_on_failure) {
		jw_key(&w, "keep_on_failure");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_pkgResume_METHOD, CIX_API_pkgResume, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	return emit(&r, json_mode, fmt_pkg_line);
}

/*
 * A request that tolerates a daemon which is momentarily too busy to
 * answer (#252).
 *
 * The deploy path polls for minutes while the host is doing the
 * heaviest work it ever does -- a build container, then a bootroot
 * assembly. A single missed request in that window used to abort the
 * whole thing with "could not reach daemon", and the deploy step never
 * ran: the package installed, nothing was staged, and a reboot came
 * back on the old version. Observed on three deploys out of six.
 *
 * A poll loop that runs for minutes has no business giving up on one
 * failure. Retries briefly and only then reports failure, so a real
 * outage is still reported -- just not a hiccup.
 */
#define POLL_RETRY_ATTEMPTS 20
#define POLL_RETRY_SLEEP_US 500000

static int poll_request(const struct cix_client *c, const char *method, const char *path,
                         struct cix_response *out)
{
	int attempt;

	for (attempt = 0; attempt < POLL_RETRY_ATTEMPTS; attempt++) {
		if (cix_client_request(c, method, path, NULL, out) == 0)
			return 0;
		usleep(POLL_RETRY_SLEEP_US);
	}
	return -1;
}

/* Polls GET /v1/pkg/hostbuild/name until state leaves "fetching"/
 * "building" (--wait's own loop, and --deploy's own prerequisite --
 * it needs the finished artifact_path, not the 202's own in-flight
 * snapshot). Prints nothing itself; *out is the final response,
 * caller-owned (cix_response_free()'d by the caller). Returns 0 on a
 * real terminal state (installed/failed), -1 if the daemon became
 * unreachable mid-poll. */
static int poll_hostbuild(const struct cix_client *c, const char *name, struct cix_response *out)
{
	char path[300];

	snprintf(path, sizeof(path), CIX_API_getPkgHostbuild, name);
	for (;;) {
		const char *state;

		if (poll_request(c, "GET", path, out) != 0) {
			fprintf(stderr,
			        "cixctl: daemon did not answer after %d attempts while waiting for the "
			        "build -- nothing was deployed\n",
			        POLL_RETRY_ATTEMPTS);
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || (strcmp(state, "fetching") != 0 && strcmp(state, "building") != 0))
			return 0;
		cix_response_free(out);
		usleep(500000);
	}
}

/*
 * Real GET /system/boot read for the cix bootroot-assembly
 * generation counters (task #737, ADR-0058/ADR-0104-adjacent fix).
 * Returns 0 with *out_completed/*out_running filled, -1 if the daemon
 * became unreachable.
 */
static int get_bootroot_assembly_generation(const struct cix_client *c, long *out_completed,
                                             int *out_running, char *out_image_path,
                                             size_t out_image_path_size)
{
	struct cix_response r;
	const struct json_value *jrunning;
	const char *image_path;

	/* Assembly state moved off /system/boot to its own endpoint
	 * (#182, ADR-0230): that endpoint's subject is what booted, not
	 * what is being built. */
	if (poll_request(c, CIX_API_getSystemAssembly_METHOD, CIX_API_getSystemAssembly, &r) != 0) {
		fprintf(stderr,
		        "cixctl: daemon did not answer after %d attempts while waiting for the bootroot "
		        "assembly -- nothing was deployed\n",
		        POLL_RETRY_ATTEMPTS);
		return -1;
	}
	*out_completed = (long)json_as_number(json_object_get(r.json, "completed_generation"));
	jrunning = json_object_get(r.json, "running");
	*out_running = jrunning != NULL && jrunning->type == JSON_BOOL && jrunning->u.boolean;
	/*
	 * The daemon reports where it put the assembled image (#178). This
	 * client used to compose that path itself from the artifact
	 * directory, which stopped being true when the image moved out of
	 * the artifact -- and was always the CLI knowing a server-side
	 * filesystem layout it has no business knowing.
	 */
	if (out_image_path != NULL) {
		out_image_path[0] = '\0';
		image_path = json_as_string(json_object_get(r.json, "image_path"));
		if (image_path != NULL)
			snprintf(out_image_path, out_image_path_size, "%s", image_path);
	}
	cix_response_free(&r);
	return 0;
}

/*
 * The real fix for task #737: `cixd-root.squashfs` existing is not
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
static int wait_for_fresh_bootroot_assembly(const struct cix_client *c, long baseline_completed,
                                            char *out_image_path, size_t out_image_path_size)
{
	for (;;) {
		long completed;
		int running;

		if (get_bootroot_assembly_generation(c, &completed, &running, out_image_path,
		                                     out_image_path_size) != 0)
			return -1;
		if (completed > baseline_completed)
			return 0;
		if (!running) {
			fprintf(stderr,
			        "cixctl: cix bootroot assembly did not produce a fresh artifact "
			        "(see GET /system/logs for the real failure)\n");
			return -1;
		}
		usleep(500000);
	}
}

static int cmd_pkg_hostbuild(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *name = NULL;
	const char *build_image = NULL;
	const char *version = NULL;
	int wait = 0, deploy = 0, upgrade = 0, keep_on_failure = 0;
	int i;
	struct json_writer w;
	struct cix_response r;
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
		else if (strcmp(argv[i], "--keep-on-failure") == 0)
			keep_on_failure = 1;
		else if (name == NULL)
			name = argv[i];
		else {
			fprintf(stderr, "cixctl: unknown pkg hostbuild option '%s'\n", argv[i]);
			return 2;
		}
	}
	/*
	 * --build-image is optional when the recipe declares one (#182):
	 * the daemon resolves it, and refuses a caller that names a
	 * different image rather than letting the mismatch fail minutes
	 * later inside a build container.
	 */
	if (name == NULL) {
		fprintf(stderr,
		        "usage: cixctl pkg hostbuild NAME [--build-image=IMAGE] [--version=VERSION] "
		        "[--wait] [--deploy] [--upgrade] [--keep-on-failure]\n");
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
	 * baseline" to a check done later. "cix" only: every other
	 * hostbuild name deploys straight from its own artifact_path with
	 * no server-side follow-on assembly to wait for.
	 */
	if (deploy && strcmp(name, "cix") == 0) {
		int running;

		if (get_bootroot_assembly_generation(c, &bootroot_baseline_completed, &running,
		                                     NULL, 0) != 0)
			return 1;
	}

	jw_init(&w);
	jw_obj_open(&w);
	jw_key(&w, "name");
	jw_str(&w, name);
	/* Omitted entirely when not given, so the daemon applies the
	 * recipe's own declaration rather than receiving an empty string. */
	if (build_image != NULL) {
		jw_key(&w, "build_image");
		jw_str(&w, build_image);
	}
	if (version != NULL) {
		jw_key(&w, "version");
		jw_str(&w, version);
	}
	jw_key(&w, "upgrade");
	jw_bool(&w, upgrade);
	/* ADR-0175/issue #35: see cmd_pkg_install()'s own identical field. */
	if (keep_on_failure) {
		jw_key(&w, "keep_on_failure");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_pkgHostbuild_METHOD, CIX_API_pkgHostbuild, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300)
		return emit(&r, json_mode, fmt_pkg_line);

	if (!wait)
		return emit(&r, json_mode, fmt_pkg_line);
	cix_response_free(&r);

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
			cix_response_free(&r);
			fprintf(stderr, "cixctl: hostbuild did not produce an artifact to deploy\n");
			return 1;
		}
		/* Honest, explicit per-name handling -- only what this
		 * mechanism has real recipes for today gets a real deploy
		 * path, not a fake-generic dispatcher pretending to support
		 * every possible hostbuild recipe name. */
		if (strcmp(name, "kernel") == 0)
			snprintf(deploy_arg, sizeof(deploy_arg), "--kernel=%s/bzImage", artifact_path);
		else if (strcmp(name, "cix") == 0) {
			/*
			 * cixd-root.squashfs is assembled server-side by the
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
			char bootroot_image[PATH_MAX];

			if (wait_for_fresh_bootroot_assembly(c, bootroot_baseline_completed,
			                                     bootroot_image,
			                                     sizeof(bootroot_image)) != 0) {
				cix_response_free(&r);
				return 1;
			}
			if (bootroot_image[0] == '\0') {
				cix_response_free(&r);
				fprintf(stderr,
				        "cixctl: the daemon did not report where it put the "
				        "assembled image (bootroot_image_path) -- it predates "
				        "the field\n");
				return 1;
			}
			snprintf(deploy_arg, sizeof(deploy_arg), "--image=%s", bootroot_image);
		} else {
			cix_response_free(&r);
			fprintf(stderr, "cixctl: --deploy has no rule for hostbuild '%s' yet\n", name);
			return 1;
		}
		cix_response_free(&r);
		deploy_argv[0] = deploy_arg;
		rc = cmd_update(c, json_mode, 1, deploy_argv);
		return rc;
	}
}

/* cixctl kmod-build --build-image=IMAGE [--version=VERSION]
 * [--symbol=CONFIG_FOO ...] [--upgrade] [--wait]  -- ADR-0159 Phase B:
 * an ordinary `pkg hostbuild kernel` under the hood, reusing
 * poll_hostbuild()/fmt_pkg_line() completely unmodified (this is the
 * exact same "kernel" hostbuild entry `pkg hostbuild kernel` itself
 * produces, GET /v1/pkg/hostbuild/kernel either way). */
#define CLI_KMOD_BUILD_MAX_SYMBOLS 16

static int cmd_kmod_build(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *build_image = NULL;
	const char *version = NULL;
	const char *symbols[CLI_KMOD_BUILD_MAX_SYMBOLS];
	int symbol_count = 0;
	int wait = 0, upgrade = 0, keep_on_failure = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--build-image=", 14) == 0)
			build_image = argv[i] + 14;
		else if (strncmp(argv[i], "--version=", 10) == 0)
			version = argv[i] + 10;
		else if (strncmp(argv[i], "--symbol=", 9) == 0) {
			if (symbol_count >= CLI_KMOD_BUILD_MAX_SYMBOLS) {
				fprintf(stderr, "cixctl: too many --symbol= flags (max %d)\n",
				        CLI_KMOD_BUILD_MAX_SYMBOLS);
				return 2;
			}
			symbols[symbol_count++] = argv[i] + 9;
		} else if (strcmp(argv[i], "--upgrade") == 0)
			upgrade = 1;
		else if (strcmp(argv[i], "--wait") == 0)
			wait = 1;
		else if (strcmp(argv[i], "--keep-on-failure") == 0)
			keep_on_failure = 1;
		else {
			fprintf(stderr, "cixctl: unknown kmod-build option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (build_image == NULL) {
		fprintf(stderr, "usage: cixctl kmod-build --build-image=IMAGE [--version=VERSION] "
		                "[--symbol=CONFIG_FOO ...] [--upgrade] [--wait] [--keep-on-failure]\n");
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
	/* ADR-0175/issue #35: the primary motivating use case for this flag
	 * -- pulling a real crash artifact out of a failed kernel/module
	 * host build for offline debugging (e.g. issue #32). */
	if (keep_on_failure) {
		jw_key(&w, "keep_on_failure");
		jw_bool(&w, 1);
	}
	jw_obj_close(&w);
	w.buf[w.len] = '\0';

	if (cix_client_request(c, CIX_API_postKmodBuild_METHOD, CIX_API_postKmodBuild, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300)
		return emit(&r, json_mode, fmt_pkg_line);
	if (!wait)
		return emit(&r, json_mode, fmt_pkg_line);
	cix_response_free(&r);

	if (poll_hostbuild(c, "kernel", &r) != 0)
		return 1;
	return emit(&r, json_mode, fmt_pkg_line);
}

static int cmd_pkg_ls(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listPkg_METHOD, CIX_API_listPkg, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_list);
}

/*
 * What publishing a recipe has committed this host to (#236).
 *
 * A publish queues a rebuild for every image tracking that package
 * `rolling`, and nothing used to report it -- so the work existed and
 * the operator could neither see it nor count it.
 */
static void fmt_pkg_rebuilds(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "queued");
	const struct json_value *jd = json_object_get(root, "depth");
	const struct json_value *jc = json_object_get(root, "capacity");
	long long depth = (jd != NULL && jd->type == JSON_NUMBER) ? (long long)jd->u.number : 0;
	long long capacity = (jc != NULL && jc->type == JSON_NUMBER) ? (long long)jc->u.number : 0;
	size_t i;

	if (depth == 0) {
		printf("no image rebuilds queued\n");
		return;
	}
	printf("%-32s %s\n", "IMAGE", "POSITION");
	if (arr != NULL && arr->type == JSON_ARRAY) {
		for (i = 0; i < arr->u.array.count; i++) {
			const struct json_value *v = arr->u.array.items[i];

			if (v != NULL && v->type == JSON_STRING)
				printf("%-32s %zu\n", v->u.string, i + 1);
		}
	}
	printf("\n%lld queued, capacity %lld", depth, capacity);
	if (capacity > 0 && depth >= capacity)
		printf("  -- FULL: further rebuilds are being discarded");
	printf("\n");
}

static int cmd_pkg_rebuilds(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_listPkgRebuilds_METHOD, CIX_API_listPkgRebuilds, NULL,
	                        &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_rebuilds);
}

static int cmd_pkg_rm(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 1) {
		fprintf(stderr, "cixctl: pkg rm requires a package name\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_deletePkg, argv[0]);
	if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
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

static int cmd_pkg_update_all(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_pkgUpdateAll_METHOD, CIX_API_pkgUpdateAll, "{}", &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pkg_update_all);
}

/* task #676: live-tail the currently in-flight build's own output --
 * a thin wrapper, all the real work is cix_pkg_build_log_run()'s own
 * WS relay (client/src/console.c), same "cmd_* just calls the client
 * library" shape every other pkg subcommand here already has. */
/*
 * #245: a value goes straight into the request line's query string, so
 * anything that could terminate or extend it is refused here rather
 * than sent. Package and image names are already constrained to this
 * set by the API contract, so a rejection means a typo, not a
 * limitation.
 */
static int query_token_ok(const char *v)
{
	size_t i;

	for (i = 0; v[i] != '\0'; i++) {
		if (!isalnum((unsigned char)v[i]) && strchr("._@+-", v[i]) == NULL)
			return 0;
	}
	return i > 0;
}

static int cmd_pkg_build_log(const struct cix_client *c, int argc, char **argv)
{
	const char *name = NULL;
	const char *image = NULL;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--name=", 7) == 0)
			name = argv[i] + 7;
		else if (strncmp(argv[i], "--image=", 8) == 0)
			image = argv[i] + 8;
		else {
			fprintf(stderr, "cixctl: unknown pkg build-log option '%s'\n", argv[i]);
			return 2;
		}
	}
	if (image != NULL && name == NULL) {
		fprintf(stderr, "cixctl: pkg build-log --image= needs --name= too\n");
		return 2;
	}
	if (name != NULL && !query_token_ok(name)) {
		fprintf(stderr, "cixctl: '%s' is not a valid package name\n", name);
		return 2;
	}
	if (image != NULL && !query_token_ok(image)) {
		fprintf(stderr, "cixctl: '%s' is not a valid image name\n", image);
		return 2;
	}
	return cix_pkg_build_log_run(c, name, image) == 0 ? 0 : 1;
}

/*
 * Issue #11: cixctl container edit NAME --field=VALUE ... -- edits the
 * stored definition in place instead of delete-and-recreate. Applies at
 * the container's next start, which the output states rather than
 * leaving the operator to infer.
 */
static void fmt_container_patch(const struct json_value *v)
{
	const struct json_value *rr = json_object_get(v, "restart_required");

	printf("%s: definition updated, applies %s%s\n",
	       json_str_field(v, "name") != NULL ? json_str_field(v, "name") : "-",
	       json_str_field(v, "applies") != NULL ? json_str_field(v, "applies") : "?",
	       (rr != NULL && rr->type == JSON_BOOL && rr->u.boolean)
	           ? " -- it is running now, so restart it to pick this up"
	           : "");
}

static int cmd_container_edit(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	char path[256];

	if (argc < 2 || argv[0][0] == '-') {
		fprintf(stderr, "usage: cixctl container edit NAME --json='{\"env\":{...}}'\n"
		                "  Fields given replace those fields; a field set to null removes it.\n"
		                "  restart/depends_on/services/follow_rolling cannot be edited --\n"
		                "  recreate the container to change those.\n");
		return 2;
	}
	if (strncmp(argv[1], "--json=", 7) != 0) {
		fprintf(stderr, "cixctl: container edit needs --json='{...}'\n");
		return 2;
	}
	snprintf(path, sizeof(path), CIX_API_patchContainer, argv[0]);
	if (cix_client_request(c, "PATCH", path, argv[1] + 7, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_container_patch);
}


/*
 * Issue #57: the persisted logs, as opposed to `pkg build-log`'s live
 * stream. `--last` is the common case by a wide margin -- the build
 * that just failed -- so it is what a bare invocation does.
 */
static void fmt_build_log_list(const struct json_value *v)
{
	const struct json_value *logs = json_object_get(v, "logs");
	size_t i;

	if (logs == NULL || logs->type != JSON_ARRAY || logs->u.array.count == 0) {
		printf("no build logs recorded yet\n");
		return;
	}
	for (i = 0; i < logs->u.array.count; i++) {
		const struct json_value *l = logs->u.array.items[i];

		printf("%-52s %10lld bytes  modified=%lld\n",
		       json_str_field(l, "file") != NULL ? json_str_field(l, "file") : "-",
		       (long long)json_as_number(json_object_get(l, "size_bytes")),
		       (long long)json_as_number(json_object_get(l, "modified_at")));
	}
}

static int cmd_pkg_build_logs(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *file = NULL;
	int want_last = 0;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--file=", 7) == 0)
			file = argv[i] + 7;
		else if (strcmp(argv[i], "--last") == 0)
			want_last = 1;
		else {
			fprintf(stderr, "usage: cixctl pkg build-logs [--last | --file=NAME]\n");
			return 2;
		}
	}

	if (cix_client_request(c, CIX_API_listBuildLogs_METHOD, CIX_API_listBuildLogs, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (file == NULL && want_last) {
		const struct json_value *logs = r.json != NULL ? json_object_get(r.json, "logs") : NULL;
		static char newest[256];

		if (logs == NULL || logs->type != JSON_ARRAY || logs->u.array.count == 0) {
			cix_response_free(&r);
			fprintf(stderr, "cixctl: no build logs recorded yet\n");
			return 1;
		}
		/* The list is already newest-first, server-side. */
		snprintf(newest, sizeof(newest), "%s",
		         json_str_field(logs->u.array.items[0], "file") != NULL
		             ? json_str_field(logs->u.array.items[0], "file")
		             : "");
		file = newest;
	}
	if (file == NULL)
		return emit(&r, json_mode, fmt_build_log_list);
	cix_response_free(&r);

	{
		char path[512];

		snprintf(path, sizeof(path), CIX_API_getBuildLog, file);
		if (cix_client_request(c, "GET", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status != 200) {
			fprintf(stderr, "cixctl: no such build log '%s'\n", file);
			cix_response_free(&r);
			return 1;
		}
		fwrite(r.body, 1, r.body_len, stdout);
		if (r.body_len > 0 && r.body[r.body_len - 1] != '\n')
			printf("\n");
		cix_response_free(&r);
	}
	return 0;
}

/*
 * Issue #64: cixctl pkg policy ls|set|clear -- which version an
 * omitted version resolves to, per package. `highest` is the
 * rolling-release default and stays it; `newest` and `pinned` exist for
 * the cases where highest is the wrong answer (a bootstrap chain whose
 * versions were published out of numeric order, most concretely).
 */
static void fmt_pkg_policies(const struct json_value *v)
{
	const struct json_value *arr = json_object_get(v, "policies");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("every package is on the default policy (highest)\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *p = arr->u.array.items[i];
		const char *version = json_str_field(p, "version");

		printf("%-24s %-8s %s\n", json_str_field(p, "name") != NULL ? json_str_field(p, "name") : "-",
		       json_str_field(p, "policy") != NULL ? json_str_field(p, "policy") : "-",
		       version != NULL ? version : "");
	}
}

static int cmd_pkg_policy(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "ls";
	char path[256];

	if (strcmp(sub, "ls") == 0) {
		if (cix_client_request(c, CIX_API_listPkgPolicies_METHOD, CIX_API_listPkgPolicies, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_pkg_policies);
	}
	if (strcmp(sub, "clear") == 0 && argc >= 2) {
		snprintf(path, sizeof(path), CIX_API_clearPkgPolicy, argv[1]);
		if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status != 204) {
			int rc = emit(&r, json_mode, NULL);

			return rc != 0 ? rc : 1;
		}
		cix_response_free(&r);
		printf("%s: back on the default policy (highest)\n", argv[1]);
		return 0;
	}
	if (strcmp(sub, "set") == 0 && argc >= 2) {
		const char *policy = NULL, *version = NULL;
		char body[256];
		int i;

		for (i = 2; i < argc; i++) {
			if (strncmp(argv[i], "--policy=", 9) == 0)
				policy = argv[i] + 9;
			else if (strncmp(argv[i], "--version=", 10) == 0)
				version = argv[i] + 10;
			else {
				fprintf(stderr, "cixctl: unknown pkg policy set option '%s'\n", argv[i]);
				return 2;
			}
		}
		if (policy == NULL) {
			fprintf(stderr, "usage: cixctl pkg policy set NAME --policy=highest|newest|pinned "
			                "[--version=V]\n");
			return 2;
		}
		if (version != NULL)
			snprintf(body, sizeof(body), "{\"policy\":\"%s\",\"version\":\"%s\"}", policy, version);
		else
			snprintf(body, sizeof(body), "{\"policy\":\"%s\"}", policy);
		snprintf(path, sizeof(path), CIX_API_setPkgPolicy, argv[1]);
		if (cix_client_request(c, "PUT", path, body, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_pkg_policies);
	}

	fprintf(stderr, "usage: cixctl pkg policy ls\n"
	                "       cixctl pkg policy set NAME --policy=highest|newest|pinned [--version=V]\n"
	                "       cixctl pkg policy clear NAME\n");
	return 2;
}

static void fmt_upstreams(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "upstreams");
	size_t i, j;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("no discovery kinds are known\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *k = arr->u.array.items[i];
		const struct json_value *ch = json_object_get(k, "channels");
		const char *name = json_str_field(k, "name");
		const char *sum = json_str_field(k, "summary");

		printf("%-14s ", name != NULL ? name : "-");
		/* null channels is "one linear sequence", not "none known" --
		 * they are different facts and the operator needs the right one. */
		if (ch == NULL || ch->type != JSON_ARRAY) {
			printf("%-34s", "(single release sequence)");
		} else {
			char buf[128];
			size_t used = 0;

			buf[0] = '\0';
			for (j = 0; j < ch->u.array.count; j++) {
				const char *cs = json_as_string(ch->u.array.items[j]);
				int n = snprintf(buf + used, sizeof(buf) - used, "%s%s", used > 0 ? "," : "",
				                  cs != NULL ? cs : "");

				if (n < 0 || (size_t)n >= sizeof(buf) - used)
					break;
				used += (size_t)n;
			}
			printf("%-34s", buf);
		}
		printf(" %s\n", sum != NULL ? sum : "");
	}
}

static void fmt_source_catalogue(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "packages");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("no package recipes\n");
		return;
	}
	printf("%-20s %-11s %-10s %-12s %-12s\n", "PACKAGE", "STATE", "CHANNEL", "RESOLVED",
	        "NEWEST RECIPE");
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		const char *state = json_str_field(e, "state");
		const char *channel = json_str_field(e, "channel");
		const char *resolved = json_str_field(e, "resolved_version");
		const char *newest = json_str_field(e, "newest_recipe_version");
		const char *name = json_str_field(e, "name");
		const char *reason = json_str_field(e, "reason");

		printf("%-20s %-11s %-10s %-12s %-12s\n", name != NULL ? name : "-",
		        state != NULL ? state : "-", channel != NULL ? channel : "-",
		        resolved != NULL ? resolved : "-", newest != NULL ? newest : "-");
		/*
		 * The reason is printed only for the rows that need an action.
		 * A `current` package explaining itself on every line would
		 * bury the handful that do not resolve, which are the whole
		 * point of reading this.
		 */
		if (state != NULL && reason != NULL &&
		    (strcmp(state, "missing") == 0 || strcmp(state, "unresolved") == 0))
			printf("  %s\n", reason);
	}
	printf("\n%ld package(s): %ld current, %ld missing, %ld unresolved, %ld pinned\n",
	        (long)json_as_number(json_object_get(root, "total")),
	        (long)json_as_number(json_object_get(root, "current")),
	        (long)json_as_number(json_object_get(root, "missing")),
	        (long)json_as_number(json_object_get(root, "unresolved")),
	        (long)json_as_number(json_object_get(root, "pinned")));
}

static void fmt_source_policy(const struct json_value *root)
{
	const struct json_value *def = json_object_get(root, "default");
	const struct json_value *arr = json_object_get(root, "packages");
	size_t i;

	if (def != NULL) {
		const char *ch = json_str_field(def, "channel");
		const char *d = json_str_field(def, "depth");

		printf("default: channel=%s depth=%s\n", ch != NULL ? ch : "(none)",
		        d != NULL ? d : "n");
	}
	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("every package follows the default\n");
		return;
	}
	printf("\n%-24s %-12s %s\n", "PACKAGE", "CHANNEL", "DEPTH");
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *p = arr->u.array.items[i];
		const char *ch = json_str_field(p, "channel");

		printf("%-24s %-12s %s\n",
		        json_str_field(p, "name") != NULL ? json_str_field(p, "name") : "-",
		        ch != NULL ? ch : "-",
		        json_str_field(p, "depth") != NULL ? json_str_field(p, "depth") : "n");
	}
}

static int cmd_pkg_source_policy(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	const char *sub = argc > 0 ? argv[0] : "ls";
	char path[256];

	if (strcmp(sub, "ls") == 0) {
		if (cix_client_request(c, CIX_API_getSourcePolicy_METHOD, CIX_API_getSourcePolicy, NULL,
		                        &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_source_policy);
	}
	if (strcmp(sub, "clear") == 0 && argc >= 2) {
		snprintf(path, sizeof(path), CIX_API_clearPkgSourcePolicy, argv[1]);
		if (cix_client_request(c, "DELETE", path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		if (r.status != 204) {
			int rc = emit(&r, json_mode, NULL);

			return rc != 0 ? rc : 1;
		}
		cix_response_free(&r);
		printf("%s: back on the default source policy\n", argv[1]);
		return 0;
	}
	if (strcmp(sub, "set") == 0 || strcmp(sub, "set-default") == 0) {
		const char *channel = NULL, *depth = NULL;
		int is_default = (strcmp(sub, "set-default") == 0);
		int first = is_default ? 1 : 2;
		char body[256];
		int i;

		if (!is_default && argc < 2) {
			fprintf(stderr, "usage: cixctl pkg source-policy set NAME [--channel=C] [--depth=D]\n");
			return 2;
		}
		for (i = first; i < argc; i++) {
			if (strncmp(argv[i], "--channel=", 10) == 0)
				channel = argv[i] + 10;
			else if (strncmp(argv[i], "--depth=", 8) == 0)
				depth = argv[i] + 8;
			else {
				fprintf(stderr, "cixctl: unknown source-policy option '%s'\n", argv[i]);
				return 2;
			}
		}
		snprintf(body, sizeof(body), "{\"channel\":%s%s%s,\"depth\":\"%s\"}",
		          channel != NULL ? "\"" : "", channel != NULL ? channel : "null",
		          channel != NULL ? "\"" : "", depth != NULL ? depth : "n");
		if (is_default) {
			if (cix_client_request(c, "PUT", CIX_API_setDefaultSourcePolicy, body, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
		} else {
			snprintf(path, sizeof(path), CIX_API_setPkgSourcePolicy, argv[1]);
			if (cix_client_request(c, "PUT", path, body, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
		}
		return emit(&r, json_mode, fmt_source_policy);
	}

	fprintf(stderr,
	        "usage: cixctl pkg source-policy ls\n"
	        "       cixctl pkg source-policy set-default [--channel=C] [--depth=n-<lines>.<releases>]\n"
	        "       cixctl pkg source-policy set NAME [--channel=C] [--depth=D]\n"
	        "       cixctl pkg source-policy clear NAME\n"
	        "  channels come from the package's own upstream -- see `cixctl pkg upstreams`\n");
	return 2;
}

/*
 * ADR-0256: the pipeline, as a flow and then as a worklist.
 *
 * The default hides `ok` and `not-implemented` rows. On this platform
 * that is most of them -- a pinned package sits permanently at
 * discover/not-implemented, which is correct and is not news -- and a
 * screen of 115 identical rows buries the handful that need someone to
 * do something. `--all` shows every row.
 */
static int g_pipeline_show_all;

static const char *onoff(const struct json_value *root, const char *key)
{
	const struct json_value *v = json_object_get(root, key);

	return (v != NULL && v->type == JSON_BOOL && v->u.boolean) ? "on" : "off";
}

static void fmt_pipeline_config(const struct json_value *root)
{
	printf("run retention   %ld runs\n",
	        (long)json_as_number(json_object_get(root, "run_retention")));
	/* ADR-0273: named for what each holds, not for where it sits. */
	printf("gate publish    %-3s  (an artifact reaching the shared cache)\n",
	        onoff(root, "gate_publish"));
	printf("gate roll       %-3s  (an image moving to a version nobody asked for)\n",
	        onoff(root, "gate_roll"));
	printf("gate deploy     %-3s  (a boot slot being written)\n", onoff(root, "gate_deploy"));
}

/* ADR-0273. Two lists with different natures: pending is derived from
 * the live queues and stored nowhere, granted is the stored half. */
static void fmt_pipeline_approvals(const struct json_value *root)
{
	const struct json_value *pend = json_object_get(root, "pending");
	const struct json_value *gr = json_object_get(root, "granted");
	size_t i;

	if (pend == NULL || pend->type != JSON_ARRAY || pend->u.array.count == 0) {
		printf("nothing is waiting for approval\n");
	} else {
		printf("%-10s %s\n", "GATE", "WAITING FOR APPROVAL");
		for (i = 0; i < pend->u.array.count; i++)
			printf("%-10s %s\n", json_str_field(pend->u.array.items[i], "gate"),
			        json_str_field(pend->u.array.items[i], "target"));
	}
	if (gr != NULL && gr->type == JSON_ARRAY && gr->u.array.count > 0) {
		printf("\n%-10s %-40s %s\n", "GATE", "APPROVED, NOT YET THROUGH", "BY");
		for (i = 0; i < gr->u.array.count; i++)
			printf("%-10s %-40s %s\n", json_str_field(gr->u.array.items[i], "gate"),
			        json_str_field(gr->u.array.items[i], "target"),
			        json_str_field(gr->u.array.items[i], "who"));
	}
}

/*
 * ADR-0272: what has happened to an atom, newest first.
 *
 * The log column says the truth about the file rather than its name
 * alone: build logs are pruned at forty and runs are kept for a
 * thousand, so "(pruned)" is the ordinary answer for anything but the
 * last day or so, and printing a filename that is not there would send
 * an operator looking for it.
 */
static void fmt_pipeline_runs(const struct json_value *root)
{
	const struct json_value *runs = json_object_get(root, "runs");
	size_t i;

	if (runs == NULL || runs->type != JSON_ARRAY || runs->u.array.count == 0) {
		printf("no runs recorded\n");
		return;
	}
	printf("%-20s %-16s %-10s %-8s %5s  %s\n", "PACKAGE", "IMAGE", "STATUS", "TRIGGER", "SECS",
	        "LOG");
	for (i = 0; i < runs->u.array.count; i++) {
		const struct json_value *r = runs->u.array.items[i];
		const char *status = json_str_field(r, "status");
		const char *stage = json_str_field(r, "stage");
		const char *log = json_str_field(r, "log");
		const struct json_value *avail = json_object_get(r, "log_available");
		const char *err = json_str_field(r, "error");
		char what[24];

		/* A failure is reported as the stage it failed at: "failed"
		 * alone is the thing ADR-0256 exists to stop saying. */
		if (status != NULL && strcmp(status, "ok") != 0 && stage != NULL)
			snprintf(what, sizeof(what), "%s/%s", stage, status);
		else
			snprintf(what, sizeof(what), "%s", status != NULL ? status : "-");

		printf("%-20s %-16s %-10s %-8s %5ld  %s\n", json_str_field(r, "name"),
		        json_str_field(r, "image"), what, json_str_field(r, "trigger"),
		        (long)json_as_number(json_object_get(r, "duration_seconds")),
		        log == NULL || log[0] == '\0'
		            ? "(none)"
		            : (avail != NULL && avail->type == JSON_BOOL && avail->u.boolean ? log : "(pruned)"));
		if (err != NULL && err[0] != '\0')
			printf("    %s\n", err);
	}
	printf("\n%ld run(s) held, retention %ld\n",
	        (long)json_as_number(json_object_get(root, "total")),
	        (long)json_as_number(json_object_get(root, "retention")));
}

static void fmt_pipeline(const struct json_value *root)
{
	const struct json_value *stages = json_object_get(root, "stages");
	const struct json_value *pkgs = json_object_get(root, "packages");
	const struct json_value *deploy = json_object_get(root, "deploy");
	size_t i;
	int shown = 0;

	if (stages != NULL && stages->type == JSON_ARRAY) {
		printf("%-14s %s\n", "STAGE", "PACKAGES");
		for (i = 0; i < stages->u.array.count; i++) {
			const struct json_value *st = stages->u.array.items[i];
			long count = (long)json_as_number(json_object_get(st, "packages"));
			const char *name = json_str_field(st, "stage");
			int bars = count > 40 ? 40 : (int)count;
			char bar[41];

			memset(bar, '#', (size_t)bars);
			bar[bars] = '\0';
			printf("%-14s %5ld %s\n", name != NULL ? name : "-", count, bar);
		}
	}

	if (deploy != NULL) {
		const char *entry = json_str_field(deploy, "entry");

		printf("\ndeploy         %s/%s  %s\n", json_str_field(deploy, "stage"),
		        json_str_field(deploy, "status"),
		        entry != NULL && entry[0] != '\0' ? entry : "(no A/B slot)");
		printf("  %s\n", json_str_field(deploy, "reason"));
	}

	if (pkgs != NULL && pkgs->type == JSON_ARRAY) {
		for (i = 0; i < pkgs->u.array.count; i++) {
			const struct json_value *e = pkgs->u.array.items[i];
			const char *status = json_str_field(e, "status");
			const char *stage = json_str_field(e, "stage");
			const char *reason = json_str_field(e, "reason");

			if (status == NULL)
				continue;
			if (!g_pipeline_show_all &&
			    (strcmp(status, "ok") == 0 || strcmp(status, "not-implemented") == 0))
				continue;
			if (shown == 0)
				printf("\n%-22s %-10s %-16s %s\n", "PACKAGE", "STAGE", "STATUS", "");
			shown++;
			printf("%-22s %-10s %-16s\n", json_str_field(e, "name"),
			        stage != NULL ? stage : "-", status);
			if (reason != NULL && reason[0] != '\0')
				printf("  %s\n", reason);
		}
	}

	printf("\n%ld package(s): %ld ok, %ld blocked, %ld failed, %ld cancelled, "
	        "%ld not-implemented\n",
	        (long)json_as_number(json_object_get(root, "total")),
	        (long)json_as_number(json_object_get(root, "ok")),
	        (long)json_as_number(json_object_get(root, "blocked")),
	        (long)json_as_number(json_object_get(root, "failed")),
	        (long)json_as_number(json_object_get(root, "cancelled")),
	        (long)json_as_number(json_object_get(root, "not_implemented")));
	if (!g_pipeline_show_all && shown == 0)
		printf("nothing is blocked or failed -- `cixctl pipeline --all` shows every row\n");
}

/* ---------- schedules (ADR-0257) ---------- */

/*
 * Flags map one-to-one onto the wire fields. There is deliberately no
 * "--every=6h" style shorthand: that would be a second grammar living
 * only in the CLI, undocumented by the contract and able to drift from
 * it, which is precisely what a structured wire format exists to avoid.
 * The rendered "describes" string goes the other way and is never sent
 * back.
 */
static void fmt_schedules(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "schedules");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("nothing is scheduled\n");
		return;
	}
	printf("%-22s %-26s %-26s %-10s %s\n", "NAME", "ACTION", "WHEN", "LAST", "REASON");
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *e = arr->u.array.items[i];
		const struct json_value *ok = json_object_get(e, "last_ok");
		const char *last;

		if (ok == NULL || ok->type == JSON_NULL)
			last = "never";
		else
			last = json_bool_field(e, "last_ok") ? "ok" : "FAILED";
		printf("%-22s %-26s %-26s %-10s %s\n", json_str_or(e, "name"),
		        json_str_or(e, "action"), json_str_or(e, "describes"), last,
		        json_str_or(e, "last_reason"));
	}
}

static void fmt_schedule_one(const struct json_value *root)
{
	const struct json_value *ok = json_object_get(root, "last_ok");

	printf("%-16s %s\n", "name", json_str_or(root, "name"));
	printf("%-16s %s\n", "action", json_str_or(root, "action"));
	printf("%-16s %s\n", "when", json_str_or(root, "describes"));
	printf("%-16s %s\n", "last run",
	        ok == NULL || ok->type == JSON_NULL
	            ? "never"
	            : (json_bool_field(root, "last_ok") ? "ok" : "FAILED"));
	printf("%-16s %s\n", "reason", json_str_or(root, "last_reason"));
}

static void fmt_schedule_actions(const struct json_value *root)
{
	const struct json_value *arr = json_object_get(root, "actions");
	size_t i;

	if (arr == NULL || arr->type != JSON_ARRAY || arr->u.array.count == 0) {
		printf("no actions are registered\n");
		return;
	}
	for (i = 0; i < arr->u.array.count; i++) {
		const struct json_value *a = arr->u.array.items[i];

		printf("%-26s %s\n", json_str_or(a, "name"), json_str_or(a, "summary"));
	}
}

static void schedule_usage(void)
{
	fprintf(stderr,
	        "usage: cixctl schedule ls\n"
	        "       cixctl schedule actions\n"
	        "       cixctl schedule show NAME\n"
	        "       cixctl schedule rm NAME\n"
	        "       cixctl schedule run NAME   -- run it now, even if disabled\n"
	        "       cixctl schedule set NAME --action=A <when> [options]\n"
	        "  when (exactly one):\n"
	        "       --every-seconds=N | --every-minutes=N | --every-hours=N | --every-days=N\n"
	        "       --daily-at=HH:MM\n"
	        "       --weekly-on=sun|mon|... --weekly-at=HH:MM\n"
	        "  options:\n"
	        "       --window-minutes=N  how long an action may keep starting work\n"
	        "       --catch-up          run once at startup if the time passed while down\n"
	        "       --disabled          store it, but do not run it\n"
	        "  actions come from `cixctl schedule actions` -- never free text\n");
}

static int cmd_schedule(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	/* argv[0] is the subcommand: dispatch_command() passes the command
	 * name separately, so this does not skip an element. */
	const char *sub = argc > 0 ? argv[0] : "ls";
	const char *name = argc > 1 ? argv[1] : NULL;
	char path[256];

	if (strcmp(sub, "ls") == 0) {
		if (cix_client_request(c, CIX_API_listSchedules_METHOD, CIX_API_listSchedules, NULL,
		                        &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_schedules);
	}
	if (strcmp(sub, "actions") == 0) {
		if (cix_client_request(c, CIX_API_listScheduleActions_METHOD,
		                        CIX_API_listScheduleActions, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_schedule_actions);
	}
	if (name == NULL) {
		schedule_usage();
		return 2;
	}
	if (strcmp(sub, "show") == 0) {
		snprintf(path, sizeof(path), CIX_API_getSchedule, name);
		if (cix_client_request(c, CIX_API_getSchedule_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_schedule_one);
	}
	if (strcmp(sub, "rm") == 0) {
		snprintf(path, sizeof(path), CIX_API_deleteSchedule, name);
		if (cix_client_request(c, CIX_API_deleteSchedule_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, NULL);
	}
	if (strcmp(sub, "run") == 0) {
		snprintf(path, sizeof(path), CIX_API_runSchedule, name);
		if (cix_client_request(c, CIX_API_runSchedule_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_schedule_one);
	}
	if (strcmp(sub, "set") == 0) {
		struct json_writer w;
		const char *action = NULL, *daily_at = NULL, *weekly_on = NULL, *weekly_at = NULL;
		long every[4] = { 0, 0, 0, 0 }; /* seconds, minutes, hours, days */
		int have_every = 0, window = 0, catch_up = 0, enabled = 1;
		int i, rc;

		for (i = 2; i < argc; i++) {
			if (strncmp(argv[i], "--action=", 9) == 0)
				action = argv[i] + 9;
			else if (strncmp(argv[i], "--every-seconds=", 16) == 0)
				every[0] = atol(argv[i] + 16), have_every = 1;
			else if (strncmp(argv[i], "--every-minutes=", 16) == 0)
				every[1] = atol(argv[i] + 16), have_every = 1;
			else if (strncmp(argv[i], "--every-hours=", 14) == 0)
				every[2] = atol(argv[i] + 14), have_every = 1;
			else if (strncmp(argv[i], "--every-days=", 13) == 0)
				every[3] = atol(argv[i] + 13), have_every = 1;
			else if (strncmp(argv[i], "--daily-at=", 11) == 0)
				daily_at = argv[i] + 11;
			else if (strncmp(argv[i], "--weekly-on=", 12) == 0)
				weekly_on = argv[i] + 12;
			else if (strncmp(argv[i], "--weekly-at=", 12) == 0)
				weekly_at = argv[i] + 12;
			else if (strncmp(argv[i], "--window-minutes=", 17) == 0)
				window = atoi(argv[i] + 17);
			else if (strcmp(argv[i], "--catch-up") == 0)
				catch_up = 1;
			else if (strcmp(argv[i], "--disabled") == 0)
				enabled = 0;
			else {
				fprintf(stderr, "cixctl: unknown schedule option '%s'\n", argv[i]);
				schedule_usage();
				return 2;
			}
		}
		if (action == NULL) {
			fprintf(stderr, "cixctl: --action is required\n");
			schedule_usage();
			return 2;
		}
		/* Exactly one form, checked here so the obvious mistake gets a
		 * local answer rather than a round trip. The daemon checks it
		 * again regardless -- it is the one that must be right. */
		if (have_every + (daily_at != NULL) + (weekly_on != NULL || weekly_at != NULL) != 1) {
			fprintf(stderr, "cixctl: give exactly one of --every-*, --daily-at or "
			                "--weekly-on/--weekly-at\n");
			return 2;
		}
		jw_init(&w);
		jw_obj_open(&w);
		jw_key(&w, "action");
		jw_str(&w, action);
		jw_key(&w, "schedule");
		jw_obj_open(&w);
		if (have_every) {
			jw_key(&w, "every");
			jw_obj_open(&w);
			if (every[0] != 0) {
				jw_key(&w, "seconds");
				jw_int(&w, every[0]);
			}
			if (every[1] != 0) {
				jw_key(&w, "minutes");
				jw_int(&w, every[1]);
			}
			if (every[2] != 0) {
				jw_key(&w, "hours");
				jw_int(&w, every[2]);
			}
			if (every[3] != 0) {
				jw_key(&w, "days");
				jw_int(&w, every[3]);
			}
			jw_obj_close(&w);
		} else if (daily_at != NULL) {
			jw_key(&w, "daily");
			jw_obj_open(&w);
			jw_key(&w, "at");
			jw_str(&w, daily_at);
			jw_obj_close(&w);
		} else {
			jw_key(&w, "weekly");
			jw_obj_open(&w);
			jw_key(&w, "on");
			jw_str(&w, weekly_on != NULL ? weekly_on : "");
			jw_key(&w, "at");
			jw_str(&w, weekly_at != NULL ? weekly_at : "");
			jw_obj_close(&w);
		}
		jw_obj_close(&w);
		jw_key(&w, "window_minutes");
		jw_int(&w, window);
		jw_key(&w, "catch_up");
		jw_bool(&w, catch_up);
		jw_key(&w, "enabled");
		jw_bool(&w, enabled);
		jw_obj_close(&w);

		snprintf(path, sizeof(path), CIX_API_setSchedule, name);
		rc = cix_client_request(c, CIX_API_setSchedule_METHOD, path, w.buf, &r);
		jw_free(&w);
		if (rc != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_schedule_one);
	}
	schedule_usage();
	return 2;
}

static int cmd_pipeline(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	int i;

	/* argv[0] is the first argument AFTER the command name -- see
	 * dispatch_command(), which passes the command separately. */
	g_pipeline_show_all = 0;

	/* ADR-0273: what is waiting for a person, and what has been let through. */
	if (argc > 0 && strcmp(argv[0], "approvals") == 0) {
		if (cix_client_request(c, CIX_API_getPipelineApprovals_METHOD,
		                        CIX_API_getPipelineApprovals, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_pipeline_approvals);
	}

	/* ADR-0273: let one held change through. */
	if (argc > 0 && strcmp(argv[0], "approve") == 0) {
		char body[512];
		struct json_writer bw;

		if (argc != 3) {
			fprintf(stderr, "usage: cixctl pipeline approve <publish|roll|deploy> <target>\n");
			return 2;
		}
		jw_init(&bw);
		jw_obj_open(&bw);
		jw_key(&bw, "gate");
		jw_str(&bw, argv[1]);
		jw_key(&bw, "target");
		jw_str(&bw, argv[2]);
		jw_obj_close(&bw);
		snprintf(body, sizeof(body), "%s", bw.buf != NULL ? bw.buf : "{}");
		jw_free(&bw);
		if (cix_client_request(c, CIX_API_approvePipeline_METHOD, CIX_API_approvePipeline, body,
		                        &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, NULL);
	}

	/*
	 * ADR-0272: how many runs to keep. A count rather than a duration,
	 * because what an operator wants is "the last N things that
	 * happened" and a duration makes the store's size depend on how
	 * busy the host has been.
	 */
	if (argc > 0 && strcmp(argv[0], "config") == 0) {
		const char *keep = NULL;
		const char *gp = NULL, *grl = NULL, *gd = NULL;

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--run-retention=", 16) == 0)
				keep = argv[i] + 16;
			else if (strncmp(argv[i], "--gate-publish=", 15) == 0)
				gp = argv[i] + 15;
			else if (strncmp(argv[i], "--gate-roll=", 12) == 0)
				grl = argv[i] + 12;
			else if (strncmp(argv[i], "--gate-deploy=", 14) == 0)
				gd = argv[i] + 14;
			else {
				fprintf(stderr,
				        "usage: cixctl pipeline config [--run-retention=N] "
				        "[--gate-publish=on|off] [--gate-roll=on|off] "
				        "[--gate-deploy=on|off]\n");
				return 2;
			}
		}
		if (keep != NULL || gp != NULL || grl != NULL || gd != NULL) {
			char body[256];
			struct json_writer bw;

			jw_init(&bw);
			jw_obj_open(&bw);
			if (keep != NULL) {
				jw_key(&bw, "run_retention");
				jw_int(&bw, atoi(keep));
			}
			if (gp != NULL) {
				jw_key(&bw, "gate_publish");
				jw_bool(&bw, strcmp(gp, "on") == 0);
			}
			if (grl != NULL) {
				jw_key(&bw, "gate_roll");
				jw_bool(&bw, strcmp(grl, "on") == 0);
			}
			if (gd != NULL) {
				jw_key(&bw, "gate_deploy");
				jw_bool(&bw, strcmp(gd, "on") == 0);
			}
			jw_obj_close(&bw);
			snprintf(body, sizeof(body), "%s", bw.buf != NULL ? bw.buf : "{}");
			jw_free(&bw);
			if (cix_client_request(c, CIX_API_setPipelineConfig_METHOD,
			                        CIX_API_setPipelineConfig, body, &r) != 0) {
				fprintf(stderr, "cixctl: could not reach daemon\n");
				return 1;
			}
		} else if (cix_client_request(c, CIX_API_getPipelineConfig_METHOD,
		                               CIX_API_getPipelineConfig, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_pipeline_config);
	}

	/* ADR-0272: `pipeline` is where it stands now, `pipeline runs` is
	 * what has happened to it. */
	if (argc > 0 && strcmp(argv[0], "runs") == 0) {
		char path[512];
		char qname[128] = "";
		char qimage[128] = "";
		char qlimit[32] = "";
		int n = 0;

		for (i = 1; i < argc; i++) {
			if (strncmp(argv[i], "--name=", 7) == 0)
				snprintf(qname, sizeof(qname), "%s", argv[i] + 7);
			else if (strncmp(argv[i], "--image=", 8) == 0)
				snprintf(qimage, sizeof(qimage), "%s", argv[i] + 8);
			else if (strncmp(argv[i], "--limit=", 8) == 0)
				snprintf(qlimit, sizeof(qlimit), "%s", argv[i] + 8);
			else {
				fprintf(stderr,
				        "usage: cixctl pipeline runs [--name=NAME] [--image=IMAGE] "
				        "[--limit=N]\n");
				return 2;
			}
		}
		n = snprintf(path, sizeof(path), "%s", CIX_API_getPipelineRuns);
		if (qname[0] != '\0')
			n += snprintf(path + n, sizeof(path) - (size_t)n, "%cname=%s",
			              n > (int)strlen(CIX_API_getPipelineRuns) ? '&' : '?', qname);
		if (qimage[0] != '\0')
			n += snprintf(path + n, sizeof(path) - (size_t)n, "%cimage=%s",
			              n > (int)strlen(CIX_API_getPipelineRuns) ? '&' : '?', qimage);
		if (qlimit[0] != '\0')
			n += snprintf(path + n, sizeof(path) - (size_t)n, "%climit=%s",
			              n > (int)strlen(CIX_API_getPipelineRuns) ? '&' : '?', qlimit);
		if (cix_client_request(c, CIX_API_getPipelineRuns_METHOD, path, NULL, &r) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		return emit(&r, json_mode, fmt_pipeline_runs);
	}

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--all") == 0) {
			g_pipeline_show_all = 1;
		} else {
			fprintf(stderr, "usage: cixctl pipeline [--all] | cixctl pipeline runs\n");
			return 2;
		}
	}
	if (cix_client_request(c, CIX_API_getPipeline_METHOD, CIX_API_getPipeline, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_pipeline);
}

static int cmd_pkg_source_catalogue(const struct cix_client *c, int json_mode, int argc,
                                     char **argv)
{
	struct cix_response r;

	(void)argc;
	(void)argv;
	if (cix_client_request(c, CIX_API_getSourceCatalogue_METHOD, CIX_API_getSourceCatalogue, NULL,
	                        &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_source_catalogue);
}

static int cmd_pkg_upstreams(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;

	(void)argc;
	(void)argv;
	if (cix_client_request(c, CIX_API_listUpstreamKinds_METHOD, CIX_API_listUpstreamKinds, NULL,
	                        &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_upstreams);
}

static int cmd_pkg(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr, "usage: cixctl pkg bootstrap [--toolchain=PATH]\n"
		                "       cixctl pkg bootstrap --toolchain-url=URL --toolchain-sha256=SHA256 [--wait]\n"
		                "       cixctl pkg bootstrap-status\n"
		                "       cixctl pkg recipes\n"
		                "       cixctl pkg recipe add --name=NAME --file=PATH\n"
		                "       cixctl pkg recipe show NAME [--version=VERSION]\n"
		                "       cixctl pkg recipe rm NAME [--version=VERSION]\n"
		                "       cixctl pkg install --name=NAME [--image=IMAGE] [--version=VERSION] "
		                "[--upgrade] [--keep-on-failure]\n"
		                "       cixctl pkg hostbuild NAME --build-image=IMAGE [--version=VERSION] "
		                "[--wait] [--deploy] [--upgrade] [--keep-on-failure]\n"
		                "       cixctl pkg resume --name=NAME [--image=IMAGE] [--version=VERSION] "
		                "[--keep-on-failure]\n"
		                "       cixctl pkg build-log [--name=NAME [--image=IMAGE]]\n"
		                "       cixctl pkg build-logs [--last | --file=NAME]\n"
		                "       cixctl pkg policy ls | set NAME --policy=... | clear NAME\n"
		                "       cixctl pkg upstreams  -- discovery kinds and their channels\n"
		                "       cixctl pkg source-catalogue  -- what upstream has that we have no recipe for\n"
		                "       cixctl pkg source-policy ls | set-default | set NAME | clear NAME\n"
		                "       cixctl pkg ls\n"
		                "       cixctl pkg rm NAME[@IMAGE]\n"
		                "       cixctl pkg update-all\n"
		                "       cixctl pkg verify  -- which installed packages are not "
		                "actually in their image (#281)\n"
		                "       cixctl pkg repo-config show\n"
		                "       cixctl pkg repo-config set [--url=URL] [--kind=gitea|github|gitlab] "
		                "[--ref=REF] [--token=TOKEN | --clear-token]  (schedule: cixctl schedule)\n"
		                "       cixctl pkg sync [--wait]\n"
		                "       cixctl pkg sync-status\n"
		                "       cixctl pkg cache-config show\n"
		                "       cixctl pkg cache-config set --max-bytes=N\n"
		                "       cixctl pkg cache-status\n"
		                "       cixctl pkg cache-clear\n"
		                "       cixctl pkg artifact-config show\n"
		                "       cixctl pkg artifact-config set [--url=URL] "
		                "[--token=TOKEN | --clear-token] [--push | --no-push]\n"
		                "       cixctl pkg artifact-export NAME [--out=FILE]\n"
		                "       cixctl pkg artifact-publish NAME  -- publish an already-built\n"
		                "               artifact without rebuilding it\n");
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
	if (strcmp(sub, "cancel") == 0)
		return cmd_pkg_cancel(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "resume") == 0)
		return cmd_pkg_resume(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "build-log") == 0)
		return cmd_pkg_build_log(c, argc - 1, argv + 1);
	if (strcmp(sub, "policy") == 0)
		return cmd_pkg_policy(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "source-policy") == 0)
		return cmd_pkg_source_policy(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "upstreams") == 0)
		return cmd_pkg_upstreams(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "source-catalogue") == 0)
		return cmd_pkg_source_catalogue(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "build-logs") == 0)
		return cmd_pkg_build_logs(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "ls") == 0)
		return cmd_pkg_ls(c, json_mode);
	if (strcmp(sub, "rebuilds") == 0)
		return cmd_pkg_rebuilds(c, json_mode);
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
	if (strcmp(sub, "drift") == 0)
		return cmd_pkg_drift(c, json_mode);
	if (strcmp(sub, "verify") == 0)
		return cmd_pkg_verify(c, json_mode);
	if (strcmp(sub, "cache-config") == 0)
		return cmd_pkg_cache_config(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "buildenv") == 0)
		return cmd_pkg_buildenv(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "cache-status") == 0)
		return cmd_pkg_cache_status(c, json_mode);
	if (strcmp(sub, "cache-clear") == 0)
		return cmd_pkg_cache_clear(c, json_mode);
	if (strcmp(sub, "artifact-export") == 0)
		return cmd_pkg_artifact_export(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "artifact-publish") == 0)
		return cmd_pkg_artifact_publish(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "artifact-config") == 0)
		return cmd_pkg_artifact_config(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown pkg subcommand '%s'\n", sub);
	return 2;
}

static void fmt_iso_status(const struct json_value *v)
{
	const char *state = json_str_field(v, "state");
	const char *iso_path = json_str_field(v, "iso_path");
	const char *error = json_str_field(v, "error");

	const char *sig = json_str_field(v, "signature_path");
	const char *pub_state = json_str_field(v, "publish_state");
	const char *pub_name = json_str_field(v, "published_name");
	const char *pub_error = json_str_field(v, "publish_error");

	printf("state=%s", state != NULL ? state : "?");
	if (iso_path != NULL)
		printf(" iso_path=%s", iso_path);
	if (error != NULL)
		printf(" error=%s", error);
	printf("\n");
	/*
	 * Signing and publishing are reported on their own lines rather
	 * than folded into the state above: a built-but-unsigned ISO and a
	 * built-but-unpublished one are both usable local artifacts, and
	 * collapsing them into one "state" would make a perfectly fine ISO
	 * look broken -- or, worse, an unsigned one look finished.
	 */
	if (sig != NULL)
		printf("signature=%s\n", sig);
	else if (state != NULL && strcmp(state, "ready") == 0)
		printf("signature=none (no release key installed -- this ISO cannot be published)\n");
	if (pub_state != NULL && strcmp(pub_state, "none") != 0) {
		printf("publish=%s", pub_state);
		if (pub_name != NULL)
			printf(" %s", pub_name);
		if (pub_error != NULL)
			printf(" error=%s", pub_error);
		printf("\n");
	}
}

/* Polls GET /v1/system/iso until state leaves "building" -- --wait's own
 * loop, the same shape poll_hostbuild() already established. */
static int poll_iso(const struct cix_client *c, struct cix_response *out)
{
	for (;;) {
		const char *state;

		if (cix_client_request(c, CIX_API_getSystemIso_METHOD, CIX_API_getSystemIso, NULL, out) != 0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return -1;
		}
		state = json_str_field(out->json, "state");
		if (state == NULL || strcmp(state, "building") != 0)
			return 0;
		cix_response_free(out);
		usleep(500000);
	}
}

static int cmd_iso_status(const struct cix_client *c, int json_mode)
{
	struct cix_response r;

	if (cix_client_request(c, CIX_API_getSystemIso_METHOD, CIX_API_getSystemIso, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_iso_status);
}

static int cmd_iso_build(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *disk = NULL;
	const char *ip = NULL;
	const char *prefix = NULL;
	const char *gateway = NULL;
	const char *interface = NULL;
	int wait = 0;
	int i;
	struct json_writer w;
	struct cix_response r;

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
			fprintf(stderr, "cixctl: unknown iso build option '%s'\n", argv[i]);
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

	if (cix_client_request(c, CIX_API_postSystemIso_METHOD, CIX_API_postSystemIso, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);
	if (r.status < 200 || r.status >= 300 || !wait)
		return emit(&r, json_mode, fmt_iso_status);
	cix_response_free(&r);

	if (poll_iso(c, &r) != 0)
		return 1;
	return emit(&r, json_mode, fmt_iso_status);
}

/*
 * cixctl iso publish [--wait] (ADR-0220) -- puts the finished ISO and
 * its signature in the artifact cache, which is the only way anything
 * off this box can get either.
 */
static int cmd_iso_publish(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;
	int wait_mode = 0;
	int i;
	int rc;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--wait") == 0)
			wait_mode = 1;
		else {
			fprintf(stderr, "cixctl: unknown iso publish option '%s'\n", argv[i]);
			return 2;
		}
	}

	memset(&r, 0, sizeof(r));
	if (cix_client_request(c, CIX_API_publishSystemIso_METHOD, CIX_API_publishSystemIso, NULL,
	                       &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	if (r.status != 202 || !wait_mode)
		return emit(&r, json_mode, fmt_iso_status);
	cix_response_free(&r);

	for (;;) {
		const char *ps;

		if (cix_client_request(c, CIX_API_getSystemIso_METHOD, CIX_API_getSystemIso, NULL, &r) !=
		    0) {
			fprintf(stderr, "cixctl: could not reach daemon\n");
			return 1;
		}
		ps = json_str_field(r.json, "publish_state");
		if (ps == NULL || strcmp(ps, "publishing") != 0)
			break;
		cix_response_free(&r);
		usleep(500000);
	}
	rc = emit(&r, json_mode, fmt_iso_status);
	return rc;
}

static int cmd_iso(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *sub;

	if (argc < 1) {
		fprintf(stderr,
		        "usage: cixctl iso build [--disk=DEV --ip=A.B.C.D --prefix=N "
		        "--gateway=A.B.C.D --interface=IFNAME] [--wait]\n"
		        "       cixctl iso status\n"
		        "       cixctl iso publish [--wait]\n");
		return 2;
	}
	sub = argv[0];
	if (strcmp(sub, "build") == 0)
		return cmd_iso_build(c, json_mode, argc - 1, argv + 1);
	if (strcmp(sub, "status") == 0)
		return cmd_iso_status(c, json_mode);
	if (strcmp(sub, "publish") == 0)
		return cmd_iso_publish(c, json_mode, argc - 1, argv + 1);

	fprintf(stderr, "cixctl: unknown iso subcommand '%s'\n", sub);
	return 2;
}

/*
 * ADR-0144: cixctl's own persisted-session support -- a single
 * dotfile (~/.cixctl_token, mode 0600, since it holds a live bearer
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
	if ((size_t)snprintf(out, out_size, "%s/.cixctl_token", home) >= out_size)
		return -1;
	return 0;
}

/*
 * The pager (#152).
 *
 * Long output -- `container ls` on a real host, `logs`, `pkg ls` --
 * scrolls off the top with no way back. This pipes stdout through a
 * pager when, and only when, that is unambiguously wanted.
 *
 * Three refusals, and each is the way this feature is usually got
 * wrong:
 *
 *   NOT a TTY. Piping to grep, jq or a file must behave exactly as
 *   before. A pager that engages in a pipeline breaks scripts, and it
 *   breaks them silently, because the pager writes nothing a caller
 *   recognises as an error.
 *
 *   --json. That output exists to be consumed by something else, so it
 *   is never for a human to scroll even when a human is watching.
 *
 *   Turned off. Remembered in a dotfile beside the token rather than
 *   passed every time, because a preference you have to repeat is not
 *   a preference.
 *
 * $PAGER is respected; the fallback is `less -FRX`. -F quits
 * immediately if the output fits on one screen, which is what keeps
 * short commands feeling unchanged; -R passes colour through; -X stops
 * less clearing the screen on exit, so the output is still there
 * afterwards -- the whole point of having scrolled it.
 */
static pid_t g_pager_pid = -1;

static int pager_pref_path(char *out, size_t out_size)
{
	const char *home = getenv("HOME");

	if (home == NULL || home[0] == '\0')
		return -1;
	if ((size_t)snprintf(out, out_size, "%s/.cixctl_pager", home) >= out_size)
		return -1;
	return 0;
}

/* Paging is on unless the preference file says "off" -- an operator who
 * has never expressed an opinion gets the pager, since running off the
 * top of the screen is the complaint this exists to fix. */
static int pager_enabled_pref(void)
{
	char path[PATH_MAX];
	char buf[16];
	FILE *f;
	size_t n;

	if (pager_pref_path(path, sizeof(path)) != 0)
		return 1;
	f = fopen(path, "r");
	if (f == NULL)
		return 1;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	return strncmp(buf, "off", 3) != 0;
}

static int pager_set_pref(int on)
{
	char path[PATH_MAX];
	FILE *f;

	if (pager_pref_path(path, sizeof(path)) != 0)
		return -1;
	f = fopen(path, "w");
	if (f == NULL)
		return -1;
	fprintf(f, "%s\n", on ? "on" : "off");
	fclose(f);
	return 0;
}

/*
 * Replaces stdout with a pipe into a pager. A no-op in every case where
 * paging would be wrong, so callers need no conditions of their own.
 */
static void pager_begin(int json_mode)
{
	const char *pager;
	int fds[2];
	pid_t pid;

	if (g_pager_pid != -1 || json_mode || !isatty(STDOUT_FILENO))
		return;
	if (!pager_enabled_pref())
		return;
	pager = getenv("PAGER");
	if (pager == NULL || pager[0] == '\0')
		pager = "less -FRX";
	/* An explicitly empty PAGER is the conventional way to say "no
	 * pager", and is honoured rather than overridden. */
	if (pipe(fds) != 0)
		return;

	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return; /* no pager is a fine outcome; failing the command is not */
	}
	if (pid == 0) {
		close(fds[1]);
		dup2(fds[0], STDIN_FILENO);
		close(fds[0]);
		execl("/bin/sh", "sh", "-c", pager, (char *)NULL);
		_exit(127);
	}
	close(fds[0]);
	dup2(fds[1], STDOUT_FILENO);
	close(fds[1]);
	g_pager_pid = pid;
}

/*
 * Closes stdout so the pager sees EOF, then waits for it.
 *
 * The wait is not optional: without it cixctl exits while the pager is
 * still drawing, the shell prints its prompt over the top, and the
 * terminal is left in the pager's own raw mode.
 */
static void pager_end(void)
{
	if (g_pager_pid == -1)
		return;
	fflush(stdout);
	close(STDOUT_FILENO);
	waitpid(g_pager_pid, NULL, 0);
	g_pager_pid = -1;
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

/*
 * show running-config -- the Cisco-style rendering of GET /v1/config.
 *
 * The DOCUMENT is the daemon's; this presentation is not (ADR-0205).
 * The text is never parsed back by anything, so its format is
 * deliberately not a contract -- which is the whole reason it lives
 * here. A daemon change on this platform means a rebuild, an A/B slot
 * write and a reboot; moving a section heading should not cost that.
 *
 * Generic on purpose: it walks whatever the document contains rather
 * than knowing the sections. A section added to the ConfigDocument
 * schema shows up here with no CLI change, which is the point -- the
 * schema is the vocabulary (ADR-0206), and a renderer that had to be
 * taught each section would be a second copy of that list.
 */
static void show_indent(int depth)
{
	int i;

	for (i = 0; i < depth; i++)
		fputs("  ", stdout);
}

static void show_scalar(const struct json_value *v)
{
	if (v == NULL || v->type == JSON_NULL) {
		fputs("-", stdout);
		return;
	}
	switch (v->type) {
	case JSON_BOOL:
		fputs(v->u.boolean ? "true" : "false", stdout);
		break;
	case JSON_NUMBER:
		printf("%lld", (long long)v->u.number);
		break;
	case JSON_STRING:
		fputs(v->u.string != NULL ? v->u.string : "", stdout);
		break;
	default:
		fputs("...", stdout);
		break;
	}
}

static int show_is_scalar(const struct json_value *v)
{
	return v == NULL || (v->type != JSON_ARRAY && v->type != JSON_OBJECT);
}

static void show_value(const struct json_value *v, int depth);

/* An object renders as one "key value" line per member, with nested
 * structures indented beneath their key -- the shape a network
 * engineer already reads without being taught it. */
static void show_object(const struct json_value *v, int depth)
{
	size_t i;

	for (i = 0; i < v->u.object.count; i++) {
		const struct json_value *m = v->u.object.values[i];

		show_indent(depth);
		fputs(v->u.object.keys[i], stdout);
		if (show_is_scalar(m)) {
			fputc(' ', stdout);
			show_scalar(m);
			fputc('\n', stdout);
		} else {
			fputc('\n', stdout);
			show_value(m, depth + 1);
		}
	}
}

static void show_value(const struct json_value *v, int depth)
{
	size_t i;

	if (v == NULL)
		return;
	if (v->type == JSON_OBJECT) {
		show_object(v, depth);
		return;
	}
	if (v->type == JSON_ARRAY) {
		if (v->u.array.count == 0) {
			show_indent(depth);
			fputs("(none)\n", stdout);
			return;
		}
		for (i = 0; i < v->u.array.count; i++) {
			const struct json_value *item = v->u.array.items[i];

			if (show_is_scalar(item)) {
				show_indent(depth);
				show_scalar(item);
				fputc('\n', stdout);
			} else {
				if (i > 0)
					putchar('\n');
				show_value(item, depth);
			}
		}
		return;
	}
	show_indent(depth);
	show_scalar(v);
	fputc('\n', stdout);
}

static void fmt_running_config(const struct json_value *v)
{
	size_t i;

	if (v == NULL || v->type != JSON_OBJECT) {
		fprintf(stderr, "cixctl: unexpected config document shape\n");
		return;
	}
	for (i = 0; i < v->u.object.count; i++) {
		printf("!\n%s\n", v->u.object.keys[i]);
		show_value(v->u.object.values[i], 1);
	}
	printf("!\n");
}

static int cmd_show(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	struct cix_response r;

	if (argc < 1 || strcmp(argv[0], "running-config") != 0) {
		fprintf(stderr, "usage: cixctl show running-config [--json]\n");
		return 2;
	}
	if (cix_client_request(c, CIX_API_getConfig_METHOD, CIX_API_getConfig, NULL, &r) != 0) {
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	return emit(&r, json_mode, fmt_running_config);
}

static int cmd_login(const struct cix_client *c, int json_mode, int argc, char **argv)
{
	const char *username = NULL;
	const char *password = NULL;
	char username_buf[128];
	char password_buf[256];
	struct json_writer w;
	struct cix_response r;
	int i;

	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "--username=", 11) == 0)
			username = argv[i] + 11;
		else if (strncmp(argv[i], "--password=", 11) == 0)
			password = argv[i] + 11;
		else {
			fprintf(stderr, "usage: cixctl login [--username=NAME] [--password=PASS]\n");
			return 2;
		}
	}

	if (username == NULL) {
		if (read_line_noecho("Username: ", username_buf, sizeof(username_buf)) != 0) {
			fprintf(stderr, "cixctl: no username given\n");
			return 2;
		}
		username = username_buf;
	}
	if (password == NULL) {
		if (read_line_noecho("Password: ", password_buf, sizeof(password_buf)) != 0) {
			fprintf(stderr, "cixctl: no password given\n");
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
	if (cix_client_request(c, CIX_API_postLogin_METHOD, CIX_API_postLogin, w.buf, &r) != 0) {
		jw_free(&w);
		fprintf(stderr, "cixctl: could not reach daemon\n");
		return 1;
	}
	jw_free(&w);

	if (r.status != 200) {
		const char *msg = json_str_field(r.json, "error");

		fprintf(stderr, "cixctl: login failed: %s (HTTP %d)\n", msg != NULL ? msg : "?",
		        r.status);
		cix_response_free(&r);
		return 1;
	}

	{
		const char *token = json_str_field(r.json, "token");

		if (token == NULL || token[0] == '\0') {
			fprintf(stderr, "cixctl: login response missing a token\n");
			cix_response_free(&r);
			return 1;
		}
		if (save_token_file(token) != 0) {
			fprintf(stderr,
			        "cixctl: warning: could not persist session token (%s) -- you'll "
			        "need to log in again for the next command\n",
			        strerror(errno));
		}
		/* Real bug, found via ADR-0164's own new prompt indicator (which
		 * surfaced it immediately -- it never flipped to "#" after a
		 * mid-session login): saving the token to disk makes it visible
		 * to the *next* cixctl invocation (main() loads it once at
		 * startup), but did nothing for *this* one -- every command run
		 * for the rest of the current interactive shell session,
		 * including the prompt's own whoami check, kept using whatever
		 * (possibly empty) token this process started with. c's real
		 * underlying storage is always main()'s own non-const `client`;
		 * the const here is routing-only (every cmd_*() handler takes
		 * one shared signature), so mutating it through this cast is
		 * safe, not a layer violation. */
		cix_client_set_token((struct cix_client *)c, token);
		if (json_mode)
			print_raw_json(r.json);
		else
			printf("Logged in as %s.\n", username);
	}
	cix_response_free(&r);
	return 0;
}

static int cmd_logout(const struct cix_client *c, int json_mode)
{
	char token[64];
	struct cix_response r;

	if (load_token_file(token, sizeof(token)) == 0) {
		memset(&r, 0, sizeof(r));
		if (cix_client_request_with_auth(c, "POST", CIX_API_postLogout, token, NULL, &r) == 0)
			cix_response_free(&r);
	}
	clear_token_file();
	/* Same reasoning as cmd_login()'s own matching call -- clear the
	 * current process's live in-memory token too, not just the on-disk
	 * copy, so the rest of *this* session doesn't keep sending a token
	 * the server was just told to invalidate. */
	cix_client_set_token((struct cix_client *)c, NULL);

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
static int dispatch_command(const struct cix_client *client, int json_mode, const char *cmd,
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
	if (strcmp(cmd, "boot-next") == 0)
		return cmd_boot_next(client, json_mode, argc, argv);
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
	if (strcmp(cmd, "control-plane-reservation") == 0)
		return cmd_cpreserve(client, json_mode, argc, argv);
	if (strcmp(cmd, "kernel-policy") == 0)
		return cmd_kernel_policy(client, json_mode, argc, argv);
	if (strcmp(cmd, "esp") == 0)
		return cmd_esp(client, json_mode, argc, argv);
	if (strcmp(cmd, "boot-console") == 0)
		return cmd_boot_console(client, json_mode, argc, argv);
	if (strcmp(cmd, "stalls") == 0)
		return cmd_stalls(client, json_mode);
	if (strcmp(cmd, "assembly") == 0)
		return cmd_assembly(client, json_mode, argc, argv);
	if (strcmp(cmd, "hostauth-config") == 0)
		return cmd_hostauth_config(client, json_mode, argc, argv);
	if (strcmp(cmd, "hostauth-sessions") == 0)
		return cmd_hostauth_sessions(client, json_mode, argc, argv);
	if (strcmp(cmd, "iso") == 0)
		return cmd_iso(client, json_mode, argc, argv);
	if (strcmp(cmd, "routes") == 0)
		return cmd_routes(client, json_mode, argc, argv);
	/*
	 * One `storage` command. Disks/partitions and storage PLACEMENT
	 * (where the log store and rebuildable data live) were separate
	 * top-level commands until the disks->storage rename collided them.
	 * Their subcommand names do not overlap, so they share one command
	 * rather than gaining an artificial `storage placement ...` level.
	 */
	if (strcmp(cmd, "storage") == 0) {
		if (argc > 0 && (strcmp(argv[0], "logs") == 0 || strcmp(argv[0], "rebuildable") == 0))
			return cmd_storage(client, json_mode, argc, argv);
		return cmd_disks(client, json_mode, argc, argv);
	}
	if (strcmp(cmd, "storage-role") == 0)
		return cmd_diskrole(client, json_mode, argc, argv);
	if (strcmp(cmd, "logs") == 0)
		return cmd_logs_top(client, json_mode, argc, argv);
	if (strcmp(cmd, "dhcp") == 0)
		return cmd_dhcp(client, json_mode, argc, argv);
	if (strcmp(cmd, "ksm") == 0)
		return cmd_ksm(client, json_mode, argc, argv);
	if (strcmp(cmd, "zswap") == 0)
		return cmd_zswap(client, json_mode, argc, argv);
	if (strcmp(cmd, "swap") == 0)
		return cmd_swap(client, json_mode, argc, argv);
	if (strcmp(cmd, "host-stats") == 0)
		return cmd_host_stats(client, json_mode);
	if (strcmp(cmd, "server-health") == 0)
		return cmd_server_health(client, json_mode, argc, argv);
	if (strcmp(cmd, "factory-reset") == 0)
		return cmd_factory_reset(client, json_mode, argc, argv);
	if (strcmp(cmd, "software") == 0)
		return cmd_software(client, json_mode);
	if (strcmp(cmd, "volume") == 0)
		return cmd_volume(client, json_mode, argc, argv);
	if (strcmp(cmd, "kmsg") == 0)
		return cmd_kmsg(client, json_mode, argc, argv);
	if (strcmp(cmd, "process") == 0)
		return cmd_process(client, json_mode, argc, argv);
	if (strcmp(cmd, "ping") == 0)
		return cmd_ping(client, json_mode, argc, argv);
	if (strcmp(cmd, "resolv") == 0)
		return cmd_resolv(client, json_mode, argc, argv);
	/*
	 * Issue #108: `cixctl console NAME` was advertised and unroutable.
	 * cmd_console()'s own usage string has always said "cixctl console
	 * NAME", so the CLI was promising a form the dispatcher did not
	 * carry -- an operator typing exactly what the error told them to
	 * got "unknown command". Routed here as a convenience alias for
	 * `container console`, the same shape `logs` already has; both
	 * spellings reach one function, so this is an alias, not a second
	 * implementation.
	 */
	if (strcmp(cmd, "console") == 0)
		return cmd_console(client, argc, argv);
	if (strcmp(cmd, "release-key") == 0)
		return cmd_release_key(client, json_mode, argc, argv);
	if (strcmp(cmd, "signing-keys") == 0)
		return cmd_signing_keys(client, json_mode, argc, argv);
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
	/*
	 * All container operations are under the `container` namespace (issue
	 * #74) -- ls/run/start/stop/pause/unpause/rm/inspect/stats/console/
	 * files/migrate-storage[-status], plus recipe/apply-recipe/network/
	 * device. No top-level container verbs, consistent with every other
	 * resource noun below.
	 */
	if (strcmp(cmd, "container") == 0)
		return cmd_container(client, json_mode, argc, argv);
	/*
	 * #371: a deployment is what used to be called a "container
	 * recipe". The name was wrong -- it describes what to RUN and
	 * where, not how to build anything -- and it sat under `container`
	 * as a sub-verb, which hid that it is a resource in its own right
	 * with its own pipeline. Clean cut-over, no alias: `container
	 * recipe` is gone rather than deprecated.
	 */
	if (strcmp(cmd, "deployment") == 0) {
		if (argc >= 1 && strcmp(argv[0], "apply") == 0)
			return cmd_container_apply_recipe(client, json_mode, argc - 1, argv + 1);
		return cmd_container_recipe(client, json_mode, argc, argv);
	}
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
	if (strcmp(cmd, "pipeline") == 0)
		return cmd_pipeline(client, json_mode, argc, argv);
	if (strcmp(cmd, "schedule") == 0)
		return cmd_schedule(client, json_mode, argc, argv);
	if (strcmp(cmd, "show") == 0)
		return cmd_show(client, json_mode, argc, argv);

	fprintf(stderr, "cixctl: unknown command '%s'\n", cmd);
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

/* The connected daemon's own full site identity (GET /v1/system/site,
 * ADR-0046) as instance.site.domain (or instance.domain when no site
 * tier is set -- mirrors daemon/src/siteconfig.c's own
 * siteconfig_host_fqdn(), client-side, since the CLI has no access to
 * that daemon-internal function), not a fixed "cix> ". Trailing
 * character follows real shell/network-device convention: "#" once
 * GET /v1/whoami (ADR-0164) reports an authenticated session, ">"
 * otherwise -- including whenever host-auth write-gating isn't even
 * active yet (a fresh install), the same as an unprivileged prompt on
 * a box with no root password set. Fetched once at shell startup by
 * shell_prompt_init() below, so the prompt actually identifies *which*
 * box this session is talking to (useful the moment an operator has
 * more than one Cix install reachable) and *whether* it's currently
 * privileged to write. Falls back to the literal string "cix" (site
 * config's own documented default) if the fetch fails for any reason
 * -- never leaves the prompt blank. Re-run after every "login"/
 * "logout" command (see the dispatch loops below) so the prompt
 * reflects a state change immediately, not just at shell startup. */
#define SHELL_PROMPT_MAX 96
static char g_shell_prompt[SHELL_PROMPT_MAX] = "cix> ";

static void shell_prompt_init(const struct cix_client *client)
{
	struct cix_response site_r, whoami_r;
	char fqdn[SHELL_PROMPT_MAX] = "cix";
	int authenticated = 0;

	if (cix_client_request(client, CIX_API_getSystemSite_METHOD, CIX_API_getSystemSite, NULL, &site_r) == 0) {
		if (site_r.status == 200) {
			const char *instance_name = json_str_field(site_r.json, "instance_name");
			const char *site_name = json_str_field(site_r.json, "site_name");
			const char *domain_suffix = json_str_field(site_r.json, "domain_suffix");

			if (instance_name != NULL && instance_name[0] != '\0' && domain_suffix != NULL) {
				if (site_name != NULL && site_name[0] != '\0')
					snprintf(fqdn, sizeof(fqdn), "%s.%s.%s", instance_name, site_name,
					         domain_suffix);
				else
					snprintf(fqdn, sizeof(fqdn), "%s.%s", instance_name, domain_suffix);
			}
		}
		cix_response_free(&site_r);
	}

	if (cix_client_request(client, CIX_API_getWhoami_METHOD, CIX_API_getWhoami, NULL, &whoami_r) == 0) {
		if (whoami_r.status == 200) {
			const struct json_value *jauth = json_object_get(whoami_r.json, "authenticated");

			authenticated = jauth != NULL && jauth->type == JSON_BOOL && jauth->u.boolean;
		}
		cix_response_free(&whoami_r);
	}

	snprintf(g_shell_prompt, sizeof(g_shell_prompt), "%s%s ", fqdn, authenticated ? "#" : ">");
}

/* Every top-level command dispatch_command() recognizes, plus the
 * shell's own "help"/"exit"/"quit" builtins -- kept as one literal
 * array specifically so a newly added command elsewhere in this file
 * doesn't silently stay uncompletable; there's no way to derive this
 * list from dispatch_command()'s own if-chain without parsing C, so
 * it's a second, explicit copy -- the same tradeoff CATEGORY_VIEWS
 * makes on the web dashboard side (web/app.js) for the identical
 * reason (a route table that can't be enumerated by walking code). */
/*
 * The shell's own extra words, which are not API commands and so are not
 * in the tree: they end the session rather than call anything. Every
 * real command now comes from CLI_TREE, so the two can no longer
 * disagree -- this list had drifted to 59 entries against the
 * dispatcher's 62 (#151).
 */
static const char *const SHELL_EXTRA_COMMANDS[] = { "exit", "quit", "help", NULL };

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

/*
 * Completion, to the end of a command rather than the first word (#151).
 *
 * Walks CLI_TREE by the words already typed and answers with whatever
 * can come next: subcommands while there are subcommands, then flags.
 * A word already complete (followed by a space) advances the walk; the
 * final, partial word is what gets matched.
 *
 * Flags are offered with their trailing "=" where the parser expects a
 * value, because "--name" and "--name=" are different things to the
 * parser and offering the wrong one produces a usage error from a
 * completion, which is the least forgivable kind.
 *
 * Deliberately does NOT complete flag VALUES. The knowable ones are
 * knowable only by asking the daemon (container names, image names),
 * and a completion that makes a blocking HTTP call on every Tab is a
 * worse experience than no completion -- it hangs the terminal exactly
 * when the daemon is slow or unreachable. Left for a later pass that
 * can cache.
 */
static const struct cli_node *cli_tree_find(const struct cli_node *level, const char *word)
{
	int i;

	if (level == NULL)
		return NULL;
	for (i = 0; level[i].name != NULL; i++) {
		if (strcmp(level[i].name, word) == 0)
			return &level[i];
	}
	return NULL;
}

/*
 * Candidates for the next word, given the complete words already typed.
 * Returns how many were written to out.
 */
static int cli_complete_candidates(char **words, int nwords, const char *partial,
                                    const char **out, int max)
{
	const struct cli_node *level = CLI_TREE;
	const struct cli_node *node = NULL;
	size_t plen = (partial != NULL) ? strlen(partial) : 0;
	int n = 0, i;

	for (i = 0; i < nwords; i++) {
		node = cli_tree_find(level, words[i]);
		if (node == NULL)
			return 0; /* off the map -- offer nothing rather than something wrong */
		level = node->subs;
	}

	/* Subcommands at this level. */
	for (i = 0; level != NULL && level[i].name != NULL && n < max; i++) {
		if (plen == 0 || strncmp(level[i].name, partial, plen) == 0)
			out[n++] = level[i].name;
	}
	/* Then this node's own flags -- offered alongside subcommands,
	 * since many commands accept both. */
	if (node != NULL && node->flags != NULL) {
		for (i = 0; node->flags[i] != NULL && n < max; i++) {
			if (plen == 0 || strncmp(node->flags[i], partial, plen) == 0)
				out[n++] = node->flags[i];
		}
	}
	/* At the top level the shell's own words complete too. */
	if (nwords == 0) {
		for (i = 0; SHELL_EXTRA_COMMANDS[i] != NULL && n < max; i++) {
			if (plen == 0 || strncmp(SHELL_EXTRA_COMMANDS[i], partial, plen) == 0)
				out[n++] = SHELL_EXTRA_COMMANDS[i];
		}
	}
	return n;
}

/*
 * Splits a line into complete words plus the trailing partial one. The
 * partial is what completion matches; a line ending in a space has an
 * empty partial, which correctly means "show me everything valid here".
 */
static int cli_split_line(char *line, char **words, int max_words, const char **partial)
{
	int n = 0;
	char *p = line;

	*partial = "";
	while (*p != '\0') {
		char *start;

		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		start = p;
		while (*p != '\0' && *p != ' ')
			p++;
		if (*p == '\0') {
			*partial = start; /* last word, no trailing space: partial */
			break;
		}
		*p++ = '\0';
		if (n < max_words)
			words[n++] = start;
	}
	return n;
}

static void shell_complete(char *buf, size_t *len, size_t *cursor)
{
	const char *matches[128];
	char line[SHELL_LINE_MAX];
	char *words[16];
	const char *partial;
	int nwords, match_count;
	size_t i, match_len, plen;

	if (*len >= sizeof(line))
		return;
	memcpy(line, buf, *len);
	line[*len] = '\0';

	nwords = cli_split_line(line, words, 16, &partial);
	plen = strlen(partial);
	match_count = cli_complete_candidates(words, nwords, partial, matches, 128);
	if (match_count == 0)
		return;

	if (match_count == 1) {
		/* Replace just the partial word, keeping everything before it. */
		size_t keep = *len - plen;
		const char *m = matches[0];

		match_len = strlen(m);
		if (keep + match_len < SHELL_LINE_MAX) {
			memcpy(buf + keep, m, match_len);
			*len = keep + match_len;
			/* A flag taking a value ends in '='; anything else is a
			 * finished word and gets a space, so the next Tab
			 * completes the level below rather than re-offering this
			 * one. */
			if (buf[*len - 1] != '=' && *len + 1 < SHELL_LINE_MAX)
				buf[(*len)++] = ' ';
			*cursor = *len;
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
static int run_shell_fallback(const struct cix_client *client, int json_mode)
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
		if (strcmp(tokens[0], "login") == 0 || strcmp(tokens[0], "logout") == 0)
			shell_prompt_init(client); /* auth state just changed -- reflect it immediately */
	}
	return 0;
}

/*
 * Interactive shell: entered when cixctl is invoked with no command
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
static int run_shell(const struct cix_client *client, int json_mode)
{
	struct termios saved;
	char line[SHELL_LINE_MAX];
	char *tokens[SHELL_MAX_TOKENS];

	printf("cixctl interactive shell -- type a command (e.g. \"ps\"), \"help\", or \"exit\"\n");

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
		if (strcmp(tokens[0], "login") == 0 || strcmp(tokens[0], "logout") == 0)
			shell_prompt_init(client); /* auth state just changed -- reflect it immediately */
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
	int rc;
	const char *cmd;
	struct cix_client client;

	while (i < argc && strncmp(argv[i], "--", 2) == 0) {
		if (strncmp(argv[i], "--host=", 7) == 0)
			host = argv[i] + 7;
		else if (strncmp(argv[i], "--port=", 7) == 0)
			port = atoi(argv[i] + 7);
		else if (strcmp(argv[i], "--json") == 0)
			json_mode = 1;
		else {
			fprintf(stderr, "cixctl: unknown option '%s'\n", argv[i]);
			print_usage(stderr);
			return 2;
		}
		i++;
	}

	cix_client_init(&client, host, port);
	{
		char saved_token[64];

		if (load_token_file(saved_token, sizeof(saved_token)) == 0)
			cix_client_set_token(&client, saved_token);
	}

	if (i >= argc) {
		if (isatty(STDIN_FILENO))
			return run_shell(&client, json_mode);
		print_usage(stderr);
		return 2;
	}
	cmd = argv[i++];

	/*
	 * #152: `pager` is handled here rather than in the dispatcher
	 * because it configures this client and talks to no daemon -- it
	 * has no endpoint and should not appear to have one.
	 */
	/*
	 * #151: the completion helper a shell calls. Hidden from usage on
	 * purpose -- it is an integration point, not a command anyone
	 * types. Prints one candidate per line and makes no HTTP call, so
	 * pressing Tab never blocks on a slow or unreachable daemon.
	 */
	if (strcmp(cmd, "__complete") == 0) {
		const char *matches[128];
		const char *partial = "";
		int n, k, w = argc - i;

		/*
		 * The shell passes every word including the partial one. A
		 * trailing empty argument is how it says the line ends in a
		 * space, i.e. "offer everything valid here".
		 */
		if (w > 0) {
			partial = argv[argc - 1];
			w--;
		}
		n = cli_complete_candidates(argv + i, w, partial, matches, 128);
		for (k = 0; k < n; k++)
			printf("%s\n", matches[k]);
		return 0;
	}

	if (strcmp(cmd, "pager") == 0) {
		if (argc - i == 1 && strcmp(argv[i], "on") == 0)
			return pager_set_pref(1) == 0 ? 0 : 1;
		if (argc - i == 1 && strcmp(argv[i], "off") == 0)
			return pager_set_pref(0) == 0 ? 0 : 1;
		if (argc - i == 0 || strcmp(argv[i], "status") == 0) {
			printf("pager: %s\n", pager_enabled_pref() ? "on" : "off");
			return 0;
		}
		fprintf(stderr, "usage: cixctl pager [on|off|status]\n");
		return 2;
	}

	/*
	 * Everything a command prints from here goes through the pager,
	 * when one is warranted -- pager_begin() decides, so no command
	 * needs to know it exists. rc is captured before pager_end()
	 * because the exit status is the command's, never the pager's.
	 */
	pager_begin(json_mode);
	rc = dispatch_command(&client, json_mode, cmd, argc - i, argv + i);
	pager_end();
	return rc;
}
