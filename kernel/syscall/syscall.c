/*
 * A20OS syscall dispatcher.
 *
 * Keep ABI dispatch here; concrete syscall implementations live in sys_*.c.
 */
#include "syscall_internal.h"

syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];
static uint64_t syscall_resched_counter;

static inline uint64_t syscall_profile_now(void)
{
    return timer_get_ticks();
}

static inline void syscall_profile_record(uint64_t num, uint64_t start, uint64_t end)
{
    if (num >= SYSCALL_PROFILE_MAX)
        return;

    uint64_t elapsed = (end >= start) ? (end - start) : 0;
    syscall_prof_t *prof = &sys_prof[num];
    __atomic_fetch_add(&prof->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&prof->cycles, elapsed, __ATOMIC_RELAXED);
}

void syscall_profile_reset(void)
{
    memset(sys_prof, 0, sizeof(sys_prof));
}

void syscall_init(void)
{
    syscall_profile_reset();
    kdebug("[SYSCALL] Initialized\n");
}

int64_t syscall_dispatch(trap_context_t *ctx)
{
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);
    uint64_t start_time = syscall_profile_now();
    uint64_t a0 = TRAP_CTX_ARG0(ctx);
    uint64_t a1 = TRAP_CTX_ARG1(ctx);
    uint64_t a2 = TRAP_CTX_ARG2(ctx);
    uint64_t a3 = TRAP_CTX_ARG3(ctx);
    uint64_t a4 = TRAP_CTX_ARG4(ctx);
    uint64_t a5 = TRAP_CTX_ARG5(ctx);
    (void)a4;
    (void)a5;

    int64_t ret = -ENOSYS;
    int context_restored = 0;
    switch (num)
    {
    case SYS_read:
        ret = sys_read((int)a0, (char *)a1, (size_t)a2);
        break;
    case SYS_write:
        ret = sys_write((int)a0, (const char *)a1, (size_t)a2);
        break;
    case SYS_writev:
        ret = sys_writev((int)a0, (const void *)a1, (int)a2);
        break;
    case SYS_readv:
        ret = sys_readv((int)a0, (const void *)a1, (int)a2);
        break;
    case SYS_preadv:
        ret = sys_preadv((int)a0, (const void *)a1, (int)a2, (long)a3);
        break;
    case SYS_pwritev:
        ret = sys_pwritev((int)a0, (const void *)a1, (int)a2, (long)a3);
        break;
    case SYS_preadv2:
        ret = sys_preadv2((int)a0, (const void *)a1, (int)a2, (long)a3, (int)a4);
        break;
    case SYS_pwritev2:
        ret = sys_pwritev2((int)a0, (const void *)a1, (int)a2, (long)a3, (int)a4);
        break;
    case SYS_openat:
        ret = sys_openat((int)a0, (const char *)a1, (int)a2, (int)a3);
        break;
    case SYS_close:
        ret = sys_close((int)a0);
        break;
    case SYS_lseek:
        ret = sys_lseek((int)a0, (long)a1, (int)a2);
        break;
    case SYS_dup:
        ret = sys_dup((int)a0);
        break;
    case SYS_dup3:
        ret = sys_dup3((int)a0, (int)a1, (int)a2);
        break;
    case SYS_fcntl:
        ret = sys_fcntl((int)a0, (int)a1, (long)a2);
        break;
    case SYS_flock:
        ret = sys_flock((int)a0, (int)a1);
        break;
    case SYS_pipe2:
        ret = sys_pipe2((int *)a0, (int)a1);
        break;
    case SYS_ioctl:
        ret = sys_ioctl((int)a0, (unsigned long)a1, (void *)a2);
        break;
    case SYS_pread64:
        ret = sys_pread64((int)a0, (char *)a1, (size_t)a2, (long)a3);
        break;
    case SYS_pwrite64:
        ret = sys_pwrite64((int)a0, (char *)a1, (size_t)a2, (long)a3);
        break;
    case SYS_sync:
        ret = sys_sync();
        break;
    case SYS_fsync:
        ret = sys_fsync((int)a0);
        break;
    case SYS_fdatasync:
        ret = sys_fdatasync((int)a0);
        break;
    case SYS_ftruncate:
        ret = sys_ftruncate((int)a0, (size_t)a1);
        break;
    case SYS_truncate:
        ret = sys_truncate((const char *)a0, (size_t)a1);
        break;
    case SYS_fallocate:
        ret = sys_fallocate((int)a0, (int)a1, (long)a2, (long)a3);
        break;
    case SYS_fadvise64:
        ret = sys_posix_fadvise((int)a0, (long)a1, (long)a2, (int)a3);
        break;
    case SYS_copy_file_range:
        ret = sys_copy_file_range((int)a0, (long *)a1, (int)a2, (long *)a3, (size_t)a4, (unsigned)a5);
        break;
    case SYS_splice:
        ret = sys_splice((int)a0, (long *)a1, (int)a2, (long *)a3, (size_t)a4, (unsigned)a5);
        break;
    case SYS_vmsplice:
        ret = sys_vmsplice((int)a0, (const void *)a1, (unsigned long)a2, (unsigned)a3);
        break;
    case SYS_tee:
        ret = sys_tee((int)a0, (int)a1, (size_t)a2, (unsigned)a3);
        break;
    case SYS_close_range:
        ret = sys_close_range((unsigned)a0, (unsigned)a1, (unsigned)a2);
        break;
    case SYS_sendfile:
        ret = sys_sendfile((int)a0, (int)a1, (long *)a2, (size_t)a3);
        break;
    case SYS_select:
        ret = sys_pselect6((int)a0, (void *)a1, (void *)a2, (void *)a3, (void *)a4, (void *)a5);
        break;
    case SYS_ppoll:
        ret = sys_ppoll((void *)a0, (int)a1, (void *)a2, (void *)a3);
        break;
    case SYS_epoll_create1:
        ret = sys_epoll_create1((int)a0);
        break;
    case SYS_epoll_ctl:
        ret = sys_epoll_ctl((int)a0, (int)a1, (int)a2, (void *)a3);
        break;
    case SYS_epoll_pwait:
        ret = sys_epoll_pwait((int)a0, (void *)a1, (int)a2, (int)a3, (const void *)a4, (size_t)a5);
        break;
    case SYS_eventfd2:
        ret = sys_eventfd2((unsigned)a0, (int)a1);
        break;
    case SYS_timerfd_create:
        ret = sys_timerfd_create((int)a0, (int)a1);
        break;
    case SYS_timerfd_settime:
        ret = sys_timerfd_settime((int)a0, (int)a1, (const void *)a2, (void *)a3);
        break;
    case SYS_timerfd_gettime:
        ret = sys_timerfd_gettime((int)a0, (void *)a1);
        break;
    case SYS_inotify_init1:
        ret = sys_inotify_init1((int)a0);
        break;
    case SYS_socket:
        ret = sys_socket((int)a0, (int)a1, (int)a2);
        break;
    case SYS_socketpair:
        ret = sys_socketpair((int)a0, (int)a1, (int)a2, (int *)a3);
        break;
    case SYS_bind:
        ret = sys_bind((int)a0, (const void *)a1, (size_t)a2);
        break;
    case SYS_listen:
        ret = sys_listen((int)a0, (int)a1);
        break;
    case SYS_accept:
        ret = sys_accept((int)a0, (void *)a1, (void *)a2);
        break;
    case SYS_accept4:
        ret = sys_accept4((int)a0, (void *)a1, (void *)a2, (int)a3);
        break;
    case SYS_connect:
        ret = sys_connect((int)a0, (const void *)a1, (size_t)a2);
        break;
    case SYS_getsockname:
        ret = sys_getsockname((int)a0, (void *)a1, (void *)a2);
        break;
    case SYS_getpeername:
        ret = sys_getpeername((int)a0, (void *)a1, (void *)a2);
        break;
    case SYS_sendto:
        ret = sys_sendto((int)a0, (const void *)a1, (size_t)a2, (int)a3, (const void *)a4, (size_t)a5);
        break;
    case SYS_recvfrom:
        ret = sys_recvfrom((int)a0, (void *)a1, (size_t)a2, (int)a3, (void *)a4, (void *)a5);
        break;
    case SYS_setsockopt:
        ret = sys_setsockopt((int)a0, (int)a1, (int)a2, (const void *)a3, (size_t)a4);
        break;
    case SYS_getsockopt:
        ret = sys_getsockopt((int)a0, (int)a1, (int)a2, (void *)a3, (void *)a4);
        break;
    case SYS_shutdown:
        ret = sys_shutdown((int)a0, (int)a1);
        break;
    case SYS_sendmsg:
        ret = sys_sendmsg((int)a0, (const void *)a1, (int)a2);
        break;
    case SYS_recvmsg:
        ret = sys_recvmsg((int)a0, (void *)a1, (int)a2);
        break;
    case SYS_sendmmsg:
        ret = sys_sendmmsg((int)a0, (void *)a1, (unsigned)a2, (int)a3);
        break;
    case SYS_recvmmsg:
        ret = sys_recvmmsg((int)a0, (void *)a1, (unsigned)a2, (int)a3, (void *)a4);
        break;

    case SYS_mkdirat:
        ret = sys_mkdirat((int)a0, (const char *)a1, (int)a2);
        break;
    case SYS_unlinkat:
        ret = sys_unlinkat((int)a0, (const char *)a1, (int)a2);
        break;
    case SYS_renameat2:
        ret = sys_renameat2((int)a0, (const char *)a1, (int)a2, (const char *)a3, (int)a4);
        break;
    case SYS_chdir:
        ret = sys_chdir((const char *)a0);
        break;
    case SYS_fchdir:
        ret = sys_fchdir((int)a0);
        break;
    case SYS_getcwd:
        ret = sys_getcwd((char *)a0, (size_t)a1);
        break;
    case SYS_fstat:
        ret = sys_fstat((int)a0, (void *)a1);
        break;
    case SYS_fstatat:
        ret = sys_fstatat((int)a0, (const char *)a1, (void *)a2, (int)a3);
        break;
    case SYS_readlinkat:
        ret = sys_readlinkat((int)a0, (const char *)a1, (char *)a2, (size_t)a3);
        break;
    case SYS_faccessat:
        ret = sys_faccessat((int)a0, (const char *)a1, (int)a2);
        break;
    case SYS_faccessat2:
        ret = sys_faccessat2((int)a0, (const char *)a1, (int)a2, (int)a3);
        break;
    case SYS_fchmod:
        ret = sys_fchmod((int)a0, (int)a1);
        break;
    case SYS_fchmodat:
        ret = sys_fchmodat((int)a0, (const char *)a1, (int)a2, 0);
        break;
    case SYS_fchmodat2:
        ret = sys_fchmodat((int)a0, (const char *)a1, (int)a2, (int)a3);
        break;
    case SYS_fchown:
        ret = sys_fchown((int)a0, (int)a1, (int)a2);
        break;
    case SYS_fchownat:
        ret = sys_fchownat((int)a0, (const char *)a1, (int)a2, (int)a3, (int)a4);
        break;
    case SYS_setxattr:
        ret = sys_setxattr((const char *)a0, (const char *)a1, (const void *)a2, (size_t)a3, (int)a4);
        break;
    case SYS_lsetxattr:
        ret = sys_lsetxattr((const char *)a0, (const char *)a1, (const void *)a2, (size_t)a3, (int)a4);
        break;
    case SYS_fsetxattr:
        ret = sys_fsetxattr((int)a0, (const char *)a1, (const void *)a2, (size_t)a3, (int)a4);
        break;
    case SYS_getxattr:
        ret = sys_getxattr((const char *)a0, (const char *)a1, (void *)a2, (size_t)a3);
        break;
    case SYS_lgetxattr:
        ret = sys_lgetxattr((const char *)a0, (const char *)a1, (void *)a2, (size_t)a3);
        break;
    case SYS_fgetxattr:
        ret = sys_fgetxattr((int)a0, (const char *)a1, (void *)a2, (size_t)a3);
        break;
    case SYS_listxattr:
        ret = sys_listxattr((const char *)a0, (char *)a1, (size_t)a2);
        break;
    case SYS_llistxattr:
        ret = sys_llistxattr((const char *)a0, (char *)a1, (size_t)a2);
        break;
    case SYS_flistxattr:
        ret = sys_flistxattr((int)a0, (char *)a1, (size_t)a2);
        break;
    case SYS_removexattr:
        ret = sys_removexattr((const char *)a0, (const char *)a1);
        break;
    case SYS_lremovexattr:
        ret = sys_lremovexattr((const char *)a0, (const char *)a1);
        break;
    case SYS_fremovexattr:
        ret = sys_fremovexattr((int)a0, (const char *)a1);
        break;
    case SYS_getdents64:
        ret = sys_getdents64((int)a0, (void *)a1, (size_t)a2);
        break;
    case SYS_linkat:
        ret = sys_linkat((int)a0, (const char *)a1, (int)a2, (const char *)a3, (int)a4);
        break;
    case SYS_symlinkat:
        ret = sys_symlinkat((const char *)a0, (int)a1, (const char *)a2);
        break;
    case SYS_statx:
        ret = sys_statx((int)a0, (const char *)a1, (int)a2, (unsigned)a3, (void *)a4);
        break;
    case SYS_statfs:
        ret = sys_statfs((const char *)a0, (void *)a1);
        break;
    case SYS_fstatfs:
        ret = sys_fstatfs((int)a0, (void *)a1);
        break;
    case SYS_mount:
        ret = sys_mount((const char *)a0, (const char *)a1, (const char *)a2, (int)a3);
        break;
    case SYS_umount2:
        ret = sys_umount2((const char *)a0, (int)a1);
        break;
    case SYS_utimensat:
        ret = sys_utimensat((int)a0, (const char *)a1, (void *)a2, (int)a3);
        break;
    case SYS_chroot:
        ret = sys_chroot((const char *)a0);
        break;
    case SYS_mknodat:
        ret = sys_mknodat((int)a0, (const char *)a1, (int)a2, (unsigned)a3);
        break;

    case SYS_exit:
        ret = sys_exit((int)a0);
        break;
    case SYS_exit_group:
        ret = sys_exit_group((int)a0);
        break;
    case SYS_waitid:
        ret = sys_waitid((int)a0, (int)a1, (void *)a2, (int)a3, (void *)a4);
        break;
    case SYS_getpid:
        ret = sys_getpid();
        break;
    case SYS_getppid:
        ret = sys_getppid();
        break;
    case SYS_gettid:
        ret = sys_gettid();
        break;
    case SYS_set_tid_address:
        ret = sys_set_tid_address((int *)a0);
        break;
    case SYS_set_robust_list:
        ret = sys_set_robust_list((void *)a0, (size_t)a1);
        break;
    case SYS_capget:
        ret = sys_capget((void *)a0, (void *)a1);
        break;
    case SYS_capset:
        ret = sys_capset((void *)a0, (const void *)a1);
        break;
    case SYS_getuid:
        ret = sys_getuid();
        break;
    case SYS_geteuid:
        ret = sys_geteuid();
        break;
    case SYS_getgid:
        ret = sys_getgid();
        break;
    case SYS_getegid:
        ret = sys_getegid();
        break;
    case SYS_setuid:
        ret = sys_setuid((int)a0);
        break;
    case SYS_setgid:
        ret = sys_setgid((int)a0);
        break;
    case SYS_setreuid:
        ret = sys_setreuid((int)a0, (int)a1);
        break;
    case SYS_setregid:
        ret = sys_setregid((int)a0, (int)a1);
        break;
    case SYS_setresuid:
        ret = sys_setresuid((int)a0, (int)a1, (int)a2);
        break;
    case SYS_getresuid:
        ret = sys_getresuid((int *)a0, (int *)a1, (int *)a2);
        break;
    case SYS_setresgid:
        ret = sys_setresgid((int)a0, (int)a1, (int)a2);
        break;
    case SYS_getresgid:
        ret = sys_getresgid((int *)a0, (int *)a1, (int *)a2);
        break;
    case SYS_setfsuid:
        ret = sys_setfsuid((int)a0);
        break;
    case SYS_setfsgid:
        ret = sys_setfsgid((int)a0);
        break;
    case SYS_getpgid:
        ret = sys_getpgid((int)a0);
        break;
    case SYS_setpgid:
        ret = sys_setpgid((int)a0, (int)a1);
        break;
    case SYS_setsid:
        ret = sys_setsid();
        break;
    case SYS_clone:
#if defined(CONFIG_LOONGARCH64)
        ret = sys_clone(a0, (void *)a1, (int *)a2, a4, (int *)a3);
#else
        ret = sys_clone(a0, (void *)a1, (int *)a2, a3, (int *)a4);
#endif
        break;
    case SYS_execve:
        ret = sys_execve((const char *)a0, (char **)a1, (char **)a2);
        break;
    case SYS_execveat:
        ret = sys_execveat((int)a0, (const char *)a1, (char **)a2, (char **)a3, (int)a4);
        break;
    case SYS_wait4:
        ret = sys_wait4((int)a0, (int *)a1, (int)a2, (void *)a3);
        break;
    case SYS_sched_yield:
        ret = sys_sched_yield();
        break;
    case SYS_sched_get_priority_max:
        ret = sys_sched_get_priority_max((int)a0);
        break;
    case SYS_sched_get_priority_min:
        ret = sys_sched_get_priority_min((int)a0);
        break;
    case SYS_sched_getaffinity:
        ret = sys_sched_getaffinity((int)a0, (size_t)a1, (void *)a2);
        break;
    case SYS_sched_setaffinity:
        ret = sys_sched_setaffinity((int)a0, (size_t)a1, (const void *)a2);
        break;
    case SYS_sched_getparam:
        ret = sys_sched_getparam((int)a0, (void *)a1);
        break;
    case SYS_sched_setparam:
        ret = sys_sched_setparam((int)a0, (const void *)a1);
        break;
    case SYS_sched_getscheduler:
        ret = sys_sched_getscheduler((int)a0);
        break;
    case SYS_sched_setscheduler:
        ret = sys_sched_setscheduler((int)a0, (int)a1, (const void *)a2);
        break;
    case SYS_sched_rr_get_interval:
        ret = sys_sched_rr_get_interval((int)a0, (void *)a1);
        break;
    case SYS_getpriority:
        ret = sys_getpriority((int)a0, (int)a1);
        break;
    case SYS_setpriority:
        ret = sys_setpriority((int)a0, (int)a1, (int)a2);
        break;
    case SYS_reboot:
        ret = sys_reboot(a0, a1, a2);
        break;
    case SYS_prctl:
        ret = sys_prctl((int)a0, a1, a2, a3, a4);
        break;
    case SYS_prlimit64:
        ret = sys_prlimit64((int)a0, (int)a1, (void *)a2, (void *)a3);
        break;
    case SYS_getrlimit:
        ret = sys_getrlimit((int)a0, (void *)a1);
        break;
    case SYS_setrlimit:
        ret = sys_setrlimit((int)a0, (void *)a1);
        break;
    case SYS_getrusage:
        ret = sys_getrusage((int)a0, (void *)a1);
        break;

    case SYS_kill:
        ret = sys_kill((int)a0, (int)a1);
        break;
    case SYS_tkill:
        ret = sys_tkill((int)a0, (int)a1);
        break;
    case SYS_tgkill:
        ret = sys_tgkill((int)a0, (int)a1, (int)a2);
        break;
    case SYS_sigaction:
        ret = sys_sigaction((int)a0, (void *)a1, (void *)a2);
        break;
    case SYS_sigprocmask:
        ret = sys_sigprocmask((int)a0, (void *)a1, (void *)a2);
        break;
    case SYS_sigtimedwait:
        ret = sys_sigtimedwait((const uint64_t *)a0, (void *)a1, (const void *)a2);
        break;
    case SYS_rt_sigqueueinfo:
        ret = sys_rt_sigqueueinfo((int)a0, (int)a1, (void *)a2);
        break;
    case SYS_sigreturn:
        ret = sys_sigreturn(ctx);
        context_restored = 1;
        break;
    case SYS_sigsuspend:
        ret = sys_sigsuspend((void *)a0);
        break;

    case SYS_brk:
        ret = sys_brk(a0);
        break;
    case SYS_mmap:
        ret = sys_mmap(a0, (size_t)a1, (int)a2, (int)a3, (int)a4, (long)a5);
        break;
    case SYS_munmap:
        ret = sys_munmap(a0, (size_t)a1);
        break;
    case SYS_mprotect:
        ret = sys_mprotect(a0, (size_t)a1, (int)a2);
        break;
    case SYS_madvise:
        ret = sys_madvise(a0, (size_t)a1, (int)a2);
        break;
    case SYS_mremap:
        ret = sys_mremap(a0, (size_t)a1, (size_t)a2, (int)a3, (uint64_t)a4);
        break;
    case SYS_shm_open:
        ret = sys_shm_open((const char *)a0, (int)a1, (int)a2);
        break;
    case SYS_memfd_create:
        ret = sys_memfd_create((const char *)a0, (unsigned)a1);
        break;
    case SYS_shmget:
        ret = sys_shmget((int)a0, (size_t)a1, (int)a2);
        break;
    case SYS_shmat:
        ret = sys_shmat((int)a0, (const void *)a1, (int)a2);
        break;
    case SYS_shmdt:
        ret = sys_shmdt((const void *)a0);
        break;
    case SYS_shmctl:
        ret = sys_shmctl((int)a0, (int)a1, (void *)a2);
        break;
    case SYS_bpf:
        ret = sys_bpf((int)a0, (void *)a1, (unsigned)a2);
        break;

    case SYS_clock_settime:
        ret = sys_clock_settime((int)a0, (void *)a1);
        break;
    case SYS_clock_gettime:
        ret = sys_clock_gettime((int)a0, (void *)a1);
        break;
    case SYS_clock_getres:
        ret = sys_clock_getres((int)a0, (void *)a1);
        break;
    case SYS_nanosleep:
        ret = sys_nanosleep((void *)a0, (void *)a1);
        break;
    case SYS_clock_nanosleep:
        ret = sys_clock_nanosleep((int)a0, (int)a1, (const void *)a2, (void *)a3);
        break;
    case SYS_getitimer:
        ret = sys_getitimer((int)a0, (void *)a1);
        break;
    case SYS_setitimer:
        ret = sys_setitimer((int)a0, (const void *)a1, (void *)a2);
        break;
    case SYS_timer_create:
        ret = sys_timer_create((int)a0, (void *)a1, (int *)a2);
        break;
    case SYS_timer_delete:
        ret = sys_timer_delete((int)a0);
        break;
    case SYS_timer_gettime:
        ret = sys_timer_gettime((int)a0, (void *)a1);
        break;
    case SYS_timer_getoverrun:
        ret = sys_timer_getoverrun((int)a0);
        break;
    case SYS_timer_settime:
        ret = sys_timer_settime((int)a0, (int)a1, (const void *)a2, (void *)a3);
        break;
    case SYS_adjtimex:
        ret = sys_adjtimex((void *)a0);
        break;
    case SYS_clock_adjtime:
        ret = sys_clock_adjtime((int)a0, (void *)a1);
        break;
    case SYS_gettimeofday:
        ret = sys_gettimeofday((void *)a0, (void *)a1);
        break;
    case SYS_settimeofday:
        ret = sys_settimeofday((void *)a0, (void *)a1);
        break;
    case SYS_times:
        ret = sys_times((void *)a0);
        break;
    case SYS_time:
        ret = sys_time((long *)a0);
        break;
    case SYS_alarm:
        ret = sys_alarm((unsigned)a0);
        break;

    case SYS_uname:
        ret = sys_uname((void *)a0);
        break;
    case SYS_sysinfo:
        ret = sys_sysinfo((void *)a0);
        break;
    case SYS_getgroups:
        ret = sys_getgroups((int)a0, (int *)a1);
        break;
    case SYS_setgroups:
        ret = sys_setgroups((size_t)a0, (const int *)a1);
        break;
    case SYS_umask:
        ret = sys_umask((int)a0);
        break;
    case SYS_syslog:
        ret = sys_syslog((int)a0, (char *)a1, (int)a2);
        break;

    case SYS_getrandom:
        ret = sys_getrandom((void *)a0, (size_t)a1, (int)a2);
        break;
    case SYS_futex:
        ret = sys_futex((int *)a0, (int)a1, (int)a2, (void *)a3, (int *)a4, (int)a5);
        break;
    case SYS_membarrier:
        ret = sys_membarrier((int)a0, (unsigned)a1, (int)a2);
        break;

    default:
        if (num < 300)
            kdebug("[SYSCALL] Unimplemented: %lu\n", (unsigned long)num);
        ret = -ENOSYS;
        break;
    }

    if (!context_restored)
        TRAP_CTX_SET_RET(ctx, ret);
    syscall_profile_record(num, start_time, syscall_profile_now());
    signal_deliver_user(ctx);
    if (!context_restored && proc_current() && (++syscall_resched_counter & 0x1f) == 0)
        proc_yield();
    if (num == SYS_sigsuspend)
    {
        task_t *cur = proc_current();
        if (cur && cur->signals && cur->sigsuspend_active && !cur->sig_handling)
        {
            signal_state_t *ss = (signal_state_t *)cur->signals;
            ss->blocked = cur->sigsuspend_old_blocked;
            cur->sigsuspend_active = 0;
            if (syscall_sig_diag_count < 128)
            {
                syscall_sig_diag_count++;
                kdebug("[SIGSYS] sigsuspend-ret pid=%d pending=0x%lx blocked=0x%lx\n",
                       cur->pid, (unsigned long)ss->pending, (unsigned long)ss->blocked);
            }
        }
    }
    return ret;
}
