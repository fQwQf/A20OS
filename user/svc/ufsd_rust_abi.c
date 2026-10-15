/* Thin Native SDK and filesystem-backend calls for the Rust ufsd supervisor. */
#include <stdint.h>
#include "liba20rt/a20_sdk.h"
#include "abi/native/syscall_nr.h"
#include "core/types.h"
#include "core/string.h"
#include "fs/ufs_proto.h"
#include "ufs_backends.h"
#include "ufsd_rust_abi.h"

uint8_t ufs_tx[UFSD_MSG_MAX];
void (*ufs_log_sink)(const char *line);
static int32_t g_block_index = -1;
static uint32_t g_stdout_handle = A20_HANDLE_NULL;
extern void (*fscompat_log_sink)(const char *line);

static void fscompat_log_to_stdout(const char *line)
{
    if (!line || g_stdout_handle == A20_HANDLE_NULL) return;
    uint32_t length = 0;
    while (line[length] && length < 255) length++;
    if (length) a20_ufsd_log(g_stdout_handle, (const uint8_t *)line, length);
}

typedef struct {
    uint32_t size, version, server_channel;
    int32_t block_index;
    uint64_t target;
    uint32_t target_len, flags;
} ufsd_fs_serve_args_t;

typedef struct {
    uint32_t size, version;
    int32_t block_index;
    uint32_t write;
    uint64_t lba;
    uint32_t count, pad;
    uint64_t buf;
} ufsd_fs_block_io_args_t;

a20_start_info_t *a20_get_start_info(void);

static const ufs_backend_t *backend(uint32_t kind)
{
    if (kind == A20_UFSD_BACKEND_FAT) return &UFS_BACKEND_FAT;
    if (kind == A20_UFSD_BACKEND_VNFS) return &UFS_BACKEND_VNFS;
    return 0;
}

int64_t a20_ufsd_startup(uint32_t *stdout_handle)
{
    if (!stdout_handle) return -A20_ERR_INVALID_ARGUMENT;
    a20_start_info_t *si = a20_get_start_info();
    if (!si) return -A20_ERR_NOT_FOUND;
    *stdout_handle = si->stdout_handle;
    g_stdout_handle = si->stdout_handle;
    fscompat_log_sink = fscompat_log_to_stdout;
    return A20_OK;
}

/* Used only by the checkpoint-failure integration fixture; writes through
 * the task's captured output handle so the marker survives immediate SIGKILL. */
void a20_ufsd_test_log(const char *line)
{
    if (!line || g_stdout_handle == A20_HANDLE_NULL) return;
    uint32_t length = 0;
    while (line[length] && length < 160) length++;
    if (length) (void)a20_hdl_write_buf(g_stdout_handle, line, length, 0);
    (void)a20_hdl_write_buf(g_stdout_handle, "\n", 1, 0);
}

int64_t a20_ufsd_set_block_index(int32_t block_index)
{
    g_block_index = block_index;
    return A20_OK;
}

/* Backends keep their block IO behind this service-owned syscall context. */
int fsio_read(uint32_t lba, void *buf, uint32_t count)
{
    if (g_block_index < 0 || count > 4096) return 1;
    ufsd_fs_block_io_args_t a = {0};
    a.size = sizeof(a); a.version = 1; a.block_index = g_block_index;
    a.write = 0; a.lba = lba; a.count = count; a.buf = (uint64_t)(uintptr_t)buf;
    return a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                         (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0)) ? 0 : 1;
}

int fsio_write(uint32_t lba, const void *buf, uint32_t count)
{
    if (g_block_index < 0 || count > 4096) return 1;
    ufsd_fs_block_io_args_t a = {0};
    a.size = sizeof(a); a.version = 1; a.block_index = g_block_index;
    a.write = 1; a.lba = lba; a.count = count; a.buf = (uint64_t)(uintptr_t)buf;
    return a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                         (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0)) ? 0 : 1;
}

/* UXFS v1 extension: count=0/write=2/buf=0 requests a durability barrier.
 * Existing read/write/capacity calls keep their original wire layout. */
int fsio_flush(void)
{
    if (g_block_index < 0) return -19; /* ENODEV */
    ufsd_fs_block_io_args_t a = {0};
    a.size = sizeof(a); a.version = 1; a.block_index = g_block_index;
    a.write = 2;
    a20_status_t status = a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(status)) return 0;
    if (status == -A20_ERR_NOT_SUPPORTED) return -95; /* EOPNOTSUPP */
    if (status == -A20_ERR_INVALID_ARGUMENT) return -22; /* EINVAL */
    if (status == -A20_ERR_BAD_HANDLE) return -19; /* ENODEV */
    return -5; /* EIO */
}

typedef int64_t (*fsio_syscall6_fn)(uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t);

int fsio_flush_query_via(fsio_syscall6_fn syscall_fn, int32_t block_index,
                         uint64_t *response)
{
    if (block_index < 0 || !syscall_fn || !response) return 0;
    *response = 0;
    ufsd_fs_block_io_args_t a = {0};
    a.size = sizeof(a); a.version = 2; a.block_index = block_index;
    a.write = 2; a.buf = (uint64_t)(uintptr_t)response;
    if (!a20_status_is_ok(syscall_fn(A20_SYS_fs_block_io,
                                    (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0)))
        return 0;
    return *response == 0x5546585300000001ull;
}

int fsio_flush_supported_via(fsio_syscall6_fn syscall_fn)
{
    uint64_t response = 0;
    return fsio_flush_query_via(syscall_fn, g_block_index, &response);
}

int fsio_flush_supported(void)
{
    return fsio_flush_supported_via(a20_syscall6);
}

uint64_t fsio_capacity_sectors(void)
{
    static uint64_t cached;
    if (cached || g_block_index < 0) return cached;
    ufsd_fs_block_io_args_t a = {0};
    a.size = sizeof(a); a.version = 1; a.block_index = g_block_index;
    a.write = 0; a.count = 0; a.buf = (uint64_t)(uintptr_t)&cached;
    if (!a20_status_is_ok(a20_syscall6(A20_SYS_fs_block_io,
                                       (uint64_t)(uintptr_t)&a, 0, 0, 0, 0, 0)))
        return 0;
    return cached;
}

int64_t a20_ufsd_channel_create(uint32_t endpoints[2])
{
    if (!endpoints) return -A20_ERR_INVALID_ARGUMENT;
    a20_channel_pair_t pair = {{A20_HANDLE_NULL, A20_HANDLE_NULL}};
    a20_status_t status = a20_channel_create(&pair);
    if (status >= 0) { endpoints[0] = pair.endpoints[0]; endpoints[1] = pair.endpoints[1]; }
    return status;
}

int64_t a20_ufsd_path_create(const char *path, uint32_t path_len)
{
    if (!path) return -A20_ERR_INVALID_ARGUMENT;
    a20_path_create_args_t args = {0};
    args.size = sizeof(args); args.version = 1; args.dir = A20_HANDLE_NULL;
    args.type = 1; args.mode = 0755; args.path = (uint64_t)(uintptr_t)path;
    args.path_len = path_len;
    return a20_syscall6(A20_SYS_path_create, (uint64_t)(uintptr_t)&args, 0, 0, 0, 0, 0);
}

int64_t a20_ufsd_serve(uint32_t endpoint, int32_t block_index,
                       const char *path, uint32_t path_len, uint32_t flags)
{
    if (!path) return -A20_ERR_INVALID_ARGUMENT;
    ufsd_fs_serve_args_t args = {0};
    args.size = sizeof(args); args.version = 1; args.server_channel = endpoint;
    args.block_index = block_index; args.target = (uint64_t)(uintptr_t)path;
    args.target_len = path_len; args.flags = flags;
    return a20_syscall6(A20_SYS_fs_serve, (uint64_t)(uintptr_t)&args, 0, 0, 0, 0, 0);
}

int64_t a20_ufsd_umount(const char *path, uint32_t path_len)
{
    return a20_syscall6(A20_SYS_fs_umount, (uint64_t)(uintptr_t)path,
                        path_len, 0, 0, 0, 0);
}

int64_t a20_ufsd_capacity(int32_t block_index, uint64_t *sectors)
{
    if (!sectors) return -A20_ERR_INVALID_ARGUMENT;
    g_block_index = block_index;
    *sectors = fsio_capacity_sectors();
    return A20_OK;
}

int64_t a20_ufsd_event_queue_create(uint32_t *queue)
{ return queue ? a20_event_queue_create(queue) : -A20_ERR_INVALID_ARGUMENT; }

int64_t a20_ufsd_event_watch(uint32_t queue, uint32_t target,
                             uint64_t mask, uint64_t tag)
{ return a20_event_watch(queue, target, mask, tag); }

int64_t a20_ufsd_event_wait(uint32_t queue)
{
    a20_event_t event;
    a20_time_t infinite = { .secs = (uint64_t)-1, .nsecs = 0 };
    a20_status_t status = a20_event_wait(queue, infinite, &event);
    return status < 0 ? status : (int64_t)event.user_data;
}

int64_t a20_ufsd_recv(uint32_t endpoint, uint8_t *bytes,
                      uint32_t capacity, uint32_t *length)
{
    if (!bytes || !length) return -A20_ERR_INVALID_ARGUMENT;
    uint32_t n = capacity, handles = 0;
    a20_status_t status = a20_channel_recv_flags(endpoint, bytes, &n, 0,
                                                  &handles, A20_MSG_NONBLOCK);
    if (status >= 0) *length = n;
    return status;
}

int64_t a20_ufsd_send(uint32_t endpoint, const ufs_resp_hdr_t *response)
{
    if (!response || response->payload_len > ufs_payload_cap())
        return -A20_ERR_INVALID_ARGUMENT;
    memcpy(ufs_tx, response, sizeof(*response));
    return a20_channel_send(endpoint, ufs_tx,
                            sizeof(*response) + response->payload_len, 0, 0);
}

int64_t a20_ufsd_send_bytes(uint32_t endpoint, const uint8_t *bytes, uint32_t length)
{ return a20_channel_send(endpoint, bytes, length, 0, 0); }

int64_t a20_ufsd_close(uint32_t handle)
{ return handle == A20_HANDLE_NULL ? A20_OK : a20_hdl_close(handle); }

void a20_ufsd_log(uint32_t handle, const uint8_t *bytes, uint32_t length)
{ if (bytes && length) (void)a20_hdl_write_buf(handle, bytes, length, 0); }

void a20_ufsd_exit(int32_t code)
{ a20_task_exit(code); }

uint8_t *a20_ufsd_payload_buffer(void) { return ufs_payload_buf(); }
uint32_t a20_ufsd_payload_capacity(void) { return ufs_payload_cap(); }

int64_t a20_ufsd_backend_mount(uint32_t kind, const char *fstype)
{ const ufs_backend_t *b = backend(kind); return b ? b->mount(fstype) : -U_ENOSYS; }
extern int vnfs_mount_generic_readonly(const char *fstype);
int64_t a20_ufsd_backend_mount_readonly(uint32_t kind, const char *fstype)
{ return kind == A20_UFSD_BACKEND_VNFS ? vnfs_mount_generic_readonly(fstype) : -U_ENOSYS; }
extern int64_t vnfs_shutdown(void);
int64_t a20_ufsd_backend_unmount(uint32_t kind)
{ return kind == A20_UFSD_BACKEND_VNFS ? vnfs_shutdown() : 0; }
int64_t a20_ufsd_backend_lookup(uint32_t k, uint64_t ino, const char *n, uint32_t nl, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->lookup(ino,n,nl,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_getattr(uint32_t k, uint64_t ino, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->getattr(ino,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_readdir(uint32_t k, uint64_t ino, uint64_t skip, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->readdir(ino,skip,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_create(uint32_t k, uint64_t ino, const char *n, uint32_t mode, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->create(ino,n,mode,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_mkdir(uint32_t k, uint64_t ino, const char *n, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->mkdir(ino,n,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_unlink(uint32_t k, uint64_t ino, const char *n)
{ const ufs_backend_t *b=backend(k); return b ? b->unlink(ino,n) : -U_ENOSYS; }
extern int64_t vnfs_link(uint64_t dir_ino, const char *name, uint64_t target_ino);
int64_t a20_ufsd_backend_link(uint32_t k, uint64_t dir, const char *n,
                              uint64_t target)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_link(dir,n,target) : -U_ENOSYS; }
int64_t a20_ufsd_backend_rmdir(uint32_t k, uint64_t ino, const char *n)
{ const ufs_backend_t *b=backend(k); return b ? b->rmdir(ino,n) : -U_ENOSYS; }
int64_t a20_ufsd_backend_truncate(uint32_t k, uint64_t ino, uint64_t size)
{ const ufs_backend_t *b=backend(k); return b ? b->truncate(ino,size) : -U_ENOSYS; }
int64_t a20_ufsd_backend_read(uint32_t k, uint64_t ino, uint64_t off, uint32_t count, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->read(ino,off,count,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_write(uint32_t k, uint64_t ino, uint64_t off, const uint8_t *p, uint32_t n, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->write(ino,off,p,n,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_rename(uint32_t k, uint64_t od, const char *on, uint64_t nd, const char *nn, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); return b ? b->rename(od,on,nd,nn,r) : -U_ENOSYS; }
int64_t a20_ufsd_backend_sync(uint32_t k)
{ const ufs_backend_t *b=backend(k); return b ? b->sync() : -U_ENOSYS; }
void a20_ufsd_backend_statfs(uint32_t k, ufs_resp_hdr_t *r)
{ const ufs_backend_t *b=backend(k); if (b) b->statfs(r); }

/* Lifecycle callbacks for the Rust-owned bounded inode/open registry.  C
 * supplies only vnode reference operations and the post-unlink nlink query. */
extern int64_t vnfs_has_inode(uint64_t ino);
extern int64_t vnfs_inode_open(uint64_t ino);
extern int64_t vnfs_inode_close(uint64_t ino);
extern int64_t vnfs_inode_link_count(uint64_t ino, uint64_t *links_out);
extern int64_t vnfs_inode_reap_unlinked(uint64_t ino);
extern int64_t vnfs_inode_drop(uint64_t ino);

int64_t a20_ufsd_backend_has_inode(uint32_t k, uint64_t ino)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_has_inode(ino) : -95; }
int64_t a20_ufsd_backend_open(uint32_t k, uint64_t ino)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_inode_open(ino) : -95; }
int64_t a20_ufsd_backend_close(uint32_t k, uint64_t ino)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_inode_close(ino) : -95; }
int64_t a20_ufsd_backend_link_count(uint32_t k, uint64_t ino,
                                    uint64_t *links_out)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_inode_link_count(ino, links_out) : -95; }
int64_t a20_ufsd_backend_reap_unlinked(uint32_t k, uint64_t ino)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_inode_reap_unlinked(ino) : -95; }
int64_t a20_ufsd_backend_drop_inode(uint32_t k, uint64_t ino)
{ return k == A20_UFSD_BACKEND_VNFS ? vnfs_inode_drop(ino) : -95; }
