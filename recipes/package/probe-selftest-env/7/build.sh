#
# probe-selftest-env 5 -- does a build container have its OWN network
# namespace? (issue #224)
#
# Probe 4 reported host interfaces (cix-ctnet0, cix-test0, vt-a, vt-b)
# visible from inside a build container and I read that as a leak: tests
# "passing" by mutating the build host. Six tests were excluded from the
# build gate on that basis.
#
# That reading is probably wrong, and the correction matters more than
# the original claim. src/container.c says in as many words that every
# build sandbox has its own netns ("a container with its own netns but
# no networks (every build sandbox)"), CLONE_NEWNET is in the default
# clone flags, and the host reports none of those devices afterwards.
#
# /sys/class/net is not per-netns when /sys is bind-mounted from the
# host -- it shows the mounting namespace's devices. /proc/net/dev IS
# per-netns. So the two disagreeing is the whole answer, and it is one
# file read rather than a rebuild of the suite.
#
pkg_name="probe-selftest-env"
pkg_version="7"
pkg_source="https://codeload.github.com/TinyCC/tinycc/tar.gz/2ba12e83b3599ca8f5d50c179fe5138fe956f0c9"
pkg_sha256="4eb5f0266d4d9deabe9650abedc3f0261dc06295f89ad798bb495abf680dc074"
pkg_build_depends="bash coreutils tcc sed grep linux-headers"
pkg_build_caps="CAP_SYS_ADMIN"
pkg_changelog="7: probe 6 could not compile its own C -- errno.h needs linux-headers, which it did not declare. Third time a probe has been stopped by its own build_depends (#224). 6: the same questions asked of a build container that DECLARED pkg_build_caps=CAP_SYS_ADMIN -- the point of the field (#224). 5: settle whether a build container has its own netns, by comparing /proc/net/dev (per-netns) against /sys/class/net (not, when /sys is bind-mounted). Probe 4's leak claim rests on the wrong file (#224)"

pkg_build() {
	cat > cap.c <<'CAP_EOF'
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
static void say(const char *w,int rc){printf("  %-32s %s\n",w,rc==0?"YES":strerror(errno));}
int main(void)
{
	say("unshare(CLONE_NEWNET)", unshare(CLONE_NEWNET));
	say("unshare(CLONE_NEWNS)", unshare(CLONE_NEWNS));
	mkdir("/mp",0700);
	say("mount tmpfs", mount("none","/mp","tmpfs",0,NULL));
	say("unshare(CLONE_NEWCGROUP)", unshare(CLONE_NEWCGROUP));
	say("mount cgroup2 on /sys/fs/cgroup", mount("cgroup2","/sys/fs/cgroup","cgroup2",0,NULL));
	say("mkdir a cgroup", mkdir("/sys/fs/cgroup/probe",0755));
	return 0;
}
CAP_EOF
	echo "=== what a build container with pkg_build_caps=CAP_SYS_ADMIN can do ==="
	tcc cap.c -o cap && ./cap
	echo
	echo "=== probe complete -- failing on purpose so the log is the product ==="
	exit 1
}

pkg_install() {
	:
}
