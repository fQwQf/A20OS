#ifndef _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H
#define _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H

#include "core/types.h"
#include "proc/proc.h"
#include "abi/native/types.h"
#include "abi/native/rights.h"

struct a20_ht_internal;

struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);

int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                    uint16_t type, a20_rights_t rights,
                                    uint64_t expiry_tick,
                                    uint32_t remaining_ops,
                                    uint32_t temporal_flags,
                                    uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                   uint16_t expected_type,
                                   a20_rights_t required_rights,
                                   a20_handle_entry_t *out);
void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);

int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                a20_handle_t *handles, uint32_t count);
void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                               a20_handle_t *handles, uint32_t count);
void a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                         a20_handle_t h, void *object,
                                         uint16_t type, a20_rights_t rights,
                                         uint64_t expiry_tick,
                                         uint32_t remaining_ops,
                                         uint32_t temporal_flags,
                                         uint8_t security_label);

struct a20_ht_internal *task_get_a20_ht(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);
void a20_temporal_sweep(struct a20_ht_internal *ht);

#endif
