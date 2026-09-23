# Building Cix

Cix builds itself. `cixd`, `cixctl`, the web dashboard and the installer tools are compiled on a Cix host, by the `cix` recipe, with the Cix-built toolchain; the result is assembled into a control-plane root and written to the host's inactive boot slot. There is no supported route that compiles Cix somewhere else and ships the result: see the Build Provenance Mandate in the repository's `CLAUDE.md`.

## What `make` produces

The root `Makefile` is what the `cix` recipe runs inside its build container. `make` (the `all` target) builds every binary this repository produces into `build/`, with `-Wall -Werror`:

- `cixd` and `cixctl`
- `cix-init`, the freestanding PID 1 of every container ([ADR-0260](../adr/0260-a-container-declares-services-not-a-command.md))
- the installer tools: `mkbootroot`, `mkinstalleriso`, `cix-install`, `cix-recover`, `mktoolchainimage`, and `cix-boot.efi` (built with gcc, because a PE/COFF EFI image needs the MS ABI, which TCC does not emit)
- one test binary per feature, each forking its own `cixd` against a scratch `--data-dir`

There is no `make install`. The named targets beyond `all` and `clean`:

| target | what it does | gates? |
|---|---|---|
| `make selftest` | runs the curated `SELFTESTS` subset: the tests measured to run inside a package build container. The `cix` recipe runs it on every build | yes: any failure fails the build |
| `make testreport` | builds `all`, then runs **every** `build/test_*` under a `TESTREPORT_TIMEOUT` (default 300 s) and reports each as PASS, FAIL (with the tail of its log from `build/testreport/`) or TIMEOUT, then a summary. The `cix-tests` recipe runs it | no: exits 0 whatever the results, since there is no baseline yet |
| `make aggressive` | runs `test_aggressive`, the harness that attacks a live daemon; the `cix-aggressive-test` recipe runs it with a negative control | yes |
| `make web-syntax` | `node --check` over `web/*.js`, when `node` is present | yes, when it runs |

A test that is not in `SELFTESTS` is **not** a gate on any cix build: passing `make selftest` says nothing about it. `testreport` is where it runs.

The tests create real containers, networks and cgroups, so they run where the platform runs: in the `cix`, `cix-tests` and `cix-aggressive-test` recipes on a Cix host, which is why those three recipes declare `CAP_SYS_ADMIN` (see [`writing-recipes.md`](writing-recipes.md#required-metadata-fields)).

The kernel Cix boots is not part of this build — see [`kernel-build-and-ab-updates.md`](kernel-build-and-ab-updates.md).

## Rebuilding Cix on a running host

The rebuild is a [hostbuild](writing-recipes.md#the-hostbuild-variant) of the `cix` recipe ([ADR-0056](../adr/0056-hostbuild-artifact-mechanism.md), [ADR-0057](../adr/0057-self-hosted-toolchain-and-control-plane-rebuild.md)). Cix's own code is compiled with TCC ([ADR-0001](../adr/0001-tcc-exclusive-toolchain.md)); `cix-boot.efi` is the one gcc-built file, for the reason above.

### 1. Nothing to prepare for the build itself

A hostbuild composes its build container from the recipe's own declared build tools ([ADR-0304](../adr/0304-a-hostbuild-composes-its-build-environment-like-every-other-build.md)), so there is no build image to create. The `cix` recipe declares what it uses — TCC, make, the headers, the libraries `cixd` links (`-lssl -lcrypto -larchive -lcurl`), quickjs for the dashboard syntax gate, gcc for `cix-boot.efi`, and the rest — and every declared tool must be installed in some image on the host, or the build fails naming it.

Two optional images change what the assembled root contains. Both are ordinary images built with ordinary installs, found by fixed name when the root is assembled.

#### `cix-hosttools` (optional, ADR-0078)

`mkbootroot` stages the binaries `cixd` shells out to at runtime into the control-plane root. Without this image it copies them from the host running the assembly — on a Cix host, that is the running control-plane root. With it, these come from Cix-built packages instead:

```
cixctl image create --name=cix-hosttools
cixctl pkg install --name=coreutils --image=cix-hosttools
cixctl pkg install --name=gzip --image=cix-hosttools
cixctl pkg install --name=squashfs-tools --image=cix-hosttools
cixctl pkg install --name=btrfs-progs --image=cix-hosttools
```

`cix-hosttools` is `HOST_TOOLS_IMAGE` in `daemon/src/main.c`; `spawn_cix_bootroot_assembly()` passes its rootfs to `mkbootroot` as the host-tools directory. From it `mkbootroot` stages `cp` and `gzip`, stages `btrfs` and `mkfs.btrfs` (which exist in the root only if this image carries them — without them `fs_type: "btrfs"` fails at exec), and runs `mksquashfs` to seal the root, with the image's own libraries on its search path ([ADR-0154](../adr/0154-host-tools-mksquashfs-ld-library-path.md)). The other tools `cixd` shells out to (`openssl`, `tar`, `bzip2`, `xz`, `unsquashfs`, `mkfs.ext4`) are still staged from the assembling host's own filesystem (`image/src/mkbootroot.c`).

#### `cix-firmware` (optional, ADR-0263)

A driver asks the kernel for firmware during device probe, and the kernel answers it out of the root filesystem it booted — before any container exists, so a blob inside a container can never be reached. Any host whose hardware needs firmware therefore needs it in the control-plane root:

```
cixctl image create --name=cix-firmware
cixctl pkg install --name=rtw88-firmware --image=cix-firmware
cixctl pkg install --name=wireless-regdb --image=cix-firmware
```

`cix-firmware` is `FIRMWARE_IMAGE` in `daemon/src/main.c`, resolved the same way: `spawn_cix_bootroot_assembly()` takes the image's current version, then its own `lib/firmware`, and hands that to `mkbootroot` as the firmware root to stage. Firmware packages install there, so its contents already mirror `/lib/firmware` — `rtw88/rtw8822b_fw.bin` and a bare `regulatory.db` land where `request_firmware()` looks.

Install only what the hardware needs — the two above are what an 802.11ac access point on an RTL8822BU adapter takes. A box without this image passes an empty firmware root and `mkbootroot` skips the staging. `mkbootroot` assembles a **fresh** root every run, so the image has to exist at every assembly, or that root has no firmware in it.

### 2. Point the `cix` recipe at a tagged source snapshot

The `cix` recipe lives in the [cix-recipes](https://git.home.arpa/itdlabs/cix-recipes) repository, one flat file per version: `recipes/package/cix@<tag>[-<release>].cbs` ([ADR-0308](../adr/0308-recipes-are-their-own-repository-flat.md); older versions are `.sh`). Its source is this repository's own archive-download endpoint, pinned to a tag, never a floating branch:

```
url "https://osakka:{{REPO_TOKEN}}@git.home.arpa/api/v1/repos/itdlabs/cix/archive/<tag>.tar.gz"
```

`{{REPO_TOKEN}}` is replaced at fetch time, host-side, with the token set by `cixctl pkg repo-config set --token=…` ([writing-recipes.md](writing-recipes.md#required-metadata-fields), issue #60), so no credential is ever written into a recipe or reaches the build container. The repository is private, so an unauthenticated fetch gets Gitea's `404`, not a `401`.

To make a newer self-build available: tag this repository, then publish a new `cix` recipe for that tag with `cixctl pkg recipe add --name=cix --file=cix@<tag>.cbs` (the `.cbs` suffix selects the PBS format). A new tag's archive has a new `sha256`; learn it from the box rather than downloading it elsewhere, with a throwaway `probe-*` recipe that declares a deliberately wrong hash. The fetch then fails and the daemon logs the real one as `computed=<64 hex>` (`probe-cix-tarball@1.sh` in cix-recipes is the template).

### 3. Run the hostbuild

```
cixctl pkg hostbuild cix --upgrade --wait --deploy
```

`--upgrade` is needed on any host that has hostbuilt `cix` before: without it an already-installed hostbuild entry answers `409` ([ADR-0094](../adr/0094-hostbuild-upgrade-flag.md)). This fetches the tagged source (host-side, before the build container starts — the build container has no network access), builds `cixd`/`cixctl`/`web/` **and** `mkbootroot`, then hands off to the daemon's own assembly step: `cixd` runs the freshly built `mkbootroot` to package those artifacts into a fresh `cixd-root.squashfs` — never the CLI invoking `mkbootroot` itself, which the API-First Mandate rules out.

`--wait` polls until the hostbuild job reaches `installed` — that only means the compile finished, not that the asynchronous assembly has. `--deploy` waits for the assembly to complete ([ADR-0105](../adr/0105-bootroot-assembly-freshness.md) — comparing `GET /system/assembly`'s `completed_generation` against a baseline captured before the round started) and then calls `/system/update` with the assembled image. It does not reboot.

Driving it by hand instead:

```
cixctl assembly status        # image_path, image_complete, image_mtime
cixctl assembly start         # re-assemble without a new build; poll status
cixctl update --image=<image_path>
cixctl reboot
```

Read the `image_*` fields rather than the generation counters. The counters live in the daemon's memory and a deploy ends in a reboot, so a host that has just booted the root it assembled reports `started_generation: 0, completed_generation: 0` — the same as one that has never assembled anything. `image_mtime` is `stat()`ed from the file and survives the reboot, and `image_complete` tells you whether `/system/update` will accept those bytes before you call it (#481). Then follow the same write → reboot → confirm sequence as any other update — see [`staying-updated.md`](staying-updated.md) and [Confirming a deploy](remote-development.md#confirming-a-deploy-actually-took-effect).

### 4. Build a fresh installer ISO, server-side

The same round also produces `cix-install` and `mkinstalleriso` — enough to assemble a new installer ISO (see [`installing.md`](installing.md) for what the ISO contains and how to boot it) via `POST /system/iso` ([ADR-0064](../adr/0064-rest-driven-iso-assembly.md)). The ISO tools themselves — GRUB, `sbsign`, `xorriso`, `mtools`, `shim`, `mokutil`, `mkfs.fat` — come from one more hostbuild artifact:

```
cixctl pkg hostbuild isotools --upgrade --wait
```

The `isotools` recipe builds nothing: it declares the installed `grub`, `sbsigntools`, `xorriso`, `mtools`, `mokutil`, `shim` and `dosfstools` packages (plus the libraries they link) as its build tools, and harvests those binaries and their shared-library closure into a single portable artifact.

ISO assembly signs the EFI binaries with the host's Secure Boot key pair, stored under the daemon's state directory (`<state-dir>/keys/cix-signing.{key,crt,cer}`) and installed over REST ([ADR-0212](../adr/0212-signing-keys-over-rest.md)):

```
cixctl signing-keys set --key=image/keys/cix-signing.key --cert=image/keys/cix-signing.crt
cixctl signing-keys            # confirm: subject, expiry, fingerprint
```

`cixctl` sends the file contents, so the private key never appears in shell history or the host's process list. `cixd` never generates or fetches this key, and only the public DER `.cer` is staged onto an installed target — put it on the one host that cuts media, not on every box.

Install a **release-signing key** as well ([ADR-0220](../adr/0220-a-separate-release-signing-key.md)) so the finished ISO is signed:

```
openssl genpkey -algorithm ed25519 -out cix-release.key   # once, kept offline
cixctl release-key set --key=cix-release.key
cixctl release-key                                        # prints the public key -- publish this
```

This is a **second, separate** key, not the pair above in another encoding. That one is RSA because UEFI requires it and it decides whether firmware will boot an image; this one is Ed25519 because minisign requires it and it tells a downloader the bytes really came from you. One key doing both jobs would mean whoever can sign a download can also sign a bootloader.

Without a release key the build still succeeds and produces no signature, but `iso publish` refuses (below).

```
cixctl iso build --wait
cixctl iso status
```

The ISO exists on that host's own disk and nothing else can reach it yet. To put it where a machine being installed can fetch it:

```
cixctl iso publish --wait
```

That uploads the **signature first, then the ISO** — the artifact cache refuses an ISO with no signature beside it, so an unsigned installer is never downloadable. Without a release key this step refuses and says so.

Published as `cix-installer-<version>-1-<arch>.iso`, alongside its `.minisig`. On the far side, with no Cix software involved:

```
curl -fsSLO http://<cache>:8080/cix-installer-<version>-1-<arch>.iso
curl -fsSLO http://<cache>:8080/cix-installer-<version>-1-<arch>.iso.minisig
minisign -Vm cix-installer-<version>-1-<arch>.iso -p cix-release-2026-09.pub
```

Verify on a machine you already trust, before writing the stick. An installer that checks its own signature is the code being checked doing the checking — a substituted ISO would report success.

The current public key is committed at [`docs/keys/cix-release-2026-09.pub`](../keys/cix-release-2026-09.pub); ISOs published before 2026-09-06 verify against the retired `cix-release.pub`. Pin a copy once and keep it, rather than re-fetching it each time (whoever could hand you a bad ISO could hand you the key that matches it). It lives in git rather than in the artifact cache on purpose: a cache that also served the key would be vouching for its own payload. See [`docs/keys/README.md`](../keys/README.md).

An empty `iso build` (no flags) is the normal case, and produces the ISO you want for general use: it passes the installer no arguments, and the installer asks for the disk and the network on the console with the machine's own disks and NICs listed. Pass flags only to build media that installs one specific machine unattended.

### A TCC-specific symptom worth recognising

If a build reports `tcc: error: file 'crt1.o' not found` even though `crt1.o` exists on disk: TCC keeps a separate, single-path search list for CRT startup objects (`crt1.o`/`crti.o`/`crtn.o`/`Scrt1.o`/`gcrt1.o`/`Mcrt1.o`), defaulting to `/usr/lib/x86_64-linux-gnu` — distinct from its `-l` library search list, and from GCC's `LIBRARY_PATH` convention. The `glibc` recipe stages both locations ([ADR-0217](../adr/0217-retiring-libc-dev.md)), so this appears only in a build environment that carries some other C library layout.
