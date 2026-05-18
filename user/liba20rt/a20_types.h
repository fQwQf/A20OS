/*
 * A20OS Native SDK — Types and constants.
 *
 * Fundamental types for the Handle-based object system.
 * Struct layouts MUST match kernel/include/abi/native/types.h exactly.
 */
#ifndef _A20_TYPES_H
#define _A20_TYPES_H

#include <stdint.h>

/* ---- Status codes ---- */
typedef int64_t a20_status_t;

#define A20_OK              ((a20_status_t)0)
#define A20_ERR_INVALID     ((a20_status_t)-1)
#define A20_ERR_NOT_FOUND   ((a20_status_t)-2)
#define A20_ERR_ACCESS      ((a20_status_t)-3)
#define A20_ERR_BUSY        ((a20_status_t)-4)
#define A20_ERR_NO_MEMORY   ((a20_status_t)-5)
#define A20_ERR_EXISTS      ((a20_status_t)-6)
#define A20_ERR_NOT_DIR     ((a20_status_t)-7)
#define A20_ERR_IS_DIR      ((a20_status_t)-8)
#define A20_ERR_NOT_SUPPORTED ((a20_status_t)-9)
#define A20_ERR_TIMEOUT     ((a20_status_t)-10)
#define A20_ERR_CANCELLED   ((a20_status_t)-11)
#define A20_ERR_PEER_CLOSED ((a20_status_t)-12)
#define A20_ERR_OUT_OF_RANGE ((a20_status_t)-13)
#define A20_ERR_IO          ((a20_status_t)-14)
#define A20_ERR_BAD_HANDLE  ((a20_status_t)-15)
#define A20_ERR_FAULT       ((a20_status_t)-16)
#define A20_ERR_INVALID_ARGUMENT ((a20_status_t)-17)

static inline int a20_status_is_ok(a20_status_t s) { return s >= 0; }
static inline int a20_status_is_err(a20_status_t s) { return s < 0; }

/* ---- Fundamental types ---- */
typedef uint32_t a20_handle_t;
typedef uint64_t a20_rights_t;     /* Must match kernel: uint64_t */
typedef uint64_t a20_flags_t;
typedef uint64_t a20_time_ns_t;
typedef uint64_t a20_off_t;
typedef uint64_t a20_size_t;
typedef uint64_t a20_vaddr_t;

#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

/* ---- Handle constants ---- */
#define A20_HANDLE_INVALID  ((a20_handle_t)0)

/* ---- Rights flags (64-bit, matches kernel) ---- */
#define A20_RIGHT_READ      ((a20_rights_t)(1ULL << 0))
#define A20_RIGHT_WRITE     ((a20_rights_t)(1ULL << 1))
#define A20_RIGHT_EXEC      ((a20_rights_t)(1ULL << 2))
#define A20_RIGHT_DUP       ((a20_rights_t)(1ULL << 3))
#define A20_RIGHT_TRANSFER  ((a20_rights_t)(1ULL << 4))
#define A20_RIGHT_STAT      ((a20_rights_t)(1ULL << 5))
#define A20_RIGHT_SEEK      ((a20_rights_t)(1ULL << 6))
#define A20_RIGHT_DESTROY   ((a20_rights_t)(1ULL << 7))
#define A20_RIGHT_CONTROL   ((a20_rights_t)(1ULL << 8))
#define A20_RIGHT_WAIT      ((a20_rights_t)(1ULL << 9))
#define A20_RIGHT_SIGNAL    ((a20_rights_t)(1ULL << 10))
#define A20_RIGHT_ALL       ((a20_rights_t)0x7FFFULL)

/* ---- Object types (matches kernel a20_object_type_t) ---- */
typedef uint32_t a20_obj_type_t;

#define A20_OBJ_INVALID          0
#define A20_OBJ_TASK             1
#define A20_OBJ_THREAD           2
#define A20_OBJ_FILE             3
#define A20_OBJ_DIRECTORY        4
#define A20_OBJ_SOCKET           5
#define A20_OBJ_PIPE_ENDPOINT    6
#define A20_OBJ_CHANNEL_ENDPOINT 7
#define A20_OBJ_EVENT_QUEUE      8
#define A20_OBJ_TIMER            9
#define A20_OBJ_MEMORY           10
#define A20_OBJ_DEVICE           11
#define A20_OBJ_NAMESPACE        12
#define A20_OBJ_DEBUG            13

/* ---- I/O vector (matches kernel exactly) ---- */
typedef struct a20_iovec {
    uint64_t base;
    uint64_t len;
} a20_iovec_t;

/* ---- I/O args (matches kernel a20_io_args_t exactly) ---- */
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

/* ---- Handle info (matches kernel a20_handle_info_t exactly) ---- */
typedef struct a20_handle_info {
    uint32_t       size;
    uint32_t       version;
    uint32_t       object_type;
    uint32_t       state;
    a20_rights_t   rights;
    uint64_t       object_id_hint;
    uint64_t       flags;
} a20_handle_info_t;

/* ---- Handle dup args (matches kernel exactly) ---- */
typedef struct a20_handle_dup_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   source;
    uint32_t       flags;
    a20_rights_t   rights_mask;
    a20_handle_t   out_handle;
    uint32_t       reserved;
} a20_handle_dup_args_t;

/* ---- Handle transfer args (matches kernel exactly) ---- */
typedef struct a20_handle_transfer_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   handle;
    a20_rights_t   new_rights;
    a20_handle_t   dest_process;
    uint32_t       reserved;
} a20_handle_transfer_args_t;

/* ---- File stat (matches kernel a20_stat_t exactly) ---- */
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

/* ---- Path open args (matches kernel exactly) ---- */
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

/* ---- VM alloc args (matches kernel exactly) ---- */
typedef struct a20_vm_alloc_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       addr_hint;
    uint64_t       length;
    uint32_t       prot;
    uint32_t       flags;
    uint64_t       out_addr;
} a20_vm_alloc_args_t;

/* ---- Task status (matches kernel exactly) ---- */
typedef struct a20_task_status {
    uint32_t       size;
    uint32_t       version;
    int32_t        exit_code;
    uint32_t       exit_reason;
    uint64_t       utime_ns;
    uint64_t       stime_ns;
} a20_task_status_t;

/* ---- Task spawn args (matches kernel exactly) ---- */
typedef struct a20_spawn_handle {
    a20_handle_t   handle;
    a20_rights_t   rights;
    uint32_t       target_slot;
    uint32_t       flags;
} a20_spawn_handle_t;

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
} a20_task_spawn_args_t;

/* ---- Task info (matches kernel exactly) ---- */
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

/* ---- Thread create args (matches kernel exactly) ---- */
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

/* ---- FS stat (matches kernel exactly) ---- */
typedef struct a20_fs_stat {
    uint64_t       block_size;
    uint64_t       total_blocks;
    uint64_t       free_blocks;
    uint64_t       available_blocks;
    uint64_t       total_files;
    uint64_t       free_files;
    uint64_t       fs_id;
} a20_fs_stat_t;

/* ---- Seek whence ---- */
#define A20_SEEK_START   0
#define A20_SEEK_CURRENT 1
#define A20_SEEK_END     2

/* ---- Protection bits ---- */
#define A20_PROT_READ    (1u << 0)
#define A20_PROT_WRITE   (1u << 1)
#define A20_PROT_EXEC    (1u << 2)
#define A20_PROT_NONE    0

/* Backward compat */
#define A20_VM_PAGE_READ    A20_PROT_READ
#define A20_VM_PAGE_WRITE   A20_PROT_WRITE
#define A20_VM_PAGE_EXEC    A20_PROT_EXEC

/* ---- Time ---- */
typedef struct {
    uint64_t secs;
    uint64_t nsecs;
} a20_time_t;

/* ---- Null-terminated — used to indicate "no data" ---- */
#define A20_NULL 0

#endif
