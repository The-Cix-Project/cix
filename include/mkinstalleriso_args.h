#ifndef MKINSTALLERISO_ARGS_H
#define MKINSTALLERISO_ARGS_H

/*
 * The argument list cixd passes to mkinstalleriso (POST /v1/system/iso),
 * defined once for both ends (#480).
 *
 * It used to be written twice: the daemon filled argv[1] to argv[15] by
 * hand, and mkinstalleriso read argv[1] to argv[15] by hand behind an
 * `argc != 16` and a usage line kept in step by eye. The count went
 * wrong twice, each time surfacing as the ISO endpoint answering with a
 * fragment of usage text. Both ends now index by name from this list,
 * the count is the list's length, and the usage line is generated from
 * the same list, so an argument cannot be added to one end alone.
 *
 * Order is the wire order. X(id, name): id names the slot
 * (MKISO_ARG_<id>), name is what the usage line prints.
 */
#define MKISO_ARGS(X)                                                                              \
	X(STAGE_DIR, "staging-dir")                                                                \
	X(INSTALL_BIN, "cix-install-bin")                                                          \
	X(RECOVER_BIN, "cix-recover-bin")                                                          \
	X(BOOT_EFI, "cix-boot.efi")                                                                \
	X(BZIMAGE, "bzImage")                                                                      \
	X(SQUASHFS, "control-plane-squashfs")                                                      \
	X(SIGNING_KEY, "signing-key")                                                              \
	X(SIGNING_CERT_PEM, "signing-cert.crt")                                                    \
	X(SIGNING_CERT_DER, "signing-cert.cer")                                                    \
	X(OUT_ISO, "out.iso")                                                                      \
	X(KERNEL_ARGS, "kernel-args")                                                              \
	X(ISOTOOLS_ROOT, "isotools-root")                                                          \
	X(SEED_DIR, "seed-dir")                                                                    \
	X(MODULES_DIR, "kernel-modules-dir")                                                       \
	X(KMOD_BIN_DIR, "kmod-bin-dir")

#define MKISO_ARG_ENUM_(id, name) MKISO_ARG_##id,
enum mkinstalleriso_arg {
	MKISO_ARG_PROG = 0,
	MKISO_ARGS(MKISO_ARG_ENUM_)
	/* argv[0] plus every argument: the argc mkinstalleriso requires,
	 * and one less than the argv array the daemon needs for its NULL. */
	MKISO_ARGC
};
#undef MKISO_ARG_ENUM_

/* " <staging-dir> <cix-install-bin> ..." -- adjacent string literals,
 * so the whole usage line is one compile-time constant. */
#define MKISO_ARG_USAGE_(id, name) " <" name ">"
#define MKISO_USAGE_ARGS MKISO_ARGS(MKISO_ARG_USAGE_)

#endif
