/*
 * ufs_backends.h — ufsd 多人格后端接口。
 *
 * 每个后端实现同一组 ufs_proto 操作；payload 直接构造在应答头部之后
 * （连续布局，见 ufs_proto.h）。ino 寻址语义由各后端自行维护：
 * fat 用 ino↔path 表，vnode 型后端用 ino→vnode* 映射。
 */
#ifndef UFS_BACKENDS_H
#define UFS_BACKENDS_H

#include <stdint.h>
#include "core/types.h"
#include "core/string.h"
#include "fs/ufs_proto.h"
#include "a20_string.h"

#define UFSD_MSG_MAX 65536u

/* 与内核 VFS 一致的 errno 值（freestanding 环境自持） */
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

/* 应答载荷区：紧随应答头部的连续缓冲 */
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
    /* 挂载：成功返回 0，负值为 -errno；INIT 握手由主循环处理 */
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
