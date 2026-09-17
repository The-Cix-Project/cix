# Building Cix

Two ways to produce `cixd`/`cixctl`/`web/`: the ordinary way, on a ordinary Linux dev machine with a system TCC already installed, and the self-hosted way, from inside a running Cix install with no separate dev machine at all. Both produce byte-for-byte the same kind of artifact; the self-hosted path exists specifically so an operator with only a Cix box, no laptop/dev-server, can still rebuild it.

## From a dev machine

Requires `tcc` and a Linux kernel with cgroup v2 and `clone3`/`CLONE_INTO_CGROUP` support (5.7+). Namespace/mount tests must run as root, and need a **privileged** container if run inside one (see `docs/roadmap/ROADMAP.md` Phase 1 for why).

```sh
make                       # builds everything into build/, -Wall -Werror, zero warnings
sudo build/cixd         # start the daemon (REST API + web dashboard on :80)
build/cixctl health     # talk to it with the CLI
make clean
```

`make` builds every binary this repo produces — `cixd`, `cixctl`, the installer tools (`mkbootroot`, `mkinstalleriso`, `cix-install`, `cix-recover`), and one test binary per phase/part (each self-contained, forking and `exec`ing its own `cixd` instance where needed — e.g. `sudo build/test_pkg` runs standalone). There is no `make install` target and no other named target beyond `all`/`clean` — everything beyond `make`/`make clean` is a manually-invoked binary out of `build/`.

`build-inputs/bzImage` (the kernel Cix boots) is intentionally **not** part of this build — see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md) for how that's produced, on a dev machine or self-hosted.

## From a running Cix host (self-hosted rebuild)

An operator with no separate dev machine can rebuild `cixd`/`cixctl`/`web/` from inside Cix's own container+recipe mechanism, using the [hostbuild](writing-recipes.md#the-hostbuild-variant) pipeline (ADR-0057) — the same mechanism [kernel builds](kernel-build-and-ab-updates.md) use, applied to Cix's own source instead. TCC ships as an ordinary recipe rather than being baked into any shared toolchain, since it's this project's own Immutable Maxim that `cixd`/`cixctl` are built with TCC, never GCC — nothing about that changes just because the build is happening on the target box itself.

### 1. Build a toolchain image

```
cixctl image create --name=cix-builder
cixctl pkg install --name=tcc --image=cix-builder
cixctl pkg install --name=make --image=cix-builder
cixctl pkg install --name=glibc --image=cix-builder
cixctl pkg install --name=linux-headers --image=cix-builder
cixctl pkg install --name=bash --image=cix-builder
cixctl pkg install --name=coreutils --image=cix-builder
cixctl pkg install --name=openssl --image=cix-builder
```

An image is just an ordinary image, built up with ordinary installs — no special "builder image" concept exists beyond having the right packages present. This exact set (`tcc`, `make`, `glibc`, `linux-headers`, `bash`, `coreutils`, `openssl`) is the minimum a plain Makefile build of this repo needs: `tcc` to compile, `make` to drive the build, `glibc` for the C headers and the CRT startup objects (`crt1.o`/`crti.o`/`crtn.o` — see the note on TCC's own CRT search path below), `linux-headers` for the kernel UAPI headers glibc's own `limits.h` chain includes (these two replaced `libc-dev`, which was retired in ADR-0217: it staged another distribution's headers copied off the bootstrap machine, and was pinned to glibc 2.36 while the runtime ran 2.44), `bash` because glibc's `popen()` hardcodes `/bin/sh` with no override and the kernel's own Kconfig-style patterns some build steps use need a real shell present, `coreutils` because the root `Makefile`'s own `mkdir -p build` needs a real `mkdir`, and `openssl` (a real from-source build, ADR-0078) because `cixd` itself has linked `-lssl -lcrypto` since the HTTPS listener landed (ADR-0059) — its own link-time `libssl.so`/`libcrypto.so` symlinks are among the real files this recipe's build produces. If a future change to this repo's own build needs something more, that surfaces as a real, specific build failure naming exactly what's missing — install it onto `cix-builder` the same way, one real gap at a time, never speculatively.

### 1b. Build the host tools image (optional, ADR-0078)

`cixd` itself shells out to a handful of real binaries at runtime -- `openssl` (PKI), `curl` (`pkg_source` fetches), `tar`/`gzip`/`bzip2`/`xz` (source extraction), `cp`/`rm`/`sha256sum` (build-container bookkeeping), `unsquashfs` (toolchain import), `mkfs.ext4` (disk format). `image/src/mkbootroot.c` (the tool that assembles the control-plane squashfs) has always sourced these from whichever machine runs it -- fine for this repo's own dev-sandbox build, but not "from source" for a produced squashfs meant to run on someone else's hardware.

An operator who wants every one of those binaries built from real source, not copied off the machine that happened to run `mkbootroot`, builds one more image:

```
cixctl image create --name=cix-hosttools
cixctl pkg install --name=coreutils --image=cix-hosttools
cixctl pkg install --name=gzip --image=cix-hosttools
cixctl pkg install --name=openssl --image=cix-hosttools
cixctl pkg install --name=curl --image=cix-hosttools
cixctl pkg install --name=tar --image=cix-hosttools
cixctl pkg install --name=bzip2 --image=cix-hosttools
cixctl pkg install --name=xz --image=cix-hosttools
cixctl pkg install --name=squashfs-tools --image=cix-hosttools
cixctl pkg install --name=e2fsprogs --image=cix-hosttools
```

`cix-hosttools` is a fixed, well-known name (`HOST_TOOLS_IMAGE` in `daemon/src/main.c`) -- `spawn_cix_bootroot_assembly()` (the server-side handler behind a `cix` hostbuild's own automatic bootroot assembly, ADR-0057) checks for it and, if present, passes its rootfs to `mkbootroot.c`'s own `host_tools_dir` argument so `cp`/`rm`/`sha256sum`/`gzip` come from there instead of the box running the build. This is entirely optional and purely additive: a box that never builds this image keeps today's behavior (those four tools sourced from wherever `mkbootroot` itself runs) -- nothing breaks either way. `openssl`/`curl`/`tar`/`bzip2`/`xz`/`squashfs-tools`/`mkfs.ext4` (e2fsprogs) don't have a `mkbootroot.c` wiring point yet (a real, tracked follow-on, not silently dropped) -- installing them onto this same image is still worthwhile today since it's the same real, from-source artifact a future wiring pass will point at.

### 1c. Build the firmware image (optional, ADR-0263)

A driver asks the kernel for firmware during device probe, and the kernel answers it out of the root filesystem it booted — before any container exists, so a blob inside a container can never be reached. Any host whose hardware needs firmware therefore needs it in the control-plane root.

Build one more image, named for what it holds:

```
cixctl image create --name=cix-firmware
cixctl pkg install --name=rtw88-firmware --image=cix-firmware
cixctl pkg install --name=wireless-regdb --image=cix-firmware
```

`cix-firmware` is a fixed, well-known name (`FIRMWARE_IMAGE` in `daemon/src/main.c`), resolved the same way `cix-hosttools` is: `spawn_cix_bootroot_assembly()` takes the image's current version, then its own `lib/firmware`, and hands that to `mkbootroot` as the firmware root to stage. That subdirectory is used rather than the image rootfs because it is where firmware packages install, so its contents already mirror `/lib/firmware` exactly — `rtw88/rtw8822b_fw.bin` and a bare `regulatory.db` land where `request_firmware()` looks, with no knowledge anywhere of which drivers exist.

Optional and purely additive, like `cix-hosttools`: a box that never builds this image passes an empty firmware root and `mkbootroot` skips the staging. Install only what the hardware actually needs — the two above are what an 802.11ac access point on an RTL8822BU adapter takes (`rtw88-firmware` for the radio, `wireless-regdb` for the channels and powers `cfg80211` will permit). A machine with an AMD GPU adds `amdgpu` firmware to this same image; nothing about the mechanism changes per device.

The staged firmware is part of the assembled root, and `mkbootroot` assembles a **fresh** root every run. So this is not a one-time action whose result persists: the image has to exist at assembly time, every time, or the resulting root simply has no firmware in it.

### 2. Point `cix.recipe` at a real source snapshot

`recipes/package/cix/`'s `pkg_source` is this repo's own self-hosted git remote's archive-download endpoint, pinned to a real tag — never floating `main`, the same fixed-version discipline every other recipe in this catalog follows:

```sh
pkg_source="https://<user>:<TOKEN>@<git-host>/api/v1/repos/<org>/cix/archive/<tag>.tar.gz"
```

The committed recipe carries a placeholder in place of a real token — substitute a real, scoped, read-only access token for the account this daemon should fetch as before uploading it (`cixctl pkg recipe add --name=cix --file=...`), the same "generate locally, never commit" posture this project's own installer signing key already established. Cut a fresh tag and bump `pkg_version`/`pkg_source`/`pkg_sha256` whenever you want a newer self-build available — a tag bump is the deliberate, auditable signal that a new self-build snapshot exists, not an automatic floating-HEAD fetch.

### 3. Run the hostbuild

```
cixctl pkg hostbuild cix --wait --deploy
```

This fetches the tagged source (host-side, before the build container starts — the build container itself has no network access, same as every other install), builds `cixd`/`cixctl`/`web/` **and** `mkbootroot` itself with the just-installed TCC, then hands off to the daemon's own server-side assembly step: `cixd` forks and execs the freshly-built `mkbootroot` (the same non-blocking, pidfd-tracked pattern it already uses for `curl` fetches) to package those artifacts into a fresh `cixd-root.squashfs` — never the CLI invoking `mkbootroot` itself, which the API-First Mandate rules out. `mkbootroot` is built by this same hostbuild round, not reused from any earlier one, so a box that's never had a self-build before (every real deployed box, since `mkbootroot` was previously only ever a dev-machine tool) has everything it needs in one self-contained round.

`--wait` polls until the hostbuild job itself reaches `installed` — that only means the compile finished, not that the async squashfs assembly has too. `--deploy` waits for that assembly's own real completion (ADR-0105 — comparing `GET /system/assembly`'s `completed_generation` against a baseline it captured before this round even started, not just trusting `cixd-root.squashfs`'s presence on disk, which could be a stale leftover from an earlier round) before calling the existing `/system/update` with it, exactly as if you'd `scp`'d it from a dev machine.

If you are driving this by hand rather than through `--deploy`, read the `image_*` fields on `GET /system/assembly` rather than the generation counters. The counters live in the daemon's memory and a deploy ends in a reboot, so a host that has just booted the root it assembled reports `started_generation: 0, completed_generation: 0` — the same as one that has never assembled anything. `image_mtime` is `stat()`ed from the file and survives the reboot, and `image_complete` tells you whether `/system/update` will accept those bytes before you call it (#481). From here, follow the same write → reboot → confirm sequence as any other update — see [`staying-updated.md`](staying-updated.md).

### 4. Build a fresh installer ISO, server-side

The same round above also produces `cix-install` and `mkinstalleriso` — enough to assemble a brand-new installer ISO (see [`installing.md`](installing.md) for what that ISO actually contains and how an operator boots it) without a separate dev machine at all, via `POST /system/iso` (ADR-0064). That endpoint also needs `grub-mkrescue`/`sbsign`/`xorriso`/`mformat`/`mcopy`, plus the pieces that go *inside* the ISO rather than assemble it — `shim`/`MokManager`/`mokutil` for the Secure Boot chain, and `fdisk`/`mkfs.fat` for the installer's own partitioning and ESP formatting. All of them are self-built the same hostbuild way rather than borrowed from whatever happens to be installed on the box, which for the last five was not a preference but a correctness fix: they were read from absolute paths that exist on a Debian development machine and on no Cix control-plane root, so an ISO could only ever be built on a dev box.

```
cixctl pkg hostbuild isotools
```

(No image is named: the build container is composed from `isotools.recipe`'s own `pkg_build_depends` ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)), so what it builds inside is what the recipe declares. The `iso-builder` image still exists as an ordinary image and its manifest still names `grub`/`sbsigntools`/`xorriso`/`mtools`/`shim`/`mokutil`/`fdisk`/`dosfstools`; see [ADR-0208](../adr/0208-build-image-taxonomy.md), which created it after a `dev` image meaning "general" was prescribed for kernel builds it could not do. The declaration carries the full toolchain those four recipes need to build from source — gcc/binutils/autotools, see [`writing-recipes.md`](writing-recipes.md); `isotools.recipe` itself doesn't rebuild them, it harvests those binaries + their real shared-library closure the installs above just produced into a single, portable, host-executable artifact.) Then, with a real Secure Boot signing key pair installed on the host at `<data-dir>/keys/cix-signing.{key,crt,cer}`:

```
cixctl signing-keys set --key=image/keys/cix-signing.key --cert=image/keys/cix-signing.crt
cixctl signing-keys            # confirm: subject, expiry, fingerprint
```

This guide used to say the pair was "staged out of band", which is what ADR-0064 specified — but a real Cix host runs no sshd and has no console, so there was no out-of-band channel and this step could not actually be performed. [ADR-0212](../adr/0212-signing-keys-over-rest.md) makes it a REST operation; `cixctl` sends the file contents, so the private key never appears in shell history or the host's process list. `cixd` still never generates or fetches this key on its own, and only the public DER `.cer` is ever staged onto an installed target — put it on the one host that cuts media, not on every box. Then:

Optionally, install a **release-signing key** as well ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)) so the finished ISO is signed:

```
openssl genpkey -algorithm ed25519 -out cix-release.key   # once, kept offline
cixctl release-key set --key=cix-release.key
cixctl release-key                                        # prints the public key -- publish this
```

This is a **second, separate** key, not the pair above in another encoding. That one is RSA because UEFI requires it and it decides whether firmware will boot an image; this one is Ed25519 because minisign requires it and it tells a downloader the bytes really came from you. One key doing both jobs would mean whoever can sign a download can also sign a bootloader.

Skipping this step is fine — the build still succeeds and simply produces no signature. Whoever downloads a signed ISO verifies it with stock `minisign -Vm cix-install.iso -p cix-release.pub`, no Cix software needed on their side.

```
cixctl iso build --wait
cixctl iso status
```

The ISO exists on that host's own disk and nothing else can reach it yet. To put it where a machine being installed can actually fetch it:

```
cixctl iso publish --wait
```

That uploads the **signature first, then the ISO** — the artifact cache refuses an ISO with no signature beside it, which is the correct refusal: an unsigned installer is exactly the thing that must not be downloadable. If you skipped the release key above, this step refuses and says so, rather than publishing something no one can verify.

Published as `cix-installer-<version>-1-<arch>.iso`, alongside its `.minisig`. On the far side, with no Cix software involved:

```
curl -fsSLO http://<cache>:8080/cix-installer-<version>-1-<arch>.iso
curl -fsSLO http://<cache>:8080/cix-installer-<version>-1-<arch>.iso.minisig
minisign -Vm cix-installer-<version>-1-<arch>.iso -p cix-release.pub
```

Verify on a machine you already trust, before writing the stick. An installer that checks its own signature is the code being checked doing the checking — a substituted ISO would report success.

The public key to check against is committed at [`docs/keys/cix-release.pub`](../keys/cix-release.pub) — pin a copy once and keep it, rather than re-fetching it each time (whoever could hand you a bad ISO could hand you the key that matches it). It lives in git rather than in the artifact cache on purpose: the cache serves the bytes, so a cache that also served the key would be vouching for its own payload. See [`docs/keys/README.md`](../keys/README.md).

An empty `iso build` (no flags at all) is the normal case, and produces the ISO you want for general use: it passes the installer no arguments, and the installer asks for the disk and the network on the console with the machine's own disks and NICs listed. Pass flags only to build media that installs one specific machine unattended.

### A real, TCC-specific gap worth knowing about

If a from-scratch build image reports `tcc: error: file 'crt1.o' not found` even though `crt1.o` genuinely exists on disk: TCC maintains a separate, single-path search list for CRT startup objects (`crt1.o`/`crti.o`/`crtn.o`/`Scrt1.o`/`gcrt1.o`/`Mcrt1.o`), defaulting to `/usr/lib/x86_64-linux-gnu` — distinct from its broader `-l`/library search list, and distinct from GCC's own `LIBRARY_PATH` convention. This project's `glibc` recipe already stages both locations -- it inherited that from the retired `libc-dev`, and ADR-0217 kept the behaviour when glibc took over the multiarch include and link directories — this note exists so the symptom is recognizable if it ever resurfaces in some other build-image combination, not because it's an open problem today.
