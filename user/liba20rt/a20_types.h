/*
 * A20OS Native SDK — Types and constants.
 *
 * User-side copy of the kernel ABI type definitions.
 * Struct layouts MUST match kernel/include/abi/native/*.h exactly.
 *
 * Copied from:
 *   kernel/include/abi/native/types.h
 *   kernel/include/abi/native/rights.h
 *   kernel/include/abi/native/errno.h
 *   kernel/include/abi/native/startup.h
 *   kernel/include/abi/native/resource.h
 */
#ifndef _A20_TYPES_H
#define _A20_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Status codes (kernel/include/abi/native/errno.h)
 *
 * Error constants are positive values in the ABI.  Syscalls return them
 * as negative numbers in a20_status_t.  Use A20_IS_ERROR() to test.
 * ======================================================================== */

typedef int64_t a20_status_t;

#define A20_OK                       ((a20_status_t)0)
#define A20_ERR_PERM                 ((a20_status_t)1)
#define A20_ERR_NO_ENTRY             ((a20_status_t)2)
#define A20_ERR_INTERRUPTED          ((a20_status_t)3)
#define A20_ERR_IO                   ((a20_status_t)4)
#define A20_ERR_BAD_HANDLE           ((a20_status_t)5)
#define A20_ERR_NO_MEMORY            ((a20_status_t)6)
#define A20_ERR_ACCESS               ((a20_status_t)7)
#define A20_ERR_FAULT                ((a20_status_t)8)
#define A20_ERR_BUSY                 ((a20_status_t)9)
#define A20_ERR_EXISTS               ((a20_status_t)10)
#define A20_ERR_NOT_SUPPORTED        ((a20_status_t)11)
#define A20_ERR_INVALID_ARGUMENT     ((a20_status_t)12)
#define A20_ERR_NO_SPACE             ((a20_status_t)13)
#define A20_ERR_NOT_DIR              ((a20_status_t)14)
#define A20_ERR_IS_DIR               ((a20_status_t)15)
#define A20_ERR_NOT_EMPTY            ((a20_status_t)16)
#define A20_ERR_NAME_TOO_LONG        ((a20_status_t)17)
#define A20_ERR_WOULD_BLOCK          ((a20_status_t)18)
#define A20_ERR_TIMED_OUT            ((a20_status_t)19)
#define A20_ERR_CANCELED             ((a20_status_t)20)
#define A20_ERR_PROTOCOL             ((a20_status_t)21)
#define A20_ERR_RANGE                ((a20_status_t)22)
#define A20_ERR_TYPE_MISMATCH        ((a20_status_t)23)
#define A20_ERR_NOT_FOUND            ((a20_status_t)24)
#define A20_ERR_EXPIRED              ((a20_status_t)25)

#define A20_IS_ERROR(status)   ((a20_status_t)(status) < 0)
#define A20_ABS_ERROR(status)  (-(a20_status_t)(status))

static inline int a20_status_is_ok(a20_status_t s) { return s >= 0; }
static inline int a20_status_is_err(a20_status_t s) { return s < 0; }

/* ========================================================================
 * Fundamental types (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef uint32_t a20_handle_t;     /* Process-local handle index */
typedef uint64_t a20_rights_t;     /* 14-bit capability rights bitmask */
typedef uint64_t a20_flags_t;      /* Operation flag bitmask */
typedef uint64_t a20_time_ns_t;    /* Nanosecond timestamp */
typedef uint64_t a20_off_t;        /* File offset */
typedef uint64_t a20_size_t;       /* Size */
typedef uint64_t a20_vaddr_t;      /* Virtual address */

#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

/* ========================================================================
 * Capability rights (kernel/include/abi/native/rights.h)
 * ======================================================================== */

#define A20_RIGHT_READ       (1ull << 0)
#define A20_RIGHT_WRITE      (1ull << 1)
#define A20_RIGHT_EXEC       (1ull << 2)
#define A20_RIGHT_STAT       (1ull << 3)
#define A20_RIGHT_SEEK       (1ull << 4)
#define A20_RIGHT_DUP        (1ull << 5)
#define A20_RIGHT_TRANSFER   (1ull << 6)
#define A20_RIGHT_MAP        (1ull << 7)
#define A20_RIGHT_WAIT       (1ull << 8)
#define A20_RIGHT_CONNECT    (1ull << 9)
#define A20_RIGHT_ACCEPT     (1ull << 10)
#define A20_RIGHT_CONTROL    (1ull << 11)
#define A20_RIGHT_ADMIN      (1ull << 12)
#define A20_RIGHT_SIGNAL     (1ull << 13)

#define A20_RIGHTS_ALL  (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_EXEC | \
                         A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP | \
                         A20_RIGHT_TRANSFER | A20_RIGHT_MAP | A20_RIGHT_WAIT | \
                         A20_RIGHT_CONNECT | A20_RIGHT_ACCEPT | A20_RIGHT_CONTROL | \
                         A20_RIGHT_ADMIN | A20_RIGHT_SIGNAL)

#define A20_RIGHTS_NONE  ((a20_rights_t)0)

/* handle_control operations */
#define A20_HANDLE_CTRL_CHDIR 5u

/* ========================================================================
 * ABI header convention
 * ======================================================================== */

typedef struct a20_abi_header {
    uint32_t size;
    uint32_t version;
} a20_abi_header_t;

/* ========================================================================
 * Object types (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef enum a20_object_type {
    A20_OBJ_INVALID          = 0,
    A20_OBJ_TASK             = 1,
    A20_OBJ_THREAD           = 2,
    A20_OBJ_FILE             = 3,
    A20_OBJ_DIRECTORY        = 4,
    A20_OBJ_SOCKET           = 5,
    A20_OBJ_PIPE_ENDPOINT    = 6,
    A20_OBJ_CHANNEL_ENDPOINT = 7,
    A20_OBJ_EVENT_QUEUE      = 8,
    A20_OBJ_TIMER            = 9,
    A20_OBJ_MEMORY           = 10,  /* Shared memory (shm) */
    A20_OBJ_DEVICE           = 11,
    A20_OBJ_NAMESPACE        = 12,
    A20_OBJ_DEBUG            = 13,
} a20_object_type_t;

/* Legacy alias used by earlier SDK revisions. */
typedef a20_object_type_t a20_obj_type_t;

/* ========================================================================
 * Temporal capability flags (kernel/include/abi/native/types.h)
 * ======================================================================== */

#define A20_TEMPORAL_EXPIRY_ABSOLUTE  (1u << 0)  /* Use absolute expiry tick */
#define A20_TEMPORAL_OP_COUNT         (1u << 1)  /* Use operation count limit */
#define A20_TEMPORAL_AUTO_CLOSE       (1u << 2)  /* Auto-close on expiry */

/* ========================================================================
 * Handle states (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef enum a20_handle_state {
    A20_HS_FREE      = 0,
    A20_HS_ACTIVE    = 1,
    A20_HS_EXPIRED   = 2,
    A20_HS_CLOSING   = 3,
} a20_handle_state_t;

/* ========================================================================
 * Handle entry / table (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef struct a20_handle_entry {
    void           *object;        /* Pointer to kernel object */
    uint16_t        type;          /* a20_object_type_t */
    uint16_t        _pad;
    a20_rights_t    rights;        /* Declared rights bitmask */
    uint64_t        expiry_tick;   /* Absolute expiry (kernel ticks), 0 = none */
    uint32_t        remaining_ops; /* Remaining ops; with A20_TEMPORAL_OP_COUNT
                                    * set, 0 = exhausted.  Flag clear = ignored
                                    * (unlimited). */
    uint32_t        temporal_flags;/* A20_TEMPORAL_* flags */
    uint8_t         security_label;/* L=0, M=1, H=2 (Bell-LaPadula) */
    uint8_t         state;         /* a20_handle_state_t */
    uint8_t         _pad2[6];
} a20_handle_entry_t;

#define A20_HT_INITIAL_CAP    256
#define A20_HT_MAX_CAP        65536
#define A20_HT_GROWTH_FACTOR  2

typedef struct a20_handle_table {
    a20_handle_entry_t *entries;
    uint32_t            capacity;
    uint32_t            count;
    uint32_t            free_hint;
    /* lock omitted here — included in kernel-internal header */
    uint64_t           *free_bitmap;
    uint32_t            bitmap_size;
} a20_handle_table_t;

/* ========================================================================
 * ABI info (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Startup info (kernel/include/abi/native/startup.h)
 * ======================================================================== */

typedef struct a20_start_info {
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
} a20_start_info_t;

#define A20_NATIVE_FD_HANDLE_BASE 64u

/* ========================================================================
 * Handle operation structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ---- handle_control commands ---- */

#define A20_HANDLE_CTRL_IOCTL          0u
#define A20_HANDLE_CTRL_FCNTL          1u
#define A20_HANDLE_CTRL_SET_TEMPORAL   2u  /* arg0 = a20_handle_temporal_args_t*  */
#define A20_HANDLE_CTRL_GET_TEMPORAL   3u  /* arg0 = a20_handle_temporal_args_t*  */
#define A20_HANDLE_CTRL_SET_LABEL      4u  /* arg0 = new label (raise-only)       */

/* Temporal capability control.  SET is strengthening-only
 * (non-refreshability, docs/native-abi/06-security.md §6.4). */
typedef struct a20_handle_temporal_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       expiry_ns;      /* absolute CLOCK_MONOTONIC ns; 0 = no expiry */
    uint32_t       remaining_ops;  /* operation budget when OP_COUNT flag set   */
    uint32_t       temporal_flags; /* A20_TEMPORAL_*                            */
} a20_handle_temporal_args_t;

/* ========================================================================
 * I/O structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Set meta flags / args (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Transfer (splice) (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Spawn handle (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef struct a20_spawn_handle {
    a20_handle_t   handle;
    a20_rights_t   rights;
    uint32_t       target_slot;
    uint32_t       flags;
} a20_spawn_handle_t;

/* ========================================================================
 * Task / Thread structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef struct a20_task_spawn_args {
    uint32_t       size;
    uint32_t       version;
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
    /* ---- version 2: child start_info stdio handles (NULL = not inherited) ---- */
    a20_handle_t   stdin_handle;
    a20_handle_t   stdout_handle;
    a20_handle_t   stderr_handle;
    uint32_t       reserved;
} a20_task_spawn_args_t;

typedef struct a20_task_status {
    uint32_t       size;
    uint32_t       version;
    int32_t        exit_code;
    uint32_t       exit_reason;
    uint64_t       utime_ns;
    uint64_t       stime_ns;
} a20_task_status_t;

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

/* ========================================================================
 * Memory structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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
} a20_vm_map_args_t;

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

/* ========================================================================
 * Path / Filesystem structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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
    uint32_t       flags;
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

/* ========================================================================
 * IPC / Event structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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
} a20_event_watch_args_t;

typedef struct a20_pending_event {
    a20_handle_t   source;
    uint32_t       type;
    uint64_t       events;
    uint64_t       user_data;
    uint64_t       data0, data1, data2;
} a20_pending_event_t;

/* ---- Observable event types (docs/native-abi/05-ipc.md §3.3) ---- */

#define A20_EVENT_READABLE        0u
#define A20_EVENT_WRITABLE        1u
#define A20_EVENT_ERROR           2u
#define A20_EVENT_CLOSED          3u
#define A20_EVENT_CONNECTION      4u
#define A20_EVENT_ACCEPT_READY    5u
#define A20_EVENT_EXPIRED         6u
#define A20_EVENT_EXITED          7u
#define A20_EVENT_MESSAGE_READY   8u
#define A20_EVENT_PEER_CLOSED     9u

#define A20_EVENT_MASK(ev)        (1ull << (ev))

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

/* ========================================================================
 * Channel structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

#define A20_CH_MAX_DATA    65536
#define A20_CH_MAX_HANDLES 8

typedef struct a20_channel_type {
    uint32_t version;
    uint32_t send_handle_types;
    uint32_t recv_handle_types;
    uint32_t max_data_size;
    uint32_t max_handles;
    uint32_t flags;
} a20_channel_type_t;

#define A20_CHAN_TYPE_ORDERED (1u << 0)
#define A20_CHAN_TYPE_STRICT  (1u << 1)

/* Channel type bit definitions (aligned with a20_object_type_t) */
#define A20_CHAN_TYPE_FILE     (1u << A20_OBJ_FILE)
#define A20_CHAN_TYPE_SOCKET   (1u << A20_OBJ_SOCKET)
#define A20_CHAN_TYPE_CHANNEL  (1u << A20_OBJ_CHANNEL_ENDPOINT)
#define A20_CHAN_TYPE_PIPE     (1u << A20_OBJ_PIPE_ENDPOINT)
#define A20_CHAN_TYPE_EVENTQ   (1u << A20_OBJ_EVENT_QUEUE)
#define A20_CHAN_TYPE_TIMER    (1u << A20_OBJ_TIMER)
#define A20_CHAN_TYPE_SHM      (1u << A20_OBJ_MEMORY)
#define A20_CHAN_TYPE_TASK     (1u << A20_OBJ_TASK)
#define A20_CHAN_TYPE_NS       (1u << A20_OBJ_NAMESPACE)
#define A20_CHAN_TYPE_ANY      0xFFFFFFFF

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

/* Fused RPC args (mirror of kernel/include/abi/native/types.h). */
typedef struct a20_channel_call_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   channel;
    uint32_t       flags;           /* A20_MSG_* */
    uint64_t       data;
    uint32_t       data_len;
    uint32_t       _pad;
    uint64_t       handles;         /* a20_handle_t[] */
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

/* ---- Message flags (channel_send / channel_recv) ---- */

#define A20_MSG_NONBLOCK   (1u << 0)  /* fail with WOULD_BLOCK instead of sleeping */

/* ========================================================================
 * Network structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Timer structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef struct a20_timer_create_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   event_queue;
    uint64_t       user_data;
    uint32_t       flags;
    a20_handle_t   out_timer;
} a20_timer_create_args_t;

/* ========================================================================
 * Security structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Debug structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

typedef struct a20_regs {
    uint64_t       regs[32];
    uint64_t       pc;
    uint64_t       sp;
    uint64_t       sr;
} a20_regs_t;

/* ========================================================================
 * Sync structures (kernel/include/abi/native/types.h)
 * ======================================================================== */

#define A20_TIMEOUT_INFINITE  ((uint64_t)-1)

typedef struct a20_futex_wait_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr;
    uint32_t       expected;
    uint32_t       flags;
    uint64_t       timeout_ns;
} a20_futex_wait_args_t;

typedef struct a20_futex_wake_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr;
    uint32_t       count;
    uint32_t       flags;
    uint32_t       out_woken;
    uint32_t       reserved;
} a20_futex_wake_args_t;

/* ========================================================================
 * System info (kernel/include/abi/native/types.h)
 * ======================================================================== */

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

/* ========================================================================
 * Resource limits (kernel/include/abi/native/resource.h)
 * ======================================================================== */

typedef struct a20_resource_limits {
    uint32_t max_handles;          /* max handle table entries (default 4096) */
    uint32_t max_channels;         /* max active channel endpoints (default 256) */
    uint32_t max_event_queues;     /* max active event queues (default 64) */
    uint32_t max_channel_depth;    /* max messages per channel endpoint (default 1024) */
    uint32_t max_event_capacity;   /* max events in ring buffer (default 256) */
    uint32_t max_vmo_count;        /* max VMOs per task (default 512) */
    uint64_t max_memory_bytes;     /* max virtual memory per task (default 1GB) */
    uint32_t max_threads;          /* max threads per task (default 128) */
    uint32_t max_pending_ops;      /* max pending async operations (default 64) */
} a20_resource_limits_t;

/* Default limits */
#define A20_LIMIT_HANDLES_DEFAULT         4096
#define A20_LIMIT_CHANNELS_DEFAULT         256
#define A20_LIMIT_EVENT_QUEUES_DEFAULT      64
#define A20_LIMIT_CHANNEL_DEPTH_DEFAULT   1024
#define A20_LIMIT_EVENT_CAPACITY_DEFAULT   256
#define A20_LIMIT_VMO_COUNT_DEFAULT        512
#define A20_LIMIT_MEMORY_DEFAULT    (1ULL << 30)  /* 1 GB */
#define A20_LIMIT_THREADS_DEFAULT            128
#define A20_LIMIT_PENDING_OPS_DEFAULT         64

/* Absolute maximums (hard caps, not configurable) */
#define A20_LIMIT_HANDLES_ABSOLUTE         65536
#define A20_LIMIT_CHANNELS_ABSOLUTE         4096
#define A20_LIMIT_EVENT_QUEUES_ABSOLUTE     1024
#define A20_LIMIT_CHANNEL_DEPTH_ABSOLUTE     8192
#define A20_LIMIT_EVENT_CAPACITY_ABSOLUTE    4096
#define A20_LIMIT_VMO_COUNT_ABSOLUTE         8192
#define A20_LIMIT_MEMORY_ABSOLUTE     (4ULL << 30)  /* 4 GB */
#define A20_LIMIT_THREADS_ABSOLUTE          4096
#define A20_LIMIT_PENDING_OPS_ABSOLUTE       512

/* Cascading depth limit (docs/native-abi/03-handle.md §3.3) */
#define A20_CASCADE_DEPTH_MAX              2

static inline void a20_resource_limits_init_default(a20_resource_limits_t *l)
{
    l->max_handles        = A20_LIMIT_HANDLES_DEFAULT;
    l->max_channels       = A20_LIMIT_CHANNELS_DEFAULT;
    l->max_event_queues   = A20_LIMIT_EVENT_QUEUES_DEFAULT;
    l->max_channel_depth  = A20_LIMIT_CHANNEL_DEPTH_DEFAULT;
    l->max_event_capacity = A20_LIMIT_EVENT_CAPACITY_DEFAULT;
    l->max_vmo_count      = A20_LIMIT_VMO_COUNT_DEFAULT;
    l->max_memory_bytes   = A20_LIMIT_MEMORY_DEFAULT;
    l->max_threads        = A20_LIMIT_THREADS_DEFAULT;
    l->max_pending_ops    = A20_LIMIT_PENDING_OPS_DEFAULT;
}

static inline int a20_limit_handles(uint32_t count, uint32_t limit)
{
    return count >= limit ? -1 : 0;
}

static inline int a20_limit_channel_depth(uint32_t depth, uint32_t limit)
{
    return depth > limit ? -1 : 0;
}

static inline int a20_limit_event_capacity(uint32_t cap, uint32_t limit)
{
    return cap > limit ? -1 : 0;
}

static inline int a20_limit_memory(uint64_t current, uint64_t requested, uint64_t limit)
{
    if (limit == 0) return 0;
    return (current + requested) > limit ? -1 : 0;
}

/* ========================================================================
 * Userspace convenience constants (not part of the kernel ABI numbers)
 * ======================================================================== */

#define A20_SEEK_START   0
#define A20_SEEK_CURRENT 1
#define A20_SEEK_END     2

/* High-level time value used by the SDK clock/timer helpers. */
typedef struct {
    uint64_t secs;
    uint64_t nsecs;
} a20_time_t;

#ifdef __cplusplus
}
#endif

#endif /* _A20_TYPES_H */
