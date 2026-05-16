/*
 * A20OS Native ABI — User-visible type definitions.
 *
 * This header defines all fundamental types for the Native ABI.
 * Design reference: docs/native-abi/01-types.md
 */
#ifndef _ABI_NATIVE_TYPES_H
#define _ABI_NATIVE_TYPES_H

#include <stdint.h>

/* ---- Fundamental types ---- */

typedef uint32_t a20_handle_t;     /* Process-local handle index */
typedef uint64_t a20_rights_t;     /* 14-bit capability rights bitmask */
typedef uint64_t a20_flags_t;      /* Operation flag bitmask */
typedef int64_t  a20_status_t;     /* Return status: >= 0 success, < 0 error */
typedef uint64_t a20_time_ns_t;    /* Nanosecond timestamp */
typedef uint64_t a20_off_t;        /* File offset */
typedef uint64_t a20_size_t;       /* Size */
typedef uint64_t a20_vaddr_t;      /* Virtual address */

#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

/* ---- ABI header convention ---- */

typedef struct a20_abi_header {
    uint32_t size;
    uint32_t version;
} a20_abi_header_t;

/* ---- Object types ---- */

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

/* ---- Temporal capability flags ---- */

#define A20_TEMPORAL_EXPIRY_ABSOLUTE  (1u << 0)  /* Use absolute expiry tick */
#define A20_TEMPORAL_OP_COUNT         (1u << 1)  /* Use operation count limit */
#define A20_TEMPORAL_AUTO_CLOSE       (1u << 2)  /* Auto-close on expiry */

/* ---- Handle states (docs/native-abi/03-handle.md §3.1) ---- */

typedef enum a20_handle_state {
    A20_HS_FREE      = 0,
    A20_HS_ACTIVE    = 1,
    A20_HS_EXPIRED   = 2,
    A20_HS_CLOSING   = 3,
} a20_handle_state_t;

/* ---- Handle entry (kernel-internal, exposed for struct definition) ---- */

typedef struct a20_handle_entry {
    void           *object;        /* Pointer to kernel object */
    uint16_t        type;          /* a20_object_type_t */
    uint16_t        _pad;
    a20_rights_t    rights;        /* Declared rights bitmask */
    uint64_t        expiry_tick;   /* Absolute expiry (kernel ticks), 0 = none */
    uint32_t        remaining_ops; /* Remaining ops, 0 = unlimited */
    uint32_t        temporal_flags;/* A20_TEMPORAL_* flags */
    uint8_t         security_label;/* L=0, M=1, H=2 (Bell-LaPadula) */
    uint8_t         state;         /* a20_handle_state_t */
    uint8_t         _pad2[6];
} a20_handle_entry_t;

/* ---- Handle table (kernel-internal) ---- */

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

typedef struct a20_readdir_args {
    uint32_t       size;
    uint32_t       version;
    uint64_t       buf;
    uint64_t       buf_len;
    uint64_t       cookie;
    uint64_t       out_len;
} a20_readdir_args_t;

typedef struct a20_fs_stat {
    uint64_t       block_size;
    uint64_t       total_blocks;
    uint64_t       free_blocks;
    uint64_t       available_blocks;
    uint64_t       total_files;
    uint64_t       free_files;
    uint64_t       fs_id;
} a20_fs_stat_t;

typedef struct a20_fs_mount_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   mount_point;
    uint64_t       source;
    uint32_t       source_len;
    uint64_t       fs_type;
    uint32_t       fs_type_len;
    uint32_t       flags;
} a20_fs_mount_args_t;

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
} a20_event_watch_args_t;

typedef struct a20_pending_event {
    a20_handle_t   source;
    uint32_t       type;
    uint64_t       events;
    uint64_t       user_data;
    uint64_t       data0, data1, data2;
} a20_pending_event_t;

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
    uint32_t       _pad3;
    uint64_t       out_data_len;
    uint32_t       out_handle_count;
    uint64_t       out_rights_buf;
} a20_msg_recv_args_t;

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
} a20_system_info_t;

#endif /* _ABI_NATIVE_TYPES_H */
