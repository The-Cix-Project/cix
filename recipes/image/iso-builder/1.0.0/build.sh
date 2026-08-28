#
# iso-builder -- one job: hold the ISO toolchain so
# `pkg hostbuild isotools` can harvest it into the artifact cixd uses
# to assemble installer ISOs (ADR-0063/0064). ADR-0208.
#
# This replaces the image called `dev`. The rename is the point. "dev"
# named no job, so it got used for anything -- most damagingly by
# docs/guides/kernel-build-and-ab-updates.md, which told operators to
# build kernels with --build-image=dev while `dev`'s manifest held
# neither gcc nor kmod. A name that means "general" cannot be wrong,
# which is precisely why it was never noticed. Kernels now build in
# kernel-builder; this image builds ISOs.
#
# grub/sbsigntools/xorriso/mtools are IN the manifest rather than
# installed on top afterwards. The old guidance was four `pkg install`
# calls followed by a hostbuild, which left the image's identity
# implicit -- an "iso-builder" without the ISO tools is not one. Naming
# them here makes the image self-describing and makes its version hash
# actually cover the thing it exists to provide.
#
# All four build with CC=tcc (confirmed in their own recipes), so no
# gcc is needed or wanted here -- the toolchain entries below are the
# same TCC set cix-builder uses, plus what those four configure scripts
# reach for.
#
# No image_artifact_sha256: never built yet. Added once a real Cix host
# has produced and published the artifact.
#
image_packages="bash:pinned:5.2.37-2 bc:pinned:1.08.1-2 binutils:pinned:2.42-8 bison:pinned:3.8.2-2 coreutils:pinned:9.11-3 flex:pinned:2.6.4-4 gawk:pinned:5.3.0-2 grep:pinned:3.11-4 grub:pinned:2.14 libc-dev:pinned:2.36-5 m4:pinned:1.4.19-2 make:pinned:4.4.1-4 mtools:pinned:4.0.49 sbsigntools:pinned:0.9.5 sed:pinned:4.9-2 tar:pinned:1.35-5 tcc:pinned:0.9.27-9 xorriso:pinned:1.5.8.pl02 xz:pinned:5.8.3-4 zlib:pinned:1.3.2-6"
