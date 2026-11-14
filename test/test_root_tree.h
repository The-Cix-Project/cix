#ifndef TEST_ROOT_TREE_H
#define TEST_ROOT_TREE_H

/*
 * Stages a control-plane root TREE at image_root from a source root
 * (host_tools_dir): cixd, cix-init and the C library through
 * test_image_fixture_build(), cixctl at /usr/bin/cixctl, every program
 * include/controlplane_programs.h names, every shared object the source
 * carries, the C.UTF-8 locale, openssl.cnf and the CA bundle, the web
 * tree, and -- each only when its argument is non-empty -- firmware, a
 * kernel module tree and the module tools. Mount points, /etc/resolv.conf
 * and /etc/os-release are not added: mkbootroot adds those when it
 * assembles any tree.
 *
 * For tests: a build container's composed "/" is the source. Returns 0,
 * or 1 with the reason on stderr.
 */
int test_root_tree_build(const char *image_root, const char *cixd_bin, const char *cixctl_bin,
                         const char *web_dir, const char *firmware_dir, const char *modules_dir,
                         const char *kmod_bin_dir, const char *host_tools_dir);

#endif
