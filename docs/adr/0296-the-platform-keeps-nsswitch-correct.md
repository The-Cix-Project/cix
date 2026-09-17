# 0296 — The platform keeps `nsswitch.conf` correct: hosts resolve through DNS, and the file converges

## Status

Accepted. Issue [#478](https://git.home.arpa/itdlabs/cix/issues/478). Supersedes one decision in [ADR-0111](0111-ssh-ldap-account-sync.md) — the content and write-condition of the baseline `/etc/nsswitch.conf`. Its account-database decision (rendered `/etc/passwd`, not real NSS/PAM against a directory) is untouched and is the reason the `passwd`/`group`/`shadow` lines are unchanged. Completes [ADR-0295](0295-a-container-in-the-directory-can-read-it.md), which gave a container resolvers it could not use.

## Context

ADR-0295 gave every container sharing a network with a registered DNS server a real `/etc/resolv.conf`. Verifying it produced this, measured on 192.168.15.95, 2026-09-17, from inside a `jumpbox` container on `services` with `dns_servers` omitted:

```
--- /etc/resolv.conf ---
nameserver 192.168.150.101
nameserver 192.168.150.102
--- getent hosts dns-1 ---
getent rc=2
```

and in the same container:

```
hosts: files
/lib/x86_64-linux-gnu/libnss_dns.so.2      <- present
```

The resolver was correct, the backend was installed, and glibc never asked it anything, because the `hosts` line named only `files`.

**The part that makes this a design fault rather than a missing line** is which containers *did* resolve. `jump` resolves every name tried (`git.home.arpa` → 192.168.15.15, `jump` → 192.168.150.109, `cix.internal` → 192.168.15.95, all rc=0) — and its `nsswitch.conf` is the `ldap_client` replacement, which carried **no `hosts` line at all**:

```
passwd:         files ldap
group:          files ldap
shadow:         files ldap
```

glibc then falls back to its own compiled-in default for `hosts`, which includes `dns`. So the container handed an explicit, pinned file could not resolve names and the container handed an incomplete one could — by way of precisely the default ADR-0111 wrote the file to stop depending on:

> glibc's own compiled-in default database list is used when this file is missing, but that default is a moving target across glibc versions and this project has no reason to depend on it being correct.

ADR-0111's reasoning for `files`-only was coherent when written:

> "files" only for every database this platform's own containers could plausibly need (no "ldap"/"dns" backend entries — this project always pushes rendered files into a container's own filesystem rather than having the container's own NSS talk to a remote service directly...)

That holds for accounts, where Cix really does render `/etc/passwd`. It does not hold for hosts, because **there is no file Cix renders a container's view of DNS into** — the whole point of a resolver is that the container asks a server. ADR-0143 then added `dns_servers`, and ADR-0295 made it a default, without anyone noticing that NSS was configured never to use it. Two parts of the platform answering one question differently: the same shape as #451's `dns_register`, found the same way — by verifying the fix rather than trusting it.

Worth recording that the backends were never the problem: `libnss_dns.so.2` ships in the **glibc package** (measured: glibc 2.44-16 in `jumpbox` carries 13 `libnss` files), and nothing in this tree stages any of them — ADR-0111's own `libnss_files.so.2` staging is gone from the code, which `grep -rn libnss` over the C sources confirms.

## Decision

**One source of truth for the file.** `daemon/src/nsswitch.c` owns both variants and builds them from a single `NSSWITCH_HOSTS_LINE`. The baseline and the `ldap_client` replacement cannot disagree about hosts, because there is only one hosts line to disagree with. `test_nsswitch` asserts they match, that both name `files dns`, and that the account lines differ in the one way they legitimately should.

**`hosts: files dns`** in both. Local file first, then the resolvers ADR-0143/ADR-0295 give the container.

**The baseline is written whenever its content DIFFERS, not merely when the file is absent**, and this is the load-bearing half. Writing only when absent was harmless while the content never changed, and became the mechanism by which every image already built keeps the old content forever — nothing else ever rewrites that file. A file whose content the platform declares is a file the platform has to converge.

## Consequences

**The `dns_servers` field and ADR-0295's default now do something for an ordinary container.** Before this, both were inert outside `ldap_client` containers.

**Convergence arrives with the next real install, not with the daemon**, because of ADR-0155: `pkg_seed_image_baseline()` runs against a staging rootfs whose resulting version is discarded when the package manifest is unchanged. So deploying this build does not by itself fix `base`, `jumpbox`, `dns`, `chrony`, `ldap`, `syslog` or `router` — each converges when something is genuinely installed into it, which the rolling rebuild does on any real package bump. Stated here rather than discovered, because the obvious post-deploy check looks like a failure.

**A converging write means the platform now overwrites this file.** Safe today and measured: no package on the box ships an `/etc/nsswitch.conf` (`GET /v1/pkg` across every installed package, zero matches). A package that later shipped one would be silently overwritten on the next install, and that needs a decision rather than this ADR's silence — most likely the same one, since the platform stages the NSS modules' configuration and a package shipping its own would be a second answer.

**`test_nsswitch` is pure logic and in `SELFTESTS`.** The convergence rule cannot be gated at runtime by this suite: it needs a *successful* package install into an image, which needs a real build container — the one thing a build container cannot create (#224). So the decision is gated where it can be, and `test_images` separately asserts that a freshly created image's rootfs really carries the line.

**The `ldap_client` file no longer changes host resolution as a side effect of an LDAP feature.** It was silently replacing the baseline's `hosts` behaviour with a glibc default; now it states the same rule explicitly.
