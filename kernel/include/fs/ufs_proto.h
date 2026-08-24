/*
 * ufs_proto.h — uxfs 用户态文件服务线协议（内核代理 <-> 用户态 ufsd 共享）。
 *
 * 架构位置见 docs/hybrid-kernel/06-user-fs.md：VFS 核心留在内核，文件系统
 * 实现（FAT32 等）作为可崩溃、可重启的用户态服务运行；内核侧的 uxfs 代理把
 * vnode 操作经 Channel IPC 转发给服务进程。本头文件定义两侧共享的消息格式，
 * 与 kernel/ipc/a20_channel.c 的 A20_CH_MAX_DATA (64 KiB) 载荷上限配合，
 * 元数据与数据负载均内联在一条 channel 消息内。
 */
#ifndef FS_UFS_PROTO_H
#define FS_UFS_PROTO_H

#include <stdint.h>

#define UFS_PROTO_VERSION 1u
#define UFS_REQ_MAGIC 0x55465352u /* 'UFSR' */
#define UFS_RESP_MAGIC 0x55465350u /* 'UFSP' */

/* ---- 操作码 ---- */
#define UFS_OP_INIT 1 /* 握手：请求方无参数，应答携带根 ino */
#define UFS_OP_LOOKUP 2
#define UFS_OP_GETATTR 3
#define UFS_OP_READDIR 4
#define UFS_OP_CREATE 5
#define UFS_OP_MKDIR 6
#define UFS_OP_UNLINK 7
#define UFS_OP_RMDIR 8
#define UFS_OP_TRUNCATE 9
#define UFS_OP_READ 10
#define UFS_OP_WRITE 11
#define UFS_OP_SYNC 12
#define UFS_OP_STATFS 13

/* GETATTR/LOOKUP 应答中的节点类型（与 VFS_FT_* 低 4 位对齐） */
#define UFS_FT_FILE 1
#define UFS_FT_DIR 2
#define UFS_FT_CHARDEV 3
#define UFS_FT_BLOCKDEV 4
#define UFS_FT_SYMLINK 5

/* 状态值：0 成功，负值为 errno（与 VFS 内部约定一致） */
#define UFS_OK 0

/*
 * 请求帧：头部之后依次紧跟 name[name_len] 与 payload[payload_len]，
 * 无对齐填充。READDIR 的应答 payload 为紧凑目录项序列：
 *   u64 ino | u8 type | u8 name_len | char name[name_len]
 * READ 的应答 payload 为文件字节；WRITE 的请求 payload 为待写字节。
 */
typedef struct ufs_req_hdr {
    uint32_t magic;    /* UFS_REQ_MAGIC */
    uint32_t version;  /* UFS_PROTO_VERSION */
    uint32_t opcode;   /* UFS_OP_* */
    uint32_t req_id;   /* 回显于应答，用于乱序丢弃 */
    uint64_t ino;      /* 目标节点 / 目录节点；根为 UFS_ROOT_INO */
    uint64_t arg0;     /* 按 op 语义：offset / size / mode / cookie */
    uint64_t arg1;     /* 按 op 语义：count 等 */
    uint32_t name_len; /* name 段长度（字节），可为 0 */
    uint32_t payload_len;
} ufs_req_hdr_t;

typedef struct ufs_resp_hdr {
    uint32_t magic;   /* UFS_RESP_MAGIC */
    uint32_t opcode;  /* 回显请求操作码 */
    uint32_t req_id;  /* 回显请求 id */
    int32_t status;   /* 0 或负 errno */
    uint64_t out0;    /* 按 op 语义：ino / size / 总字节数等 */
    uint64_t out1;    /* 按 op 语义：type / 剩余量等 */
    uint64_t out2;    /* 保留 */
    uint32_t payload_len;
    uint32_t _pad;
} ufs_resp_hdr_t;

#define UFS_ROOT_INO 1ull

/* 单条消息内 payload 的实际上限（channel 上限减去两个头部余量） */
#define UFS_MAX_PAYLOAD (48u * 1024u)

#endif /* FS_UFS_PROTO_H */
