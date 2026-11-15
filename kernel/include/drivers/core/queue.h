/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_DRIVER_QUEUE_H
#define A20_DRIVER_QUEUE_H
#include "core/types.h"
struct device;
struct vmo;
struct driver_dma_buffer;
struct driver_fence;
struct driver_workqueue;
struct driver_queue;
struct driver_request;

/* Hosted native request execution, independent of a bus, device class or
 * Linux UAPI. One queue belongs to ONE binding; old references never rebind.
 * Queue/request allocation and final puts are task context outside spinlocks.
 * A caller owns a reference for every operation, including IRQ completion.
 * Queue creation is probe-only, before hardware activation. The binding owns
 * a second reference through a managed execution action; final caller puts
 * cannot tear down live hardware. No device_t/driver_t layout extension. */
struct driver_queue_limits {
    uint32_t depth;                 /* accepted requests, queued + active */
    uint32_t max_active;            /* parallel prepare/issue/retire slots */
    uint32_t max_command;           /* copied immutable command, <= 4096 */
    uint64_t max_bytes;             /* sum of command + memory views accepted */
};
/* Optional retained native memory views. Extents must be fixed RAM VMOs
 * (resident/contiguous) or native DMA buffers. Retention includes the original
 * VMO charge. This is lifetime ownership, not a DMA mapping, cache sync or
 * permission grant: brokers validate capabilities, backends map/sync views.
 * The contents remain mutable; commands needing immutable data must copy it.
 * No driver-defined finalizer may survive a binding. At most 64 views/request. */
#define DRIVER_REQUEST_VMO 1U
#define DRIVER_REQUEST_DMA_BUFFER 2U
struct driver_request_memory {
    uint32_t type;
    union { struct vmo *vmo; struct driver_dma_buffer *buffer; } owner;
    uint64_t offset, length;
};
struct driver_request_description {
    const void *command;
    size_t command_size;
    const struct driver_request_memory *memory;
    size_t memory_count;
    /* Optional one-shot dependency. FIFO queue: a pending head dependency
     * blocks later issuance; worker mode uses a cancellable fence watch,
     * not tick polling. Use separate queues for independent timelines.
     * Failed dependency retires without prepare/issue. Retained until final
     * request put, never waited on under a driver lock. */
    struct driver_fence *dependency;
};
struct driver_queue_ops {
    size_t size; /* sizeof(struct driver_queue_ops); copied during creation */
    /* Serialized task callbacks, outside the queue spinlock. Optional prepare
     * may install a cookie with set_private; error still goes through retire.
     * EAGAIN with NULL cookie means retry/backpressure, no hardware touched.
     * prepare must not publish DMA; issue is the only publication point. */
    int (*prepare)(void *, struct driver_request *);
    /* Nonblocking publication. No ambiguous "error but accepted" return:
     * report immediate failure with complete(error), otherwise complete from
     * IRQ/poll. Inline completion is allowed; retirement waits for return. */
    void (*issue)(void *, struct driver_request *);
    /* Optional task cleanup after completion or prepare error. 0 consumes
     * cookie and proves request storage reusable (also handle NULL/unprepared
     * requests); failure retains everything
     * for retry, including pending fence and queue capacity. No new DMA. */
    int (*retire)(void *, struct driver_request *);
    /* Required, idempotent, fallible shutdown including partial probe. Close
     * admission, stop producers/IRQs and prove no future DMA/completions before
     * returning 0. Never release context or request storage here. Failure
     * retains binding and requests offline for explicit teardown retry. */
    int (*stop)(void *);
};
int driver_queue_create(struct device *, const struct driver_queue_limits *,
                          const struct driver_queue_ops *, void *context,
                          struct driver_queue **out);
void driver_queue_get(struct driver_queue *);
void driver_queue_put(struct driver_queue *);
/* Optional autonomous task progress, AFTER scheduler initialization. Shares
 * a caller-owned native workqueue (no thread per hardware/software queue).
 * Keep that worker alive until all attached queues have stopped.
 * Queue creates/owns one core callback work item. Stop cancels/drains it.
 * Serialize start against stop; returns EALREADY if started, ESHUTDOWN if
 * closing. Without start, caller MUST run progress (boot polling works).
 * No implicit hardware polling: IRQ/poll producers call complete(). */
int driver_queue_start(struct driver_queue *, struct driver_workqueue *);
/* Bounded task progress; first dependency-watch setup may allocate, as may
 * provider callbacks. Serializes prepare/issue/retire without blocking; EBUSY
 * if another pump/stop owns dispatch. Returns processed transitions, or first
 * cleanup error. budget must be 1..4096. No queue spinlock across callbacks,
 * fence notifications, native backing puts or module code. */
int driver_queue_run(struct driver_queue *, unsigned budget);
/* Permanent close; task context, serialized retries, no caller-held lock
 * needed by backend/work/fence callbacks. EBUSY if dispatch is active (does
 * not wait for itself). stop failure preserves pending fences; stop success
 * retires outstanding requests with ESHUTDOWN (already completed results
 * win). Retirement failure also retains binding. Called automatically before
 * class-call drain; available explicitly for per-queue shutdown. */
int driver_queue_stop(struct driver_queue *);
struct driver_queue_status {
    uint32_t outstanding, active;
    uint64_t bytes;
    int last_error;
    bool closing, stopped;
};
void driver_queue_status(struct driver_queue *, struct driver_queue_status *);

int driver_request_create(struct driver_queue *, const struct driver_request_description *,
                            struct driver_request **out);
void driver_request_get(struct driver_request *);
void driver_request_put(struct driver_request *);
/* Task, no allocation. 0 accepts and retains a request reference;
 * EAGAIN depth/byte limit, ESHUTDOWN closed, EALREADY previously accepted.
 * Failure does not consume request. Accepted requests are one-shot. */
int driver_request_submit(struct driver_request *);
/* IRQ-safe, no allocation/free/module callback. 0 records hardware
 * completion, negative errno fails; first completion wins. This is NOT yet
 * the user completion: task retirement must succeed before fence signalling.
 * Producer cannot access request/cookie after this call without its own pin.
 * Caller must prove device no longer accesses this request's memory. */
int driver_request_complete(struct driver_request *, int error);
/* Task: queued request cancellation is exact and runs normal retirement;
 * active -> EBUSY (no hardware abort implied), new -> ENOENT, completion already recorded ->
 * EALREADY. Stop the queue explicitly when an active abort is required.
 * Wait timeout/interruption never invokes cancel or authorizes DMA release. */
int driver_request_cancel(struct driver_request *);
/* Borrowed immutable metadata for a request owner; fence is borrowed too.
 * Use fence_get when retaining it independently. Never signal it yourself. */
const void *driver_request_command(const struct driver_request *, size_t *size);
const struct driver_request_memory *driver_request_memory(const struct driver_request *, size_t *count);
struct driver_fence *driver_request_fence(const struct driver_request *);
/* Backend-only, serialized prepare/issue/retire callbacks. No implicit
 * ownership/free; retire must release the cookie before returning 0. */
void *driver_request_private(const struct driver_request *);
void driver_request_set_private(struct driver_request *, void *);
#endif
