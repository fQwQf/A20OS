#ifndef _A20_VIRTIO_BLK_CORE_H
#define _A20_VIRTIO_BLK_CORE_H

#include "core/types.h"

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} a20_virtio_blk_desc_t;

typedef struct {
    uint64_t addr;
    uint32_t len;
} a20_virtio_blk_segment_t;

typedef struct {
    uint16_t head;
    uint16_t descriptor_count;
    uint32_t expected_len;
    uint8_t write;
    uint8_t state;
    uint8_t reserved[2];
} a20_virtio_blk_request_state_t;

typedef struct {
    uint16_t queue_size;
    uint16_t slot_count;
    uint16_t avail_idx;
    uint16_t last_used;
    uint16_t descriptor_cursor;
    uint16_t active_count;
    uint32_t epoch;
    uint64_t descriptor_bitmap;
    a20_virtio_blk_request_state_t requests[64];
} a20_virtio_blk_queue_state_t;

/*
 * Queue-state contract:
 * - queue storage is writable and aligned for this type; call queue_init
 *   before every other operation. init accepts uninitialized storage.
 * - init accepts power-of-two queue_size in [4, 64] and slot_count in
 *   [1, queue_size]. All subsequent calls rely on those initialized bounds.
 * - the placement adapter serializes every operation. It must stop/quiesce
 *   device DMA before queue_reset; epoch is diagnostic and does not validate
 *   stale completions.
 * - completion state changes are Rust-owned; ring/DMA/MMIO/IRQ and waiter
 *   wakeup operations remain with the C adapter.
 */

_Static_assert(sizeof(a20_virtio_blk_desc_t) == 16, "Rust virtio descriptor ABI");
_Static_assert(sizeof(a20_virtio_blk_segment_t) == 16, "Rust virtio segment ABI");
_Static_assert(__builtin_offsetof(a20_virtio_blk_desc_t, len) == 8 &&
               __builtin_offsetof(a20_virtio_blk_desc_t, flags) == 12 &&
               __builtin_offsetof(a20_virtio_blk_desc_t, next) == 14,
               "Rust virtio descriptor offsets");
_Static_assert(__builtin_offsetof(a20_virtio_blk_segment_t, len) == 8,
               "Rust virtio segment offsets");
_Static_assert(sizeof(a20_virtio_blk_request_state_t) == 12,
               "Rust virtio request-state ABI");
_Static_assert(sizeof(a20_virtio_blk_queue_state_t) == 792 &&
               __builtin_offsetof(a20_virtio_blk_queue_state_t, epoch) == 12 &&
               __builtin_offsetof(a20_virtio_blk_queue_state_t, descriptor_bitmap) == 16 &&
               __builtin_offsetof(a20_virtio_blk_queue_state_t, requests) == 24,
               "Rust virtio queue-state ABI");

/* Returns descriptor count, or a negative errno without mutating the table. */
int a20_virtio_blk_build_chain(a20_virtio_blk_desc_t *descs,
                               uint16_t queue_size, uint16_t head,
                               uint64_t header_addr, uint32_t header_len,
                               const a20_virtio_blk_segment_t *segments,
                               uint16_t segment_count, uint64_t status_addr,
                               uint32_t device_writes);
int a20_virtio_blk_queue_init(a20_virtio_blk_queue_state_t *queue,
                              uint16_t queue_size, uint16_t slot_count);
int a20_virtio_blk_queue_reserve(a20_virtio_blk_queue_state_t *queue,
                                 uint16_t descriptor_count,
                                 uint32_t expected_len, uint32_t write,
                                 uint16_t *out_slot, uint16_t *out_head);
int a20_virtio_blk_queue_prepare_publish(a20_virtio_blk_queue_state_t *queue,
                                         uint16_t slot,
                                         uint16_t *out_ring_slot);
int a20_virtio_blk_queue_commit_publish(a20_virtio_blk_queue_state_t *queue,
                                        uint16_t slot,
                                        uint16_t *out_avail_idx);
int a20_virtio_blk_queue_used_ring_slot(a20_virtio_blk_queue_state_t *queue,
                                        uint16_t device_idx,
                                        uint16_t *out_ring_slot);
int a20_virtio_blk_queue_validate_head(a20_virtio_blk_queue_state_t *queue,
                                       uint16_t head, uint16_t *out_slot);
int a20_virtio_blk_queue_complete_used(a20_virtio_blk_queue_state_t *queue,
                                       uint16_t device_idx, uint16_t head,
                                       uint32_t used_len, uint32_t device_status,
                                       uint16_t *out_slot, int32_t *out_result);
int a20_virtio_blk_queue_fail_all(a20_virtio_blk_queue_state_t *queue,
                                  uint16_t *out_slots, uint16_t capacity);
int a20_virtio_blk_queue_reset(a20_virtio_blk_queue_state_t *queue);
int a20_virtio_blk_queue_release(a20_virtio_blk_queue_state_t *queue,
                                 uint16_t slot);
int a20_virtio_blk_queue_cancel(a20_virtio_blk_queue_state_t *queue,
                                uint16_t slot);

#endif
