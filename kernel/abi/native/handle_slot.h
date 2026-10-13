#ifndef _ABI_NATIVE_HANDLE_SLOT_H
#define _ABI_NATIVE_HANDLE_SLOT_H

#include "ipc/handle_table.h"

/* Rust owns entry-state transitions when CONFIG_HANDLE_SLOT_TRANSITIONS_RUST
 * is selected. Every function below except destroy requires the owning table
 * lock to remain held for the full call; destroy is called only after the
 * final table reference has gone and excludes all concurrent users. The Rust side performs no allocation, callbacks,
 * object refcount operations, or lock operations. Entry pointers must refer to
 * live, naturally aligned table elements; outputs must be writable, mutually
 * non-overlapping, and disjoint from the entry. install/commit transfer the
 * caller's already-owned object reference into the slot, but do not ref/unref.
 * remove/expire/destroy transfer a detached reference back to C for release
 * after dropping the table lock. */
int32_t a20_rust_handle_slot_quota_allows(uint32_t used, uint32_t requested,
                                          uint32_t quota);
int32_t a20_rust_handle_slot_install(a20_handle_entry_t *entry, void *object,
                                     uint16_t type, a20_rights_t rights,
                                     uint64_t expiry_tick,
                                     uint32_t remaining_ops,
                                     uint32_t temporal_flags,
                                     uint8_t security_label);
int32_t a20_rust_handle_slot_reserve(a20_handle_entry_t *entry);
int32_t a20_rust_handle_slot_abort(a20_handle_entry_t *entry);
int32_t a20_rust_handle_slot_commit(a20_handle_entry_t *entry, void *object,
                                    uint16_t type, a20_rights_t rights,
                                    uint64_t expiry_tick,
                                    uint32_t remaining_ops,
                                    uint32_t temporal_flags,
                                    uint8_t security_label);
int32_t a20_rust_handle_slot_remove(a20_handle_entry_t *entry,
                                    void **object, uint16_t *type);
int64_t a20_rust_handle_slot_lookup(a20_handle_entry_t *entry,
                                    uint16_t expected_type,
                                    a20_rights_t required_rights,
                                    uint64_t now,
                                    a20_handle_entry_t *out);
int32_t a20_rust_handle_slot_set_temporal(a20_handle_entry_t *entry,
                                          uint64_t expiry_tick,
                                          uint32_t remaining_ops,
                                          uint32_t temporal_flags);
int32_t a20_rust_handle_slot_get_temporal(const a20_handle_entry_t *entry,
                                          uint64_t *expiry_tick,
                                          uint32_t *remaining_ops,
                                          uint32_t *temporal_flags);
int32_t a20_rust_handle_slot_set_label(a20_handle_entry_t *entry,
                                       uint8_t label);
/* Returns 1 if AUTO_CLOSE detached an object, 0 if unchanged/marked expired. */
int32_t a20_rust_handle_slot_expire(a20_handle_entry_t *entry, uint64_t now,
                                    void **object, uint16_t *type);
int32_t a20_rust_handle_slot_destroy(a20_handle_entry_t *entry,
                                     void **object, uint16_t *type);

#endif
