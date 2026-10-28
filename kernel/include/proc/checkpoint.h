#ifndef A20_PROC_CHECKPOINT_H
#define A20_PROC_CHECKPOINT_H

/*
 * Service-level checkpoint/restore (CRIU-lite), the route-2 companion to
 * kexec hot update: a single-threaded user task is frozen through the ptrace
 * machinery, its register file, memory and file-descriptor table are
 * serialized to a regular file, and after a kexec handoff (or any later
 * reboot) another process restores the image with proc_ckpt_restore(), which
 * replaces the caller's address space exec-style and resumes from the
 * checkpointed instruction.
 *
 * Deliberately out of scope (rejected with a clear error, never silently
 * dropped): multi-threaded tasks, sockets, pipes, channels, SysV/posix IPC,
 * timers, signal state, namespace state, and NOMMU/flat tasks.  Sockets and
 * pipes cannot survive a kernel switch by construction.
 */

#include "core/types.h"
#include "proc/debug_regs.h"

#define A20_CKPT_MAGIC   0x3154504b43303241ULL /* "A20CKPT1" little-endian */
#define A20_CKPT_VERSION 1

/* checkpoint flags */
#define A20_CKPT_F_RESUME   0x1 /* thaw the target after a successful write */
#define A20_CKPT_F_SKIP_UNSUPPORTED_FDS 0x2 /* record unrestorable fds as closed */

/* fd record kinds */
#define A20_CKPT_FD_FILE  1 /* regular file or device with a stable path */
#define A20_CKPT_FD_CLOSED 2 /* present but unrestorable (only with _SKIP_) */

typedef struct a20_ckpt_header {
    uint64_t magic;
    uint32_t version;
    uint32_t arch;        /* ARCH_ELF_MACHINE of the producing kernel */
    uint32_t abi_mode;    /* task_t::abi_mode at checkpoint time */
    uint32_t flags;
    uint32_t nr_vma;
    uint32_t nr_fd;
    uint32_t uid;
    uint32_t gid;
    int32_t  pgid;
    int32_t  sid;
    uint64_t brk;
    uint64_t regset_size; /* must equal sizeof(proc_debug_regs_t) */
    char     name[64];
    char     exec_path[256];
    proc_debug_regs_t regs;
} a20_ckpt_header_t;

typedef struct a20_ckpt_vma {
    uint64_t start;  /* inclusive, page aligned */
    uint64_t end;    /* exclusive */
    uint64_t vm_flags;
    uint64_t data_offset; /* absolute file offset of the page contents */
} a20_ckpt_vma_t;

typedef struct a20_ckpt_fd {
    int32_t  fd;
    int32_t  kind;    /* A20_CKPT_FD_* */
    int32_t  flags;   /* kernel open flags, creation bits stripped at restore */
    int32_t  cloexec;
    uint64_t offset;
    char     path[256];
} a20_ckpt_fd_t;

/* out_fd/in_fd live in the CALLER's Linux-ABI fd table.  Restore only returns
 * on failure: success swaps the caller into the checkpointed image. */
int proc_ckpt_checkpoint(int pid, int out_fd, uint32_t flags);
int proc_ckpt_restore(int in_fd);

/* Path-based variants for the Native ABI thin wrapper (the native handle
 * table has no fd space to write through). */
int proc_ckpt_checkpoint_path(int pid, const char *path, uint32_t flags);
int proc_ckpt_restore_path(const char *path);

#endif /* A20_PROC_CHECKPOINT_H */
