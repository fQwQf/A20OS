/*
 * mlibc sysdeps for the A20OS native ABI.
 *
 * Design rules (docs/native-abi/00-overview.md):
 *  - POSIX fds are a libc construct; the kernel only knows handles.
 *    The fd -> handle mapping lives in this file (fdtable).
 *  - fork/execve do not exist; task_spawn is the process model (ENOSYS here).
 *  - signals do not exist; sigaction/sigprocmask/kill are inert stubs.
 *  - blocking waits use the native futex (Sync 0x0B00), never event hacks.
 */
#include "a20.hpp"

#include <abi-bits/errno.h>
#include <abi-bits/fcntl.h>
#include <abi-bits/stat.h>
#include <abi-bits/seek-whence.h>
#include <bits/ensure.h>
#include <bits/winsize.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/spawn-types.hpp>
#include <mlibc/tcb.hpp>
#include <mlibc/thread.hpp>
#include <mlibc/threads.hpp>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <termios.h>
#include <unistd.h>

extern "C" const a20_start_info *__a20_start_info;

namespace {

/* ------------------------------------------------------------------ */
/* fd table: POSIX fd  <->  A20 handle                                 */
/* ------------------------------------------------------------------ */

constexpr int kFdInitial = 64;

struct FdEntry {
	a20_handle_t handle = A20_HANDLE_NULL;
	int flags = 0;          /* original O_* flags */
	bool close_on_spawn = false;
	uint8_t kind = 0;       /* 0 = handle-backed, 1 = pipe (channel endpoint) */
};

/* Pipe read-side byte-stream buffering (channels are datagram-based). */
struct PipeRx {
	char *buf = nullptr;
	size_t len = 0;
	size_t off = 0;
};
constexpr int kPipeRxMax = 4096;
PipeRx g_pipe_rx[128];


FdEntry *g_fds = nullptr;
int g_fd_count = 0;
__attribute__((unused)) static volatile int g_fd_lock;

static void fd_lock() {
	while (__atomic_test_and_set(&g_fd_lock, __ATOMIC_ACQUIRE))
		;
}
static void fd_unlock() {
	__atomic_clear(&g_fd_lock, __ATOMIC_RELEASE);
}

static void fd_table_init() {
	if (g_fds)
		return;
	g_fds = (FdEntry *)calloc(kFdInitial, sizeof(FdEntry));
	g_fd_count = kFdInitial;
	for (int i = 0; i < g_fd_count; i++)
		g_fds[i].handle = A20_HANDLE_NULL;
	if (__a20_start_info) {
		g_fds[0].handle = __a20_start_info->stdin_handle;
		g_fds[0].flags = O_RDONLY;
		g_fds[1].handle = __a20_start_info->stdout_handle;
		g_fds[1].flags = O_WRONLY;
		g_fds[2].handle = __a20_start_info->stderr_handle;
		g_fds[2].flags = O_WRONLY;
	}
}

static int fd_alloc(a20_handle_t h, int flags) {
	fd_table_init();
	fd_lock();
	for (int i = 0; i < g_fd_count; i++) {
		if (g_fds[i].handle == A20_HANDLE_NULL) {
			g_fds[i].handle = h;
			g_fds[i].flags = flags;
			fd_unlock();
			return i;
		}
	}
	int old_count = g_fd_count;
	FdEntry *grown = (FdEntry *)calloc(old_count * 2, sizeof(FdEntry));
	memcpy(grown, g_fds, old_count * sizeof(FdEntry));
	for (int i = old_count; i < old_count * 2; i++)
		grown[i].handle = A20_HANDLE_NULL;
	free(g_fds);
	g_fds = grown;
	g_fd_count = old_count * 2;
	g_fds[old_count].handle = h;
	g_fds[old_count].flags = flags;
	fd_unlock();
	return old_count;
}

static a20_handle_t fd_handle(int fd) {
	fd_table_init();
	if (fd < 0 || fd >= g_fd_count)
		return A20_HANDLE_NULL;
	return g_fds[fd].handle;
}

static int fd_flags(int fd) {
	fd_table_init();
	if (fd < 0 || fd >= g_fd_count)
		return 0;
	return g_fds[fd].flags;
}

static int fd_kind(int fd) {
	fd_table_init();
	if (fd < 0 || fd >= g_fd_count)
		return 0;
	return g_fds[fd].kind;
}

static void fd_set_kind(int fd, int kind) {
	fd_lock();
	if (fd >= 0 && fd < g_fd_count)
		g_fds[fd].kind = (uint8_t)kind;
	fd_unlock();
}

static void fd_clear(int fd) {
	fd_table_init();
	fd_lock();
	if (fd >= 0 && fd < g_fd_count) {
		g_fds[fd].handle = A20_HANDLE_NULL;
		g_fds[fd].flags = 0;
		g_fds[fd].close_on_spawn = false;
		g_fds[fd].kind = 0;
	}
	fd_unlock();
	if (fd >= 0 && fd < 128) {
		free(g_pipe_rx[fd].buf);
		g_pipe_rx[fd] = PipeRx{};
	}
}

/* ------------------------------------------------------------------ */
/* cwd tracking (native ABI has no "path of handle" query)             */
/* ------------------------------------------------------------------ */

char g_cwd[PATH_MAX] = "/";

static a20_handle_t cwd_handle() {
	if (!__a20_start_info)
		return A20_HANDLE_NULL;
	return __a20_start_info->cwd_dir != A20_HANDLE_NULL
	           ? __a20_start_info->cwd_dir
	           : __a20_start_info->root_dir;
}

static a20_handle_t root_handle() {
	return __a20_start_info ? __a20_start_info->root_dir : A20_HANDLE_NULL;
}

/* Make an absolute path using the libc cwd (kernel path syscalls that do
 * not take a dir handle resolve against the kernel cwd, which we keep in
 * sync, but absolutizing is robust either way). */
static void absolutize(const char *path, char *out, size_t out_size) {
	if (path[0] == '/') {
		strncpy(out, path, out_size);
		out[out_size - 1] = '\0';
		return;
	}
	size_t cl = strlen(g_cwd);
	strncpy(out, g_cwd, out_size);
	out[out_size - 1] = '\0';
	if (cl == 0 || out[cl - 1] != '/')
		strncat(out, "/", out_size - strlen(out) - 1);
	strncat(out, path, out_size - strlen(out) - 1);
}

/* ------------------------------------------------------------------ */
/* error mapping                                                       */
/* ------------------------------------------------------------------ */

static int a20_to_errno(a20_status_t st) {
	switch (-st) {
	case A20_ERR_PERM:             return EPERM;
	case A20_ERR_NO_ENTRY:         return ENOENT;
	case A20_ERR_INTERRUPTED:      return EINTR;
	case A20_ERR_IO:               return EIO;
	case A20_ERR_BAD_HANDLE:       return EBADF;
	case A20_ERR_NO_MEMORY:        return ENOMEM;
	case A20_ERR_ACCESS:           return EACCES;
	case A20_ERR_FAULT:            return EFAULT;
	case A20_ERR_BUSY:             return EBUSY;
	case A20_ERR_EXISTS:           return EEXIST;
	case A20_ERR_NOT_SUPPORTED:    return ENOSYS;
	case A20_ERR_INVALID_ARGUMENT: return EINVAL;
	case A20_ERR_NO_SPACE:         return ENOSPC;
	case A20_ERR_NOT_DIR:          return ENOTDIR;
	case A20_ERR_IS_DIR:           return EISDIR;
	case A20_ERR_NOT_EMPTY:        return ENOTEMPTY;
	case A20_ERR_NAME_TOO_LONG:    return ENAMETOOLONG;
	case A20_ERR_WOULD_BLOCK:      return EAGAIN;
	case A20_ERR_TIMED_OUT:        return ETIMEDOUT;
	case A20_ERR_CANCELED:         return ECANCELED;
	case A20_ERR_PROTOCOL:         return EPROTO;
	case A20_ERR_RANGE:            return ERANGE;
	case A20_ERR_TYPE_MISMATCH:    return EBADF;
	case A20_ERR_NOT_FOUND:        return ENOENT;
	case A20_ERR_EXPIRED:          return EACCES;
	default:                       return EIO;
	}
}

/* ------------------------------------------------------------------ */
/* path_open helper                                                    */
/* ------------------------------------------------------------------ */

static a20_status_t do_path_open(a20_handle_t dir, const char *path,
                                 uint32_t flags, uint32_t mode,
                                 a20_handle_t *out) {
	a20_path_open_args args{
	    .size = sizeof(args), .version = 1, .dir = dir,
	    .flags = flags, .rights = 0,
	    .path = (uint64_t)path, .path_len = 0,
	    .mode = mode, .out_handle = A20_HANDLE_NULL,
	};
	a20_status_t st = a20_syscall6(A20_SYS_path_open, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st >= 0)
		*out = args.out_handle;
	return st;
}

static a20_status_t do_handle_stat(a20_handle_t h, a20_stat *st_out) {
	a20_stat st{};
	st.size = sizeof(st);
	st.version = 1;
	a20_status_t r = a20_syscall6(A20_SYS_handle_stat, h, (uint64_t)&st, 0, 0, 0, 0);
	if (r >= 0)
		*st_out = st;
	return r;
}

static void fill_stat(struct stat *out, const a20_stat *ks) {
	memset(out, 0, sizeof(*out));
	out->st_dev = ks->dev;
	out->st_ino = ks->ino;
	out->st_mode = ks->mode;
	out->st_nlink = ks->nlink;
	out->st_uid = ks->uid;
	out->st_gid = ks->gid;
	out->st_size = (off_t)ks->size_bytes;
	out->st_blksize = 4096;
	out->st_blocks = (blkcnt_t)ks->blocks;
	out->st_atim.tv_sec = (time_t)(ks->atime_ns / 1000000000ULL);
	out->st_atim.tv_nsec = (long)(ks->atime_ns % 1000000000ULL);
	out->st_mtim.tv_sec = (time_t)(ks->mtime_ns / 1000000000ULL);
	out->st_mtim.tv_nsec = (long)(ks->mtime_ns % 1000000000ULL);
	out->st_ctim.tv_sec = (time_t)(ks->ctime_ns / 1000000000ULL);
	out->st_ctim.tv_nsec = (long)(ks->ctime_ns % 1000000000ULL);
}

static int a20_seek(int fd, off_t offset, int whence, off_t *new_offset) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	uint64_t off = (uint64_t)offset;
	a20_status_t st = a20_syscall6(A20_SYS_handle_seek, h, (uint64_t)&off, (uint32_t)whence, 0, 0, 0);
	if (st < 0) {
		/* The native ABI reports non-seekable objects as NOT_SUPPORTED;
		 * POSIX wants ESPIPE from lseek(). */
		if (st == -A20_ERR_NOT_SUPPORTED)
			return ESPIPE;
		return a20_to_errno(st);
	}
	*new_offset = (off_t)off;
	return 0;
}

} // namespace

namespace mlibc {

/* ------------------------------------------------------------------ */
/* logging / panic                                                     */
/* ------------------------------------------------------------------ */

void Sysdeps<LibcLog>::operator()(const char *msg) {
	fd_table_init();
	size_t len = strlen(msg);
	a20_handle_t h = fd_handle(2);
	if (h == A20_HANDLE_NULL)
		return;
	a20_rt_handle_write(h, msg, len, A20_OFFSET_CURRENT);
}

[[noreturn]] void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("!!! mlibc panic !!!\n");
	sysdep<Exit>(127);
}

/* ------------------------------------------------------------------ */
/* TLS                                                                 */
/* ------------------------------------------------------------------ */

int Sysdeps<TcbSet>::operator()(void *pointer) {
#if defined(__riscv) || defined(__loongarch64)
	uintptr_t tp = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb);
#if defined(__riscv)
	asm volatile("mv tp, %0" ::"r"(tp));
#else
	asm volatile("move $tp, %0" ::"r"(tp));
#endif
#elif defined(__aarch64__)
	uintptr_t tp = reinterpret_cast<uintptr_t>(pointer) + sizeof(Tcb) - 0x10;
	asm volatile("msr tpidr_el0, %0" ::"r"(tp));
#elif defined(__x86_64__)
	asm volatile("mov %0, %%fs:0" ::"r"(pointer) : "memory");
	asm volatile("wrfsbase %0" ::"r"(pointer) : "memory");
#else
#error "Missing architecture specific code."
#endif
	return 0;
}

/* ------------------------------------------------------------------ */
/* futex (native Sync 0x0B00)                                          */
/* ------------------------------------------------------------------ */

static int g_next_tid = 2;
static __thread int t_cached_tid = 0;

pid_t Sysdeps<FutexTid>::operator()() {
	if (!tcb_available_flag)
		return 1;
	if (!t_cached_tid)
		t_cached_tid = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
	return t_cached_tid;
}

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *time) {
	uint64_t timeout_ns = A20_TIMEOUT_INFINITE;
	if (time)
		timeout_ns = (uint64_t)time->tv_sec * 1000000000ULL + (uint64_t)time->tv_nsec;
	a20_status_t st = a20_rt_futex_wait((uint32_t *)pointer, (uint32_t)expected, timeout_ns);
	if (st == A20_OK)
		return 0;
	if (st == -A20_ERR_WOULD_BLOCK)
		return EAGAIN;
	if (st == -A20_ERR_TIMED_OUT)
		return ETIMEDOUT;
	return a20_to_errno(st);
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool all) {
	uint32_t woken = 0;
	a20_status_t st = a20_rt_futex_wake((uint32_t *)pointer,
	                                    all ? UINT32_MAX : 1, &woken);
	if (st < 0)
		return a20_to_errno(st);
	return (int)woken;
}

/* ------------------------------------------------------------------ */
/* memory                                                              */
/* ------------------------------------------------------------------ */

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	a20_vm_alloc_args args{
	    .size = sizeof(args), .version = 1, .addr_hint = 0,
	    .length = (uint64_t)size, .prot = A20_PROT_READ | A20_PROT_WRITE,
	    .flags = 0x20 /* MAP_ANONYMOUS */, .out_addr = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_vm_alloc, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*pointer = (void *)args.out_addr;
	return 0;
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	a20_status_t st = a20_syscall6(A20_SYS_vm_unmap, (uint64_t)pointer, (uint64_t)size, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

static uint32_t prot_to_a20(int prot) {
	uint32_t out = 0;
	if (prot & PROT_READ) out |= A20_PROT_READ;
	if (prot & PROT_WRITE) out |= A20_PROT_WRITE;
	if (prot & PROT_EXEC) out |= A20_PROT_EXEC;
	return out;
}

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
	uint32_t a20_prot = prot_to_a20(prot);
	if (flags & MAP_ANONYMOUS) {
		a20_vm_alloc_args args{
		    .size = sizeof(args), .version = 1,
		    .addr_hint = (uint64_t)hint,
		    .length = (uint64_t)size, .prot = a20_prot,
		    .flags = 0x20, .out_addr = 0,
		};
		a20_status_t st = a20_syscall6(A20_SYS_vm_alloc, (uint64_t)&args, 0, 0, 0, 0, 0);
		if (st < 0)
			return a20_to_errno(st);
		*window = (void *)args.out_addr;
		return 0;
	}
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_vm_map_args args{
	    .size = sizeof(args), .version = 1, .source = h, ._pad = 0,
	    .addr_hint = (uint64_t)hint, .length = (uint64_t)size,
	    .offset = (uint64_t)offset, .prot = a20_prot, .flags = 0, .out_addr = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_vm_map, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*window = (void *)args.out_addr;
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	a20_status_t st = a20_syscall6(A20_SYS_vm_unmap, (uint64_t)pointer, (uint64_t)size, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<VmProtect>::operator()(void *pointer, size_t size, int prot) {
	a20_status_t st = a20_syscall6(A20_SYS_vm_protect, (uint64_t)pointer,
	                               (uint64_t)size, prot_to_a20(prot), 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

/* ------------------------------------------------------------------ */
/* process lifecycle                                                   */
/* ------------------------------------------------------------------ */

[[noreturn]] void Sysdeps<Exit>::operator()(int status) {
	a20_syscall6(A20_SYS_task_exit, (uint64_t)(int32_t)status, 0, 0, 0, 0, 0);
	__builtin_trap();
}

[[noreturn]] void Sysdeps<ThreadExit>::operator()() {
	a20_syscall6(A20_SYS_thread_exit, 0, 0, 0, 0, 0, 0);
	__builtin_trap();
}

pid_t Sysdeps<GetPid>::operator()() {
	static int cached_pid = 0;
	if (cached_pid)
		return cached_pid;
	if (!__a20_start_info || __a20_start_info->self_task == A20_HANDLE_NULL)
		return 1;
	a20_task_info info{};
	info.size = sizeof(info);
	info.version = 1;
	a20_status_t st = a20_syscall6(A20_SYS_task_info,
	                               __a20_start_info->self_task, (uint64_t)&info, 0, 0, 0, 0);
	if (st < 0 || info.pid <= 0)
		return 1;
	cached_pid = info.pid;
	return cached_pid;
}

pid_t Sysdeps<GetTid>::operator()() {
	return sysdep<FutexTid>();
}

pid_t Sysdeps<GetPpid>::operator()() {
	if (!__a20_start_info || __a20_start_info->self_task == A20_HANDLE_NULL)
		return 0;
	a20_task_info info{};
	info.size = sizeof(info);
	info.version = 1;
	a20_status_t st = a20_syscall6(A20_SYS_task_info,
	                               __a20_start_info->self_task, (uint64_t)&info, 0, 0, 0, 0);
	return st < 0 ? 0 : info.ppid;
}

/* ------------------------------------------------------------------ */
/* threads                                                             */
/* ------------------------------------------------------------------ */

extern "C" void __a20_thread_trampoline();

extern "C" void __mlibc_enter_thread(void *entry, void *user_arg) {
	auto tcb = mlibc::get_current_tcb();

	/* Wait until our parent publishes our TID. */
	while (!__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED))
		mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);

	__atomic_fetch_or(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);

	tcb->invokeThreadFunc(entry, user_arg);

	mlibc::thread_exit(tcb->returnValue);
}

int Sysdeps<PrepareStack>::operator()(void **stack, void *entry, void *user_arg, void *tcb,
                                      size_t *stack_size, size_t *guard_size, void **stack_base) {
	(void)tcb;
	static const size_t default_stacksize = 0x200000;
	if (!*stack_size)
		*stack_size = default_stacksize;

	void *map;
	if (*stack) {
		map = *stack;
		*guard_size = 0;
	} else {
		size_t total = *stack_size + *guard_size;
		a20_vm_alloc_args args{
		    .size = sizeof(args), .version = 1, .addr_hint = 0,
		    .length = (uint64_t)total, .prot = A20_PROT_READ | A20_PROT_WRITE,
		    .flags = 0x20, .out_addr = 0,
		};
		a20_status_t st = a20_syscall6(A20_SYS_vm_alloc, (uint64_t)&args, 0, 0, 0, 0, 0);
		if (st < 0)
			return EAGAIN;
		map = (void *)args.out_addr;
	}

	*stack_base = map;
	auto sp = reinterpret_cast<uintptr_t *>(
	    reinterpret_cast<uintptr_t>(map) + *guard_size + *stack_size);
	*--sp = reinterpret_cast<uintptr_t>(user_arg);
	*--sp = reinterpret_cast<uintptr_t>(entry);
	*stack = reinterpret_cast<void *>(sp);
	return 0;
}

int Sysdeps<Clone>::operator()(void *tcb, pid_t *pid_out, void *stack) {
#if defined(__riscv) || defined(__loongarch64)
	auto tls = reinterpret_cast<uintptr_t>(tcb) + sizeof(Tcb);
#elif defined(__aarch64__)
	auto tls = reinterpret_cast<uintptr_t>(tcb) + sizeof(Tcb) - 0x10;
#elif defined(__x86_64__)
	auto tls = reinterpret_cast<uintptr_t>(tcb);
#else
#error "Missing architecture specific code."
#endif

	a20_thread_create_args args{
	    .size = sizeof(args), .version = 1,
	    .entry = (uint64_t)&__a20_thread_trampoline,
	    .arg = 0,
	    .stack_base = (uint64_t)stack,
	    .stack_size = 16, /* initial frame only; real size owned by pthread layer */
	    .tls_base = (uint64_t)tls,
	    .flags = 0, .out_thread = A20_HANDLE_NULL,
	};
	a20_status_t st = a20_syscall6(A20_SYS_thread_create, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	if (pid_out)
		*pid_out = __atomic_fetch_add(&g_next_tid, 1, __ATOMIC_RELAXED);
	return 0;
}

} // namespace mlibc

namespace mlibc {

/* ------------------------------------------------------------------ */
/* file descriptors                                                    */
/* ------------------------------------------------------------------ */

int Sysdeps<Open>::operator()(const char *pathname, int flags, mode_t mode, int *fd) {
	fd_table_init();
	a20_handle_t h;
	a20_status_t st = do_path_open(cwd_handle(), pathname, (uint32_t)flags, (uint32_t)mode, &h);
	if (st < 0)
		return a20_to_errno(st);
	*fd = fd_alloc(h, flags);
	return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
	fd_table_init();
	a20_handle_t base = (dirfd == AT_FDCWD) ? cwd_handle() : fd_handle(dirfd);
	if (base == A20_HANDLE_NULL)
		return EBADF;
	a20_handle_t h;
	a20_status_t st = do_path_open(base, path, (uint32_t)flags, (uint32_t)mode, &h);
	if (st < 0)
		return a20_to_errno(st);
	*fd = fd_alloc(h, flags);
	return 0;
}

static ssize_t pipe_read(int fd, a20_handle_t h, void *buf, size_t count, int flags);

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	if (fd < 128 && fd_kind(fd) == 1) {
		ssize_t r = pipe_read(fd, h, buf, count, fd_flags(fd));
		if (r < 0)
			return (int)-r;
		*bytes_read = r;
		return 0;
	}
	a20_status_t st = a20_rt_handle_read(h, buf, count, A20_OFFSET_CURRENT);
	if (st < 0)
		return a20_to_errno(st);
	*bytes_read = (ssize_t)st;
	return 0;
}

static ssize_t pipe_read(int fd, a20_handle_t h, void *buf, size_t count, int flags) {
	PipeRx &rx = g_pipe_rx[fd];
	if (!rx.buf)
		rx.buf = (char *)malloc(kPipeRxMax);
	if (!rx.buf)
		return -ENOMEM;
	if (rx.off >= rx.len) {
		a20_msg_recv_args args{
		    .size = sizeof(args), .version = 1, .channel = h, ._pad = 0,
		    .data_buf = (uint64_t)rx.buf, .data_buf_len = kPipeRxMax, ._pad2 = 0,
		    .handle_buf = 0, .handle_buf_count = 0,
		    .flags = (flags & O_NONBLOCK) ? A20_MSG_NONBLOCK : 0u,
		    .out_data_len = 0, .out_handle_count = 0, .out_rights_buf = 0,
		};
		a20_status_t st = a20_syscall6(A20_SYS_channel_recv, (uint64_t)&args, 0, 0, 0, 0, 0);
		if (st < 0) {
			if (st == -A20_ERR_CANCELED)
				return 0; /* peer closed and drained: EOF */
			if (st == -A20_ERR_WOULD_BLOCK)
				return -EAGAIN;
			return -a20_to_errno(st);
		}
		rx.len = (size_t)args.out_data_len;
		rx.off = 0;
		if (rx.len == 0)
			return 0;
	}
	size_t n = rx.len - rx.off;
	if (n > count)
		n = count;
	memcpy(buf, rx.buf + rx.off, n);
	rx.off += n;
	return (ssize_t)n;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	if (fd < 128 && fd_kind(fd) == 1) {
		a20_msg_send_args args{
		    .size = sizeof(args), .version = 1, .channel = h, ._pad = 0,
		    .data = (uint64_t)buf,
		    .data_len = count > 65536 ? 65536u : (uint32_t)count,
		    .flags = (fd_flags(fd) & O_NONBLOCK) ? A20_MSG_NONBLOCK : 0u,
		    .handles = 0, .handle_count = 0, .transfer_rights = 0,
		};
		a20_status_t st = a20_syscall6(A20_SYS_channel_send, (uint64_t)&args, 0, 0, 0, 0, 0);
		if (st < 0) {
			if (st == -A20_ERR_CANCELED)
				return EPIPE;
			return a20_to_errno(st);
		}
		*bytes_written = (ssize_t)args.data_len;
		return 0;
	}
	a20_status_t st = a20_rt_handle_write(h, buf, count, A20_OFFSET_CURRENT);
	if (st < 0)
		return a20_to_errno(st);
	*bytes_written = (ssize_t)st;
	return 0;
}

int Sysdeps<Pread>::operator()(int fd, void *buf, size_t n, off_t off, ssize_t *bytes_read) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	/* The kernel handle_read only consumes the current position, so do a
	 * seek-read-restore dance.  Not atomic vs. other threads; acceptable
	 * until the ABI grows positional I/O. */
	off_t saved;
	if (int e = a20_seek(fd, 0, SEEK_CUR, &saved))
		return e;
	off_t ignored;
	if (int e = a20_seek(fd, off, SEEK_SET, &ignored))
		return e;
	a20_status_t st = a20_rt_handle_read(h, buf, n, A20_OFFSET_CURRENT);
	a20_seek(fd, saved, SEEK_SET, &ignored);
	if (st < 0)
		return a20_to_errno(st);
	*bytes_read = (ssize_t)st;
	return 0;
}

int Sysdeps<Pwrite>::operator()(int fd, const void *buf, size_t n, off_t off, ssize_t *bytes_written) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	off_t saved;
	if (int e = a20_seek(fd, 0, SEEK_CUR, &saved))
		return e;
	off_t ignored;
	if (int e = a20_seek(fd, off, SEEK_SET, &ignored))
		return e;
	a20_status_t st = a20_rt_handle_write(h, buf, n, A20_OFFSET_CURRENT);
	a20_seek(fd, saved, SEEK_SET, &ignored);
	if (st < 0)
		return a20_to_errno(st);
	*bytes_written = (ssize_t)st;
	return 0;
}

int Sysdeps<Readv>::operator()(int fd, const struct iovec *iovs, int iovc, ssize_t *bytes_read) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	if (iovc <= 0)
		return EINVAL;
	/* Single-iov fast path avoids the bounce buffer. */
	if (iovc == 1)
		return sysdep<Read>(fd, iovs[0].iov_base, iovs[0].iov_len, bytes_read);
	size_t total = 0;
	for (int i = 0; i < iovc; i++)
		total += iovs[i].iov_len;
	void *bounce = malloc(total);
	if (!bounce)
		return ENOMEM;
	ssize_t got = 0;
	int e = sysdep<Read>(fd, bounce, total, &got);
	if (!e) {
		size_t off = 0;
		size_t left = (size_t)got;
		for (int i = 0; i < iovc && left; i++) {
			size_t chunk = iovs[i].iov_len < left ? iovs[i].iov_len : left;
			memcpy(iovs[i].iov_base, (char *)bounce + off, chunk);
			off += chunk;
			left -= chunk;
		}
		*bytes_read = got;
	}
	free(bounce);
	return e;
}

int Sysdeps<Writev>::operator()(int fd, const struct iovec *iovs, int iovc, ssize_t *bytes_written) {
	if (iovc <= 0)
		return EINVAL;
	if (iovc == 1)
		return sysdep<Write>(fd, iovs[0].iov_base, iovs[0].iov_len, bytes_written);
	size_t total = 0;
	for (int i = 0; i < iovc; i++)
		total += iovs[i].iov_len;
	void *bounce = malloc(total);
	if (!bounce)
		return ENOMEM;
	size_t off = 0;
	for (int i = 0; i < iovc; i++) {
		memcpy((char *)bounce + off, iovs[i].iov_base, iovs[i].iov_len);
		off += iovs[i].iov_len;
	}
	int e = sysdep<Write>(fd, bounce, total, bytes_written);
	free(bounce);
	return e;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	return a20_seek(fd, offset, whence, new_offset);
}

int Sysdeps<Close>::operator()(int fd) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	fd_clear(fd);
	a20_status_t st = a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Stat>::operator()(mlibc::fsfd_target fsfdt, int fd, const char *path, int flags, struct stat *statbuf) {
	(void)flags;
	fd_table_init();
	a20_handle_t h;
	a20_handle_t opened = A20_HANDLE_NULL;
	switch (fsfdt) {
	case mlibc::fsfd_target::fd:
		h = fd_handle(fd);
		break;
	case mlibc::fsfd_target::path:
		if (do_path_open(cwd_handle(), path, O_RDONLY, 0, &opened) < 0)
			return ENOENT;
		h = opened;
		break;
	case mlibc::fsfd_target::fd_path: {
		a20_handle_t base = fd_handle(fd);
		if (base == A20_HANDLE_NULL)
			return EBADF;
		if (do_path_open(base, path, O_RDONLY, 0, &opened) < 0)
			return ENOENT;
		h = opened;
		break;
	}
	default:
		return EINVAL;
	}
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_stat ks;
	a20_status_t st = do_handle_stat(h, &ks);
	if (opened != A20_HANDLE_NULL)
		a20_syscall6(A20_SYS_handle_close, opened, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	fill_stat(statbuf, &ks);
	return 0;
}

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	fd_table_init();
	a20_handle_t h;
	a20_status_t st = do_path_open(cwd_handle(), path, O_RDONLY | O_DIRECTORY, 0, &h);
	if (st < 0)
		return a20_to_errno(st);
	*handle = fd_alloc(h, O_RDONLY | O_DIRECTORY);
	return 0;
}

int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
	a20_handle_t h = fd_handle(handle);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	/* The kernel emits fixed-size a20_dirent records; repack as getdents64. */
	char kbuf[2048];
	a20_status_t st = a20_syscall6(A20_SYS_path_readdir, h, (uint64_t)kbuf, sizeof(kbuf), 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	size_t in_len = (size_t)st;
	size_t in_off = 0, out_off = 0;
	while (in_off + sizeof(a20_dirent) <= in_len) {
		auto *de = (a20_dirent *)(kbuf + in_off);
		in_off += sizeof(a20_dirent);
		if (de->name_len == 0)
			continue;
		size_t reclen = offsetof(struct dirent, d_name) + de->name_len + 1;
		reclen = (reclen + 7) & ~7UL;
		if (out_off + reclen > max_size)
			break;
		auto *out = (struct dirent *)((char *)buffer + out_off);
		out->d_ino = 1;
		out->d_off = out_off + reclen;
		out->d_reclen = reclen;
		out->d_type = (unsigned char)de->type;
		memcpy(out->d_name, de->name, de->name_len);
		out->d_name[de->name_len] = '\0';
		out_off += reclen;
	}
	*bytes_read = out_off;
	return 0;
}

int Sysdeps<Isatty>::operator()(int fd) {
	/* stdio handles are the console; everything else is not a tty yet. */
	if (fd >= 0 && fd <= 2)
		return 0;
	return ENOTTY;
}

int Sysdeps<Dup>::operator()(int fd, int flags, int *newfd) {
	(void)flags;
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_handle_dup_args args{
	    .size = sizeof(args), .version = 1, .source = h, .flags = 0,
	    .rights_mask = ~0ULL, .out_handle = A20_HANDLE_NULL, .reserved = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_dup, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*newfd = fd_alloc(args.out_handle, fd_flags(fd));
	return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int flags, int newfd) {
	(void)flags;
	if (fd == newfd)
		return 0;
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_handle_dup_args args{
	    .size = sizeof(args), .version = 1, .source = h, .flags = 0,
	    .rights_mask = ~0ULL, .out_handle = A20_HANDLE_NULL, .reserved = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_dup, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	if (fd_handle(newfd) != A20_HANDLE_NULL)
		sysdep<Close>(newfd);
	fd_lock();
	if (newfd >= 0 && newfd < g_fd_count) {
		g_fds[newfd].handle = args.out_handle;
		g_fds[newfd].flags = fd_flags(fd);
		fd_unlock();
		return 0;
	}
	fd_unlock();
	/* Slot beyond current table: fall back to a fresh allocation. */
	int allocated = fd_alloc(args.out_handle, fd_flags(fd));
	return allocated == newfd ? 0 : EBADF;
}

int Sysdeps<Access>::operator()(const char *path, int mode) {
	(void)mode;
	/* No dedicated access() in the native ABI; open + close approximates it. */
	a20_handle_t h;
	a20_status_t st = do_path_open(cwd_handle(), path, O_RDONLY, 0, &h);
	if (st < 0)
		return a20_to_errno(st);
	a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
	return 0;
}

int Sysdeps<Faccessat>::operator()(int dirfd, const char *pathname, int mode, int flags) {
	(void)dirfd; (void)flags;
	return sysdep<Access>(pathname, mode);
}

int Sysdeps<Ftruncate>::operator()(int fd, size_t size) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_set_meta_args args{
	    .size = sizeof(args), .version = 1, .handle = h,
	    .flags = A20_SET_META_TRUNCATE, .mode = 0, .uid = 0, .gid = 0,
	    .atime_ns = 0, .mtime_ns = 0, .ctime_ns = 0,
	    .truncate_size = (uint64_t)size, .allocate_size = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_set_meta, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Truncate>::operator()(const char *path, off_t length) {
	a20_handle_t h;
	a20_status_t st = do_path_open(cwd_handle(), path, O_WRONLY, 0, &h);
	if (st < 0)
		return a20_to_errno(st);
	a20_set_meta_args args{
	    .size = sizeof(args), .version = 1, .handle = h,
	    .flags = A20_SET_META_TRUNCATE, .mode = 0, .uid = 0, .gid = 0,
	    .atime_ns = 0, .mtime_ns = 0, .ctime_ns = 0,
	    .truncate_size = (uint64_t)length, .allocate_size = 0,
	};
	st = a20_syscall6(A20_SYS_handle_set_meta, (uint64_t)&args, 0, 0, 0, 0, 0);
	a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	char abs[PATH_MAX];
	absolutize(path, abs, sizeof(abs));
	a20_status_t st = a20_syscall6(A20_SYS_path_unlink, (uint64_t)abs, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int flags) {
	(void)dirfd; (void)flags;
	return sysdep<Rmdir>(path);
}

int Sysdeps<Rename>::operator()(const char *path, const char *new_path) {
	char old_abs[PATH_MAX], new_abs[PATH_MAX];
	absolutize(path, old_abs, sizeof(old_abs));
	absolutize(new_path, new_abs, sizeof(new_abs));
	a20_status_t st = a20_syscall6(A20_SYS_path_rename, (uint64_t)old_abs, 0,
	                               (uint64_t)new_abs, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Renameat>::operator()(int olddirfd, const char *oldpath, int newdirfd, const char *newpath) {
	(void)olddirfd; (void)newdirfd;
	return sysdep<Rename>(oldpath, newpath);
}

} // namespace mlibc

namespace mlibc {

/* ------------------------------------------------------------------ */
/* directories and links                                               */
/* ------------------------------------------------------------------ */

static int do_create_node(const char *path, uint32_t type, uint32_t mode) {
	a20_path_create_args args{
	    .size = sizeof(args), .version = 1, .dir = cwd_handle(),
	    .type = type, .mode = mode,
	    .path = (uint64_t)path, .path_len = 0, .dev = 0,
	    .out_handle = A20_HANDLE_NULL,
	};
	a20_status_t st = a20_syscall6(A20_SYS_path_create, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	if (args.out_handle != A20_HANDLE_NULL)
		a20_syscall6(A20_SYS_handle_close, args.out_handle, 0, 0, 0, 0, 0);
	return 0;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t mode) {
	return do_create_node(path, 1 /* dir */, (uint32_t)mode);
}

int Sysdeps<Mkdirat>::operator()(int dirfd, const char *path, mode_t mode) {
	(void)dirfd;
	return sysdep<Mkdir>(path, mode);
}

int Sysdeps<Link>::operator()(const char *oldpath, const char *newpath) {
	char old_abs[PATH_MAX], new_abs[PATH_MAX];
	absolutize(oldpath, old_abs, sizeof(old_abs));
	absolutize(newpath, new_abs, sizeof(new_abs));
	a20_status_t st = a20_syscall6(A20_SYS_path_link, (uint64_t)old_abs, 0,
	                               (uint64_t)new_abs, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Linkat>::operator()(int olddirfd, const char *oldpath, int newdirfd, const char *newpath, int flags) {
	(void)olddirfd; (void)newdirfd; (void)flags;
	return sysdep<Link>(oldpath, newpath);
}

int Sysdeps<Symlink>::operator()(const char *target, const char *linkpath) {
	char link_abs[PATH_MAX];
	absolutize(linkpath, link_abs, sizeof(link_abs));
	a20_status_t st = a20_syscall6(A20_SYS_path_symlink, (uint64_t)target, 0,
	                               (uint64_t)link_abs, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Symlinkat>::operator()(const char *target, int dirfd, const char *linkpath) {
	(void)dirfd;
	return sysdep<Symlink>(target, linkpath);
}

int Sysdeps<Readlink>::operator()(const char *path, void *buffer, size_t max_size, ssize_t *length) {
	char abs[PATH_MAX];
	absolutize(path, abs, sizeof(abs));
	a20_status_t st = a20_syscall6(A20_SYS_path_readlink, (uint64_t)abs, 0,
	                               (uint64_t)buffer, (uint64_t)max_size, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*length = (ssize_t)st;
	return 0;
}

int Sysdeps<Readlinkat>::operator()(int dirfd, const char *path, void *buffer, size_t max_size, ssize_t *length) {
	(void)dirfd;
	return sysdep<Readlink>(path, buffer, max_size, length);
}

/* ------------------------------------------------------------------ */
/* sync                                                                */
/* ------------------------------------------------------------------ */

int Sysdeps<Fsync>::operator()(int fd) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_status_t st = a20_syscall6(A20_SYS_fs_sync, h, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Fdatasync>::operator()(int fd) {
	return sysdep<Fsync>(fd);
}

void Sysdeps<Sync>::operator()() {
	a20_syscall6(A20_SYS_fs_sync, A20_HANDLE_NULL, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	uint64_t ns = 0;
	a20_status_t st = a20_syscall6(A20_SYS_clock_get, (uint32_t)clock, (uint64_t)&ns, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*secs = (time_t)(ns / 1000000000ULL);
	*nanos = (long)(ns % 1000000000ULL);
	return 0;
}

int Sysdeps<ClockGetres>::operator()(int clock, time_t *secs, long *nanos) {
	uint64_t ns = 0;
	a20_status_t st = a20_syscall6(A20_SYS_clock_resolution, (uint32_t)clock, (uint64_t)&ns, 0, 0, 0, 0);
	if (st < 0) {
		/* Fall back to timer tick granularity. */
		*secs = 0;
		*nanos = 10000000L;
		return 0;
	}
	*secs = (time_t)(ns / 1000000000ULL);
	*nanos = (long)(ns % 1000000000ULL);
	return 0;
}

int Sysdeps<Sleep>::operator()(time_t *secs, long *nanos) {
	uint64_t duration = (uint64_t)*secs * 1000000000ULL + (uint64_t)*nanos;
	a20_status_t st = a20_syscall6(A20_SYS_thread_sleep, duration, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*secs = 0;
	*nanos = 0;
	return 0;
}

void Sysdeps<Yield>::operator()() {
	a20_syscall6(A20_SYS_thread_yield, 0, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ */
/* identity (native ABI has no POSIX uid model; read from security ctx) */
/* ------------------------------------------------------------------ */

static int read_security_context(a20_security_context *out) {
	memset(out, 0, sizeof(*out));
	out->size = sizeof(*out);
	out->version = 1;
	a20_status_t st = a20_syscall6(A20_SYS_security_get_context, (uint64_t)out, 0, 0, 0, 0, 0);
	return st < 0 ? a20_to_errno(st) : 0;
}

uid_t Sysdeps<GetUid>::operator()() {
	a20_security_context sc;
	if (read_security_context(&sc))
		return 0;
	return (uid_t)sc.uid;
}

uid_t Sysdeps<GetEuid>::operator()() {
	a20_security_context sc;
	if (read_security_context(&sc))
		return 0;
	return (uid_t)sc.euid;
}

gid_t Sysdeps<GetGid>::operator()() {
	a20_security_context sc;
	if (read_security_context(&sc))
		return 0;
	return (gid_t)sc.gid;
}

gid_t Sysdeps<GetEgid>::operator()() {
	a20_security_context sc;
	if (read_security_context(&sc))
		return 0;
	return (gid_t)sc.egid;
}

/* ------------------------------------------------------------------ */
/* fork/exec: intentionally unsupported by the native ABI              */
/* ------------------------------------------------------------------ */

int Sysdeps<Fork>::operator()(pid_t *child) {
	(void)child;
	return ENOSYS; /* design: task_spawn + posix_spawn, never fork */
}

int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const envp[]) {
	(void)path; (void)argv; (void)envp;
	return ENOSYS; /* design: task_spawn replaces exec */
}

/* ------------------------------------------------------------------ */
/* signals: inert compatibility stubs (native ABI has no signals)      */
/* ------------------------------------------------------------------ */

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	(void)pid; (void)sig;
	return 0;
}

int Sysdeps<Sigprocmask>::operator()(int how, const sigset_t *__restrict set, sigset_t *__restrict retrieve) {
	(void)how; (void)set;
	if (retrieve)
		memset(retrieve, 0, sizeof(*retrieve));
	return 0;
}

int Sysdeps<Sigaction>::operator()(int signum, const struct sigaction *__restrict act, struct sigaction *__restrict oldact) {
	(void)signum;
	if (oldact)
		memset(oldact, 0, sizeof(*oldact));
	(void)act;
	return 0;
}

int Sysdeps<Sigsuspend>::operator()(const sigset_t *set) {
	(void)set;
	return ENOSYS;
}

int Sysdeps<Sigpending>::operator()(sigset_t *set) {
	memset(set, 0, sizeof(*set));
	return 0;
}

int Sysdeps<Sigaltstack>::operator()(const stack_t *ss, stack_t *oss) {
	(void)ss;
	if (oss)
		memset(oss, 0, sizeof(*oss));
	return 0;
}

/* ------------------------------------------------------------------ */
/* termios stubs (console is always in a fixed sane mode)              */
/* ------------------------------------------------------------------ */

int Sysdeps<Tcgetattr>::operator()(int fd, struct termios *ti) {
	if (fd < 0 || fd > 2)
		return ENOTTY;
	memset(ti, 0, sizeof(*ti));
	ti->c_iflag = ICRNL | IXON;
	ti->c_oflag = OPOST | ONLCR;
	ti->c_cflag = B38400 | CS8 | CREAD | HUPCL;
	ti->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
	ti->c_cc[VINTR] = 3;
	ti->c_cc[VQUIT] = 28;
	ti->c_cc[VERASE] = 127;
	ti->c_cc[VKILL] = 21;
	ti->c_cc[VEOF] = 4;
	ti->c_cc[VTIME] = 0;
	ti->c_cc[VMIN] = 1;
	ti->c_cc[VSTART] = 17;
	ti->c_cc[VSTOP] = 19;
	ti->c_cc[VSUSP] = 26;
	return 0;
}

int Sysdeps<Tcsetattr>::operator()(int fd, int action, const struct termios *ti) {
	(void)fd; (void)action; (void)ti;
	return 0;
}

int Sysdeps<Tcsendbreak>::operator()(int fd, int duration) { (void)fd; (void)duration; return 0; }
int Sysdeps<Tcdrain>::operator()(int fd) { (void)fd; return 0; }
int Sysdeps<Tcflow>::operator()(int fd, int action) { (void)fd; (void)action; return 0; }
int Sysdeps<Tcflush>::operator()(int fd, int queue) { (void)fd; (void)queue; return 0; }

int Sysdeps<Tcgetwinsize>::operator()(int fd, struct winsize *winsz) {
	(void)fd;
	winsz->ws_row = 25;
	winsz->ws_col = 80;
	winsz->ws_xpixel = 0;
	winsz->ws_ypixel = 0;
	return 0;
}

int Sysdeps<Tcsetwinsize>::operator()(int fd, const struct winsize *winsz) {
	(void)fd; (void)winsz;
	return 0;
}

/* ------------------------------------------------------------------ */
/* misc                                                                */
/* ------------------------------------------------------------------ */

int Sysdeps<Pipe>::operator()(int *fds, int flags) {
	(void)flags;
	a20_channel_create_args args{
	    .size = sizeof(args), .version = 1, .msg_capacity = 64, .flags = 0,
	    .type = 0, .out_endpoints = {A20_HANDLE_NULL, A20_HANDLE_NULL},
	};
	a20_status_t st = a20_syscall6(A20_SYS_channel_create, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	fds[0] = fd_alloc(args.out_endpoints[0], O_RDONLY);
	fds[1] = fd_alloc(args.out_endpoints[1], O_WRONLY);
	fd_set_kind(fds[0], 1);
	fd_set_kind(fds[1], 1);
	return 0;
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	size_t len = strlen(g_cwd) + 1;
	if (size < len)
		return ERANGE;
	memcpy(buffer, g_cwd, len);
	return 0;
}

int Sysdeps<Chdir>::operator()(const char *path) {
	/* Validate that the target is a directory, then track it libc-side. */
	a20_handle_t h;
	a20_status_t st = do_path_open(cwd_handle(), path, O_RDONLY | O_DIRECTORY, 0, &h);
	if (st < 0)
		return a20_to_errno(st);
	a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);

	char abs[PATH_MAX];
	absolutize(path, abs, sizeof(abs));
	/* Normalize trailing slashes. */
	size_t len = strlen(abs);
	while (len > 1 && abs[len - 1] == '/')
		abs[--len] = '\0';
	strncpy(g_cwd, abs, sizeof(g_cwd) - 1);
	g_cwd[sizeof(g_cwd) - 1] = '\0';
	return 0;
}

int Sysdeps<Fchdir>::operator()(int fd) {
	(void)fd;
	return ENOSYS; /* cannot recover a path from a handle */
}

int Sysdeps<Umask>::operator()(mode_t mode, mode_t *old) {
	static mode_t current = 022;
	*old = current;
	current = mode;
	return 0;
}

int Sysdeps<Uname>::operator()(struct utsname *buf) {
	a20_system_info info{};
	info.size = sizeof(info);
	info.struct_version = 2;
	a20_status_t st = a20_syscall6(A20_SYS_system_info, (uint64_t)&info, 0, 0, 0, 0, 0);
	memset(buf, 0, sizeof(*buf));
	if (st < 0) {
		strncpy(buf->sysname, "A20OS", sizeof(buf->sysname) - 1);
		strncpy(buf->machine, "unknown", sizeof(buf->machine) - 1);
		return 0;
	}
	strncpy(buf->sysname, info.sysname, sizeof(buf->sysname) - 1);
	strncpy(buf->nodename, info.nodename, sizeof(buf->nodename) - 1);
	strncpy(buf->release, info.release, sizeof(buf->release) - 1);
	strncpy(buf->version, info.version, sizeof(buf->version) - 1);
	strncpy(buf->machine, info.machine, sizeof(buf->machine) - 1);
	return 0;
}

int Sysdeps<GetHostname>::operator()(char *name, size_t len) {
	struct utsname uts;
	if (int e = sysdep<Uname>(&uts))
		return e;
	strncpy(name, uts.nodename[0] ? uts.nodename : "a20os", len - 1);
	name[len - 1] = '\0';
	return 0;
}

int Sysdeps<SetHostname>::operator()(const char *name, size_t len) {
	(void)name; (void)len;
	return EPERM;
}

int Sysdeps<GetEntropy>::operator()(void *buffer, size_t length) {
	a20_status_t st = a20_syscall6(A20_SYS_system_random, (uint64_t)buffer,
	                               (uint64_t)length, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<GetRlimit>::operator()(int resource, struct rlimit *limit) {
	(void)resource;
	limit->rlim_cur = RLIM_INFINITY;
	limit->rlim_max = RLIM_INFINITY;
	return 0;
}

int Sysdeps<GetRusage>::operator()(int scope, struct rusage *usage) {
	(void)scope;
	memset(usage, 0, sizeof(*usage));
	if (!__a20_start_info || __a20_start_info->self_task == A20_HANDLE_NULL)
		return 0;
	a20_rusage ru{};
	a20_status_t st = a20_syscall6(A20_SYS_task_get_usage,
	                               __a20_start_info->self_task, (uint64_t)&ru, 0, 0, 0, 0);
	if (st < 0)
		return 0;
	usage->ru_utime.tv_sec = (time_t)(ru.user_time_ns / 1000000000ULL);
	usage->ru_utime.tv_usec = (suseconds_t)((ru.user_time_ns / 1000ULL) % 1000000ULL);
	usage->ru_stime.tv_sec = (time_t)(ru.sys_time_ns / 1000000000ULL);
	usage->ru_stime.tv_usec = (suseconds_t)((ru.sys_time_ns / 1000ULL) % 1000000ULL);
	usage->ru_maxrss = (long)(ru.max_rss / 1024ULL);
	return 0;
}

int Sysdeps<Sysconf>::operator()(int num, long *ret) {
	switch (num) {
	case _SC_PAGE_SIZE:
		*ret = 4096;
		return 0;
	case _SC_OPEN_MAX:
		*ret = 1024;
		return 0;
	case _SC_NPROCESSORS_ONLN:
		*ret = 1;
		return 0;
	case _SC_CLK_TCK:
		*ret = 100;
		return 0;
	case _SC_PHYS_PAGES:
	case _SC_AVPHYS_PAGES: {
		a20_system_info info{};
		info.size = sizeof(info);
		info.struct_version = 2;
		if (a20_syscall6(A20_SYS_system_info, (uint64_t)&info, 0, 0, 0, 0, 0) >= 0) {
			*ret = (long)((num == _SC_PHYS_PAGES ? info.total_ram : info.free_ram) / 4096);
			return 0;
		}
		*ret = 32768;
		return 0;
	}
	default:
		return EINVAL;
	}
}

} // namespace mlibc

/* ==================================================================== */
/* posix_spawn over task_spawn (native process model, no fork/execve)   */
/* ==================================================================== */

namespace {

struct ChildReg {
	pid_t pid = 0;
	a20_handle_t task = A20_HANDLE_NULL;
};

ChildReg g_children[32];

int child_register(pid_t pid, a20_handle_t task) {
	for (auto &c : g_children) {
		if (c.pid == 0) {
			c.pid = pid;
			c.task = task;
			return 0;
		}
	}
	return ENOMEM;
}

ChildReg *child_find(pid_t pid) {
	for (auto &c : g_children) {
		if (c.pid == pid)
			return &c;
	}
	return nullptr;
}

} // namespace

extern "C" int posix_spawn(pid_t *pid_out, const char *path,
                           const posix_spawn_file_actions_t *fa,
                           const posix_spawnattr_t *attr,
                           char *const argv[], char *const envp[]) {
	(void)attr; /* attrs (setsid/sched/sigmask) have no native meaning yet */
	fd_table_init();

	/* 1. Open the image (path_open grants EXEC for regular files). */
	a20_handle_t image;
	{
		char abs[PATH_MAX];
		absolutize(path, abs, sizeof(abs));
		a20_status_t st = do_path_open(cwd_handle(), abs, O_RDONLY, 0, &image);
		if (st < 0)
			return a20_to_errno(st);
	}

	/* 2. stdio inheritance, applying file actions to slots 0..2. */
	a20_handle_t stdio_h[3] = { fd_handle(0), fd_handle(1), fd_handle(2) };
	a20_handle_t close_after[8];
	int n_close = 0;
	if (fa) {
		auto *actions = __mlibc_spawn_file_actions::from(fa);
		if (actions) {
			for (auto &op : actions->ops) {
				switch (op.cmd) {
				case 1: /* CLOSE */
					if (op.fd >= 0 && op.fd <= 2)
						stdio_h[op.fd] = A20_HANDLE_NULL;
					break;
				case 2: /* DUP2 */
					if (op.fd >= 0 && op.fd <= 2)
						stdio_h[op.fd] = fd_handle(op.srcfd);
					break;
				case 3: /* OPEN */
					if (op.fd >= 0 && op.fd <= 2 && n_close < 8) {
						a20_handle_t oh;
						if (do_path_open(cwd_handle(), op.path.data(),
						                 (uint32_t)op.oflag, op.mode, &oh) >= 0) {
							stdio_h[op.fd] = oh;
							close_after[n_close++] = oh;
						}
					}
					break;
				default:
					break;
				}
			}
		}
	}

	/* 3. Count arguments; inherit environ when envp is NULL. */
	extern char **environ;
	if (!envp)
		envp = environ;
	uint32_t argc = 0, envc = 0;
	if (argv)
		while (argv[argc]) argc++;
	if (envp)
		while (envp[envc]) envc++;

	a20_task_spawn_args sa{
	    .size = sizeof(sa), .version = 2,
	    .image = image,
	    .root_dir = root_handle(),
	    .cwd_dir = cwd_handle(),
	    .event_queue = A20_HANDLE_NULL,
	    .argv = (uint64_t)argv, .envp = (uint64_t)envp,
	    .argc = argc, .envc = envc,
	    .handles = 0, .handle_count = 0, .flags = 0,
	    .out_task = A20_HANDLE_NULL,
	    .stdin_handle = stdio_h[0],
	    .stdout_handle = stdio_h[1],
	    .stderr_handle = stdio_h[2],
	    .reserved = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)&sa, 0, 0, 0, 0, 0);

	a20_syscall6(A20_SYS_handle_close, image, 0, 0, 0, 0, 0);
	for (int i = 0; i < n_close; i++)
		a20_syscall6(A20_SYS_handle_close, close_after[i], 0, 0, 0, 0, 0);

	if (st < 0)
		return a20_to_errno(st);

	/* 4. Resolve the pid and register for waitpid. */
	pid_t child_pid = -1;
	a20_task_info info{};
	info.size = sizeof(info);
	info.version = 1;
	if (a20_syscall6(A20_SYS_task_info, sa.out_task, (uint64_t)&info, 0, 0, 0, 0) >= 0)
		child_pid = info.pid;
	if (child_pid <= 0) {
		a20_syscall6(A20_SYS_handle_close, sa.out_task, 0, 0, 0, 0, 0);
		return EIO;
	}
	if (int e = child_register(child_pid, sa.out_task)) {
		a20_syscall6(A20_SYS_handle_close, sa.out_task, 0, 0, 0, 0, 0);
		return e;
	}

	if (pid_out)
		*pid_out = child_pid;
	return 0;
}

extern "C" int posix_spawnp(pid_t *pid_out, const char *file,
                            const posix_spawn_file_actions_t *fa,
                            const posix_spawnattr_t *attr,
                            char *const argv[], char *const envp[]) {
	if (strchr(file, '/'))
		return posix_spawn(pid_out, file, fa, attr, argv, envp);

	const char *path_env = getenv("PATH");
	char path_copy[PATH_MAX];
	if (!path_env || !*path_env)
		path_env = "/bin:/usr/bin";
	strncpy(path_copy, path_env, sizeof(path_copy) - 1);
	path_copy[sizeof(path_copy) - 1] = '\0';

	int last_err = ENOENT;
	for (char *dir = strtok(path_copy, ":"); dir; dir = strtok(nullptr, ":")) {
		char candidate[PATH_MAX];
		snprintf(candidate, sizeof(candidate), "%s/%s", dir, file);
		if (mlibc::sysdep<Access>(candidate, 1 /* X_OK */) == 0)
			return posix_spawn(pid_out, candidate, fa, attr, argv, envp);
		last_err = ENOENT;
	}
	return last_err;
}

namespace mlibc {

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *ru, pid_t *ret_pid) {
	ChildReg *c = child_find(pid);
	if (!c)
		return ECHILD;

	if (flags & WNOHANG) {
		uint64_t events = 0;
		if (a20_rt_handle_poll(c->task, 1ull << A20_EVENT_EXITED, &events) < 0 ||
		    !(events & (1ull << A20_EVENT_EXITED))) {
			*ret_pid = 0;
			return 0;
		}
	}

	a20_task_status ts{};
	a20_status_t st = a20_syscall6(A20_SYS_task_wait, c->task, 0, (uint64_t)&ts, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);

	if (status)
		*status = (ts.exit_code & 0xff) << 8; /* POSIX "normal exit" encoding */
	if (ru)
		memset(ru, 0, sizeof(*ru));
	*ret_pid = pid;

	a20_syscall6(A20_SYS_handle_close, c->task, 0, 0, 0, 0, 0);
	c->pid = 0;
	c->task = A20_HANDLE_NULL;
	return 0;
}

} // namespace mlibc

namespace mlibc {

/* ------------------------------------------------------------------ */
/* poll via handle_poll (level query) + sleep backoff                   */
/* ------------------------------------------------------------------ */

int Sysdeps<Poll>::operator()(struct pollfd *fds, nfds_t count, int timeout, int *num_events) {
	if (timeout < 0)
		timeout = -1;
	time_t now_s;
	long now_ns;
	int64_t deadline = -1;
	if (timeout >= 0) {
		if (sysdep<ClockGet>(1 /* CLOCK_MONOTONIC */, &now_s, &now_ns))
			return EIO;
		deadline = (int64_t)now_s * 1000 + now_ns / 1000000 + timeout;
	}

	for (;;) {
		int ready = 0;
		for (nfds_t i = 0; i < count; i++) {
			fds[i].revents = 0;
			a20_handle_t h = fd_handle(fds[i].fd);
			if (h == A20_HANDLE_NULL) {
				fds[i].revents = POLLNVAL;
				ready++;
				continue;
			}
			uint64_t mask = 0;
			if (fds[i].events & (POLLIN | POLLPRI | POLLRDNORM | POLLRDBAND))
				mask |= (1ull << A20_EVENT_READABLE) | (1ull << A20_EVENT_MESSAGE_READY);
			if (fds[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND))
				mask |= 1ull << A20_EVENT_WRITABLE;
			mask |= (1ull << A20_EVENT_ERROR) | (1ull << A20_EVENT_CLOSED);

			uint64_t active = 0;
			if (a20_rt_handle_poll(h, mask, &active) < 0) {
				fds[i].revents = POLLNVAL;
				ready++;
				continue;
			}
			if (active & ((1ull << A20_EVENT_READABLE) | (1ull << A20_EVENT_MESSAGE_READY)))
				fds[i].revents |= POLLIN;
			if (active & (1ull << A20_EVENT_WRITABLE))
				fds[i].revents |= POLLOUT;
			if (active & (1ull << A20_EVENT_ERROR))
				fds[i].revents |= POLLERR;
			if (active & (1ull << A20_EVENT_CLOSED))
				fds[i].revents |= POLLHUP;
			if (fds[i].revents)
				ready++;
		}
		if (ready > 0 || timeout == 0) {
			*num_events = ready;
			return 0;
		}

		if (deadline >= 0) {
			time_t s;
			long ns;
			if (sysdep<ClockGet>(1, &s, &ns))
				return EIO;
			int64_t now = (int64_t)s * 1000 + ns / 1000000;
			if (now >= deadline) {
				*num_events = 0;
				return 0;
			}
			int64_t left = deadline - now;
			long sleep_ns = (left < 10 ? left : 10) * 1000000L;
			time_t zs = 0;
			sysdep<Sleep>(&zs, &sleep_ns);
		} else {
			time_t zs = 0;
			long sleep_ns = 10000000L; /* 10ms readiness granularity */
			sysdep<Sleep>(&zs, &sleep_ns);
		}
	}
}

/* ------------------------------------------------------------------ */
/* sockets                                                              */
/* ------------------------------------------------------------------ */

int Sysdeps<Socket>::operator()(int family, int type, int protocol, int *fd) {
	a20_status_t st = a20_syscall6(A20_SYS_net_socket, (uint32_t)family,
	                               (uint32_t)(type & 0xFFFF), (uint32_t)protocol, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*fd = fd_alloc((a20_handle_t)st, 0);
	return 0;
}

int Sysdeps<Socketpair>::operator()(int domain, int type_and_flags, int proto, int *fds) {
	a20_handle_t out[2] = { A20_HANDLE_NULL, A20_HANDLE_NULL };
	a20_status_t st = a20_syscall6(A20_SYS_net_socketpair, (uint32_t)domain,
	                               (uint32_t)(type_and_flags & 0xFFFF), (uint32_t)proto,
	                               (uint64_t)out, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	fds[0] = fd_alloc(out[0], 0);
	fds[1] = fd_alloc(out[1], 0);
	return 0;
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_status_t st = a20_syscall6(A20_SYS_net_bind, h, (uint64_t)addr_ptr,
	                               (uint64_t)addr_length, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr *addr_ptr, socklen_t addr_length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_status_t st = a20_syscall6(A20_SYS_net_connect, h, (uint64_t)addr_ptr,
	                               (uint64_t)addr_length, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Listen>::operator()(int fd, int backlog) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_status_t st = a20_syscall6(A20_SYS_net_listen, h, (uint64_t)backlog, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<Accept>::operator()(int fd, int *newfd, struct sockaddr *addr_ptr, socklen_t *addr_length, int flags) {
	(void)flags;
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	socklen_t alen = addr_length ? *addr_length : 0;
	a20_status_t st = a20_syscall6(A20_SYS_net_accept, h, (uint64_t)addr_ptr,
	                               (uint64_t)&alen, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	if (addr_length)
		*addr_length = alen;
	*newfd = fd_alloc((a20_handle_t)st, 0);
	return 0;
}

int Sysdeps<Sockname>::operator()(int fd, struct sockaddr *addr_ptr, socklen_t max_addr_length, socklen_t *actual_length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	socklen_t alen = max_addr_length;
	a20_status_t st = a20_syscall6(A20_SYS_net_getname, h, (uint64_t)addr_ptr,
	                               (uint64_t)&alen, 0 /* local */, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*actual_length = alen;
	return 0;
}

int Sysdeps<Peername>::operator()(int fd, struct sockaddr *addr_ptr, socklen_t max_addr_length, socklen_t *actual_length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	socklen_t alen = max_addr_length;
	a20_status_t st = a20_syscall6(A20_SYS_net_getname, h, (uint64_t)addr_ptr,
	                               (uint64_t)&alen, 1 /* peer */, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*actual_length = alen;
	return 0;
}

static void sockaddr_to_a20(a20_net_addr *out, const struct sockaddr *sa, socklen_t len) {
	memset(out, 0, sizeof(*out));
	/* a20_net_addr mirrors sockaddr_in for IPv4/IPv6: family, port, addr. */
	if (len >= 2)
		out->family = sa->sa_family;
	if (sa->sa_family == 2 /* AF_INET */ && len >= 8) {
		memcpy(&out->port, ((const char *)sa) + 2, 2);
		memcpy(out->addr, ((const char *)sa) + 4, 4);
	} else if (len > 2) {
		size_t n = len - 2 < 22 ? len - 2 : 22;
		memcpy(&out->port, ((const char *)sa) + 2, n);
	}
}

int Sysdeps<MsgSend>::operator()(int fd, const struct msghdr *hdr, int flags, ssize_t *length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_iovec iov[16];
	a20_net_addr na;
	uint64_t addr_ptr = 0;
	int iovc = hdr->msg_iovlen > 16 ? 16 : hdr->msg_iovlen;
	for (int i = 0; i < iovc; i++) {
		iov[i].base = (uint64_t)hdr->msg_iov[i].iov_base;
		iov[i].len = (uint64_t)hdr->msg_iov[i].iov_len;
	}
	if (hdr->msg_name && hdr->msg_namelen) {
		sockaddr_to_a20(&na, (const struct sockaddr *)hdr->msg_name, hdr->msg_namelen);
		addr_ptr = (uint64_t)&na;
	}
	a20_net_sendmsg_args args{
	    .size = sizeof(args), .version = 1, .socket = h,
	    .iov = (uint64_t)iov, .iov_count = (uint32_t)iovc,
	    .flags = (uint32_t)flags, .addr = addr_ptr,
	    .control = 0, .control_len = 0, .out_sent = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_net_sendmsg, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	*length = (ssize_t)args.out_sent;
	return 0;
}

int Sysdeps<Sendto>::operator()(int fd, const void *buffer, size_t size, int flags,
                                const struct sockaddr *sock_addr, socklen_t addr_length, ssize_t *length) {
	struct iovec v = { (void *)buffer, size };
	struct msghdr hdr{};
	hdr.msg_iov = &v;
	hdr.msg_iovlen = 1;
	hdr.msg_name = (void *)sock_addr;
	hdr.msg_namelen = addr_length;
	return sysdep<MsgSend>(fd, &hdr, flags, length);
}

int Sysdeps<MsgRecv>::operator()(int fd, struct msghdr *hdr, int flags, ssize_t *length) {
	a20_handle_t h = fd_handle(fd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_iovec iov[16];
	a20_net_addr na;
	int iovc = hdr->msg_iovlen > 16 ? 16 : hdr->msg_iovlen;
	for (int i = 0; i < iovc; i++) {
		iov[i].base = (uint64_t)hdr->msg_iov[i].iov_base;
		iov[i].len = (uint64_t)hdr->msg_iov[i].iov_len;
	}
	a20_net_recvmsg_args args{
	    .size = sizeof(args), .version = 1, .socket = h,
	    .iov = (uint64_t)iov, .iov_count = (uint32_t)iovc,
	    .flags = (uint32_t)flags,
	    .addr = (hdr->msg_name && hdr->msg_namelen) ? (uint64_t)&na : 0,
	    .control = 0, .control_len = 0,
	    .out_received = 0, .out_addr_len = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_net_recvmsg, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	if (hdr->msg_name && args.out_addr_len) {
		size_t n = args.out_addr_len < hdr->msg_namelen ? args.out_addr_len : hdr->msg_namelen;
		memcpy(hdr->msg_name, &na, n);
		hdr->msg_namelen = n;
	}
	*length = (ssize_t)args.out_received;
	return 0;
}

int Sysdeps<Recvfrom>::operator()(int fd, void *buffer, size_t size, int flags,
                                  struct sockaddr *sock_addr, socklen_t *addr_length, ssize_t *length) {
	struct iovec v = { buffer, size };
	struct msghdr hdr{};
	hdr.msg_iov = &v;
	hdr.msg_iovlen = 1;
	hdr.msg_name = sock_addr;
	hdr.msg_namelen = addr_length ? *addr_length : 0;
	int e = sysdep<MsgRecv>(fd, &hdr, flags, length);
	if (!e && addr_length)
		*addr_length = hdr.msg_namelen;
	return e;
}

int Sysdeps<Shutdown>::operator()(int sockfd, int how) {
	a20_handle_t h = fd_handle(sockfd);
	if (h == A20_HANDLE_NULL)
		return EBADF;
	a20_status_t st = a20_syscall6(A20_SYS_net_shutdown, h, (uint64_t)how, 0, 0, 0, 0);
	if (st < 0)
		return a20_to_errno(st);
	return 0;
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number, void *__restrict buffer, socklen_t *__restrict size) {
	(void)fd; (void)layer; (void)number; (void)buffer; (void)size;
	return ENOSYS;
}

int Sysdeps<SetSockopt>::operator()(int fd, int layer, int number, const void *buffer, socklen_t size) {
	(void)fd; (void)layer; (void)number; (void)buffer; (void)size;
	return 0; /* tolerate option setting (O_NONBLOCK etc. tracked fd-side later) */
}

} // namespace mlibc
