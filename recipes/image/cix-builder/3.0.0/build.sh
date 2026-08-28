#
# cix-builder -- one job: build Cix itself (cixd, cixctl, web/,
# mkbootroot) via `pkg hostbuild cix`. ADR-0208.
#
# 3.0.0 REMOVES gcc, which 2.0.0 carried at 9.5.0-6. That was never a
# decision: these manifests were captured from whatever a real host
# happened to hold (see 2.0.0's own header, "Captured from
# 192.168.15.95"), and gcc was resident there from the gcc-bootstrap
# work, so the snapshot took it.
#
# It has to go, because ADR-0001 makes TCC the exclusive toolchain for
# this project's own code and an available gcc is how that gets
# violated without anyone noticing: a `configure` that does not pin
# CC=tcc silently selects the real compiler. That is not hypothetical
# here -- it is exactly how an sshd was once produced with -lpam on its
# link line and no PAM recorded in the resulting ELF at all.
#
# tcc is pinned to 0.9.27-9, which adds __has_include (issue #167's own
# chain) on top of -7's do-while codegen fix.
#
# Everything else earns its place for a plain Makefile build of this
# repo: make to drive it, libc-dev for headers and the CRT startup
# objects, bash because glibc's popen() hardcodes /bin/sh, coreutils
# for the Makefile's own mkdir, openssl because cixd links -lssl
# -lcrypto, and binutils/zlib/m4/perl/bc/flex/tar as the closure those
# bring with them.
#
# No image_artifact_sha256: this version has never been built. That
# line approves one specific byte sequence and is added only once a
# real Cix host has produced and published the artifact -- never from a
# tarball built anywhere else.
#
image_packages="bash:pinned:5.2.37 bc:pinned:1.08.1-2 binutils:pinned:2.42-8 coreutils:pinned:9.11 flex:pinned:2.6.4-4 libc-dev:pinned:2.36-5 m4:pinned:1.4.19-2 make:pinned:4.4.1 openssl:pinned:3.0.20 perl:pinned:5.40.1 tar:pinned:1.35-5 tcc:pinned:0.9.27-9 zlib:pinned:1.3.2-6"
