#ifndef LINUX_COMPAT_H
#define LINUX_COMPAT_H

#include <stdint.h>
#include <sys/epoll.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_clone3
#define SYS_clone3 435
#endif

#ifndef SYS_pivot_root
#define SYS_pivot_root 155
#endif

#ifndef SYS_pidfd_send_signal
#define SYS_pidfd_send_signal 424
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

static inline int sys_pidfd_send_signal(int pidfd, int sig)
{
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
}

/*
 * TCC ignores __attribute__((packed)) entirely -- confirmed even on a
 * struct we write ourselves with the attribute, not just on system
 * headers. The real kernel ABI for struct epoll_event is 12 bytes
 * (uint32_t events at offset 0, an 8-byte data union at offset 4, no
 * padding), which is exactly why <sys/epoll.h> marks it packed in the
 * first place. Under TCC's default alignment the system struct
 * compiles to 16 bytes with data at offset 8, so every epoll_ctl()/
 * epoll_wait() call silently corrupts the data field -- intermittently,
 * depending on what garbage ends up at the misread offset.
 *
 * TCC does honor #pragma pack, so we define our own byte-exact
 * replacement and use it everywhere in place of the system struct.
 * The glibc epoll_ctl()/epoll_wait() wrappers only forward the
 * pointer to the kernel syscall -- they never interpret the struct's
 * fields themselves -- so casting our correctly-laid-out pointer to
 * `struct epoll_event *` at the call site is safe.
 */
#pragma pack(push, 1)
union kx_epoll_data {
	void *ptr;
	int fd;
	uint32_t u32;
	uint64_t u64;
};
struct kx_epoll_event {
	uint32_t events;
	union kx_epoll_data data;
};
#pragma pack(pop)

static inline int kx_epoll_ctl(int epfd, int op, int fd, struct kx_epoll_event *ev)
{
	return epoll_ctl(epfd, op, fd, (struct epoll_event *)ev);
}

static inline int kx_epoll_wait(int epfd, struct kx_epoll_event *events, int maxevents,
                                 int timeout)
{
	return epoll_wait(epfd, (struct epoll_event *)events, maxevents, timeout);
}

#endif /* LINUX_COMPAT_H */
