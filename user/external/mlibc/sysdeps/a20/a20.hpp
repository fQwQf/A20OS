#pragma once

/*
 * A20OS Native ABI syscall interface for the mlibc sysdep layer.
 *
 * Self-contained C++ adaptation of user/liba20rt.  Keep struct layouts in
 * sync with kernel/include/abi/native/types.h and user/liba20rt/a20_types.h.
 */

#include <stddef.h>
#include <stdint.h>

using a20_handle_t = uint32_t;
using a20_rights_t = uint64_t;
using a20_status_t = int64_t;

inline constexpr a20_handle_t A20_HANDLE_NULL = 0xFFFFFFFFu;
inline constexpr a20_status_t A20_OK = 0;
inline constexpr uint64_t A20_OFFSET_CURRENT = ~0ULL;
inline constexpr uint64_t A20_TIMEOUT_INFINITE = ~0ULL;

inline constexpr a20_status_t A20_ERR_PERM              = 1;
inline constexpr a20_status_t A20_ERR_NO_ENTRY          = 2;
inline constexpr a20_status_t A20_ERR_INTERRUPTED       = 3;
inline constexpr a20_status_t A20_ERR_IO                = 4;
inline constexpr a20_status_t A20_ERR_BAD_HANDLE        = 5;
inline constexpr a20_status_t A20_ERR_NO_MEMORY         = 6;
inline constexpr a20_status_t A20_ERR_ACCESS            = 7;
inline constexpr a20_status_t A20_ERR_FAULT             = 8;
inline constexpr a20_status_t A20_ERR_BUSY              = 9;
inline constexpr a20_status_t A20_ERR_EXISTS            = 10;
inline constexpr a20_status_t A20_ERR_NOT_SUPPORTED     = 11;
inline constexpr a20_status_t A20_ERR_INVALID_ARGUMENT  = 12;
inline constexpr a20_status_t A20_ERR_NO_SPACE          = 13;
inline constexpr a20_status_t A20_ERR_NOT_DIR           = 14;
inline constexpr a20_status_t A20_ERR_IS_DIR            = 15;
inline constexpr a20_status_t A20_ERR_NOT_EMPTY         = 16;
inline constexpr a20_status_t A20_ERR_NAME_TOO_LONG     = 17;
inline constexpr a20_status_t A20_ERR_WOULD_BLOCK       = 18;
inline constexpr a20_status_t A20_ERR_TIMED_OUT         = 19;
inline constexpr a20_status_t A20_ERR_CANCELED          = 20;
inline constexpr a20_status_t A20_ERR_PROTOCOL          = 21;
inline constexpr a20_status_t A20_ERR_RANGE             = 22;
inline constexpr a20_status_t A20_ERR_TYPE_MISMATCH     = 23;
inline constexpr a20_status_t A20_ERR_NOT_FOUND         = 24;
inline constexpr a20_status_t A20_ERR_EXPIRED           = 25;

/* Native object types and the small subset needed by the fd bridge. */
inline constexpr uint32_t A20_OBJ_FILE             = 3;
inline constexpr uint32_t A20_OBJ_DIRECTORY        = 4;
inline constexpr uint32_t A20_OBJ_SOCKET           = 5;
inline constexpr uint32_t A20_OBJ_CHANNEL_ENDPOINT = 7;
inline constexpr uint32_t A20_OBJ_DEVICE           = 11;

inline constexpr a20_rights_t A20_RIGHT_READ  = 1ull << 0;
inline constexpr a20_rights_t A20_RIGHT_WRITE = 1ull << 1;

/* Syscall numbers (kernel/include/abi/native/syscall_nr.h) */
inline constexpr uint64_t A20_SYS_abi_info          = 0x0000;
inline constexpr uint64_t A20_SYS_handle_close      = 0x0100;
inline constexpr uint64_t A20_SYS_handle_dup        = 0x0101;
inline constexpr uint64_t A20_SYS_handle_query      = 0x0102;
inline constexpr uint64_t A20_SYS_handle_replace    = 0x0103;
inline constexpr uint64_t A20_SYS_handle_close_many = 0x0104;
inline constexpr uint64_t A20_SYS_handle_seek       = 0x0105;
inline constexpr uint64_t A20_SYS_handle_transfer   = 0x0106;
inline constexpr uint64_t A20_SYS_handle_set_meta   = 0x0107;
inline constexpr uint64_t A20_SYS_task_exit         = 0x0200;
inline constexpr uint64_t A20_SYS_task_spawn        = 0x0201;
inline constexpr uint64_t A20_SYS_task_wait         = 0x0202;
inline constexpr uint64_t A20_SYS_task_kill         = 0x0203;
inline constexpr uint64_t A20_SYS_signal_check      = 0x020F;
inline constexpr uint64_t A20_SYS_signal_mask       = 0x0210;
inline constexpr uint64_t A20_SYS_task_info         = 0x0204;
inline constexpr uint64_t A20_SYS_thread_create     = 0x0205;
inline constexpr uint64_t A20_SYS_thread_exit       = 0x0206;
inline constexpr uint64_t A20_SYS_thread_sleep      = 0x0207;
inline constexpr uint64_t A20_SYS_thread_yield      = 0x0208;
inline constexpr uint64_t A20_SYS_task_get_usage    = 0x020D;
inline constexpr uint64_t A20_SYS_vm_alloc          = 0x0300;
inline constexpr uint64_t A20_SYS_vm_unmap          = 0x0301;
inline constexpr uint64_t A20_SYS_vm_protect        = 0x0302;
inline constexpr uint64_t A20_SYS_vm_map            = 0x0303;
inline constexpr uint64_t A20_SYS_path_open         = 0x0400;
inline constexpr uint64_t A20_SYS_handle_read       = 0x0401;
inline constexpr uint64_t A20_SYS_handle_write      = 0x0402;
inline constexpr uint64_t A20_SYS_handle_stat       = 0x0403;
inline constexpr uint64_t A20_SYS_path_create       = 0x0404;
inline constexpr uint64_t A20_SYS_path_unlink       = 0x0405;
inline constexpr uint64_t A20_SYS_path_rename       = 0x0406;
inline constexpr uint64_t A20_SYS_handle_control    = 0x0407;
inline constexpr uint64_t A20_SYS_path_readdir      = 0x0408;
inline constexpr uint64_t A20_SYS_path_link         = 0x0409;
inline constexpr uint64_t A20_SYS_path_symlink      = 0x040A;
inline constexpr uint64_t A20_SYS_path_readlink     = 0x040B;
inline constexpr uint64_t A20_SYS_path_resolve      = 0x040C;
inline constexpr uint64_t A20_SYS_fs_stat           = 0x040D;
inline constexpr uint64_t A20_SYS_fs_mount          = 0x040E;
inline constexpr uint64_t A20_SYS_fs_umount         = 0x040F;
inline constexpr uint64_t A20_SYS_fs_sync           = 0x0410;
inline constexpr uint64_t A20_SYS_path_unlink_at    = 0x0411;
inline constexpr uint64_t A20_SYS_path_rename_at    = 0x0412;
inline constexpr uint64_t A20_SYS_path_link_at      = 0x0413;
inline constexpr uint64_t A20_SYS_path_symlink_at   = 0x0414;
inline constexpr uint64_t A20_SYS_path_readlink_at  = 0x0415;
inline constexpr uint64_t A20_SYS_event_queue_create = 0x0500;
inline constexpr uint64_t A20_SYS_event_watch       = 0x0501;
inline constexpr uint64_t A20_SYS_event_wait        = 0x0502;
inline constexpr uint64_t A20_SYS_event_cancel      = 0x0503;
inline constexpr uint64_t A20_SYS_channel_create    = 0x0504;
inline constexpr uint64_t A20_SYS_channel_send      = 0x0505;
inline constexpr uint64_t A20_SYS_channel_recv      = 0x0506;
inline constexpr uint64_t A20_SYS_net_socket        = 0x0600;
inline constexpr uint64_t A20_SYS_net_bind          = 0x0601;
inline constexpr uint64_t A20_SYS_net_connect       = 0x0602;
inline constexpr uint64_t A20_SYS_net_accept        = 0x0603;
inline constexpr uint64_t A20_SYS_net_listen        = 0x0604;
inline constexpr uint64_t A20_SYS_net_sendmsg       = 0x0605;
inline constexpr uint64_t A20_SYS_net_recvmsg       = 0x0606;
inline constexpr uint64_t A20_SYS_net_socketpair    = 0x0607;
inline constexpr uint64_t A20_SYS_net_getname       = 0x0608;
inline constexpr uint64_t A20_SYS_net_shutdown      = 0x0609;
inline constexpr uint64_t A20_SYS_clock_get         = 0x0700;
inline constexpr uint64_t A20_SYS_clock_resolution  = 0x0705;
inline constexpr uint64_t A20_SYS_security_get_context = 0x0802;
inline constexpr uint64_t A20_SYS_system_info       = 0x0A00;
inline constexpr uint64_t A20_SYS_system_random     = 0x0A01;
inline constexpr uint64_t A20_SYS_system_reboot     = 0x0A02;
inline constexpr uint64_t A20_SYS_futex_wait        = 0x0B00;
inline constexpr uint64_t A20_SYS_futex_wake        = 0x0B01;
inline constexpr uint64_t A20_SYS_handle_poll       = 0x010C;

inline constexpr uint32_t A20_HANDLE_CTRL_CHDIR     = 5;
inline constexpr uint32_t A20_HANDLE_CTRL_FCNTL     = 1;

#if defined(__riscv)
#define A20_SYSCALL_INSN "ecall"
#elif defined(__aarch64__)
#define A20_SYSCALL_INSN "svc #0"
#elif defined(__x86_64__)
#define A20_SYSCALL_INSN "int $0x80"
#elif defined(__loongarch64)
#define A20_SYSCALL_INSN "syscall 0"
#else
#error "Unsupported architecture for A20 syscall"
#endif

static inline int64_t a20_syscall6(uint64_t nr, uint64_t a0, uint64_t a1,
                                   uint64_t a2, uint64_t a3,
                                   uint64_t a4, uint64_t a5) {
#if defined(__x86_64__)
	register uint64_t rax __asm__("rax") = nr;
	register uint64_t rdi __asm__("rdi") = a0;
	register uint64_t rsi __asm__("rsi") = a1;
	register uint64_t rdx __asm__("rdx") = a2;
	register uint64_t r10 __asm__("r10") = a3;
	register uint64_t r8  __asm__("r8")  = a4;
	register uint64_t r9  __asm__("r9")  = a5;
	__asm__ volatile(A20_SYSCALL_INSN
	    : "+a"(rax)
	    : "D"(rdi), "S"(rsi), "d"(rdx), "r"(r10), "r"(r8), "r"(r9)
	    : "memory");
	return (int64_t)rax;
#elif defined(__aarch64__)
	register uint64_t x8 __asm__("x8") = nr;
	register uint64_t x0 __asm__("x0") = a0;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = a4;
	register uint64_t x5 __asm__("x5") = a5;
	__asm__ volatile(A20_SYSCALL_INSN
	    : "+r"(x0)
	    : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
	    : "memory");
	return (int64_t)x0;
#elif defined(__loongarch64)
	register uint64_t a7 __asm__("$a7") = nr;
	register uint64_t _a0 __asm__("$a0") = a0;
	register uint64_t _a1 __asm__("$a1") = a1;
	register uint64_t _a2 __asm__("$a2") = a2;
	register uint64_t _a3 __asm__("$a3") = a3;
	register uint64_t _a4 __asm__("$a4") = a4;
	register uint64_t _a5 __asm__("$a5") = a5;
	__asm__ volatile(A20_SYSCALL_INSN
	    : "+r"(_a0)
	    : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5)
	    : "memory");
	return (int64_t)_a0;
#else /* riscv64 */
	register uint64_t a7 __asm__("a7") = nr;
	register uint64_t _a0 __asm__("a0") = a0;
	register uint64_t _a1 __asm__("a1") = a1;
	register uint64_t _a2 __asm__("a2") = a2;
	register uint64_t _a3 __asm__("a3") = a3;
	register uint64_t _a4 __asm__("a4") = a4;
	register uint64_t _a5 __asm__("a5") = a5;
	__asm__ volatile(A20_SYSCALL_INSN
	    : "+r"(_a0)
	    : "r"(a7), "r"(_a1), "r"(_a2), "r"(_a3), "r"(_a4), "r"(_a5)
	    : "memory");
	return (int64_t)_a0;
#endif
}

/* ---- Startup protocol (docs/native-abi/07-startup.md) ---- */

struct a20_start_info {
	uint32_t size;
	uint32_t version;
	uint32_t argc;
	uint32_t envc;
	uint32_t auxc;
	uint32_t reserved0; /* Native spawn fd mapping limit; zero for normal exec. */
	uint64_t argv;
	uint64_t envp;
	uint64_t auxv;
	a20_handle_t root_dir;
	a20_handle_t cwd_dir;
	a20_handle_t stdin_handle;
	a20_handle_t stdout_handle;
	a20_handle_t stderr_handle;
	a20_handle_t self_task;
	a20_handle_t main_thread;
	a20_handle_t default_event_queue;
	uint64_t page_size;
	uint64_t user_clock_freq;
};

inline constexpr uint32_t A20_NATIVE_FD_HANDLE_BASE = 64;

/* ---- I/O ---- */

struct a20_iovec {
	uint64_t base;
	uint64_t len;
};

struct a20_io_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t handle;
	uint32_t _pad0;
	uint64_t iov;
	uint32_t iov_count;
	uint32_t _pad1;
	uint64_t offset;
	uint64_t out_count;
};

/* ---- Path / filesystem ---- */

struct a20_path_open_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t dir;
	uint32_t flags;
	a20_rights_t rights;
	uint64_t path;
	uint32_t path_len;
	uint32_t mode;
	a20_handle_t out_handle;
};

struct a20_stat {
	uint32_t size;
	uint32_t version;
	uint64_t dev;
	uint64_t ino;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint64_t size_bytes;
	uint64_t blocks;
	uint64_t atime_ns;
	uint64_t mtime_ns;
	uint64_t ctime_ns;
};

struct a20_path_create_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t dir;
	uint32_t type;
	uint32_t mode;
	uint64_t path;
	uint32_t path_len;
	uint64_t dev;
	a20_handle_t out_handle;
};

struct a20_path_unlink_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t dir;
	uint32_t flags;
	uint64_t path;
	uint32_t path_len;
	uint32_t _pad;
};

struct a20_path_rename_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t old_dir;
	a20_handle_t new_dir;
	uint64_t old_path;
	uint32_t old_path_len;
	uint32_t _pad0;
	uint64_t new_path;
	uint32_t new_path_len;
	uint32_t flags;
};

struct a20_path_link_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t old_dir;
	a20_handle_t new_dir;
	uint64_t old_path;
	uint32_t old_path_len;
	uint64_t new_path;
	uint32_t new_path_len;
	uint32_t flags;
};

struct a20_path_symlink_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t dir;
	uint64_t target;
	uint32_t target_len;
	uint64_t linkpath;
	uint32_t linkpath_len;
};

struct a20_path_readlink_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t dir;
	uint64_t path;
	uint32_t path_len;
	uint64_t buf;
	uint64_t buf_len;
	uint64_t out_len;
};

struct a20_dirent {
	uint32_t type;
	uint32_t name_len;
	char name[256];
};

/* ---- Task / thread ---- */

struct a20_spawn_handle {
	a20_handle_t handle;
	a20_rights_t rights;
	uint32_t target_slot;
	uint32_t flags;
};

struct a20_task_spawn_args {
	uint32_t size;
	uint32_t version;              /* 2 */
	a20_handle_t image;
	a20_handle_t root_dir;
	a20_handle_t cwd_dir;
	a20_handle_t event_queue;
	uint64_t argv;
	uint64_t envp;
	uint32_t argc;
	uint32_t envc;
	uint64_t handles;
	uint32_t handle_count;
	uint32_t flags;
	a20_handle_t out_task;
	a20_handle_t stdin_handle;
	a20_handle_t stdout_handle;
	a20_handle_t stderr_handle;
	uint32_t reserved;
};

struct a20_thread_create_args {
	uint32_t size;
	uint32_t version;
	uint64_t entry;
	uint64_t arg;
	uint64_t stack_base;
	uint64_t stack_size;
	uint64_t tls_base;
	uint32_t flags;
	a20_handle_t out_thread;
};

struct a20_task_status {
	uint32_t size;
	uint32_t version;
	int32_t exit_code;
	uint32_t exit_reason;
	uint64_t utime_ns;
	uint64_t stime_ns;
};

struct a20_task_info {
	uint32_t size;
	uint32_t version;
	int32_t pid;
	int32_t ppid;
	int32_t thread_count;
	int32_t _pad;
	uint64_t vm_size;
	uint64_t vm_rss;
	uint64_t user_time_ns;
	uint64_t sys_time_ns;
};

struct a20_rusage {
	uint64_t user_time_ns;
	uint64_t sys_time_ns;
	uint64_t max_rss;
	uint64_t page_faults;
	uint64_t io_read;
	uint64_t io_write;
};

/* ---- Memory ---- */

inline constexpr uint32_t A20_PROT_READ  = 1;
inline constexpr uint32_t A20_PROT_WRITE = 2;
inline constexpr uint32_t A20_PROT_EXEC  = 4;

struct a20_vm_alloc_args {
	uint32_t size;
	uint32_t version;
	uint64_t addr_hint;
	uint64_t length;
	uint32_t prot;
	uint32_t flags;
	uint64_t out_addr;
};

struct a20_vm_map_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t source;
	uint32_t _pad;
	uint64_t addr_hint;
	uint64_t length;
	uint64_t offset;
	uint32_t prot;
	uint32_t flags;
	uint64_t out_addr;
};

/* ---- Handle ops ---- */

struct a20_handle_dup_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t source;
	uint32_t flags;
	a20_rights_t rights_mask;
	a20_handle_t out_handle;
	uint32_t reserved;
};

struct a20_handle_info {
	uint32_t size;
	uint32_t version;
	uint32_t object_type;
	uint32_t state;
	a20_rights_t rights;
	uint64_t object_id_hint;
	uint64_t flags;
};

struct a20_set_meta_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t handle;
	uint32_t flags;
	uint32_t mode;
	uint32_t uid;
	uint32_t gid;
	uint64_t atime_ns;
	uint64_t mtime_ns;
	uint64_t ctime_ns;
	uint64_t truncate_size;
	uint64_t allocate_size;
};

inline constexpr uint32_t A20_SET_META_MODE     = 1u << 0;
inline constexpr uint32_t A20_SET_META_OWNER    = 1u << 1;
inline constexpr uint32_t A20_SET_META_ATIME    = 1u << 2;
inline constexpr uint32_t A20_SET_META_MTIME    = 1u << 3;
inline constexpr uint32_t A20_SET_META_CTIME    = 1u << 4;
inline constexpr uint32_t A20_SET_META_TRUNCATE = 1u << 5;

/* ---- Sync ---- */

struct a20_futex_wait_args {
	uint32_t size;
	uint32_t version;
	uint64_t addr;
	uint32_t expected;
	uint32_t flags;
	uint64_t timeout_ns;
};

struct a20_futex_wake_args {
	uint32_t size;
	uint32_t version;
	uint64_t addr;
	uint32_t count;
	uint32_t flags;
	uint32_t out_woken;
	uint32_t reserved;
};

/* ---- handle_poll ---- */

struct a20_handle_poll_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t handle;
	uint32_t flags;
	uint64_t event_mask;
	uint64_t out_events;
};

inline constexpr uint32_t A20_EVENT_READABLE       = 0;
inline constexpr uint32_t A20_EVENT_WRITABLE       = 1;
inline constexpr uint32_t A20_EVENT_ERROR          = 2;
inline constexpr uint32_t A20_EVENT_CLOSED         = 3;
inline constexpr uint32_t A20_EVENT_CONNECTION     = 4;
inline constexpr uint32_t A20_EVENT_ACCEPT_READY   = 5;
inline constexpr uint32_t A20_EVENT_EXPIRED        = 6;
inline constexpr uint32_t A20_EVENT_EXITED         = 7;
inline constexpr uint32_t A20_EVENT_MESSAGE_READY  = 8;

/* ---- Channel ---- */

struct a20_channel_create_args {
	uint32_t size;
	uint32_t version;
	uint32_t msg_capacity;
	uint32_t flags;
	uint64_t type;
	a20_handle_t out_endpoints[2];
};

struct a20_msg_send_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t channel;
	uint32_t _pad;
	uint64_t data;
	uint32_t data_len;
	uint32_t flags;
	uint64_t handles;
	uint32_t handle_count;
	uint64_t transfer_rights;
};

struct a20_msg_recv_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t channel;
	uint32_t _pad;
	uint64_t data_buf;
	uint32_t data_buf_len;
	uint32_t _pad2;
	uint64_t handle_buf;
	uint32_t handle_buf_count;
	uint32_t flags;
	uint64_t out_data_len;
	uint32_t out_handle_count;
	uint64_t out_rights_buf;
};

inline constexpr uint32_t A20_MSG_NONBLOCK = 1u << 0;

/* ---- Network ---- */

struct a20_net_addr {
	uint16_t family;
	uint16_t port;
	uint32_t _pad;
	uint8_t addr[16];
};

struct a20_net_sendmsg_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t socket;
	uint64_t iov;
	uint32_t iov_count;
	uint32_t flags;
	uint64_t addr;
	uint64_t control;
	uint32_t control_len;
	uint64_t out_sent;
};

struct a20_net_recvmsg_args {
	uint32_t size;
	uint32_t version;
	a20_handle_t socket;
	uint64_t iov;
	uint32_t iov_count;
	uint32_t flags;
	uint64_t addr;
	uint64_t control;
	uint32_t control_len;
	uint64_t out_received;
	uint32_t out_addr_len;
};

static inline a20_status_t a20_rt_handle_poll(a20_handle_t h, uint64_t mask,
                                              uint64_t *out_events) {
	a20_handle_poll_args args{
	    .size = sizeof(args), .version = 1, .handle = h, .flags = 0,
	    .event_mask = mask, .out_events = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_poll, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st == A20_OK && out_events)
		*out_events = args.out_events;
	return st;
}

/* ---- System ---- */

struct a20_system_info {
	uint32_t size;
	uint32_t struct_version;
	char sysname[64];
	char nodename[64];
	char release[64];
	char version[64];
	char machine[64];
	uint64_t total_ram;
	uint64_t free_ram;
	uint64_t total_swap;
	uint64_t free_swap;
	uint16_t num_procs;
	uint16_t _pad;
	uint32_t configured_cpus;
	uint32_t online_cpus;
	uint32_t current_cpu;
	uint32_t page_size;
	uint64_t uptime_ns;
};

struct a20_security_context {
	uint32_t size;
	uint32_t version;
	int32_t uid;
	int32_t euid;
	int32_t gid;
	int32_t egid;
	int32_t ngroups;
	int32_t _pad;
	uint64_t groups;
	uint64_t cap_effective;
	uint64_t namespace_mask;
	a20_rights_t effective_rights;
	uint32_t flags;
	uint32_t label;
};

/* ---- start_info access (set by crt0/entry) ---- */

extern "C" const a20_start_info *__a20_start_info;

/* ---- small inline helpers ---- */

static inline a20_status_t a20_rt_handle_write(a20_handle_t h, const void *buf,
                                               uint64_t len, uint64_t offset) {
	a20_iovec iov{ reinterpret_cast<uint64_t>(buf), len };
	a20_io_args args{
	    .size = sizeof(args), .version = 1, .handle = h, ._pad0 = 0,
	    .iov = reinterpret_cast<uint64_t>(&iov), .iov_count = 1, ._pad1 = 0,
	    .offset = offset, .out_count = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return st;
	return (a20_status_t)args.out_count;
}

static inline a20_status_t a20_rt_handle_read(a20_handle_t h, void *buf,
                                              uint64_t len, uint64_t offset) {
	a20_iovec iov{ reinterpret_cast<uint64_t>(buf), len };
	a20_io_args args{
	    .size = sizeof(args), .version = 1, .handle = h, ._pad0 = 0,
	    .iov = reinterpret_cast<uint64_t>(&iov), .iov_count = 1, ._pad1 = 0,
	    .offset = offset, .out_count = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st < 0)
		return st;
	return (a20_status_t)args.out_count;
}

static inline a20_status_t a20_rt_futex_wait(uint32_t *addr, uint32_t expected,
                                             uint64_t timeout_ns) {
	a20_futex_wait_args args{
	    .size = sizeof(args), .version = 1, .addr = (uint64_t)addr,
	    .expected = expected, .flags = 0, .timeout_ns = timeout_ns,
	};
	return a20_syscall6(A20_SYS_futex_wait, (uint64_t)&args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_rt_futex_wake(uint32_t *addr, uint32_t count,
                                             uint32_t *out_woken) {
	a20_futex_wake_args args{
	    .size = sizeof(args), .version = 1, .addr = (uint64_t)addr,
	    .count = count, .flags = 0, .out_woken = 0, .reserved = 0,
	};
	a20_status_t st = a20_syscall6(A20_SYS_futex_wake, (uint64_t)&args, 0, 0, 0, 0, 0);
	if (st == A20_OK && out_woken)
		*out_woken = args.out_woken;
	return st;
}

/* Checkpoint-based signal simulation.  Signals are recorded by task_kill and
 * consumed at explicit checkpoints (futex wait return, pthread_testcancel);
 * they are never delivered asynchronously. */
static inline int64_t a20_rt_signal_check() {
	return a20_syscall6(A20_SYS_signal_check, 0, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_rt_signal_mask(uint64_t new_mask,
                                              uint64_t *old_mask) {
	return a20_syscall6(A20_SYS_signal_mask, new_mask,
	                    (uint64_t)old_mask, 0, 0, 0, 0);
}
