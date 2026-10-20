/*
 * ufs_backends.h — the ufsd multi-personality backend interface.
 *
 * Each backend implements the same set of ufs_proto operations; the payload
 * is built directly after the response header (contiguous layout, see
 * ufs_proto.h). ino addressing semantics are maintained by each backend
 * itself: fat uses an ino-to-path table, vnode-type backends use an
 * ino-to-vnode map.
 */
#ifndef UFS_BACKENDS_H
#define UFS_BACKENDS_H

#include <stdint.h>
#include "core/types.h"
#include "core/string.h"
#include "fs/ufs_proto.h"
#include "a20_string.h"

#define UFSD_MSG_MAX 65536u

/* errno values consistent with the kernel VFS (self-hosted in the
 * freestanding environment) */
#define U_ENOENT 2
#define U_EAGAIN 11
#define U_EIO 5
#define U_EEXIST 17
#define U_ENOTDIR 20
#define U_EISDIR 21
#define U_EINVAL 22
#define U_EMFILE 24
#define U_ENOSPC 28
#define U_EROFS 30
#define U_ENOSYS 38
#define U_ESTALE 116

/* Response payload area: the contiguous buffer immediately following the
 * response header */
extern uint8_t ufs_tx[UFSD_MSG_MAX];
extern void   (*ufs_log_sink)(const char *line);

static inline uint8_t *ufs_payload_buf(void)
{
    return ufs_tx + sizeof(ufs_resp_hdr_t);
}

static inline uint32_t ufs_payload_cap(void)
{
    return UFSD_MSG_MAX - sizeof(ufs_resp_hdr_t);
}

typedef struct ufs_backend {
    const char *name;
    /* Mount: returns 0 on success, a negative -errno on failure; the INIT
     * handshake is handled by the main loop */
    int (*mount)(const char *fstype);
    /* Called only on a graceful peer shutdown. A supervisor task kill is a
     * crash and deliberately does not run filesystem clean-state handling. */
    int64_t (*unmount)(void);
    int64_t (*lookup)(uint64_t dir_ino, const char *name, uint32_t name_len,
                      ufs_resp_hdr_t *r);
    int64_t (*getattr)(uint64_t ino, ufs_resp_hdr_t *r);
    int64_t (*readdir)(uint64_t dir_ino, uint64_t skip, ufs_resp_hdr_t *r);
    int64_t (*create)(uint64_t dir_ino, const char *name, uint32_t mode,
                      ufs_resp_hdr_t *r);
    int64_t (*mkdir)(uint64_t dir_ino, const char *name, ufs_resp_hdr_t *r);
    int64_t (*unlink)(uint64_t dir_ino, const char *name);
    int64_t (*rmdir)(uint64_t dir_ino, const char *name);
    int64_t (*truncate)(uint64_t ino, uint64_t size);
    int64_t (*read)(uint64_t ino, uint64_t off, uint32_t count,
                    ufs_resp_hdr_t *r);
    int64_t (*write)(uint64_t ino, uint64_t off, const uint8_t *data,
                     uint32_t count, ufs_resp_hdr_t *r);
    int64_t (*rename)(uint64_t old_dir, const char *old_name,
                      uint64_t new_dir, const char *new_name,
                      ufs_resp_hdr_t *r);
    int64_t (*sync)(void);
    /* Optional v2 per-open lifetime hooks. A backend advertises lifetime
     * tokens only when OPEN, CLOSE, and CLOSE_CONFIRM callbacks are present.
     * Close-token extension (docs/fs/ufs-close-token.md): open
     * receives a non-NULL token_out only when the client negotiated
     * UFS_CLOSE_TOKEN_CAPABILITY; it may then store a nonzero open token
     * (token 0 keeps that open on the legacy path).  close receives the
     * token from the CLOSE request arg0 (0 = legacy close). */
    int64_t (*open)(uint64_t ino, uint64_t *token_out);
    int64_t (*close)(uint64_t ino, uint64_t token);
    int64_t (*close_confirm)(uint64_t ino, uint64_t token);
    void (*statfs)(ufs_resp_hdr_t *r);
} ufs_backend_t;

/* INIT negotiation shared with the C dispatcher host test. Token-only request
 * tags deliberately fall back to ordinary v2 lifetime semantics: only the
 * confirm protocol can safely retry an ambiguous CLOSE. */
static inline uint64_t ufs_backend_lifetime_negotiate(
    const ufs_backend_t *be, uint64_t request_capability,
    int *tokens_enabled, int *confirm_enabled)
{
    if (tokens_enabled) *tokens_enabled = 0;
    if (confirm_enabled) *confirm_enabled = 0;
    if (!be || !be->open || !be->close)
        return 0;
    if (request_capability == UFS_CLOSE_CONFIRM_CAPABILITY &&
        be->close_confirm) {
        if (tokens_enabled) *tokens_enabled = 1;
        if (confirm_enabled) *confirm_enabled = 1;
        return UFS_CLOSE_CONFIRM_CAPABILITY;
    }
    return UFS_LIFETIME_CAPABILITY;
}

extern const ufs_backend_t UFS_BACKEND_FAT;
extern const ufs_backend_t UFS_BACKEND_VNFS;

#endif /* UFS_BACKENDS_H */
