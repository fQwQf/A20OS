/* C reference for the Native IPC receive ownership transaction. */
#include "channel_recv_txn.h"

#ifdef A20_CHANNEL_RECV_TXN_HOST_TEST
#include <stddef.h>
extern void *kmalloc(size_t size);
extern void kfree(void *object);
extern long copy_to_user(void *dst, const void *src, size_t size);
extern void a20_object_release(void *object, uint16_t type);
#else
#include "core/defs.h"
#include "core/errno.h"
#include "mm/slab.h"
#include "sys/usercopy.h"
#include "ipc/ipc.h"
#include "ipc/handle_table.h"
#endif

_Static_assert(sizeof(a20_ch_handle_info_t) ==
               (sizeof(void *) == 8 ? 48 : 40),
               "C/Rust channel handle-info size mismatch");
#if defined(__arm__) || (defined(__riscv) && __riscv_xlen == 32)
/* AAPCS32 gives uint64_t eight-byte aggregate alignment even though pointers
 * are four bytes. RV32 ILP32D also aligns these uint64_t fields to eight;
 * keep both distinct from 32-bit ABIs that align them to four. */
#define A20_CH_HANDLE_INFO_ALIGN 8
#else
#define A20_CH_HANDLE_INFO_ALIGN (sizeof(void *) == 8 ? 8 : 4)
#endif
_Static_assert(__alignof__(a20_ch_handle_info_t) ==
               A20_CH_HANDLE_INFO_ALIGN,
               "C/Rust channel handle-info alignment mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, object) == 0,
               "C/Rust handle-info object offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, type) == sizeof(void *),
               "C/Rust handle-info type offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, _pad) == sizeof(void *) + 2,
               "C/Rust handle-info pad offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, transfer_rights) ==
               (sizeof(void *) == 8 ? 16 : 8),
               "C/Rust handle-info rights offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, expiry_tick) ==
               (sizeof(void *) == 8 ? 24 : 16),
               "C/Rust handle-info expiry offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, remaining_ops) ==
               (sizeof(void *) == 8 ? 32 : 24),
               "C/Rust handle-info remaining-ops offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, temporal_flags) ==
               (sizeof(void *) == 8 ? 36 : 28),
               "C/Rust handle-info temporal-flags offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, security_label) ==
               (sizeof(void *) == 8 ? 40 : 32),
               "C/Rust handle-info label offset mismatch");
_Static_assert(__builtin_offsetof(a20_ch_handle_info_t, _pad2) ==
               (sizeof(void *) == 8 ? 41 : 33),
               "C/Rust handle-info tail-pad offset mismatch");

extern int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                       a20_handle_t *handles, uint32_t count);
extern void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                                      a20_handle_t *handles, uint32_t count);
extern int64_t a20_handle_commit_reserved_temporal(
    struct a20_ht_internal *ht, a20_handle_t h, void *object, uint16_t type,
    a20_rights_t rights, uint64_t expiry_tick, uint32_t remaining_ops,
    uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);

#ifdef A20_CHANNEL_RECV_TXN_HOST_TEST
int64_t a20_channel_recv_txn_c(a20_channel_ep_t *ep,
                               struct a20_ht_internal *ht,
                               uint32_t flags, int donate,
                               void *user_data, uint32_t data_capacity,
                               void *user_handles, uint32_t handle_capacity,
                               void *user_rights, void *publish_context,
                               a20_channel_recv_publish_fn publish)
{
    uint32_t msg_data_len = 0;
    uint32_t msg_handles = 0;
    int64_t r = donate
        ? a20_channel_recv_begin_donate(ep, flags, &msg_data_len, &msg_handles)
        : a20_channel_recv_begin(ep, flags, &msg_data_len, &msg_handles);
    if (r < 0)
        return r;

    if ((msg_data_len > 0 && (!user_data || data_capacity < msg_data_len)) ||
        (msg_handles > 0 &&
         (!user_handles || handle_capacity < msg_handles))) {
        a20_channel_recv_abort(ep);
        return -A20_ERR_NO_SPACE;
    }
    if (msg_handles > A20_CH_MAX_HANDLES) {
        a20_channel_recv_abort(ep);
        return -A20_ERR_PROTOCOL;
    }

    /* Reserve before dequeue: on failure, the head message remains queued. */
    a20_handle_t reserved[A20_CH_MAX_HANDLES];
    uint32_t nreserved = 0;
    if (msg_handles > 0) {
        r = a20_handle_reserve_many(ht, reserved, msg_handles);
        if (r < 0) {
            a20_channel_recv_abort(ep);
            return r;
        }
        nreserved = msg_handles;
    }

    void *kdata = NULL;
    if (msg_data_len > 0) {
        kdata = kmalloc(msg_data_len);
        if (!kdata) {
            a20_handle_abort_reserved(ht, reserved, nreserved);
            a20_channel_recv_abort(ep);
            return -A20_ERR_NO_MEMORY;
        }
    }

    a20_ch_handle_info_t infos[A20_CH_MAX_HANDLES] = {0};
    uint32_t out_len = msg_data_len;
    uint32_t out_count = msg_handles;
    /* recv_finish consumes the begin-held endpoint ref on every return. */
    r = a20_channel_recv_finish(ep, kdata, &out_len, infos, &out_count);
    if (r < 0) {
        kfree(kdata);
        a20_handle_abort_reserved(ht, reserved, nreserved);
        return r;
    }
    if (out_count > nreserved || out_count > A20_CH_MAX_HANDLES) {
        for (uint32_t i = 0; i < out_count && i < A20_CH_MAX_HANDLES; i++)
            a20_object_release(infos[i].object, infos[i].type);
        a20_handle_abort_reserved(ht, reserved, nreserved);
        kfree(kdata);
        return -A20_ERR_PROTOCOL;
    }
    if (nreserved > out_count) {
        a20_handle_abort_reserved(ht, &reserved[out_count], nreserved - out_count);
        nreserved = out_count;
    }

    a20_handle_t installed_handles[A20_CH_MAX_HANDLES];
    a20_rights_t out_rights[A20_CH_MAX_HANDLES];
    uint32_t installed = 0;
    for (uint32_t i = 0; i < out_count; i++) {
        out_rights[i] = infos[i].transfer_rights;
        r = a20_handle_commit_reserved_temporal(
            ht, reserved[i], infos[i].object, infos[i].type,
            infos[i].transfer_rights, infos[i].expiry_tick,
            infos[i].remaining_ops, infos[i].temporal_flags,
            infos[i].security_label);
        if (r < 0) {
            a20_object_release(infos[i].object, infos[i].type);
            for (uint32_t j = i + 1; j < out_count; j++)
                a20_object_release(infos[j].object, infos[j].type);
            for (uint32_t j = 0; j < installed; j++)
                a20_handle_remove(ht, installed_handles[j]);
            a20_handle_abort_reserved(ht, &reserved[i], nreserved - i);
            kfree(kdata);
            return r;
        }
        installed_handles[installed++] = reserved[i];
    }

    if ((out_len > 0 && copy_to_user(user_data, kdata, out_len) < 0) ||
        (installed > 0 &&
         copy_to_user(user_handles, installed_handles,
                      installed * sizeof(a20_handle_t)) < 0) ||
        (user_rights && installed > 0 &&
         copy_to_user(user_rights, out_rights,
                      installed * sizeof(a20_rights_t)) < 0) ||
        !publish || publish(publish_context, out_len, installed) != 0) {
        r = -A20_ERR_FAULT;
        goto rollback;
    }

    kfree(kdata);
    return (int64_t)out_len;

rollback:
    for (uint32_t i = 0; i < installed; i++)
        a20_handle_remove(ht, installed_handles[i]);
    kfree(kdata);
    return r;
}
#endif /* A20_CHANNEL_RECV_TXN_HOST_TEST */
