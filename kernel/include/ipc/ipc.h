/*
 * A20OS core — internal IPC subsystem and object model.
 *
 * Ownership principle (docs/OS-Design.md): the internal kernel
 * implementation (kernel/ipc, kernel/proc, kernel/mm, kernel/drivers)
 * is self-contained and exposes its API here; the ABI layers
 * (kernel/abi/native, kernel/abi/linux) are thin wrappers that call into
 * it.  This header must NOT include anything under abi/.
 */
#ifndef _IPC_IPC_H
#define _IPC_IPC_H

#include <stdint.h>
#include "core/types.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/sync.h"

/* ---- Object model (docs/native-abi/03-handle.md) ---- */

typedef uint32_t a20_handle_t;   /* process-local handle index */
typedef uint64_t a20_rights_t;   /* capability rights bitmask */

#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

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
    A20_OBJ_MEMORY           = 10,  /* shared memory (shm) */
    A20_OBJ_DEVICE           = 11,
    A20_OBJ_NAMESPACE        = 12,
    A20_OBJ_DEBUG            = 13,
} a20_object_type_t;

/* 14 capability rights bits (docs/native-abi/06-security.md §1) */
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

#define A20_RIGHTS_NONE ((a20_rights_t)0)

/* ---- Internal status codes (docs/native-abi/02-errors.md) ---- */

#define A20_OK                       0
#define A20_ERR_PERM                 1
#define A20_ERR_NO_ENTRY             2
#define A20_ERR_INTERRUPTED          3
#define A20_ERR_IO                   4
#define A20_ERR_BAD_HANDLE           5
#define A20_ERR_NO_MEMORY            6
#define A20_ERR_ACCESS               7
#define A20_ERR_FAULT                8
#define A20_ERR_BUSY                 9
#define A20_ERR_EXISTS               10
#define A20_ERR_NOT_SUPPORTED        11
#define A20_ERR_INVALID_ARGUMENT     12
#define A20_ERR_NO_SPACE             13
#define A20_ERR_NOT_DIR              14
#define A20_ERR_IS_DIR               15
#define A20_ERR_NOT_EMPTY            16
#define A20_ERR_NAME_TOO_LONG        17
#define A20_ERR_WOULD_BLOCK          18
#define A20_ERR_TIMED_OUT            19
#define A20_ERR_CANCELED             20
#define A20_ERR_PROTOCOL             21
#define A20_ERR_NOT_FOUND            24
#define A20_ERR_RANGE                22
#define A20_ERR_TYPE_MISMATCH        23
#define A20_ERR_EXPIRED              25

/* ---- Observable event types (docs/native-abi/05-ipc.md §3.3) ---- */

#define A20_EVENT_READABLE        0u   /* file/socket/pipe: data readable   */
#define A20_EVENT_WRITABLE        1u   /* file/socket/pipe: space writable  */
#define A20_EVENT_ERROR           2u   /* file/socket: I/O error            */
#define A20_EVENT_CLOSED          3u   /* object closed                     */
#define A20_EVENT_CONNECTION      4u   /* socket: new connection arrived    */
#define A20_EVENT_ACCEPT_READY    5u   /* socket: accept would not block    */
#define A20_EVENT_EXPIRED         6u   /* timer: expiry fired               */
#define A20_EVENT_EXITED          7u   /* task/thread: exited               */
#define A20_EVENT_MESSAGE_READY   8u   /* channel: message available        */
#define A20_EVENT_PEER_CLOSED     9u   /* channel: peer endpoint closed     */
#define A20_EVENT_SIGNALED       10u   /* device: irq/signaled (udriver)    */

#define A20_EVENT_MASK(ev)        (1ull << (ev))

/* ---- Message flags ---- */

#define A20_MSG_NONBLOCK   (1u << 0)  /* fail with WOULD_BLOCK instead of sleeping */

#define A20_TIMEOUT_INFINITE  ((uint64_t)-1)

/* ---- Channel ---- */

#define A20_CH_MAX_DATA    65536
#define A20_CH_MAX_HANDLES 8
#define A20_CH_DEFAULT_CAP 64
#define A20_EVQ_DEFAULT_CAP 256

/* Wait-queue keys: receivers sleep waiting for messages, senders sleep
 * waiting for queue space (key 0 wakes either class). */
#define A20_CH_WAIT_RECV     1
#define A20_CH_WAIT_SEND     2

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

typedef struct a20_pending_event {
    a20_handle_t   source;
    uint32_t       type;
    uint64_t       events;
    uint64_t       user_data;
    uint64_t       data0, data1, data2;
} a20_pending_event_t;

/* Handle info carried inside a channel message; preserves temporal
 * constraints and the security label so transfer cannot refresh them. */
typedef struct a20_ch_handle_info {
    void           *object;
    uint16_t        type;
    uint16_t        _pad;
    a20_rights_t    transfer_rights;
    uint64_t        expiry_tick;
    uint32_t        remaining_ops;
    uint32_t        temporal_flags;
    uint8_t         security_label;
    uint8_t         _pad2[7];
} a20_ch_handle_info_t;

typedef struct a20_ch_message {
    uint32_t                data_len;
    uint32_t                handle_count;
    struct a20_ch_message  *next;
    uint8_t                 data[];
} a20_ch_message_t;

struct a20_ht_internal;

typedef struct a20_channel_ep {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;
    struct a20_channel_ep  *peer;
    int                     peer_closed;
    a20_channel_type_t     *chan_type;         /* NULL or &chan_type_storage */
    a20_channel_type_t      chan_type_storage; /* per-endpoint copy */

    a20_ch_message_t       *msg_head;
    a20_ch_message_t       *msg_tail;
    uint32_t                msg_count;
    uint32_t                msg_cap;
    uint32_t                total_data;
} a20_channel_ep_t;

typedef struct a20_watch_entry {
    a20_handle_t            target_handle;
    void                   *target_object;
    uint16_t                target_type;
    uint16_t                _pad;
    uint64_t                event_mask;
    uint64_t                user_data;
    struct a20_eventq      *owner_queue;
    struct a20_watch_entry *next;
} a20_watch_entry_t;

typedef struct a20_eventq {
    refcount_t              refcount;
    spinlock_t              lock;
    wait_queue_t            waiters;

    a20_watch_entry_t      *watches;
    uint32_t                watch_count;

    a20_pending_event_t    *ring;
    uint32_t                ring_cap;
    uint32_t                ring_head;
    uint32_t                ring_tail;
    uint32_t                ring_count;
} a20_eventq_t;

/* ---- Kernel objects carried by handles (native object model) ---- */

struct vmo;

typedef struct a20_socket {
    refcount_t  refcount;
    int         kern_fd;
} a20_socket_t;

typedef struct a20_shm {
    refcount_t  refcount;
    struct vmo *vmo;
    uint32_t    export_rights;
} a20_shm_t;

typedef struct a20_namespace {
    refcount_t  refcount;
    uint32_t    ns_type;
    uint32_t    flags;
    void       *isolated_data;
    char        root_path[256];
    uint32_t    net_ifindex;
    uint64_t    pid_offset;
    uint32_t    dev_access_mask;
} a20_namespace_t;

typedef struct a20_debug {
    refcount_t  refcount;
    struct task_struct *target;
    uint32_t    options;
} a20_debug_t;

/* ---- Object lifetime (type-aware ref/release) ---- */

int  a20_object_is_vfile_backed(uint16_t type);
void a20_object_ref(void *object, uint16_t type);
void a20_object_release(void *object, uint16_t type);
void a20_eventq_on_vfile_destroy(int fd);

/* ---- Channel API ---- */

a20_channel_ep_t *a20_channel_create(uint32_t msg_cap, const a20_channel_type_t *type);
int64_t a20_channel_send(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                         a20_ch_handle_info_t *handles, uint32_t handle_count,
                         struct a20_ht_internal *sender_ht, uint32_t flags);
int64_t a20_channel_send_dwc(a20_channel_ep_t *ep, const void *data, uint32_t data_len,
                             a20_ch_handle_info_t *handles, uint32_t handle_count,
                             struct a20_ht_internal *sender_ht, uint32_t flags,
                             int defer_wake);
int64_t a20_channel_recv(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                         a20_ch_handle_info_t *handles, uint32_t *handle_count,
                         struct a20_ht_internal *receiver_ht, uint32_t flags);
int64_t a20_channel_recv_begin(a20_channel_ep_t *ep, uint32_t flags,
                               uint32_t *out_msg_data_len,
                               uint32_t *out_msg_handles);
int64_t a20_channel_recv_begin_donate(a20_channel_ep_t *ep, uint32_t flags,
                                      uint32_t *out_msg_data_len,
                                      uint32_t *out_msg_handles);
int64_t a20_channel_recv_finish(a20_channel_ep_t *ep, void *data, uint32_t *data_len,
                                a20_ch_handle_info_t *handles, uint32_t *handle_count);
void a20_channel_recv_abort(a20_channel_ep_t *ep);
void a20_channel_ep_release(a20_channel_ep_t *ep);

/* ---- Event queue API ---- */

a20_eventq_t *a20_eventq_create(uint32_t capacity_hint);
int64_t a20_eventq_watch(a20_eventq_t *eq, a20_handle_t target_h, void *target_obj,
                         uint16_t target_type, uint64_t event_mask, uint64_t user_data);
int64_t a20_eventq_wait(a20_eventq_t *eq, a20_pending_event_t *out,
                        uint32_t max_events, uint64_t timeout_ns);
int64_t a20_eventq_cancel(a20_eventq_t *eq, a20_handle_t target_h);
void a20_eventq_release(a20_eventq_t *eq);

void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1);
void a20_eventq_on_object_destroy(void *object, uint16_t object_type);

/* Native timer backend (drives timer object expiries). */
void a20_timer_tick(void);

#endif /* _IPC_IPC_H */
