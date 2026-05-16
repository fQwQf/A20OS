/*
 * A20 resource limits — per-task resource caps and enforcement.
 *
 * Prevents resource exhaustion by capping handle count, channel depth,
 * and event_queue capacity per task.
 *
 * These limits are checked in the kernel's syscall implementation
 * before allocating resources.
 */

#ifndef _A20_RESOURCE_H
#define _A20_RESOURCE_H

#include <stdint.h>

/* Per-task resource limits */
struct a20_resource_limits {
    uint32_t max_handles;          /* max handle table entries (default 4096) */
    uint32_t max_channels;         /* max active channel endpoints (default 256) */
    uint32_t max_event_queues;     /* max active event queues (default 64) */
    uint32_t max_channel_depth;    /* max messages per channel endpoint (default 1024) */
    uint32_t max_event_capacity;   /* max events in ring buffer (default 256) */
    uint32_t max_vmo_count;        /* max VMOs per task (default 512) */
    uint64_t max_memory_bytes;     /* max virtual memory per task (default 1GB) */
    uint32_t max_threads;          /* max threads per task (default 128) */
    uint32_t max_pending_ops;      /* max pending async operations (default 64) */
};

/* Default limits */
#define A20_LIMIT_HANDLES_DEFAULT       4096
#define A20_LIMIT_CHANNELS_DEFAULT       256
#define A20_LIMIT_EVENT_QUEUES_DEFAULT    64
#define A20_LIMIT_CHANNEL_DEPTH_DEFAULT 1024
#define A20_LIMIT_EVENT_CAPACITY_DEFAULT 256
#define A20_LIMIT_VMO_COUNT_DEFAULT      512
#define A20_LIMIT_MEMORY_DEFAULT   (1ULL << 30)  /* 1 GB */
#define A20_LIMIT_THREADS_DEFAULT        128
#define A20_LIMIT_PENDING_OPS_DEFAULT     64

/* Absolute maximums (hard caps, not configurable) */
#define A20_LIMIT_HANDLES_ABSOLUTE      65536
#define A20_LIMIT_CHANNELS_ABSOLUTE      4096
#define A20_LIMIT_EVENT_QUEUES_ABSOLUTE  1024
#define A20_LIMIT_CHANNEL_DEPTH_ABSOLUTE  8192
#define A20_LIMIT_EVENT_CAPACITY_ABSOLUTE 4096
#define A20_LIMIT_VMO_COUNT_ABSOLUTE     8192
#define A20_LIMIT_MEMORY_ABSOLUTE   (4ULL << 30)  /* 4 GB */
#define A20_LIMIT_THREADS_ABSOLUTE       4096
#define A20_LIMIT_PENDING_OPS_ABSOLUTE    512

/* Cascading depth limit (docs/native-abi/03-handle.md §3.3) */
#define A20_CASCADE_DEPTH_MAX            2

static inline void a20_resource_limits_init_default(struct a20_resource_limits *l)
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

#endif /* _A20_RESOURCE_H */
