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
    void (*statfs)(ufs_resp_hdr_t *r);
} ufs_backend_t;

extern const ufs_backend_t UFS_BACKEND_FAT;
extern const ufs_backend_t UFS_BACKEND_VNFS;

#endif /* UFS_BACKENDS_H */
