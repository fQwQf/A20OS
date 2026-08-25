#ifndef _FS_QUOTA_H
#define _FS_QUOTA_H

#include "core/types.h"

struct mount;
struct vnode;
typedef struct mount mount_t;
typedef struct vnode vnode_t;

/* linux/quota.h command base values; userspace passes QCMD(cmd, type). */
#define USRQUOTA   0
#define GRPQUOTA   1
#define MAXQUOTAS  2

#define Q_SYNC         0x800001
#define Q_QUOTAON      0x800002
#define Q_QUOTAOFF     0x800003
#define Q_GETFMT       0x800004
#define Q_GETINFO      0x800005
#define Q_SETINFO      0x800006
#define Q_GETQUOTA     0x800007
#define Q_SETQUOTA     0x800008
#define Q_GETNEXTQUOTA 0x800009

#define SUBCMDSHIFT 8
#define SUBCMDMASK  0x00ffU
#define QCMD(cmd, type) ((((uint32_t)(cmd)) << SUBCMDSHIFT) | \
                         ((uint32_t)(type) & SUBCMDMASK))

#define QFMT_VFS_OLD 1
#define QFMT_VFS_V0  2
#define QFMT_VFS_V1  4

#ifndef _LINUX_QUOTA_CONSTANTS_FROM_UAPI
struct if_dqblk {
    uint64_t dqb_bhardlimit;
    uint64_t dqb_bsoftlimit;
    uint64_t dqb_curspace;
    uint64_t dqb_ihardlimit;
    uint64_t dqb_isoftlimit;
    uint64_t dqb_curinodes;
    uint64_t dqb_btime;
    uint64_t dqb_itime;
    uint32_t dqb_valid;
};

struct if_nextdqblk {
    uint64_t dqb_bhardlimit;
    uint64_t dqb_bsoftlimit;
    uint64_t dqb_curspace;
    uint64_t dqb_ihardlimit;
    uint64_t dqb_isoftlimit;
    uint64_t dqb_curinodes;
    uint64_t dqb_btime;
    uint64_t dqb_itime;
    uint32_t dqb_valid;
    uint32_t dqb_id;
};

struct if_dqinfo {
    uint64_t dqi_bgrace;
    uint64_t dqi_igrace;
    uint32_t dqi_flags;
    uint32_t dqi_valid;
};
#endif

#define QIF_BLIMITS  (1U << 0)
#define QIF_SPACE    (1U << 1)
#define QIF_ILIMITS  (1U << 2)
#define QIF_INODES   (1U << 3)
#define QIF_BTIME    (1U << 4)
#define QIF_ITIME    (1U << 5)

int  quota_init(void);
int  quota_cmd(uint32_t cmd, const char *special, uint32_t id, void *addr);
int  quota_cmd_fd(int fd, uint32_t cmd, uint32_t id, void *addr);

/* Enforcement/accounting hooks called from the VFS core. */
int  quota_check_space(vnode_t *vn, uint64_t extra_bytes);
int  quota_check_inode_mnt(mount_t *mnt, uint32_t uid, uint32_t gid);
void quota_account_space(vnode_t *vn, int64_t bytes_delta);
void quota_account_inode_create(vnode_t *vn);
void quota_account_inode_ids(mount_t *mnt, uint32_t uid, uint32_t gid);
void quota_account_inode_remove(vnode_t *vn, uint64_t freed_bytes);

#endif /* _FS_QUOTA_H */
