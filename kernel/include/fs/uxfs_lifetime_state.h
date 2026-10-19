#ifndef _FS_UXFS_LIFETIME_STATE_H
#define _FS_UXFS_LIFETIME_STATE_H

#include "core/types.h"

/*
 * UXFS lifetime/close-token negotiation state.
 *
 * The decisions and transitions are owned by
 * kernel/fs/uxfs/lifetime_state.rs; uxfs.c only embeds this struct, takes
 * sb->lifetime_lock and performs the INIT/OPEN/CLOSE RPCs.  Keep the field
 * order in sync with the Rust mirror (layout asserted in uxfs.c).
 */
typedef struct uxfs_lifetime_state {
    uint32_t mode;              /* 0 unknown, 1 explicit v1, 2 v2 */
    uint32_t shutdown_mode;     /* 1 when the server ACKed SHUTDOWN */
    uint32_t close_token_mode;  /* OPEN out2 / CLOSE arg0 carry open tokens */
    uint32_t close_confirm_mode;/* accepted tokens retire via CONFIRM */
    uint64_t generation;        /* server generation this state was learned from */
    void    *server;            /* struct task_t * this state was learned from */
} uxfs_lifetime_state_t;

/* Capability values carried in the INIT reply live in fs/ufs_proto.h. */
void a20_uxfs_lifetime_reset(uxfs_lifetime_state_t *st);
int  a20_uxfs_lifetime_negotiated(const uxfs_lifetime_state_t *st,
                                  uint64_t server_generation, void *server);
void a20_uxfs_lifetime_apply_init(uxfs_lifetime_state_t *st, uint64_t out1,
                                  uint64_t out2, uint64_t server_generation,
                                  void *server);
int  a20_uxfs_lifetime_is_v1(const uxfs_lifetime_state_t *st);
int  a20_uxfs_lifetime_is_v2(const uxfs_lifetime_state_t *st);
int  a20_uxfs_lifetime_shutdown_supported(const uxfs_lifetime_state_t *st);
uint64_t a20_uxfs_lifetime_open_token(const uxfs_lifetime_state_t *st,
                                      uint64_t server_token);
int  a20_uxfs_lifetime_close_needs_rpc(const uxfs_lifetime_state_t *st);
int  a20_uxfs_lifetime_close_v1_acked(const uxfs_lifetime_state_t *st);

#endif /* _FS_UXFS_LIFETIME_STATE_H */
