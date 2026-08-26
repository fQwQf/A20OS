/*
 * uxfs.h — 用户态文件系统代理（uxfs）的内核侧接口。
 *
 * 设计与边界见 docs/hybrid-kernel/06-user-fs.md。ABI 层的 fs_serve /
 * fs_block_io syscall（kernel/abi/native/sys_native_fs.c）经此接口完成
 * 挂载注册与受控块 IO。
 */
#ifndef FS_UXFS_H
#define FS_UXFS_H

#include "core/types.h"

struct a20_channel_ep;
struct task_t;
struct vnode;

/*
 * uxfs_serve_mount — 把 @ep 指向的用户态文件服务挂载到 @path。
 *
 * @path          挂载点（必须已存在且为目录）
 * @ep            服务端 channel 端点；成功后引用所有权移交 uxfs
 * @server        发起注册的服务任务（block IO 所有权校验用）
 * @block_index   服务可访问的块设备 class 序号；<0 表示无块后端
 * @serve_flags   bit0：服务端声明只读后端（如 iso9660）；置位时挂载
 *                标记 VFS_MOUNT_RDONLY，页缓存缓冲写据此禁用
 *
 * 返回 0 或负 errno。挂载前会先做 UFS_OP_INIT 握手，服务不可用时报
 * -EIO/-ETIMEDOUT。
 */
int uxfs_serve_mount(const char *path, struct a20_channel_ep *ep,
                     struct task_t *server, int block_index,
                     uint32_t serve_flags);

/* umount 收尾：释放服务端点引用（由 vfs_umount 的 FS_TYPE 分支调用）。 */
void uxfs_unmount(struct vnode *root);

/*
 * uxfs_block_io — 受控块 IO：仅当 @task 是当前 uxfs 服务任务时允许，
 * 避免任意进程绕过文件系统直接读写服务盘。@write 为 0 读 1 写。
 * @buf 指向内核缓冲，@lba/@count 以扇区为单位。
 */
int uxfs_block_io(struct task_t *task, int block_index, int write, uint64_t lba,
                  void *buf, uint32_t count);

/* 容量查询（扇区数）：同受控语义；服务进程用于初始化块设备描述符。 */
int uxfs_block_capacity(struct task_t *task, int block_index,
                        uint64_t *out_sectors);

#endif /* FS_UXFS_H */
