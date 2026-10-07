/*
 * A20OS Native ABI — User-visible type definitions.
 *
 * This header defines all fundamental types for the Native ABI.
 * Design reference: docs/native-abi/01-types.md
 */
#ifndef _ABI_NATIVE_TYPES_H
#define _ABI_NATIVE_TYPES_H

#include <stdint.h>

/* The object model, handle table and IPC subsystem are internal
 * (kernel/include/ipc/); this ABI header only adds the syscall wire
 * structures on top. */
#include "ipc/ipc.h"
#include "ipc/handle_table.h"

/* ---- Fundamental types ---- */

typedef uint64_t a20_flags_t;      /* Operation flag bitmask */
typedef int64_t  a20_status_t;     /* Return status: >= 0 success, < 0 error */
typedef uint64_t a20_time_ns_t;    /* Nanosecond timestamp */
typedef uint64_t a20_off_t;        /* File offset */
typedef uint64_t a20_size_t;       /* Size */
typedef uint64_t a20_vaddr_t;      /* Virtual address */

#define A20_NATIVE_FD_HANDLE_BASE 64u

/* ---- ABI header convention ---- */

typedef struct a20_abi_header {
    uint32_t size;
    uint32_t version;
} a20_abi_header_t;


/* ---- ABI info structure ---- */

typedef struct a20_abi_info {
    uint32_t size;
    uint32_t version;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t abi_patch;
    uint32_t pointer_bits;
    uint32_t page_size;
    uint32_t handle_bits;
    uint64_t feature_bits[4];
    uint64_t syscall_bitmap_addr;
    uint64_t syscall_bitmap_size;
} a20_abi_info_t;

#define A20_ABI_MAJOR  1
#define A20_ABI_MINOR  0
#define A20_ABI_PATCH  0

/* ---- Handle operation structures ---- */

typedef struct a20_handle_dup_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   source;
    uint32_t       flags;
    a20_rights_t   rights_mask;
    a20_handle_t   out_handle;
    uint32_t       reserved;
} a20_handle_dup_args_t;

typedef struct a20_handle_info {
    uint32_t       size;
    uint32_t       version;
    uint32_t       object_type;
    uint32_t       state;
    a20_rights_t   rights;
    uint64_t       object_id_hint;
    uint64_t       flags;
} a20_handle_info_t;

typedef struct a20_control_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       namespace_id;
    uint32_t       command;
    uint64_t       in_ptr;
    uint64_t       in_size;
    uint64_t       out_ptr;
    uint64_t       out_size;
    uint64_t       out_actual;
} a20_control_args_t;

/* ---- handle_control commands ----
 * The syscall form is handle_control(handle, op, arg0, arg1).
 * op 0/1 are file/device ioctl/fcntl pass-through; op 2-4 operate on the
 * handle entry itself (temporal capability + label management).  Op 5 changes
 * the calling task's cwd to a directory handle. */

#define A20_HANDLE_CTRL_SET_TEMPORAL   2u  /* arg0 = a20_handle_temporal_args_t* */
#define A20_HANDLE_CTRL_GET_TEMPORAL   3u  /* arg0 = a20_handle_temporal_args_t* */
#define A20_HANDLE_CTRL_SET_LABEL      4u  /* arg0 = a20_ctl_int_args_t* (label) */
#define A20_HANDLE_CTRL_CHDIR          5u  /* directory handle -> current cwd    */
#define A20_HANDLE_CTRL_GET_WINSIZE    6u  /* arg0 = a20_winsize_args_t* (out)   */
#define A20_HANDLE_CTRL_SET_WINSIZE    7u  /* arg0 = a20_winsize_args_t* (in)    */
#define A20_HANDLE_CTRL_TCFLUSH        8u  /* arg0 = a20_ctl_int_args_t* (queue) */
#define A20_HANDLE_CTRL_SET_FLAGS      9u  /* arg0 = a20_ctl_flags_args_t*       */

/*
 * Typed control operations (the A20 answer to ioctl).  ioctl's problems are
 * its untyped void* argument, opaque magic-number commands, and lack of any
 * capability discipline.  A20 control ops are instead:
 *   - typed: each (object type, op) has a fixed, documented argument struct;
 *   - versioned: the argument struct carries {size, version} and follows the
 *     E-APPEND / E-DEPRECATE / E-RESERVED evolution rules, so a newer kernel
 *     can extend it without breaking older callers;
 *   - capability-gated: requires the handle's Control right.
 * There is no generic ioctl in the native ABI: terminal/device control is
 * expressed with the typed ops above.  The POSIX ioctl() surface is
 * translated to these ops in the libc (mlibc) and returns ENOTTY for ops
 * with no native equivalent.
 */
typedef struct a20_winsize_args {
    uint32_t       size;        /* sizeof(a20_winsize_args_t) */
    uint32_t       version;     /* 1 */
    uint16_t       ws_row;
    uint16_t       ws_col;
    uint16_t       ws_xpixel;
    uint16_t       ws_ypixel;
} a20_winsize_args_t;

/* Single-scalar control argument (TCFLUSH queue selector, security label). */
typedef struct a20_ctl_int_args {
    uint32_t       size;
    uint32_t       version;     /* 1 */
    int32_t        value;
    uint32_t       reserved;
} a20_ctl_int_args_t;

/* Open-file flag change (SET_FLAGS): valid_mask selects which O_* bits to
 * change; flags carries the new bits.  Access mode and creation flags are
 * immutable after open, like POSIX fcntl(F_SETFL). */
typedef struct a20_ctl_flags_args {
    uint32_t       size;
    uint32_t       version;     /* 1 */
    int32_t        valid_mask;
    int32_t        flags;
} a20_ctl_flags_args_t;

/* Temporal capability control (docs/native-abi/03-handle.md §2.6,
 * docs/native-abi/06-security.md §6).  SET_TEMPORAL is strengthening-only
 * (non-refreshability): an existing expiry can only be lowered, an existing
 * operation count can only be decreased, and set flags cannot be cleared. */
typedef struct a20_handle_temporal_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       expiry_ns;      /* absolute CLOCK_MONOTONIC ns; 0 = no expiry */
    uint32_t       remaining_ops;  /* operation budget when OP_COUNT flag set   */
    uint32_t       temporal_flags; /* A20_TEMPORAL_*                            */
} a20_handle_temporal_args_t;

/* ---- I/O structures ---- */

typedef struct a20_iovec {
    uint64_t base;
    uint64_t len;
} a20_iovec_t;

#define A20_OFFSET_CURRENT  ((uint64_t)-1)

typedef struct a20_io_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       _pad0;
    uint64_t       iov;
    uint32_t       iov_count;
    uint32_t       _pad1;
    uint64_t       offset;
    uint64_t       out_count;
} a20_io_args_t;

typedef struct a20_stat {
    uint32_t       size;
    uint32_t       version;
    uint64_t       dev;
    uint64_t       ino;
    uint32_t       mode;
    uint32_t       nlink;
    uint32_t       uid;
    uint32_t       gid;
    uint64_t       size_bytes;
    uint64_t       blocks;
    uint64_t       atime_ns;
    uint64_t       mtime_ns;
    uint64_t       ctime_ns;
} a20_stat_t;

/* ---- Set meta flags ---- */

#define A20_SET_META_MODE      (1u << 0)
#define A20_SET_META_OWNER     (1u << 1)
#define A20_SET_META_ATIME     (1u << 2)
#define A20_SET_META_MTIME     (1u << 3)
#define A20_SET_META_CTIME     (1u << 4)
#define A20_SET_META_TRUNCATE  (1u << 5)
#define A20_SET_META_ALLOCATE  (1u << 6)

typedef struct a20_set_meta_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       flags;
    uint32_t       mode;
    uint32_t       uid;
    uint32_t       gid;
    uint64_t       atime_ns;
    uint64_t       mtime_ns;
    uint64_t       ctime_ns;
    uint64_t       truncate_size;
    uint64_t       allocate_size;
} a20_set_meta_args_t;

typedef struct a20_xattr_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       _pad;
    uint64_t       name;
    uint32_t       name_len;
    uint32_t       _pad2;
    uint64_t       value;
    uint64_t       value_len;
    uint32_t       flags;
} a20_xattr_args_t;

typedef struct a20_xattr_list_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       _pad;
    uint64_t       buf;
    uint64_t       buf_len;
    uint64_t       out_len;
} a20_xattr_list_args_t;

/* ---- Transfer (splice) ---- */

#define A20_TRANSFER_PEEK  (1u << 0)  /* tee semantics (don't consume source) */

typedef struct a20_transfer_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   source;
    a20_handle_t   dest;
    uint32_t       flags;
    uint64_t       source_offset;
    uint64_t       dest_offset;
    uint64_t       length;
    uint64_t       out_transferred;
} a20_transfer_args_t;

/* ---- Spawn handle ---- */

typedef struct a20_spawn_handle {
    a20_handle_t   handle;
    a20_rights_t   rights;
    uint32_t       target_slot;
    uint32_t       flags;
} a20_spawn_handle_t;

/* ---- Task structures ---- */

typedef struct a20_task_spawn_args {
    uint32_t       size;
    uint32_t       version;        /* 1 = base layout; 2 = adds the stdio fields */
    a20_handle_t   image;
    a20_handle_t   root_dir;
    a20_handle_t   cwd_dir;
    a20_handle_t   event_queue;
    uint64_t       argv;
    uint64_t       envp;
    uint32_t       argc;
    uint32_t       envc;
    uint64_t       handles;
    uint32_t       handle_count;
    uint32_t       flags;
    a20_handle_t   out_task;
    /* ---- appended in version 2: the standard I/O handles for the child's
     * start_info ---- */
    a20_handle_t   stdin_handle;   /* A20_HANDLE_NULL means do not inherit */
    a20_handle_t   stdout_handle;
    a20_handle_t   stderr_handle;
    uint32_t       reserved;
} a20_task_spawn_args_t;

/* Size of the v1 struct including trailing alignment padding, i.e. the layout
 * this struct had up to stdin_handle in v2. */
#define A20_TASK_SPAWN_ARGS_V1_SIZE  72

/* ---- Capability-safe clone (task_clone) ----
 *
 * A20OS has no fork.  task_clone is the capability-safe "child continuation"
 * primitive:
 *  - Register continuation: the child resumes at the call site (a0 == 0 tells
 *    parent from child) and memory is copied COW.  This copies the thread's own
 *    state; it grants no capability.
 *  - Capability manifest: the child's handle table is built strictly from the
 *    per-entry declarations in handles[], under the same discipline as
 *    task_spawn: permissions are a subset of the parent's, may be reduced, and
 *    nothing is inherited implicitly.  The child cannot obtain any handle
 *    outside the manifest -- this is the fundamental difference from fork,
 *    which implicitly duplicates every capability.
 *  - Returns: the parent gets the child's pid; the child sees a0 == 0. */
#define A20_CLONE_COW_VM   (1u << 0)   /* child gets a COW copy of the address space (default) */
#define A20_CLONE_STACK    (1u << 1)   /* override the child's SP with the stack field */

typedef struct a20_clone_handle {
    a20_handle_t parent_handle;   /* in: handle held by the parent */
    a20_rights_t  child_rights;   /* in: 0 = inherit the parent's rights, else a subset of them */
    a20_handle_t  child_handle;   /* out: value installed into the child's handle table */
} a20_clone_handle_t;

typedef struct a20_clone_args {
    uint32_t       size;
    uint32_t       version;        /* 1 */
    uint32_t       flags;
    uint32_t       reserved;
    uint64_t       handles;        /* user pointer: a20_clone_handle_t[] */
    uint32_t       handle_count;
    uint32_t       reserved1;
    a20_handle_t   root_dir;       /* parent's root directory handle (written into the child) */
    a20_handle_t   cwd_dir;        /* parent's cwd directory handle (written into the child) */
    uint64_t       stack;          /* used as the child's SP when A20_CLONE_STACK is set */
    a20_handle_t   out_task;       /* out: child task handle returned to the parent */
    a20_handle_t   out_root;       /* out: child's root handle (written back into child memory) */
    a20_handle_t   out_cwd;        /* out: child's cwd handle */
    a20_handle_t   out_self;       /* out: child's own task handle */
} a20_clone_args_t;

typedef struct a20_task_status {
    uint32_t       size;
    uint32_t       version;
    int32_t        exit_code;
    uint32_t       exit_reason;
    uint64_t       utime_ns;
    uint64_t       stime_ns;
} a20_task_status_t;

/* task_wait flags (docs/native-abi/09-native-abi-deepening.md §8).
 * Unknown bits must be zero. */
#define A20_TASK_WAIT_NONBLOCK 0x1u

typedef struct a20_task_info {
    uint32_t       size;
    uint32_t       version;
    int32_t        pid;
    int32_t        ppid;
    int32_t        thread_count;
    int32_t        _pad;
    uint64_t       vm_size;
    uint64_t       vm_rss;
    uint64_t       user_time_ns;
    uint64_t       sys_time_ns;
} a20_task_info_t;

typedef struct a20_thread_create_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       entry;
    uint64_t       arg;
    uint64_t       stack_base;
    uint64_t       stack_size;
    uint64_t       tls_base;
    uint32_t       flags;
    a20_handle_t   out_thread;
} a20_thread_create_args_t;

typedef struct a20_sched_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   task;
    uint32_t       flags;
    int32_t        policy;
    int32_t        priority;
    int32_t        nice;
    uint64_t       affinity;
    uint64_t       affinity_size;
} a20_sched_args_t;

#define A20_SCHED_POLICY   (1U << 0)
#define A20_SCHED_PRIORITY (1U << 1)
#define A20_SCHED_AFFINITY (1U << 2)
#define A20_SCHED_NICE     (1U << 3)

typedef struct a20_rlimit_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   task;
    uint32_t       resource;
    uint64_t       cur;
    uint64_t       max;
} a20_rlimit_args_t;

typedef struct a20_rusage {
    uint64_t       user_time_ns;
    uint64_t       sys_time_ns;
    uint64_t       max_rss;
    uint64_t       page_faults;
    uint64_t       io_read;
    uint64_t       io_write;
} a20_rusage_t;

/* ---- Memory structures ---- */

typedef struct a20_vm_alloc_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr_hint;
    uint64_t       length;
    uint32_t       prot;
    uint32_t       flags;
    uint64_t       out_addr;
} a20_vm_alloc_args_t;

typedef struct a20_vm_map_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   source;
    uint32_t       _pad;
    uint64_t       addr_hint;
    uint64_t       length;
    uint64_t       offset;
    uint32_t       prot;
    uint32_t       flags;
    uint64_t       out_addr;
    /* v1.1 E-APPEND: map through a VMAR reservation (A20_OBJ_VMAR).
     * NULL keeps the legacy whole-address-space behavior. */
    a20_handle_t   vmar;
} a20_vm_map_args_t;

/* vm_create_vmar: sub-allocate an address range under a parent VMAR
 * (docs/native-abi/04-memory.md §3).  Ceiling bits are the existing
 * A20_VMAR_CAN_MAP_* above. */
typedef struct a20_vm_create_vmar_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   parent;   /* A20_HANDLE_NULL -> root reservation */
    uint32_t       _pad;
    uint64_t       base;
    uint64_t       length;
    uint64_t       flags;    /* A20_VMAR_CAN_MAP_* ceiling */
    a20_handle_t   out_vmar;
} a20_vm_create_vmar_args_t;

typedef struct a20_vm_share_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr;
    uint64_t       length;
    a20_rights_t   rights;
    a20_handle_t   out_handle;
} a20_vm_share_args_t;

typedef struct a20_vm_remap_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       old_addr;
    uint64_t       old_size;
    uint64_t       new_addr_hint;
    uint64_t       new_size;
    uint32_t       flags;
    uint64_t       out_addr;
} a20_vm_remap_args_t;

typedef struct a20_vm_object_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       size_bytes;
    uint32_t       flags;
    a20_handle_t   out_handle;
} a20_vm_object_args_t;

/* ---- Protection bits ---- */

#define A20_PROT_READ    (1u << 0)
#define A20_PROT_WRITE   (1u << 1)
#define A20_PROT_EXEC    (1u << 2)
#define A20_PROT_NONE    0

/* ---- VMAR flags ---- */

#define A20_VMAR_CAN_MAP_READ     (1u << 0)
#define A20_VMAR_CAN_MAP_WRITE    (1u << 1)
#define A20_VMAR_CAN_MAP_EXEC     (1u << 2)
#define A20_VMAR_CAN_MAP_SPECIFIC (1u << 3)

/* ---- Flush flags ---- */

#define A20_FLUSH_CLEAN       (1u << 0)
#define A20_FLUSH_INVALIDATE  (1u << 1)
#define A20_FLUSH_SYNC        (1u << 2)

/* ---- VMO types ---- */

#define A20_VMO_ANONYMOUS  0
#define A20_VMO_PHYSICAL   1
#define A20_VMO_PAGED      2

/* ---- Path/Filesystem structures ---- */

typedef struct a20_path_open_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint32_t       flags;
    a20_rights_t   rights;
    uint64_t       path;
    uint32_t       path_len;
    uint32_t       mode;
    a20_handle_t   out_handle;
} a20_path_open_args_t;

typedef struct a20_path_create_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint32_t       type;       /* file, dir, device, ... */
    uint32_t       mode;
    uint64_t       path;
    uint32_t       path_len;
    uint64_t       dev;        /* device node major:minor */
    a20_handle_t   out_handle;
} a20_path_create_args_t;

typedef struct a20_path_unlink_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint32_t       flags;       /* AT_REMOVEDIR or 0 */
    uint64_t       path;
    uint32_t       path_len;
    uint32_t       _pad;
} a20_path_unlink_args_t;

typedef struct a20_path_rename_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   old_dir;
    a20_handle_t   new_dir;
    uint64_t       old_path;
    uint32_t       old_path_len;
    uint32_t       _pad0;
    uint64_t       new_path;
    uint32_t       new_path_len;
    uint32_t       flags;
} a20_path_rename_args_t;

typedef struct a20_path_link_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   old_dir;
    a20_handle_t   new_dir;
    uint64_t       old_path;
    uint32_t       old_path_len;
    uint64_t       new_path;
    uint32_t       new_path_len;
    uint32_t       flags;
} a20_path_link_args_t;

typedef struct a20_path_symlink_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint64_t       target;
    uint32_t       target_len;
    uint64_t       linkpath;
    uint32_t       linkpath_len;
} a20_path_symlink_args_t;

typedef struct a20_path_readlink_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint64_t       path;
    uint32_t       path_len;
    uint64_t       buf;
    uint64_t       buf_len;
    uint64_t       out_len;
} a20_path_readlink_args_t;

typedef struct a20_path_resolve_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   dir;
    uint64_t       path;
    uint32_t       path_len;
    uint32_t       flags;
    uint64_t       out_path;
    uint64_t       out_path_len;
} a20_path_resolve_args_t;

typedef struct a20_fs_stat {
    uint64_t       block_size;
    uint64_t       total_blocks;
    uint64_t       free_blocks;
    uint64_t       available_blocks;
    uint64_t       total_files;
    uint64_t       free_files;
    uint64_t       fs_id;
} a20_fs_stat_t;

typedef struct a20_dirent {
    uint32_t       type;
    uint32_t       name_len;
    char           name[256];
} a20_dirent_t;

typedef struct a20_fs_mount_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       source;
    uint32_t       source_len;
    uint32_t       _pad;
    uint64_t       target;
    uint32_t       target_len;
    uint32_t       _pad2;
    uint64_t       fs_type;
    uint32_t       fs_type_len;
    uint32_t       flags;
} a20_fs_mount_args_t;

/* fs_serve: registers one of the caller's channel endpoints as the user-space
 * file service for a mount point (docs/hybrid-kernel/06-user-fs.md).
 * block_index < 0 means there is no block backing. */
typedef struct a20_fs_serve_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   server_channel;  /* A20_OBJ_CHANNEL_ENDPOINT, R|W */
    int32_t        block_index;     /* DEV_CLASS_BLOCK index */
    uint64_t       target;
    uint32_t       target_len;
    uint32_t       flags;
} a20_fs_serve_args_t;

/* fs_block_io: controlled block IO for a uxfs service task, at sector
 * granularity, passed straight through to the block layer synchronously. */
typedef struct a20_fs_block_io_args {
    uint32_t       size;
    uint32_t       version;
    int32_t        block_index;
    uint32_t       write;           /* 0 = read, 1 = write */
    uint64_t       lba;
    uint32_t       count;           /* sector count */
    uint32_t       _pad;
    uint64_t       buf;
} a20_fs_block_io_args_t;

/* ---- IPC/Event structures ---- */

typedef struct a20_event_queue_create_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       capacity_hint;
    uint32_t       flags;
    a20_handle_t   out_queue;
} a20_event_queue_create_args_t;

typedef struct a20_event_watch_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   queue;
    a20_handle_t   target;
    uint64_t       event_mask;
    uint64_t       user_data;
    /* v1.1 E-APPEND: watch delivery mode flags (A20_WATCH_*). */
    uint64_t       flags;
} a20_event_watch_args_t;

/* a20_event_watch_args_t.flags */
#define A20_WATCH_LEVEL   0x1ull /* level-triggered: readiness is re-checked
                                  * on every wait, not only on transitions */



typedef struct a20_event_wait_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   queue;
    uint32_t       _pad;
    uint64_t       events;
    uint32_t       max_events;
    uint32_t       _pad2;
    uint64_t       timeout_ns;
    uint32_t       flags;
    uint32_t       out_count;
} a20_event_wait_args_t;

typedef struct a20_event_watch_fs_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   queue;
    a20_handle_t   dir;
    uint64_t       path;
    uint32_t       path_len;
    uint32_t       event_mask;
    uint64_t       user_data;
} a20_event_watch_fs_args_t;

/* ---- Channel structures ---- */

#define A20_NS_FILESYSTEM 0
#define A20_NS_NETWORK    1
#define A20_NS_PID        2
#define A20_NS_DEVICE     3

typedef struct a20_channel_create_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       msg_capacity;
    uint32_t       flags;
    uint64_t       type;            /* a20_channel_type_t* or 0 */
    a20_handle_t   out_endpoints[2];
} a20_channel_create_args_t;

typedef struct a20_msg_send_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   channel;
    uint32_t       _pad;
    uint64_t       data;
    uint32_t       data_len;
    uint32_t       flags;
    uint64_t       handles;         /* a20_handle_t[] */
    uint32_t       handle_count;
    uint64_t       transfer_rights; /* a20_rights_t[] per-handle, or 0 */
} a20_msg_send_args_t;

typedef struct a20_msg_recv_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   channel;
    uint32_t       _pad;
    uint64_t       data_buf;
    uint32_t       data_buf_len;
    uint32_t       _pad2;
    uint64_t       handle_buf;      /* a20_handle_t[] */
    uint32_t       handle_buf_count;
    uint32_t       flags;           /* A20_MSG_* (was _pad3) */
    uint64_t       out_data_len;
    uint32_t       out_handle_count;
    uint64_t       out_rights_buf;
} a20_msg_recv_args_t;

/*
 * Fused RPC (docs/hybrid-kernel/00-design.md §4.1): one trap performs the
 * request send and the reply wait, sharing a single handle lookup and a
 * single rights check (READ|WRITE).  Semantics equal channel_send followed
 * by channel_recv on the same endpoint; the reply stage honors
 * A20_MSG_NONBLOCK (request may already be delivered when the reply stage
 * returns A20_ERR_WOULD_BLOCK — the caller can recv it later).
 */
typedef struct a20_channel_call_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   channel;
    uint32_t       flags;           /* A20_MSG_* */
    uint64_t       data;            /* request bytes */
    uint32_t       data_len;
    uint32_t       _pad;
    uint64_t       handles;         /* a20_handle_t[] sent with the request */
    uint32_t       handle_count;
    uint64_t       transfer_rights; /* a20_rights_t[] per-handle, or 0 */
    uint64_t       reply_buf;
    uint32_t       reply_buf_len;
    uint32_t       _pad2;
    uint64_t       reply_handle_buf;      /* a20_handle_t[] */
    uint32_t       reply_handle_buf_count;
    uint64_t       reply_rights_buf;      /* a20_rights_t[] out, or 0 */
    uint32_t       out_reply_len;         /* out: reply bytes */
    uint32_t       out_reply_handles;     /* out: reply handle count */
} a20_channel_call_args_t;

/* ---- User-space driver (udriver) structures ---- */

typedef struct a20_device_map_mmio_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       prot;            /* bit0 = read, bit1 = write */
    uint32_t       _pad;
    uint64_t       phys_base;       /* must be page-aligned and whitelisted */
    uint64_t       length;
    uint64_t       out_addr;
} a20_device_map_mmio_args_t;

typedef struct a20_device_irq_listen_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       irq;
    a20_handle_t   queue;           /* event queue receiving SIGNALED */
    uint64_t       user_data;       /* echoed back as event data0 */
} a20_device_irq_listen_args_t;

typedef struct a20_device_vmo_phys_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   vmo;             /* MEMORY handle (needs STAT right) */
    uint32_t       _pad;
    uint64_t       out_paddrs;      /* u64[] written with physical addrs */
    uint32_t       max_pages;
    uint32_t       out_count;
} a20_device_vmo_phys_args_t;

#define A20_DEVICE_INFO_F_IOMMU   (1u << 0)
#define A20_DEVICE_INFO_F_BLOCKED (1u << 1)

typedef struct a20_device_info_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       bus;
    uint32_t       vendor;
    uint32_t       device;
    uint32_t       index;
    uint32_t       out_flags;
    uint32_t       out_devid;
    uint32_t       out_irq;
    uint32_t       out_fault_cause;
    uint64_t       out_mmio_base;
    uint64_t       out_mmio_size;
    uint64_t       out_fault_count;
    uint64_t       out_fault_iova;
} a20_device_info_args_t;

/* ---- Message flags (channel_send / channel_recv) ---- */


/* ---- Network structures ---- */

typedef struct a20_net_addr {
    uint16_t family;    /* AF_INET, AF_INET6 */
    uint16_t port;
    uint32_t _pad;
    uint8_t  addr[16];  /* IPv4 uses first 4 bytes */
} a20_net_addr_t;

typedef struct a20_net_socket_args {
    uint32_t       size;
    uint32_t       version;
    int32_t        domain;
    int32_t        type;
    int32_t        protocol;
    a20_rights_t   rights;
    a20_handle_t   out_socket;
} a20_net_socket_args_t;

typedef struct a20_net_sendmsg_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   socket;
    uint64_t       iov;
    uint32_t       iov_count;
    uint32_t       flags;
    uint64_t       addr;            /* a20_net_addr_t* or 0 */
    uint64_t       control;
    uint32_t       control_len;
    uint64_t       out_sent;
} a20_net_sendmsg_args_t;

typedef struct a20_net_recvmsg_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   socket;
    uint64_t       iov;
    uint32_t       iov_count;
    uint32_t       flags;
    uint64_t       addr;            /* a20_net_addr_t* output */
    uint64_t       control;
    uint32_t       control_len;
    uint64_t       out_received;
    uint32_t       out_addr_len;
} a20_net_recvmsg_args_t;

typedef struct a20_net_socketpair_args {
    uint32_t       size;
    uint32_t       version;
    int32_t        domain;
    int32_t        type;
    int32_t        protocol;
    a20_handle_t   out_sockets[2];
} a20_net_socketpair_args_t;

/* ---- Timer structures ---- */

typedef struct a20_timer_create_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   event_queue;
    uint64_t       user_data;
    uint32_t       flags;
    a20_handle_t   out_timer;
} a20_timer_create_args_t;

/* ---- Security structures ---- */

typedef struct a20_security_context {
    uint32_t       size;
    uint32_t       version;
    int32_t        uid;
    int32_t        euid;
    int32_t        gid;
    int32_t        egid;
    int32_t        ngroups;
    int32_t        _pad;
    uint64_t       groups;          /* int[] */
    uint64_t       cap_effective;
    uint64_t       namespace_mask;
    a20_rights_t   effective_rights;
    uint32_t       flags;
    uint32_t       label;           /* Security label: 0=L, 1=M, 2=H */
} a20_security_context_t;

/* ---- Debug structures ---- */

typedef struct a20_regs {
    uint64_t       regs[32];
    uint64_t       pc;
    uint64_t       sp;
    uint64_t       sr;
} a20_regs_t;

/* Stop kinds reported by debug_wait / debug_event. */
#define A20_DEBUG_STOP_SIGNAL       1  /* stopped on a signal */
#define A20_DEBUG_STOP_EVENT        2  /* stopped on a watch event (exec/exit) */
#define A20_DEBUG_STOP_SYSCALL_ENTRY 3 /* syscall-entry stop */
#define A20_DEBUG_STOP_SYSCALL_EXIT 4  /* syscall-exit stop */

/* Watch events (same values as the internal PT_DEBUG_EVENT_*). */
#define A20_DEBUG_EVENT_EXEC        4
#define A20_DEBUG_EVENT_EXIT        6

/* Resume modes for debug_resume. */
#define A20_DEBUG_RESUME_CONT       0
#define A20_DEBUG_RESUME_SYSCALL    1

typedef struct a20_debug_event_info {
    uint32_t       size;        /* sizeof(a20_debug_event_info_t) */
    uint32_t       version;     /* 1 */
    uint32_t       kind;        /* A20_DEBUG_STOP_* */
    uint32_t       sig;         /* stop signal (0 when kind != SIGNAL) */
    uint32_t       event;       /* A20_DEBUG_EVENT_* (0 when kind != EVENT) */
    uint32_t       reserved;
    uint64_t       event_msg;   /* event payload (e.g. exit code) */
} a20_debug_event_info_t;

/* Kernel extension point info (ext_point_info). */
typedef struct a20_ext_point_info {
    uint32_t       size;        /* sizeof(a20_ext_point_info_t) */
    uint32_t       version;     /* 1 */
    uint32_t       id;
    uint32_t       nwords;      /* context size in 64-bit words */
    char           name[32];
} a20_ext_point_info_t;

/* ---- Sync structures ---- */

/* handle_poll: non-blocking readiness query, with the query semantics of POSIX
 * poll() -- it never sleeps.  Blocking waits remain the job of event_queue;
 * handle_poll only answers "is this ready right now". */

typedef struct a20_handle_poll_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    uint32_t       flags;         /* reserved, must be 0 */
    uint64_t       event_mask;    /* in: event bitmap of interest (1ull << A20_EVENT_*) */
    uint64_t       out_events;    /* out: bitmap of currently active events */
} a20_handle_poll_args_t;

/* A futex is a synchronisation primitive on a user address, not a kernel
 * object, so it is not allocated a handle.  Its semantics match Zircon's
 * zx_futex_wait / zx_futex_wake: atomically compare *addr against expected,
 * sleeping on equality and otherwise returning A20_ERR_WOULD_BLOCK at once. */


typedef struct a20_futex_wait_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr;          /* user-space 32-bit futex word address, must be 4-byte aligned */
    uint32_t       expected;      /* expected value */
    uint32_t       flags;         /* reserved, must be 0 */
    uint64_t       timeout_ns;    /* relative timeout in ns; A20_TIMEOUT_INFINITE means forever */
} a20_futex_wait_args_t;

typedef struct a20_futex_wake_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr;          /* user-space 32-bit futex word address */
    uint32_t       count;         /* maximum number of waiters to wake, must be >= 1 */
    uint32_t       flags;         /* reserved, must be 0 */
    uint32_t       out_woken;     /* out: number actually woken */
    uint32_t       reserved;
} a20_futex_wake_args_t;

/* ---- Pager structures (docs/native-abi/09-native-abi-deepening.md §2) ---- */

typedef struct a20_pager_create_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;
    uint32_t       reserved;
    a20_handle_t   out_pager;          /* A20_OBJ_PAGER */
    a20_handle_t   out_requests;       /* A20_OBJ_CHANNEL_ENDPOINT */
} a20_pager_create_args_t;

typedef struct a20_pager_vmo_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   pager;              /* A20_RIGHT_CONTROL */
    a20_handle_t   vmo;                /* A20_OBJ_MEMORY (PAGED), CONTROL */
} a20_pager_vmo_args_t;

typedef struct a20_pager_supply_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   pager;              /* A20_RIGHT_WRITE */
    a20_handle_t   vmo;                /* A20_RIGHT_WRITE */
    a20_handle_t   source;             /* A20_OBJ_MEMORY, A20_RIGHT_READ */
    uint64_t       vmo_offset;
    uint64_t       source_offset;
    uint64_t       len;
    uint64_t       out_supplied;
} a20_pager_supply_args_t;

/* ---- Monitor structures (docs/native-abi/09-native-abi-deepening.md §3) ----
 * A20_MONITOR_* kind constants are defined in ipc/ipc.h (core, ABI-free). */

typedef struct a20_monitor_create_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   target;             /* TASK handle or A20_HANDLE_NULL */
    uint32_t       kind;               /* A20_MONITOR_* */
    uint32_t       flags;
    a20_handle_t   queue;              /* optional EventQ or A20_HANDLE_NULL */
    uint64_t       period_ns;
    a20_handle_t   out_monitor;
} a20_monitor_create_args_t;

typedef struct a20_monitor_value {
    uint32_t       size;
    uint32_t       version;
    uint32_t       kind;
    uint32_t       flags;
    uint64_t       count;
    uint64_t       time_active_ns;
    uint64_t       prev;
} a20_monitor_value_t;

/* ---- Task memory structures (docs/native-abi/09-native-abi-deepening.md §4) ---- */

typedef struct a20_task_mem_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   task;
    uint32_t       flags;              /* 0; reserved */
    uint64_t       local_iov;          /* a20_iovec_t[] */
    uint32_t       local_iov_count;
    uint32_t       _pad;
    uint64_t       remote_iov;         /* a20_iovec_t[] (target address space) */
    uint32_t       remote_iov_count;
    uint32_t       _pad2;
    uint64_t       out_transferred;
} a20_task_mem_args_t;

/* ---- System info ---- */

typedef struct a20_system_info {
    uint32_t       size;
    uint32_t       struct_version;
    char           sysname[64];
    char           nodename[64];
    char           release[64];
    char           version[64];
    char           machine[64];
    uint64_t       total_ram;
    uint64_t       free_ram;
    uint64_t       total_swap;
    uint64_t       free_swap;
    uint16_t       num_procs;
    uint16_t       _pad;
    uint32_t       configured_cpus;
    uint32_t       online_cpus;
    uint32_t       current_cpu;
    uint32_t       page_size;
    uint64_t       uptime_ns;
} a20_system_info_t;

/* ---- Cluster structures (docs/cluster/01-abi.md) ----
 *
 * Frozen. Append-only: new fields may be added at the tail, gated by the
 * size/version header every args struct carries, never inserted into the
 * middle. */

/* 128-bit cluster-wide unique node identity. All-zero is A20_NODE_ID_LOCAL
 * (this machine; addressing (LOCAL, slot) must take the local fast path and is
 * never serialized), all-0xff is A20_NODE_ID_BROADCAST (SERVER tier, SEND
 * frames only). */
typedef struct a20_node_id {
    uint8_t bytes[16];
} a20_node_id_t;

/* cluster_set_self capability bits. */
#define A20_CLUSTER_CAP_RELAY     (1u << 0)  /* forwards frames for other nodes */
#define A20_CLUSTER_CAP_RELIABLE  (1u << 1)  /* ACK / retransmit / reassembly */
#define A20_CLUSTER_CAP_LEAF      (1u << 2)  /* answers only, never dials out */
#define A20_CLUSTER_CAPS_ALL      (A20_CLUSTER_CAP_RELAY | \
                                   A20_CLUSTER_CAP_RELIABLE | \
                                   A20_CLUSTER_CAP_LEAF)

/* cluster_export flags. */
#define A20_EXPORT_REPLACE      (1u << 0)
#define A20_EXPORT_LOCAL_ONLY   (1u << 1)
#define A20_EXPORT_FLAGS_ALL    (A20_EXPORT_REPLACE | A20_EXPORT_LOCAL_ONLY)

/* cluster_connect flags. */
#define A20_CONNECT_RELIABLE    (1u << 0)
#define A20_CONNECT_FLAGS_ALL   (A20_CONNECT_RELIABLE)

/* cluster_route op. */
#define A20_ROUTE_ADD       0u
#define A20_ROUTE_DEL       1u
#define A20_ROUTE_REPLACE   2u

/* cluster_event_subscribe mask. */
#define A20_CLX_EVENT_LINK_UP        (1u << 0)
#define A20_CLX_EVENT_LINK_DOWN      (1u << 1)
#define A20_CLX_EVENT_ROUTE_LOST     (1u << 2)
#define A20_CLX_EVENT_EXPORT_DROPPED (1u << 3)
#define A20_CLX_EVENT_MASK_ALL       0xFu

/* Link state reported by cluster_link_status. */
#define A20_CLX_LINK_DOWN     0u
#define A20_CLX_LINK_SUSPECT  1u
#define A20_CLX_LINK_UP       2u

/* transport_id values owned by the cluster subsystem. */
#define A20_CLX_TRANSPORT_LOOPBACK 0u
#define A20_CLX_TRANSPORT_UDP      1u
#define A20_CLX_TRANSPORT_UART     2u

#define A20_CLUSTER_SERVICE_NAME_MAX 64

/* Wire identifiers for the six syscalls live in syscall_nr.h at 0x0520+. */

typedef struct a20_cluster_set_self_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       caps;          /* A20_CLUSTER_CAP_* */
    uint32_t       _pad;
    a20_node_id_t  node_id;
    uint64_t       reserved[2];   /* must be zero */
} a20_cluster_set_self_args_t;

typedef struct a20_cluster_export_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;         /* A20_EXPORT_* */
    uint32_t       _pad;
    a20_handle_t   channel;       /* A20_OBJ_CHANNEL_ENDPOINT, needs R */
    const char    *service_name;  /* UTF-8, need not be NUL-terminated */
    uint32_t       name_len;      /* <= A20_CLUSTER_SERVICE_NAME_MAX */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_export_args_t;

typedef struct a20_cluster_connect_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       flags;         /* A20_CONNECT_* */
    uint32_t       slot;
    a20_node_id_t  node_id;       /* LOCAL means this machine */
    const char    *service_name;  /* optional; takes priority over slot */
    uint32_t       name_len;
    uint32_t       timeout_ms;    /* 0 = 5000 default */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_connect_args_t;

typedef struct a20_cluster_route_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       op;            /* A20_ROUTE_* */
    uint32_t       transport_id;  /* 0=loopback 1=udp 2=uart */
    a20_node_id_t  node_id;
    uint8_t        next_hop[16];  /* interpretation is per-transport */
    uint32_t       next_hop_len;
    uint32_t       metric;        /* lowest wins for one destination */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_route_args_t;

typedef struct a20_cluster_event_subscribe_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       mask;          /* A20_CLX_EVENT_* */
    uint32_t       reserved;      /* must be zero */
} a20_cluster_event_subscribe_args_t;

typedef struct a20_cluster_link_status_args {
    uint32_t       size;
    uint32_t       version;
    uint32_t       reserved;
    uint32_t       _pad;
    a20_node_id_t  node_id;       /* LOCAL aggregates every link */
    /* out */
    uint32_t       state;         /* A20_CLX_LINK_* */
    uint32_t       rtt_us;        /* sliding average; always 0 on MCU tier */
    uint64_t       tx_frames;
    uint64_t       rx_frames;
    uint64_t       tx_drops;
    uint64_t       rx_drops;
    uint64_t       retransmits;
    uint64_t       last_hello_age_ms;
} a20_cluster_link_status_args_t;

#endif /* _ABI_NATIVE_TYPES_H */
