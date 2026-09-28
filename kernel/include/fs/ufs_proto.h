/*
 * ufs_proto.h -- wire protocol between the uxfs kernel proxy and the
 * user-space uxfs service (ufsd); shared by both sides.
 *
 * Architectural placement: docs/hybrid-kernel/06-user-fs.md.  The VFS core
 * stays in the kernel while filesystem implementations (FAT32 and friends) run
 * as crashable, restartable user-space services; the kernel-side uxfs proxy
 * forwards vnode operations to the service process over Channel IPC.  This
 * header defines the message format shared by both sides, sized against
 * A20_CH_MAX_DATA (64 KiB) in kernel/ipc/a20_channel.c, with both metadata and
 * data payloads inlined into a single channel message.
 */
#ifndef FS_UFS_PROTO_H
#define FS_UFS_PROTO_H

#include <stdint.h>

#define UFS_PROTO_VERSION 1u
#define UFS_REQ_MAGIC 0x55465352u /* 'UFSR' */
#define UFS_RESP_MAGIC 0x55465350u /* 'UFSP' */

/* ---- opcodes ---- */
#define UFS_OP_INIT 1 /* handshake: requester sends no arguments, reply carries the root ino */
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
#define UFS_OP_RENAME 14

/* Node type in a GETATTR/LOOKUP reply (aligned with the low 4 bits of VFS_FT_*) */
#define UFS_FT_FILE 1
#define UFS_FT_DIR 2
#define UFS_FT_CHARDEV 3
#define UFS_FT_BLOCKDEV 4
#define UFS_FT_SYMLINK 5

/* Status value: 0 on success, negative errno on failure (matching the VFS
 * internal convention) */
#define UFS_OK 0

/*
 * Request frame: name[name_len] and payload[payload_len] follow the header
 * back to back, with no alignment padding.  A READDIR reply payload is a
 * compact sequence of directory entries:
 *   u64 ino | u8 type | u8 name_len | char name[name_len]
 * A READ reply payload is file bytes; a WRITE request payload is the bytes to
 * write.
 * RENAME: ino = source directory, arg0 = target directory ino, name = source
 * name, payload = target name string.
 */
typedef struct ufs_req_hdr {
    uint32_t magic;    /* UFS_REQ_MAGIC */
    uint32_t version;  /* UFS_PROTO_VERSION */
    uint32_t opcode;   /* UFS_OP_* */
    uint32_t req_id;   /* echoed in the reply, so out-of-order replies can be dropped */
    uint64_t ino;      /* target node or directory node; the root is UFS_ROOT_INO */
    uint64_t arg0;     /* op-dependent: offset / size / mode / cookie */
    uint64_t arg1;     /* op-dependent: count and so on */
    uint32_t name_len; /* length of the name segment in bytes; may be 0 */
    uint32_t payload_len;
} ufs_req_hdr_t;

typedef struct ufs_resp_hdr {
    uint32_t magic;   /* UFS_RESP_MAGIC */
    uint32_t opcode;  /* echoes the request opcode */
    uint32_t req_id;  /* echoes the request id */
    int32_t status;   /* 0, or a negative errno */
    uint64_t out0;    /* op-dependent: ino / size / total byte count and so on */
    uint64_t out1;    /* op-dependent: type / remaining amount and so on */
    uint64_t out2;    /* reserved */
    uint32_t payload_len;
    uint32_t _pad;
} ufs_resp_hdr_t;

#define UFS_ROOT_INO 1ull

/* Effective payload limit within a single message (the channel limit minus
 * the two header allowances) */
#define UFS_MAX_PAYLOAD (48u * 1024u)

#endif /* FS_UFS_PROTO_H */
