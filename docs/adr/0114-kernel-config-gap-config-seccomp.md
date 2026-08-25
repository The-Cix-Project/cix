# 0114 — Fourth `allnoconfig`-starting kernel config gap: `CONFIG_SECCOMP` blocked every SSH login (closes task #759)

## Status

Accepted

## Context

Task #759 asked to stand up the glauth+jumpbox+LDAP SSH-target chain on 192.168.15.95 and verify SSH login live. The jump box container (task #730) had already been proven able to start `sshd` cleanly, and the LDAP SSH-target rendering mechanism (ADR-0111) had already been proven to write correct `/etc/passwd`/`/etc/shadow`/`~/.ssh/authorized_keys` content. Every real SSH login attempt against it still failed identically: the client saw `kex_exchange_identification: read: Connection reset by peer` mid-handshake, while `sshd`'s own parent process stayed `running` and healthy throughout — exactly the shape of symptom (a per-connection child failing, not the daemon itself) that makes a bug look intermittent rather than pointing at a clear cause.

`capture_output` (ADR-0112) made the real cause directly visible, the same tool that closed ADR-0113's glauth mystery: `ssh_sandbox_child: prctl(PR_SET_SECCOMP): Invalid argument [preauth]`. OpenSSH's own privilege-separation preauth child (`sandbox-seccomp-filter.c`) unconditionally tries to install a seccomp-BPF filter as a hardening measure and treats `EINVAL` from `prctl()` as fatal, killing the connection before authentication can even begin. `grep -n "CONFIG_SECCOMP" .config` confirmed `CONFIG_HAVE_ARCH_SECCOMP=y`/`CONFIG_HAVE_ARCH_SECCOMP_FILTER=y` (x86_64 always supports the feature) but `# CONFIG_SECCOMP is not set` — despite `arch/Kconfig` declaring `CONFIG_SECCOMP` as `def_bool y depends on HAVE_ARCH_SECCOMP`.

This is the same class of gap as `CONFIG_VETH` (container networking, earlier this project) and `CONFIG_INOTIFY_USER` (ADR-0113, glauth's live config reload) before it, now confirmed a third time over: this project's kernel build starts from `allnoconfig`, which forces every overridable bool to `n` regardless of its own `def_bool y` default — a Kconfig option needs explicit listing in `image/kernel/qemu-part1.config`'s merge fragment, or it silently never turns on, no matter how "obviously always-on" its own Kconfig entry claims to be.

## Decision

Added `CONFIG_SECCOMP=y` and `CONFIG_SECCOMP_FILTER=y` to `image/kernel/qemu-part1.config`, following the file's own established documentation convention (a comment explaining what broke, how it was actually diagnosed, and why this exact symbol is the fix — not just "add the flag"). Rebuilt via the project's own documented recipe (the config file's own header comment: cached source, `merge_config.sh -m`, `make olddefconfig`, `make bzImage`), deployed to 192.168.15.95 via the established LAN-serve `scratch-deploy` recipe fetch + `system/update --kernel_path=` + reboot round-trip (`docs/guides/remote-development.md`) — no ISO reinstall needed, matching every prior kernel-only fix this project has shipped.

## Verification

Live, end to end, against the real box, post-reboot: created a fresh jump box container (baseline `sshd_config`/host key/`/etc/passwd`/`/etc/group`/`/etc/shadow`, `capture_output: true`), registered it via `POST /v1/ldap/ssh-targets` (ADR-0111), and ran a real `ssh -i <client key> osakka@<jumpbox-ip> "id"` from this dev box — it succeeded cleanly (`uid=1000(osakka) gid=1000(users) groups=1000(users)`), no `Connection reset by peer`, confirming both this kernel fix and ADR-0111's LDAP-to-filesystem account sync working together end to end for the first time.

## Consequences

- Every future kernel build now carries real seccomp support. This project's own runtime (`cixd`, container workloads) has never used seccomp itself (device passthrough enforcement uses `BPF_CGROUP_DEVICE`, a different mechanism — see ADR-0017) — this fix is purely about letting an unmodified upstream binary (`sshd`) use a hardening feature it already assumes is always present on any real Linux system, not about Cix adopting seccomp itself.
- This is the third confirmed instance of the `allnoconfig`-plus-`def_bool y` gap class (`CONFIG_VETH`, `CONFIG_INOTIFY_USER`/ADR-0113, now `CONFIG_SECCOMP`) — any future report of an unmodified upstream binary failing in a way that looks like a missing kernel feature should check this class of gap early, not last.
- A second, unrelated real bug was found and fixed in the same investigation session (crash-looped `restart:"always"` containers never having their on-disk state cleaned up on `DELETE`) — see ADR-0115, not folded into this ADR since it's a genuinely separate, code-level decision.
