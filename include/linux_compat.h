#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

#include <stdint.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif

/*
 * glibc declares no wrapper for clone3(2); the kernel uapi struct is
 * ABI-stable, so we declare it ourselves rather than pull in
 * <linux/sched.h>, which clashes with glibc's <sched.h> over CLONE_*
 * and struct sched_param.
 */
struct clone_args {
	uint64_t flags;
	uint64_t pidfd;
	uint64_t child_tid;
	uint64_t parent_tid;
	uint64_t exit_signal;
	uint64_t stack;
	uint64_t stack_size;
	uint64_t tls;
	uint64_t set_tid;
	uint64_t set_tid_size;
	uint64_t cgroup;
};

#ifndef CLONE_INTO_CGROUP
#define CLONE_INTO_CGROUP 0x200000000ULL
#endif

#ifndef CLONE_NEWCGROUP
#define CLONE_NEWCGROUP 0x02000000
#endif

#ifndef CLONE_PIDFD
#define CLONE_PIDFD 0x00001000
#endif

static inline long sys_clone3(struct clone_args *args, size_t size)
{
	return syscall(SYS_clone3, args, size);
}

static inline int sys_pivot_root(const char *new_root, const char *put_old)
{
	return (int)syscall(SYS_pivot_root, new_root, put_old);
}

#endif /* LINUX_COMPAT_H */
