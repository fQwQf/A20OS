/*
 * C ABI of the Rust socket-registry state machine
 * (kernel/net/socket_registry.rs, selected by NET_SOCKET_REGISTRY_IMPL=rust).
 *
 * The Rust module owns the transitions; C owns the primitives.  Every entry
 * point below therefore runs with whatever lock the caller already holds, takes
 * no lock of its own, and neither allocates nor blocks.  The views passed in are
 * pointers into the existing C layout (g_sockets, a bucket's free_bits,
 * s->refs, s->ref_magic, s->in_registry, s->reg_idx, g_net_bh_pending), so no
 * struct ABI moves.
 *
 * Return-code conventions, because they are the contract:
 *
 *   a20_net_ref_release   1 = last reference, the caller destroys the object;
 *                         0 = another reference remains;
 *                        <0 = the ledger recorded a fault, reason = -code - 1
 *                             (0 already-freed, 1 canary mismatch).
 *   a20_net_slot_release  1 = the socket owned a registry slot, so the caller
 *                             still owes the registry's reference;
 *                         0 = it never owned one (an AF_UNIX accepted child).
 *   a20_net_bucket_claim  bucket-local slot index, or -1 when the bucket is
 *                         full.
 */
#ifndef _NET_SOCKET_REGISTRY_STATE_H
#define _NET_SOCKET_REGISTRY_STATE_H

#include <stdbool.h>
#include <stdint.h>

/* Reference-ledger fault reasons, mirrored from the Rust module. */
#define NET_SOCK_REF_FAULT_REASONS 4

/*
 * The counter views are declared `volatile` because that is what the fields
 * they point at are (net_socket_t.refs, g_net_bh_pending); the Rust side sees
 * the same pointers as plain int*, which is the same object and the same ABI.
 */

/* ---------------------------------------------------------------- references */

int a20_net_ref_init(volatile int *refs, uint32_t *magic);
int a20_net_ref_acquire(volatile int *refs);
int a20_net_ref_release(volatile int *refs, uint32_t *magic);
int a20_net_ref_live(void);
void a20_net_ref_ledger(int *allocs, int *frees, int *faults, int *reasons,
                        int reasons_len);
int a20_net_ref_fault_count(int reason);

/* ------------------------------------------------------------- slot bitmap */

int a20_net_bucket_init(uint32_t *bits, int words, int slots_per_bucket);
int a20_net_bucket_claim(uint32_t *bits, int words);
int a20_net_bucket_release(uint32_t *bits, int words, int local);

/* ------------------------------------------------- bottom-half pending flags */

int a20_net_bh_mark(volatile int *pending, volatile int *count, int slots,
                    int idx);
int a20_net_bh_clear(volatile int *pending, volatile int *count, int slots,
                     int idx);
int a20_net_bh_any_pending(const volatile int *count);
int a20_net_bh_slot_pending(const volatile int *pending, int slots, int idx);

/* --------------------------------------------------- slot publish / release */

int a20_net_slot_publish(void **table, int slots, int idx, void *s,
                         int *in_registry, int *reg_idx, volatile int *refs,
                         volatile int *pending, volatile int *pending_count);
int a20_net_slot_release(void **table, int slots, int slots_per_bucket, int idx,
                         const void *s, int *in_registry, int *reg_idx,
                         uint32_t *bits, int words, volatile int *pending,
                         volatile int *pending_count);

/* --------------------------------------- /proc/net membership and readiness */

int a20_net_table_claims(int kind, int domain, int sock_type);
int a20_net_table_walk_match(int kind, int family, int domain, int sock_type,
                             int closed, int ns_match);
int a20_net_rx_available_gate(int domain);

#endif /* _NET_SOCKET_REGISTRY_STATE_H */
